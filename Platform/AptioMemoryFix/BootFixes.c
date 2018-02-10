/**

  Methods for setting callback jump from kernel entry point, callback, fixes to kernel boot image.

  by dmazar

**/

#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/LoadedImage.h>

#include "Config.h"
#include "BootArgs.h"
#include "BootFixes.h"
#include "AsmFuncs.h"
#include "VMem.h"
#include "Lib.h"
#include "Mach-O/Mach-O.h"
#include "Hibernate.h"
#include "CustomSlide.h"
#include "Utils.h"
#include "RtShims.h"


// buffer and size for original kernel entry code
STATIC UINT8 gOrigKernelCode[32];
STATIC UINTN gOrigKernelCodeSize = 0;

// buffer for virtual address map - only for RT areas
// note: DescriptorSize is usually > sizeof(EFI_MEMORY_DESCRIPTOR)
// so this buffer can hold less then 64 descriptors
STATIC EFI_MEMORY_DESCRIPTOR gVirtualMemoryMap[64];
STATIC UINTN gVirtualMapSize = 0;
STATIC UINTN gVirtualMapDescriptorSize = 0;

STATIC RT_RELOC_PROTECT_DATA gRelocInfoData;

// XNU requires gST pointers to be passed relative to boot.efi.
// We have to allocate a new system table and let boot.efi relocate it.
EFI_PHYSICAL_ADDRESS gSysTableRtArea = 0;
EFI_PHYSICAL_ADDRESS gRelocatedSysTableRtArea = 0;

// TRUE if we are doing hibernate wake
BOOLEAN gHibernateWake = FALSE;

// TRUE if booting with -aptiodump
BOOLEAN gDumpMemArgPresent = FALSE;
// TRUE if booting with a manually specified slide=X
BOOLEAN gSlideArgPresent = FALSE;

//
// Kernel entry patching
//

/** Saves current 64 bit state and copies JumpToKernel32 function to higher mem
  * (for copying kernel back to proper place and jumping back to it).
  */
EFI_STATUS
PrepareJumpFromKernel (
  VOID
  )
{
  EFI_STATUS              Status;
  EFI_PHYSICAL_ADDRESS    HigherMem;
  UINTN                   Size;

  //
  // Check if already prepared
  //
  if (JumpToKernel32Addr != 0) {
    DEBUG ((DEBUG_VERBOSE, "PrepareJumpFromKernel() - already prepared\n"));
    return EFI_SUCCESS;
  }

  //
  // Save current 64bit state - will be restored later in callback from kernel jump
  //
  AsmPrepareJumpFromKernel();

  //
  // Allocate higher memory for JumpToKernel code
  // Must be 32-bit to access via a relative jump
  //
  HigherMem = BASE_4GB;
  Status = AllocatePagesFromTop(EfiBootServicesCode, 1, &HigherMem, FALSE);
  if (Status != EFI_SUCCESS) {
    PrintScreen (L"AMF: Failed to allocate JumpToKernel memory (0x%X pages on mem top) - %r\n",
      1, Status);
    return Status;
  }

  //
  // And relocate it to higher mem
  //
  JumpToKernel32Addr = HigherMem + ( (UINT8*)(UINTN)&JumpToKernel32 - (UINT8*)(UINTN)&JumpToKernel );
  JumpToKernel64Addr = HigherMem + ( (UINT8*)(UINTN)&JumpToKernel64 - (UINT8*)(UINTN)&JumpToKernel );

  Size = (UINT8*)&JumpToKernelEnd - (UINT8*)&JumpToKernel;
  if (Size > EFI_PAGES_TO_SIZE (1)) {
    PrintScreen (L"AMF: JumpToKernel32 size is too big - %ld\n", Size);
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem((VOID *)(UINTN)HigherMem, (VOID *)&JumpToKernel, Size);

  DEBUG ((DEBUG_VERBOSE, "PrepareJumpFromKernel(): JumpToKernel relocated from %p, to %x, size = %x\n",
    &JumpToKernel, HigherMem, Size));
  DEBUG ((DEBUG_VERBOSE, " JumpToKernel32 relocated from %p, to %x\n",
    &JumpToKernel32, JumpToKernel32Addr));
  DEBUG ((DEBUG_VERBOSE, " JumpToKernel64 relocated from %p, to %x\n",
    &JumpToKernel64, JumpToKernel64Addr));
  DEBUG ((DEBUG_VERBOSE, "SavedCR3 = %x, SavedGDTR = %x, SavedIDTR = %x\n", SavedCR3, SavedGDTR, SavedIDTR));

  DEBUG ((DEBUG_VERBOSE, "PrepareJumpFromKernel(): JumpToKernel relocated from %p, to %x, size = %x\n",
    &JumpToKernel, HigherMem, Size));

  //
  // Allocate 1 RT data page for copy of EFI system table for kernel
  // This one also has to be 32-bit due to XNU BootArgs structure
  //
  gSysTableRtArea = BASE_4GB;
  Status = AllocatePagesFromTop(EfiRuntimeServicesData, 1, &gSysTableRtArea, FALSE);
  if (Status != EFI_SUCCESS) {
    PrintScreen (L"AMF: Failed to allocate RT memory for system table - %r\n",
      1, Status);
    return Status;
  }
  DEBUG ((DEBUG_VERBOSE, "gSysTableRtArea = %lx\n", gSysTableRtArea));

  // Copy sys table to our location
  EFI_SYSTEM_TABLE *Src  = (EFI_SYSTEM_TABLE*)(UINTN)gST;
  EFI_SYSTEM_TABLE *Dest = (EFI_SYSTEM_TABLE*)(UINTN)gSysTableRtArea;
  DEBUG ((DEBUG_VERBOSE, "-Copy %p <- %p, size=0x%lx\n", Dest, Src, Src->Hdr.HeaderSize));
  CopyMem(Dest, Src, Src->Hdr.HeaderSize);

  return Status;
}

/** Patches kernel entry point with jump to AsmJumpFromKernel (AsmFuncsX64). This will then call KernelEntryPatchJumpBack. */
EFI_STATUS
KernelEntryPatchJump (
  UINT32 KernelEntry
  )
{
  EFI_STATUS  Status;

  Status = EFI_SUCCESS;

  DEBUG ((DEBUG_VERBOSE, "KernelEntryPatchJump KernelEntry (reloc): %lx (%lx)\n", KernelEntry, KernelEntry));

  // Size of EntryPatchCode code
  gOrigKernelCodeSize = (UINT8*)&EntryPatchCodeEnd - (UINT8*)&EntryPatchCode;
  if (gOrigKernelCodeSize > sizeof(gOrigKernelCode)) {
    DEBUG ((DEBUG_WARN, "KernelEntryPatchJump: not enough space for orig. kernel entry code: size needed: %d\n", gOrigKernelCodeSize));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_VERBOSE, "EntryPatchCode: %p, Size: %d, AsmJumpFromKernel: %p\n", &EntryPatchCode, gOrigKernelCodeSize, &AsmJumpFromKernel));

  // Save original kernel entry code
  CopyMem((VOID *)gOrigKernelCode, (VOID *)(UINTN)KernelEntry, gOrigKernelCodeSize);

  // Copy EntryPatchCode code to kernel entry address
  CopyMem((VOID *)(UINTN)KernelEntry, (VOID *)&EntryPatchCode, gOrigKernelCodeSize);

  // pass KernelEntry to assembler funcs
  // this is not needed really, since asm code will determine
  // kernel entry address from the stack
  AsmKernelEntry = KernelEntry;

  return Status;
}

/** Reads kernel entry from Mach-O load command and patches it with jump to AsmJumpFromKernel. */
EFI_STATUS
KernelEntryFromMachOPatchJump(VOID *MachOImage, UINTN SlideAddr)
{
  UINTN  KernelEntry;

  DEBUG ((DEBUG_VERBOSE, "KernelEntryFromMachOPatchJump: MachOImage = %p, SlideAddr = %x\n", MachOImage, SlideAddr));

  KernelEntry = MachOGetEntryAddress(MachOImage);
  DEBUG ((DEBUG_VERBOSE, "KernelEntryFromMachOPatchJump: KernelEntry = %x\n", KernelEntry));

  if (KernelEntry == 0) {
    return EFI_NOT_FOUND;
  }

  if (SlideAddr > 0) {
    KernelEntry += SlideAddr;
    DEBUG ((DEBUG_VERBOSE, "KernelEntryFromMachOPatchJump: Slided KernelEntry = %x\n", KernelEntry));
  }

  return KernelEntryPatchJump((UINT32)KernelEntry);
}

//
// Boot fixes
//

/** Copies RT flagged areas to separate memmap, defines virtual to phisycal address mapping
 * and calls SetVirtualAddressMap() only with that partial memmap.
 *
 * About partial memmap:
 * Some UEFIs are converting pointers to virtual addresses even if they do not
 * point to regions with RT flag. This means that those UEFIs are using
 * Desc->VirtualStart even for non-RT regions. Linux had issues with this:
 * http://git.kernel.org/?p=linux/kernel/git/torvalds/linux-2.6.git;a=commit;h=7cb00b72876ea2451eb79d468da0e8fb9134aa8a
 * They are doing it Windows way now - copying RT descriptors to separate
 * mem map and passing that stripped map to SetVirtualAddressMap().
 * We'll do the same, although it seems that just assigning
 * VirtualStart = PhysicalStart for non-RT areas also does the job.
 *
 * About virtual to phisycal mappings:
 * Also adds virtual to phisycal address mappings for RT areas. This is needed since
 * SetVirtualAddressMap() does not work on my Aptio without that. Probably because some driver
 * has a bug and is trying to access new virtual addresses during the change.
 * Linux and Windows are doing the same thing and problem is
 * not visible there.
 */
EFI_STATUS
ExecSetVirtualAddressesToMemMap (
  IN UINTN                    MemoryMapSize,
  IN UINTN                    DescriptorSize,
  IN UINT32                   DescriptorVersion,
  IN EFI_MEMORY_DESCRIPTOR    *MemoryMap
  )
{
  UINTN                           NumEntries;
  UINTN                           Index;
  EFI_MEMORY_DESCRIPTOR           *Desc;
  EFI_MEMORY_DESCRIPTOR           *VirtualDesc;
  EFI_STATUS                      Status;
  PAGE_MAP_AND_DIRECTORY_POINTER  *PageTable;
  UINTN                           Flags;
  UINTN                           BlockSize;

  Desc                      = MemoryMap;
  NumEntries                = MemoryMapSize / DescriptorSize;
  VirtualDesc               = gVirtualMemoryMap;
  gVirtualMapSize           = 0;
  gVirtualMapDescriptorSize = DescriptorSize;
  DEBUG ((DEBUG_VERBOSE, "ExecSetVirtualAddressesToMemMap: Size=%d, Addr=%p, DescSize=%d\n", MemoryMapSize, MemoryMap, DescriptorSize));

  // get current VM page table
  GetCurrentPageTable (&PageTable, &Flags);

  for (Index = 0; Index < NumEntries; Index++) {
    //
    // Some UEFIs end up with "reserved" area with EFI_MEMORY_RUNTIME flag set when Intel HD3000 or HD4000 is used.
    // For example, on GA-H81N-D2H there is a single 1 GB descriptor:
    // 000000009F800000-00000000DF9FFFFF 0000000000040200 8000000000000000
    //
    // All known boot.efi starting from at least 10.5.8 properly handle this flag and do not assign virtual addresses
    // to reserved descriptors.
    // However, the issue was with AptioFix itself, which did not check for EfiReservedMemoryType and replaced
    // it by EfiMemoryMappedIO to prevent boot.efi relocations.
    //
    // The relevant discussion and the original fix can be found here:
    // http://web.archive.org/web/20141111124211/http://www.projectosx.com:80/forum/lofiversion/index.php/t2428-450.html
    // https://sourceforge.net/p/cloverefiboot/code/605/
    //
    // Since it is not the bug in boot.efi, AptioMemoryFix only needs to properly handle EfiReservedMemoryType with
    // EFI_MEMORY_RUNTIME attribute set, and there is no reason to mess with the memory map passed to boot.efi.
    //
    if (Desc->Type != EfiReservedMemoryType && (Desc->Attribute & EFI_MEMORY_RUNTIME) != 0) {
      // check if there is enough space in gVirtualMemoryMap
      if (gVirtualMapSize + DescriptorSize > sizeof(gVirtualMemoryMap)) {
        DEBUG ((DEBUG_WARN, "ERROR: too much mem map RT areas\n"));
        return EFI_OUT_OF_RESOURCES;
      }

      // copy region with EFI_MEMORY_RUNTIME flag to gVirtualMemoryMap
      CopyMem ((VOID*)VirtualDesc, (VOID*)Desc, DescriptorSize);

      // define virtual to phisical mapping
      DEBUG ((DEBUG_VERBOSE, "Map pages: %lx (%x) -> %lx\n", Desc->VirtualStart, Desc->NumberOfPages, Desc->PhysicalStart));
      VmMapVirtualPages (PageTable, Desc->VirtualStart, Desc->NumberOfPages, Desc->PhysicalStart);

      // next gVirtualMemoryMap slot
      VirtualDesc = NEXT_MEMORY_DESCRIPTOR (VirtualDesc, DescriptorSize);
      gVirtualMapSize += DescriptorSize;

      // Remember future physical address for our special relocated
      // efi system table
      BlockSize = EFI_PAGES_TO_SIZE ((UINTN)Desc->NumberOfPages);
      if (Desc->PhysicalStart <= gSysTableRtArea &&  gSysTableRtArea < (Desc->PhysicalStart + BlockSize)) {
        // block contains our future sys table - remember new address
        // future physical = VirtualStart & 0x7FFFFFFFFF
        gRelocatedSysTableRtArea = (Desc->VirtualStart & 0x7FFFFFFFFF) + (gSysTableRtArea - Desc->PhysicalStart);
      }
    }

    Desc = NEXT_MEMORY_DESCRIPTOR (Desc, DescriptorSize);
  }

  VmFlashCaches ();

  DEBUG ((DEBUG_VERBOSE, "ExecSetVirtualAddressesToMemMap: Size=%d, Addr=%p, DescSize=%d\nSetVirtualAddressMap ... ",
    gVirtualMapSize, MemoryMap, DescriptorSize));
  Status = gRT->SetVirtualAddressMap (gVirtualMapSize, DescriptorSize, DescriptorVersion, gVirtualMemoryMap);
  DEBUG ((DEBUG_VERBOSE, "%r\n", Status));

  return Status;
}

VOID
CopyEfiSysTableToSeparateRtDataArea (
  IN OUT UINT32   *EfiSystemTable
  )
{
  EFI_SYSTEM_TABLE  *Src;
  EFI_SYSTEM_TABLE  *Dest;

  Src = (EFI_SYSTEM_TABLE*)(UINTN)*EfiSystemTable;
  Dest = (EFI_SYSTEM_TABLE*)(UINTN)gSysTableRtArea;

  DEBUG ((DEBUG_VERBOSE, "-Copy %p <- %p, size=0x%lx\n", Dest, Src, Src->Hdr.HeaderSize));
  CopyMem(Dest, Src, Src->Hdr.HeaderSize);

  *EfiSystemTable = (UINT32)(UINTN)Dest;
}

VOID
ReadBooterArguments (
    CHAR16 *Options,
    UINTN OptionsSize
)
{
  EFI_STATUS Status;

  // Just in case we do not have 0-termination
  UINTN LastIndex = OptionsSize - 1;
  CHAR16 Last = Options[LastIndex];
  Options[LastIndex] = '\0';

  {
    CHAR16 *Slide = StrStr(Options, L"slide=");
    VERIFY_BOOT_ARG(Slide, Options, L"slide=");
    gSlideArgPresent |= Slide != NULL;

#if APTIOFIX_ALLOW_MEMORY_DUMP_ARG == 1
    CHAR16 *Dump  = StrStr(Options, L"-aptiodump");
    VERIFY_BOOT_ARG(Dump,  Options, L"-aptiodump");
    gDumpMemArgPresent |= Dump != NULL;
#endif

    Options[LastIndex] = Last;

    if (Slide) {
      DEBUG((DEBUG_VERBOSE, "Found custom slide param\n"));
    }
  }

  CHAR8 BootArgsVar[BOOT_LINE_LENGTH];
  UINTN BootArgsVarLen = BOOT_LINE_LENGTH;

  // Important to avoid triggering boot-args wrapper too early
  Status = ((EFI_GET_VARIABLE)gGetVariable)(
      L"boot-args",
      &gAppleBootVariableGuid,
      NULL, &BootArgsVarLen,
      &BootArgsVar[0]
  );

  if (!EFI_ERROR(Status) && BootArgsVarLen > 0) {
    // Just in case we do not have 0-termination
    BootArgsVar[BootArgsVarLen-1] = '\0';

    CHAR8 *Slide = AsciiStrStr(BootArgsVar, "slide=");
    VERIFY_BOOT_ARG(Slide, BootArgsVar, "slide=");
    gSlideArgPresent |= Slide != NULL;

#if APTIOFIX_ALLOW_MEMORY_DUMP_ARG == 1
    CHAR8 *Dump  = AsciiStrStr(BootArgsVar, "-aptiodump");
    VERIFY_BOOT_ARG(Dump,  BootArgsVar, "-aptiodump");
    gDumpMemArgPresent |= Dump != NULL;
#endif

    if (Slide) {
      DEBUG((DEBUG_VERBOSE, "Found custom slide boot-arg value\n"));
    }
  }
}


VOID
RestoreRelocInfoProtectMemTypes (
  UINTN                   MemoryMapSize,
  UINTN                   DescriptorSize,
  EFI_MEMORY_DESCRIPTOR   *MemoryMap
  )
{
  UINTN Index;
  UINTN Index2;
  UINTN NumEntriesLeft;

  NumEntriesLeft = gRelocInfoData.NumEntries;

  if (NumEntriesLeft > 0) {
    for (Index = 0; Index < (MemoryMapSize / DescriptorSize); ++Index) {
      if (NumEntriesLeft > 0) {
        for (Index2 = 0; Index2 < gRelocInfoData.NumEntries; ++Index2) {
          if (MemoryMap->PhysicalStart == gRelocInfoData.RelocInfo[Index2].PhysicalStart) {
            MemoryMap->Type = gRelocInfoData.RelocInfo[Index2].Type;
            --NumEntriesLeft;
          }
        }
      }

      MemoryMap = NEXT_MEMORY_DESCRIPTOR (MemoryMap, DescriptorSize);
    }
  }
}

/** Protect RT data from relocation by marking them MemMapIO. Except area with EFI system table.
 *  This one must be relocated into kernel boot image or kernel will crash (kernel accesses it
 *  before RT areas are mapped into vm).
 *  This fixes NVRAM issues on some boards where access to nvram after boot services is possible
 *  only in SMM mode. RT driver passes data to SM handler through previously negotiated buffer
 *  and this buffer must not be relocated.
 *  Explained and examined in detail by CodeRush and night199uk:
 *  http://www.projectosx.com/forum/index.php?showtopic=3298
 *
 *  It seems this does not do any harm to others where this is not needed,
 *  so it's added as standard fix for all.
 *
 *  Starting with APTIO V for nvram to work not only data but could too can no longer be moved
 *  due to the use of commbuffers. This, however, creates a memory protection issue, because
 *  XNU maps RT data as RW and code as RX, and AMI appears use global variables in some RT drivers.
 *  For this reason we shim (most?) affected RT services via wrapers that unset the WP bit during
 *  the UEFI call and set it back on return.
 *  Explained in detail by Download-Fritz and vit9696:
 *  http://www.insanelymac.com/forum/topic/331381-aptiomemoryfix (first 2 links in particular).
 */
VOID
ProtectRtMemoryFromRelocation (
  IN UINTN                  MemoryMapSize,
  IN UINTN                  DescriptorSize,
  IN UINT32                 DescriptorVersion,
  IN EFI_MEMORY_DESCRIPTOR  *MemoryMap
  )
{
  UINTN                   NumEntries;
  UINTN                   Index;
  EFI_MEMORY_DESCRIPTOR   *Desc;

  RT_RELOC_PROTECT_INFO *RelocInfo;

  Desc = MemoryMap;
  NumEntries = MemoryMapSize / DescriptorSize;
  DEBUG ((DEBUG_VERBOSE, "FixNvramRelocation\n"));

  gRelocInfoData.NumEntries = 0;

  RelocInfo = &gRelocInfoData.RelocInfo[0];

  for (Index = 0; Index < NumEntries; Index++) {
    if ((Desc->Attribute & EFI_MEMORY_RUNTIME) != 0 &&
        (Desc->Type == EfiRuntimeServicesCode ||
        (Desc->Type == EfiRuntimeServicesData && Desc->PhysicalStart != gSysTableRtArea))) {

      if (gRelocInfoData.NumEntries < ARRAY_SIZE (gRelocInfoData.RelocInfo)) {
        RelocInfo->PhysicalStart = Desc->PhysicalStart;
        RelocInfo->Type          = Desc->Type;
        ++RelocInfo;
        ++gRelocInfoData.NumEntries;
      } else {
        DEBUG ((DEBUG_WARN, " WARNING: Cannot save mem type for entry: %lx (type 0x%x)\n", Desc->PhysicalStart, (UINTN)Desc->Type));
      }

      DEBUG ((DEBUG_VERBOSE, " RT mem %lx (0x%x) -> MemMapIO\n", Desc->PhysicalStart, Desc->NumberOfPages));
      Desc->Type = EfiMemoryMappedIO;
    }

    Desc = NEXT_MEMORY_DESCRIPTOR(Desc, DescriptorSize);
  }
}

/** AMI CSM module allocates up to two regions for legacy video output.
 *  1. For PMM and EBDA areas.
 *     On Ivy Bridge and below it ends at 0xA0000-0x1000-0x1 and has EfiBootServicesCode type.
 *     On Haswell and above it is allocated below 0xA0000 address with the same type.
 *  2. For Intel RC S3 reserved area, fixed from 0x9F000 to 0x9FFFF.
 *     On Sandy Bridge and below it is not present in memory map.
 *     On Ivy Bridge and newer it is present as EfiRuntimeServicesData.
 *     Starting from at least SkyLake it is present as EfiReservedMemoryType.
 *
 *  Prior to AptioMemoryFix EfiRuntimeServicesData could have been relocated by boot.efi,
 *  and the 2nd region could have been overwritten by the kernel. Now it is no longer the
 *  case, and only the 1st region may need special handling.
 *
 *  For the 1st region there appear to be (unconfirmed) reports that it may still be accessed
 *  after waking from sleep. This does not seem to be valid according to AMI code, but we still
 *  protect it in case such systems really exist.
 *
 *  Researched and fixed on gigabyte boards by Slice
 */
VOID
ProtectCsmRegion (
  UINTN                   MemoryMapSize,
  EFI_MEMORY_DESCRIPTOR   *MemoryMap,
  UINTN                   DescriptorSize
  )
{
  UINTN                   NumEntries;
  UINTN                   Index;
  EFI_MEMORY_DESCRIPTOR   *Desc;
  UINTN                   BlockSize;
  UINTN                   PhysicalEnd;

  Desc = MemoryMap;
  NumEntries = MemoryMapSize / DescriptorSize;

  for (Index = 0; Index < NumEntries; Index++) {
    BlockSize = EFI_PAGES_TO_SIZE ((UINTN)Desc->NumberOfPages);
    PhysicalEnd = Desc->PhysicalStart + BlockSize;

    if (PhysicalEnd >= 0x9E000 && PhysicalEnd < 0xA0000 && Desc->Type == EfiBootServicesData) {
      Desc->Type = EfiACPIMemoryNVS;
      break;
    }

    Desc = NEXT_MEMORY_DESCRIPTOR(Desc, DescriptorSize);
  }
}

/** Fixes stuff when booting without relocation block. Called when boot.efi jumps to kernel. */
UINTN
FixBooting (
  UINTN    BootArgs
  )
{
  VOID                    *pBootArgs = (VOID*)BootArgs;
  struct BootArguments    *BA;
  UINTN                   MemoryMapSize;
  EFI_MEMORY_DESCRIPTOR   *MemoryMap;
  UINTN                   DescriptorSize;

  BA = GetBootArgs(pBootArgs);

  MemoryMapSize = *BA->MemoryMapSize;
  MemoryMap = (EFI_MEMORY_DESCRIPTOR*)(UINTN)(*BA->MemoryMap);
  DescriptorSize = *BA->MemoryMapDescriptorSize;

  FixBootingForCustomSlide(BA);

  // We must restore EfiRuntimeServicesCode memory areas, because otherwise
  // RuntimeServices won't be executable.
  RestoreRelocInfoProtectMemTypes(MemoryMapSize, DescriptorSize, MemoryMap);

  // Restore original kernel entry code.
  CopyMem((VOID *)(UINTN)AsmKernelEntry, (VOID *)gOrigKernelCode, gOrigKernelCodeSize);

  return BootArgs;
}

/** Fixes stuff when waking from hibernate without relocation block. Called when boot.efi jumps to kernel. */
UINTN
FixHibernateWake (
  UINTN    ImageHeaderPage
  )
{
  IOHibernateImageHeader  *ImageHeader;
  IOHibernateHandoff      *Handoff;

  ImageHeader = (IOHibernateImageHeader *)(UINTN)(ImageHeaderPage << EFI_PAGE_SHIFT);

  // Pass our relocated copy of system table
  ImageHeader->systemTableOffset = (UINT32)(UINTN)(gRelocatedSysTableRtArea - ImageHeader->runtimePages);

#if APTIOFIX_HIBERNATION_FORCE_OLD_MEMORYMAP == 1
  // XNU replaces the original restored UEFI mapping by a new one based on kIOHibernateHandoffTypeMemoryMap
  // passed values. This caused instant reboots after hibernation wake for dmazar during the development
  // of the original AptioFixV2 driver. The reasons mentioned were XNU attempts to map the rt pages which
  // overlap with the existing memory.
  //
  // To workaround this issue AptioFixV2 disables memory map handoff, and XNU reuses the original mapping.
  // Due to dynamic allocation memory mapping may sometimes change across the boots, and as a result
  // some of the wakes will fail or result in a memory corruption after some time.
  Handoff = (IOHibernateHandoff *)(UINTN)(ImageHeader->handoffPages << EFI_PAGE_SHIFT);
  while (Handoff->type != kIOHibernateHandoffTypeEnd) {
    if (Handoff->type == kIOHibernateHandoffTypeMemoryMap) {
      Handoff->type = kIOHibernateHandoffType;
      break;
    }
    Handoff = (IOHibernateHandoff *)(UINTN)((UINTN)Handoff + sizeof(Handoff) + Handoff->bytecount);
  }
#else
  // When reusing the original memory mapping we do not have to restore memory protection types & attributes,
  // since the new memory map is discarded anyway.
  // Otherwise we must restore memory map types just like at a normal boot, because MMIO regions are not
  // mapped as executable by XNU.
  //
  // However, there is an issue here. After hibernation restoration we may get corrupted memory, which
  // sometimes results in crashing apps and not working NVRAM. The exact cause is unknown, dumping
  // the memory shows that the handoff memory map is mostly similar, but partially differs.
  //
  // Due to a non-contiguous RT_Code/RT_Data areas (thanks to NVRAM hack) the original areas
  // will not be unmapped and this will result in a memory leak if some new runtime pages are added.
  // But even that should not cause crashes.
  //
  // From the top of my head I could imagine a new memory mapping
  // SystemTable gets a new address, and this address is marked as "Available".
  Handoff = (IOHibernateHandoff *)(UINTN)(ImageHeader->handoffPages << EFI_PAGE_SHIFT);
  while (Handoff->type != kIOHibernateHandoffTypeEnd) {
    if (Handoff->type == kIOHibernateHandoffTypeMemoryMap) {
      // boot.efi removes any memory from the memory map but the one with runtime attribute.
      RestoreRelocInfoProtectMemTypes(Handoff->bytecount, gMemoryMapDescriptorSize, (EFI_MEMORY_DESCRIPTOR *)Handoff->data);
      break;
    }
    Handoff = (IOHibernateHandoff *)(UINTN)((UINTN)Handoff + sizeof(Handoff) + Handoff->bytecount);
  }
#endif

  // Restore original kernel entry code
  CopyMem((VOID *)(UINTN)AsmKernelEntry, (VOID *)gOrigKernelCode, gOrigKernelCodeSize);

  return ImageHeaderPage;
}

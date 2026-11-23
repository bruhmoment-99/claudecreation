/** @file
  LabMmEntryStandalone.c - entry point for STANDALONE MM (MM_STANDALONE).

  Standalone MM: the MM core is loaded during PEI (StandaloneMmIplPei),
  before DXE exists at all. MM drivers get only the MM system table: no
  gBS, no UEFI system table, nothing outside MMRAM to call into. Everything
  MM needs to know about the platform (where MMRAM is, which normal memory
  it may touch) arrives as HOBs, Hand-Off Blocks, copied in at load time.

  Result: MM code can't accidentally depend on DXE code, and the set of
  non-MMRAM memory it will accept from ring 0 is a fixed list decided at
  boot, not "any memory the UEFI memory map called safe".
**/

#include <PiMm.h>
#include <Guid/SmramMemoryReserve.h>
#include <Library/HobLib.h>
#include <Library/StandaloneMmMemLib.h>
#include "LabMmCommon.h"

BOOLEAN
LabIsBufferOutsideMmValid (
  IN EFI_PHYSICAL_ADDRESS  Buffer,
  IN UINT64                Length
  )
{
  /* StandaloneMmMemLib: outside MMRAM AND inside one of the non-MMRAM
     ranges described to MM by resource-descriptor HOBs at boot. */
  return MmIsBufferOutsideMmValid (Buffer, Length);
}

/* Standalone MM does the copy for us: the MM core checks the shared
   communication buffer, copies the request into its own buffer INSIDE
   MMRAM (mInternalCommBufferCopy in StandaloneMmCore.c) and passes us that
   copy. So the pointer we get is MMRAM by design, and ring 0 can't change
   it while we read it. Rule 2 is enforced by the core for every handler. */
BOOLEAN
LabIsCommBufferValid (
  IN VOID   *CommBuffer,
  IN UINTN  Length
  )
{
  /* Sanity check: it must NOT be ring 0 memory, i.e. the core's MMRAM copy. */
  return !MmIsBufferOutsideMmValid ((EFI_PHYSICAL_ADDRESS)(UINTN)CommBuffer, Length);
}

EFI_STATUS
EFIAPI
LabMmHandlerStandaloneEntry (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_MM_SYSTEM_TABLE  *MmSystemTable
  )
{
  EFI_HOB_GUID_TYPE               *GuidHob;
  EFI_SMRAM_HOB_DESCRIPTOR_BLOCK  *Block;
  UINTN                           Index;
  UINT64                          Base = 0, Len = 0;

  /* No boot services: MMRAM ranges come from a HOB instead. */
  GuidHob = GetFirstGuidHob (&gEfiSmmSmramMemoryGuid);
  if (GuidHob != NULL) {
    Block = GET_GUID_HOB_DATA (GuidHob);
    for (Index = 0; Index < Block->NumberOfSmmReservedRegions; Index++) {
      if (Block->Descriptor[Index].PhysicalSize > Len) {
        Base = Block->Descriptor[Index].CpuStart;
        Len  = Block->Descriptor[Index].PhysicalSize;
      }
    }
  }

  return LabMmCommonInit (MmSystemTable, LAB_MM_MODEL_STANDALONE, Base, Len);
}

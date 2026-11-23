/** @file
  LabSmmEntryTraditional.c - entry point for TRADITIONAL MM (DXE_SMM_DRIVER).

  Traditional MM: the SMM core is loaded during DXE by PiSmmIpl, and SMM
  drivers start while UEFI boot services still exist. The entry point gets
  the full UEFI system table, so it can (and here does) call into DXE code
  through gBS. That convenience is also the weakness: SMM and DXE are
  tangled together, and an SMM driver that keeps calling DXE code after boot
  ("SMM callout") is a classic vulnerability.
**/

#include <PiSmm.h>
#include <Protocol/SmmAccess2.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SmmServicesTableLib.h>
#include <Library/SmmMemLib.h>
#include "LabMmCommon.h"

BOOLEAN
LabIsBufferOutsideMmValid (
  IN EFI_PHYSICAL_ADDRESS  Buffer,
  IN UINT64                Length
  )
{
  /* SmmMemLib: outside SMRAM, and (after SmmReadyToLock) inside memory the
     UEFI memory map marked as safe for communication. */
  return SmmIsBufferOutsideSmmValid (Buffer, Length);
}

/* Traditional MM passes handlers a pointer straight into ring 0's buffer,
   outside SMRAM. The handler must validate it itself (and copy it in). */
BOOLEAN
LabIsCommBufferValid (
  IN VOID   *CommBuffer,
  IN UINTN  Length
  )
{
  return SmmIsBufferOutsideSmmValid ((EFI_PHYSICAL_ADDRESS)(UINTN)CommBuffer, Length);
}

EFI_STATUS
EFIAPI
LabSmmHandlerEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_SMM_ACCESS2_PROTOCOL  *Access;
  EFI_SMRAM_DESCRIPTOR      Ranges[8];
  UINTN                     Size;
  UINTN                     Index;
  UINT64                    Base = 0, Len = 0;

  /* Ask a DXE protocol, through boot services, where SMRAM is.
     Standalone MM can't do this: it has no gBS at all. */
  Status = gBS->LocateProtocol (&gEfiSmmAccess2ProtocolGuid, NULL, (VOID **)&Access);
  if (!EFI_ERROR (Status)) {
    Size   = sizeof (Ranges);
    Status = Access->GetCapabilities (Access, &Size, Ranges);
    if (!EFI_ERROR (Status)) {
      for (Index = 0; Index < Size / sizeof (Ranges[0]); Index++) {
        if (Ranges[Index].PhysicalSize > Len) {
          Base = Ranges[Index].CpuStart;
          Len  = Ranges[Index].PhysicalSize;
        }
      }
    }
  }

  /* gSmst and the MM system table have the same layout (PiSmmCis.h). */
  return LabMmCommonInit ((EFI_MM_SYSTEM_TABLE *)gSmst, LAB_MM_MODEL_TRADITIONAL, Base, Len);
}

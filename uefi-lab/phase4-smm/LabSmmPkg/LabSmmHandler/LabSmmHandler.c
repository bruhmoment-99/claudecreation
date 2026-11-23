/** @file
  LabSmmHandler.c - Phase 4: our own code running in System Management Mode.

  How code gets to ring -2:
    - This driver is part of the FIRMWARE IMAGE (OVMF_CODE.fd), not a file on
      a disk. During boot the SMM core loads it into SMRAM, a region the
      chipset hides from everything that isn't in SMM, and calls our entry
      point. Then firmware LOCKS SMRAM (SmmReadyToLock). After that nothing,
      not even the firmware's own DXE code or the OS kernel, can read or
      change this code.
    - From then on our handlers run whenever an SMI (System Management
      Interrupt) fires. The CPU saves its full state, switches to SMM, runs
      the SMM core, which calls our handlers, then RSMs (resumes) back to
      whatever the OS was doing. The OS never sees it happen.

  Why SMM code is a classic source of security bugs:
    Everything a handler gets from outside is ATTACKER-CONTROLLED: the comm
    buffer, pointers inside it, the mailbox contents. SMM can read and write
    ALL memory, so a handler that follows an unchecked pointer can be tricked
    into overwriting SMRAM or the kernel for the attacker (a "confused
    deputy"). The rules applied below:
      1. Validate every external buffer with SmmIsBufferOutsideSmmValid().
      2. Copy the request into SMRAM before parsing it, so ring 0 (another
         CPU or a DMA device) can't change it between our check and our use
         (TOCTOU, time-of-check to time-of-use).
      3. Never return SMRAM contents to ring 0. READ_SMRAM below breaks this
         rule on purpose, but only for the first 32 bytes of our own handler
         code, to show that the bytes exist. A real handler must never do it.
**/

#include <PiSmm.h>
#include <Protocol/SmmAccess2.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SmmServicesTableLib.h>
#include <Library/SmmMemLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Guid/LabSmmComm.h>

/* ---- State. All of it lives in SMRAM, so ring 0 can't see or change it. ---- */
STATIC UINT64  mCallCount;
STATIC UINT64  mSmramBase;
STATIC UINT64  mSmramSize;
STATIC UINT64  mMailbox;        /* physical address registered via SET_MAILBOX, 0 = none */

EFI_STATUS EFIAPI LabGuidHandler (EFI_HANDLE, CONST VOID *, VOID *, UINTN *);

/* ---- Direct UART output (COM1). SMM has no console of its own. ---- */
STATIC VOID
SerialPutc (CHAR8 C)
{
  UINTN  Spin;

  for (Spin = 0; Spin < 100000 && (IoRead8 (0x3F8 + 5) & 0x20) == 0; Spin++) {
  }

  IoWrite8 (0x3F8, (UINT8)C);
}

STATIC VOID
SerialPuts (CONST CHAR8 *S)
{
  for ( ; *S != '\0'; S++) {
    if (*S == '\n') {
      SerialPutc ('\r');
    }

    SerialPutc (*S);
  }
}

STATIC VOID
SerialHex (UINT64 V)
{
  INTN  I;

  SerialPuts ("0x");
  for (I = 15; I >= 0; I--) {
    UINT8  N = (UINT8)((V >> (I * 4)) & 0xF);
    SerialPutc ((CHAR8)(N < 10 ? '0' + N : 'A' + N - 10));
  }
}

STATIC VOID
SerialDec (UINT64 V)
{
  CHAR8  Buf[21];
  UINTN  I = 20;

  Buf[I] = '\0';
  do {
    Buf[--I] = (CHAR8)('0' + (V % 10));
    V       /= 10;
  } while (V != 0);

  SerialPuts (&Buf[I]);
}

/* ---- The actual work, on a private SMRAM copy of the request. ---- */
STATIC VOID
ProcessMessage (
  IN OUT LAB_SMM_MSG  *Msg
  )
{
  mCallCount++;

  Msg->Magic          = LAB_SMM_MAGIC;
  Msg->CallCount      = mCallCount;
  Msg->CpuIndex       = gSmst->CurrentlyExecutingCpu;
  Msg->CpuCount       = gSmst->NumberOfCpus;
  Msg->SmramBase      = mSmramBase;
  Msg->SmramSize      = mSmramSize;
  Msg->HandlerAddress = (UINT64)(UINTN)LabGuidHandler;
  Msg->Status         = LAB_STATUS_OK;

  switch (Msg->Command) {
    case LAB_CMD_PING:
      break;

    case LAB_CMD_READ_SMRAM:
      /* Teaching only (see rule 3 above): 32 bytes of our own code. */
      CopyMem (Msg->Data, (VOID *)(UINTN)LabGuidHandler, sizeof (Msg->Data));
      break;

    case LAB_CMD_SET_MAILBOX:
      /* The pointer comes from ring 0. If we accepted an SMRAM address here,
         a later mailbox request would make SMM write its reply INTO SMRAM
         for the attacker. Validate the whole 4 KiB page. */
      if ((Msg->Arg == 0) || ((Msg->Arg & 0xFFF) != 0) ||
          !SmmIsBufferOutsideSmmValid ((EFI_PHYSICAL_ADDRESS)Msg->Arg, SIZE_4KB))
      {
        Msg->Status = LAB_STATUS_REJECTED;
      } else {
        mMailbox = Msg->Arg;
      }

      break;

    default:
      Msg->Status = LAB_STATUS_BAD_COMMAND;
      break;
  }
}

/**
  GUID handler: runs when ring 0 calls EFI_SMM_COMMUNICATION_PROTOCOL with our
  GUID. CommBuffer points at the payload (the LAB_SMM_MSG) in NORMAL memory.
**/
EFI_STATUS
EFIAPI
LabGuidHandler (
  IN     EFI_HANDLE  DispatchHandle,
  IN     CONST VOID  *Context         OPTIONAL,
  IN OUT VOID        *CommBuffer      OPTIONAL,
  IN OUT UINTN       *CommBufferSize  OPTIONAL
  )
{
  LAB_SMM_MSG  Local;

  if ((CommBuffer == NULL) || (CommBufferSize == NULL)) {
    return EFI_SUCCESS;
  }

  /* Rule 1: the buffer must be fully outside SMRAM and in memory that is
     allowed for communication. Rule 2: copy it in before reading it. */
  if ((*CommBufferSize != sizeof (LAB_SMM_MSG)) ||
      !SmmIsBufferOutsideSmmValid ((EFI_PHYSICAL_ADDRESS)(UINTN)CommBuffer, sizeof (LAB_SMM_MSG)))
  {
    DEBUG ((DEBUG_ERROR, "LabSmm: rejected comm buffer %p size %lu\n", CommBuffer, (UINT64)*CommBufferSize));
    return EFI_SUCCESS;
  }

  CopyMem (&Local, CommBuffer, sizeof (Local));
  ProcessMessage (&Local);
  CopyMem (CommBuffer, &Local, sizeof (Local));
  return EFI_SUCCESS;
}

/**
  Root handler: the SMM core calls this on EVERY SMI, whatever caused it.
  We only act when ring 0 wrote our value to APM_CNT (port 0xB2). This path
  needs no UEFI protocol at all, so it keeps working after ExitBootServices,
  the same way an OS-time SMI does on real hardware.
**/
EFI_STATUS
EFIAPI
LabRootHandler (
  IN     EFI_HANDLE  DispatchHandle,
  IN     CONST VOID  *Context         OPTIONAL,
  IN OUT VOID        *CommBuffer      OPTIONAL,
  IN OUT UINTN       *CommBufferSize  OPTIONAL
  )
{
  LAB_SMM_MSG  Local;

  if (IoRead8 (LAB_SMM_APM_CNT_PORT) != LAB_SMM_SW_SMI_VALUE) {
    return EFI_WARN_INTERRUPT_SOURCE_PENDING;     /* not ours */
  }

  /* Clear the command byte so a later, unrelated SMI isn't mistaken for ours. */
  IoWrite8 (LAB_SMM_APM_CNT_PORT, 0);

  if (mMailbox == 0) {
    SerialPuts ("[SMM] software SMI 0x5A, but no mailbox registered - ignoring\n");
    return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
  }

  /* The address was validated at registration. Still copy before use (rule 2). */
  CopyMem (&Local, (VOID *)(UINTN)mMailbox, sizeof (Local));
  ProcessMessage (&Local);
  CopyMem ((VOID *)(UINTN)mMailbox, &Local, sizeof (Local));

  SerialPuts ("[SMM] ring -2 here: software SMI 0x5A on CPU ");
  SerialDec (Local.CpuIndex);
  SerialPuts (", request #");
  SerialDec (Local.CallCount);
  SerialPuts (", mailbox ");
  SerialHex (mMailbox);
  SerialPuts (". Resuming the OS (RSM).\n");
  return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
}

/**
  Entry point. Runs ONCE, inside SMRAM, during boot (DXE phase), before
  SMRAM is locked. Boot services still exist at this point.
**/
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
  EFI_HANDLE                Handle;

  /* Learn the SMRAM ranges (for reporting). Pick the largest one: TSEG. */
  Status = gBS->LocateProtocol (&gEfiSmmAccess2ProtocolGuid, NULL, (VOID **)&Access);
  if (!EFI_ERROR (Status)) {
    Size   = sizeof (Ranges);
    Status = Access->GetCapabilities (Access, &Size, Ranges);
    if (!EFI_ERROR (Status)) {
      for (Index = 0; Index < Size / sizeof (Ranges[0]); Index++) {
        if (Ranges[Index].PhysicalSize > mSmramSize) {
          mSmramBase = Ranges[Index].CpuStart;
          mSmramSize = Ranges[Index].PhysicalSize;
        }
      }
    }
  }

  Handle = NULL;
  Status = gSmst->SmiHandlerRegister (LabGuidHandler, &gLabSmmCommGuid, &Handle);
  ASSERT_EFI_ERROR (Status);

  Handle = NULL;
  Status = gSmst->SmiHandlerRegister (LabRootHandler, NULL, &Handle);
  ASSERT_EFI_ERROR (Status);

  SerialPuts ("[SMM] LabSmmHandler loaded into SMRAM. Handler at ");
  SerialHex ((UINT64)(UINTN)LabGuidHandler);
  SerialPuts (", SMRAM ");
  SerialHex (mSmramBase);
  SerialPuts (" + ");
  SerialDec (mSmramSize / 1024);
  SerialPuts (" KiB\n");
  return EFI_SUCCESS;
}

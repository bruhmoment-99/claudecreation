/** @file
  LabSmmHandler.c - our code running in Management Mode (ring -2).
  Shared by both MM architectures; the entry points live in
  LabSmmEntryTraditional.c and LabMmEntryStandalone.c.

  How code gets to ring -2:
    - This driver is part of the FIRMWARE IMAGE (OVMF_CODE.fd), not a file on
      a disk. During boot the MM core loads it into SMRAM (also called MMRAM),
      a region the chipset hides from everything that isn't in SMM, and calls
      our entry point. Then firmware LOCKS SMRAM. After that nothing, not even
      the firmware's own DXE code or the OS kernel, can read or change it.
    - From then on our handlers run whenever an SMI (System Management
      Interrupt) fires. The CPU saves its full state, switches to SMM, runs
      the MM core, which calls our handlers, then RSMs (resumes) back to
      whatever the OS was doing. The OS never sees it happen.

  Why SMM code is a classic source of security bugs:
    Everything a handler gets from outside is ATTACKER-CONTROLLED: the comm
    buffer, pointers inside it, the mailbox contents. SMM can read and write
    ALL memory, so a handler that follows an unchecked pointer can be tricked
    into overwriting SMRAM or the kernel for the attacker (a "confused
    deputy"). The rules applied below:
      1. Validate every external buffer (LabIsBufferOutsideMmValid).
      2. Copy the request into SMRAM before parsing it, so ring 0 (another
         CPU or a DMA device) can't change it between our check and our use
         (TOCTOU, time-of-check to time-of-use).
      3. Never return SMRAM contents to ring 0. READ_SMRAM below breaks this
         rule on purpose, but only for the first 32 bytes of our own handler
         code, to show that the bytes exist. A real handler must never do it.
**/

#include <Protocol/SmmVariable.h>
#include <Protocol/SmmVarCheck.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include "LabMmCommon.h"

extern EFI_GUID  gLabSmmCommGuid;
extern EFI_GUID  gEfiSmmVariableProtocolGuid;
extern EFI_GUID  gEdkiiSmmVarCheckProtocolGuid;

/* ---- State. All of it lives in SMRAM, so ring 0 can't see or change it. ---- */
STATIC EFI_MM_SYSTEM_TABLE  *mMmst;
STATIC UINT64               mMmModel;
STATIC UINT64               mCallCount;
STATIC UINT64               mSmramBase;
STATIC UINT64               mSmramSize;
STATIC UINT64               mMailbox;        /* physical address registered via SET_MAILBOX, 0 = none */
STATIC UINT64               mTotalSmis;      /* every SMI, counted by the root handler */
STATIC UINT64               mMailboxSmis;    /* software SMIs 0x5A served through the mailbox */
STATIC EFI_SMM_VARIABLE_PROTOCOL  *mSmmVariable;

#define COUNTER_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

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

STATIC CONST CHAR8 *
ModelTag (VOID)
{
  return mMmModel == LAB_MM_MODEL_STANDALONE ? "[MM standalone]" : "[SMM]";
}

/* ---- Phase 6: the SMM-only counter in flash ----
   Ring 0's SetVariable() also ends up in SMM (the variable driver lives
   there), but it arrives as an UNTRUSTED request, and LabBootCounter is
   marked read-only for those. Calls made through EFI_SMM_VARIABLE_PROTOCOL
   come from inside SMM and are TRUSTED, so only this code can change it.
   The bytes land in the flash chip, which the chipset lets only SMM write. */
STATIC EFI_STATUS
IncrementCounter (
  OUT UINT64  *NewValue
  )
{
  EFI_GUID    Guid = LAB_SMM_COMM_GUID;
  UINT64      Value = 0;
  UINTN       Size  = sizeof (Value);
  UINT32      Attributes;
  EFI_STATUS  Status;

  if (mSmmVariable == NULL) {
    return EFI_NOT_READY;
  }

  Status = mSmmVariable->SmmGetVariable (LAB_SMM_COUNTER_NAME, &Guid, &Attributes, &Size, &Value);
  if (Status == EFI_NOT_FOUND) {
    Value = 0;
  } else if (EFI_ERROR (Status) || (Size != sizeof (Value))) {
    return EFI_ERROR (Status) ? Status : EFI_BAD_BUFFER_SIZE;
  }

  Value++;
  Status = mSmmVariable->SmmSetVariable (LAB_SMM_COUNTER_NAME, &Guid, COUNTER_ATTRIBUTES, sizeof (Value), &Value);
  if (!EFI_ERROR (Status)) {
    *NewValue = Value;
  }

  return Status;
}

/* ---- Phase 9: how is SMM protecting ITSELF? ----
   Walk the page tables the CPU is using RIGHT NOW (inside SMM, CR3 points at
   SMM's own tables, which live in SMRAM) and report the effective access
   rights for one address: present, writable, no-execute. A page is writable
   only if every level allows writes, and no-execute if any level says so. */
STATIC UINT8
PageFlags (
  IN UINT64  Address
  )
{
  UINT64   *Table;
  UINT64   Entry;
  INTN     Level;
  INTN     Levels;
  BOOLEAN  Writable = TRUE;
  BOOLEAN  NoExec   = FALSE;

  Levels = ((AsmReadCr4 () & BIT12) != 0) ? 5 : 4;   /* CR4.LA57: 5-level paging */
  Table  = (UINT64 *)(UINTN)(AsmReadCr3 () & 0x000FFFFFFFFFF000ULL);

  for (Level = Levels; Level >= 1; Level--) {
    Entry = Table[(Address >> (12 + 9 * (Level - 1))) & 0x1FF];
    if ((Entry & BIT0) == 0) {
      return 0;                                         /* not mapped at all */
    }

    Writable = Writable && ((Entry & BIT1) != 0);
    NoExec   = NoExec || ((Entry & BIT63) != 0);
    if ((Level <= 3) && (Level >= 2) && ((Entry & BIT7) != 0)) {
      break;                                            /* 1 GiB or 2 MiB page */
    }

    Table = (UINT64 *)(UINTN)(Entry & 0x000FFFFFFFFFF000ULL);
  }

  return (UINT8)(LAB_PAGE_PRESENT | (Writable ? LAB_PAGE_WRITABLE : 0) | (NoExec ? LAB_PAGE_NX : 0));
}

STATIC VOID
ReportPageInfo (
  IN OUT LAB_SMM_MSG  *Msg
  )
{
  UINT64  Cr4 = AsmReadCr4 ();

  Msg->Data[LAB_PAGE_IDX_CODE]  = PageFlags ((UINT64)(UINTN)LabGuidHandler);
  Msg->Data[LAB_PAGE_IDX_DATA]  = PageFlags ((UINT64)(UINTN)&mCallCount);
  Msg->Data[LAB_PAGE_IDX_STACK] = PageFlags ((UINT64)(UINTN)Msg);   /* Msg is on our stack */
  Msg->Data[LAB_PAGE_IDX_ARG]   = PageFlags (Msg->Arg);
  Msg->Value                    =
    (((AsmReadCr0 () & BIT16) != 0) ? LAB_CPU_CR0_WP : 0) |
    (((AsmReadMsr64 (0xC0000080) & BIT11) != 0) ? LAB_CPU_EFER_NXE : 0) |
    (((Cr4 & BIT20) != 0) ? LAB_CPU_CR4_SMEP : 0) |
    (((Cr4 & BIT21) != 0) ? LAB_CPU_CR4_SMAP : 0);
}

/* The live test: write one byte of our own code (the same value it already
   has, so nothing would change even if it were allowed). If SMM's code pages
   are read-only, the CPU raises a page fault INSIDE SMM right here, and the
   firmware's SMM exception handler prints it and halts the machine. */
STATIC VOID
TryWriteOwnCode (
  VOID
  )
{
  volatile UINT8  *Code = (volatile UINT8 *)(UINTN)LabGuidHandler;

  SerialPuts (ModelTag ());
  SerialPuts (" about to write to my own code page at ");
  SerialHex ((UINT64)(UINTN)Code);
  SerialPuts ("...\n");
  *Code = *Code;
  SerialPuts (ModelTag ());
  SerialPuts (" the write SUCCEEDED: SMM code is writable (no protection)\n");
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
  Msg->CpuIndex       = mMmst->CurrentlyExecutingCpu;
  Msg->CpuCount       = mMmst->NumberOfCpus;
  Msg->SmramBase      = mSmramBase;
  Msg->SmramSize      = mSmramSize;
  Msg->HandlerAddress = (UINT64)(UINTN)LabGuidHandler;
  Msg->TotalSmis      = mTotalSmis;
  Msg->MmModel        = mMmModel;
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
         for the attacker. Validate the whole 4 KiB page. In Standalone MM
         the check is stricter still: only memory described to MM at boot
         is accepted (see LabMmEntryStandalone.c). */
      if ((Msg->Arg == 0) || ((Msg->Arg & 0xFFF) != 0) ||
          !LabIsBufferOutsideMmValid ((EFI_PHYSICAL_ADDRESS)Msg->Arg, SIZE_4KB))
      {
        Msg->Status = LAB_STATUS_REJECTED;
      } else {
        mMailbox = Msg->Arg;
      }

      break;

    case LAB_CMD_INCREMENT_COUNTER:
    {
      EFI_STATUS  Status = IncrementCounter (&Msg->Value);
      if (EFI_ERROR (Status)) {
        Msg->Status = LAB_STATUS_FAILED;
        Msg->Arg    = Status;
      }

      break;
    }

    case LAB_CMD_PAGE_INFO:
      ReportPageInfo (Msg);
      break;

    case LAB_CMD_TRY_WRITE_CODE:
      TryWriteOwnCode ();
      break;

    default:
      Msg->Status = LAB_STATUS_BAD_COMMAND;
      break;
  }
}

/**
  GUID handler: runs when ring 0 calls the MM Communication protocol with
  our GUID. CommBuffer points at the payload (the LAB_SMM_MSG) outside SMRAM.
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

  /* Rule 1: validate the buffer (how depends on the MM model: see
     LabIsCommBufferValid in the two entry files). Rule 2: copy it in
     before reading it. */
  if ((*CommBufferSize != sizeof (LAB_SMM_MSG)) ||
      !LabIsCommBufferValid (CommBuffer, sizeof (LAB_SMM_MSG)))
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
  Root handler: the MM core calls this on EVERY SMI, whatever caused it.
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

  mTotalSmis++;   /* the MM core calls root handlers on every SMI */

  if (IoRead8 (LAB_SMM_APM_CNT_PORT) != LAB_SMM_SW_SMI_VALUE) {
    return EFI_WARN_INTERRUPT_SOURCE_PENDING;     /* not ours */
  }

  /* Clear the command byte so a later, unrelated SMI isn't mistaken for ours. */
  IoWrite8 (LAB_SMM_APM_CNT_PORT, 0);

  if (mMailbox == 0) {
    SerialPuts (ModelTag ());
    SerialPuts (" software SMI 0x5A, but no mailbox registered - ignoring\n");
    return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
  }

  /* The address was validated at registration. Still copy before use (rule 2). */
  CopyMem (&Local, (VOID *)(UINTN)mMailbox, sizeof (Local));
  ProcessMessage (&Local);
  CopyMem ((VOID *)(UINTN)mMailbox, &Local, sizeof (Local));

  /* Report the first few, then every 100th, so a stress test (Phase 11)
     doesn't flood the serial port. */
  mMailboxSmis++;
  if ((mMailboxSmis > 3) && ((mMailboxSmis % 100) != 0)) {
    if (mMailboxSmis == 4) {
      SerialPuts (ModelTag ());
      SerialPuts (" (more mailbox SMIs: reporting every 100th from now on)\n");
    }

    return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
  }

  SerialPuts (ModelTag ());
  SerialPuts (" ring -2 here: software SMI 0x5A on CPU ");
  SerialDec (Local.CpuIndex);
  SerialPuts (", request #");
  SerialDec (Local.CallCount);
  SerialPuts (", mailbox ");
  SerialHex (mMailbox);
  SerialPuts (". Resuming the OS (RSM).\n");
  return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
}

/**
  Shared initialisation, called once from either entry point while MM is
  still being set up (before SMRAM is locked).
**/
EFI_STATUS
LabMmCommonInit (
  IN EFI_MM_SYSTEM_TABLE  *Mmst,
  IN UINT64               MmModel,
  IN UINT64               MmramBase,
  IN UINT64               MmramSize
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    Handle;
  EDKII_SMM_VAR_CHECK_PROTOCOL  *VarCheck;
  VAR_CHECK_VARIABLE_PROPERTY   Property;

  mMmst      = Mmst;
  mMmModel   = MmModel;
  mSmramBase = MmramBase;
  mSmramSize = MmramSize;

  /* Phase 6: make LabBootCounter read-only for everyone outside SMM.
     Variable properties can only be set before EndOfDxe; after that the
     list is locked, so ring 0 can't remove this rule later. */
  Status = mMmst->MmLocateProtocol (&gEfiSmmVariableProtocolGuid, NULL, (VOID **)&mSmmVariable);
  if (EFI_ERROR (Status)) {
    mSmmVariable = NULL;
  }

  Status = mMmst->MmLocateProtocol (&gEdkiiSmmVarCheckProtocolGuid, NULL, (VOID **)&VarCheck);
  if (!EFI_ERROR (Status)) {
    Property.Revision   = VAR_CHECK_VARIABLE_PROPERTY_REVISION;
    Property.Property   = VAR_CHECK_VARIABLE_PROPERTY_READ_ONLY;
    Property.Attributes = COUNTER_ATTRIBUTES;
    Property.MinSize    = sizeof (UINT64);
    Property.MaxSize    = sizeof (UINT64);
    Status              = VarCheck->SmmVariablePropertySet (LAB_SMM_COUNTER_NAME, &gLabSmmCommGuid, &Property);
  }

  SerialPuts (ModelTag ());
  SerialPuts (EFI_ERROR (Status) ? " could not protect LabBootCounter\n"
                                 : " LabBootCounter is now read-only outside SMM\n");

  Handle = NULL;
  Status = mMmst->MmiHandlerRegister (LabGuidHandler, &gLabSmmCommGuid, &Handle);
  ASSERT_EFI_ERROR (Status);

  Handle = NULL;
  Status = mMmst->MmiHandlerRegister (LabRootHandler, NULL, &Handle);
  ASSERT_EFI_ERROR (Status);

  SerialPuts (ModelTag ());
  SerialPuts (MmModel == LAB_MM_MODEL_STANDALONE ? " LabMmHandler (Standalone MM) loaded into MMRAM. Handler at "
                                                 : " LabSmmHandler loaded into SMRAM. Handler at ");
  SerialHex ((UINT64)(UINTN)LabGuidHandler);
  SerialPuts (", SMRAM ");
  SerialHex (mSmramBase);
  SerialPuts (" + ");
  SerialDec (mSmramSize / 1024);
  SerialPuts (" KiB\n");
  return EFI_SUCCESS;
}

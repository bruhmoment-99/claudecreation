/** @file
  LabSmmComm.h - the contract between ring 0 and our ring -2 handler.

  Shared by the SMM driver (built with EDK II) and the test app (built
  freestanding with clang). Both define UINT8/UINT32/UINT64 before including.

  Two ways to reach the handler:
    1. EFI_SMM_COMMUNICATION_PROTOCOL.Communicate() with HeaderGuid =
       LAB_SMM_COMM_GUID and a LAB_SMM_MSG as the payload. Boot time only,
       because that protocol is a boot service.
    2. A raw software SMI: put a LAB_SMM_MSG in the registered mailbox page,
       then write LAB_SMM_SW_SMI_VALUE to I/O port 0xB2 (APM_CNT). This works
       from any ring-0 code, including an OS kernel after ExitBootServices().
**/
#ifndef LAB_SMM_COMM_H_
#define LAB_SMM_COMM_H_

#define LAB_SMM_COMM_GUID \
  { 0xee02351b, 0xdd25, 0x41d0, { 0x95, 0x6b, 0x51, 0x6c, 0x41, 0x4d, 0x36, 0x25 } }

#define LAB_SMM_APM_CNT_PORT  0xB2
#define LAB_SMM_SW_SMI_VALUE  0x5A
#define LAB_SMM_MAGIC         0x214D4D535F42414CULL  /* "LAB_SMM!" little-endian */

/* Commands */
#define LAB_CMD_PING         1  /* who are you? counters, CPU info, addresses   */
#define LAB_CMD_READ_SMRAM   2  /* return the first 32 bytes of our own handler  */
#define LAB_CMD_SET_MAILBOX  3  /* Arg = physical address of a 4 KiB mailbox     */
#define LAB_CMD_INCREMENT_COUNTER 4  /* SMM adds 1 to the LabBootCounter variable  */
#define LAB_CMD_PAGE_INFO         5  /* Phase 9: SMM reports its own page-table protections.
                                        Arg = a ring 0 address to check too. Results in Data[0..3]
                                        (LAB_PAGE_* flags) and Value (LAB_CPU_* flags). */
#define LAB_CMD_TRY_WRITE_CODE    6  /* Phase 9: SMM writes one byte of its OWN code. With SMM
                                        memory protection on, this page-faults inside SMM and the
                                        firmware halts the machine. Never returns in that case. */

/* LAB_CMD_PAGE_INFO: which page each Data[] byte describes */
#define LAB_PAGE_IDX_CODE    0   /* our handler's code             */
#define LAB_PAGE_IDX_DATA    1   /* our global variables           */
#define LAB_PAGE_IDX_STACK   2   /* the SMM stack we're running on */
#define LAB_PAGE_IDX_ARG     3   /* the ring 0 address in Arg      */
/* ...and the flags in each byte, as seen in the SMM page tables */
#define LAB_PAGE_PRESENT     0x01
#define LAB_PAGE_WRITABLE    0x02
#define LAB_PAGE_NX          0x04   /* no-execute */
/* LAB_CMD_PAGE_INFO: CPU protection switches while in SMM, in Value */
#define LAB_CPU_CR0_WP       0x01   /* read-only pages are enforced even for ring 0/SMM */
#define LAB_CPU_EFER_NXE     0x02   /* no-execute bits are honoured */
#define LAB_CPU_CR4_SMEP     0x04
#define LAB_CPU_CR4_SMAP     0x08

/* LabBootCounter: a UEFI variable stored in flash that ring 0 may READ but
   not write. Only our SMM handler can change it (Phase 6). */
#define LAB_SMM_COUNTER_NAME  L"LabBootCounter"   /* vendor GUID = LAB_SMM_COMM_GUID */

/* Which firmware architecture the handler was built for (Phase 7) */
#define LAB_MM_MODEL_TRADITIONAL  1   /* DXE_SMM_DRIVER, loaded during DXE  */
#define LAB_MM_MODEL_STANDALONE   2   /* MM_STANDALONE, loaded during PEI   */

/* Status codes written back by the handler */
#define LAB_STATUS_OK           0
#define LAB_STATUS_BAD_COMMAND  1
#define LAB_STATUS_REJECTED     2  /* pointer failed validation (e.g. points into SMRAM) */
#define LAB_STATUS_FAILED       3  /* the operation failed; Arg holds the EFI_STATUS */

#pragma pack(1)
typedef struct {
  UINT32  Command;        /* in  */
  UINT32  Status;         /* out */
  UINT64  Arg;            /* in: command argument; out: EFI_STATUS on LAB_STATUS_FAILED */
  UINT64  Magic;          /* out: LAB_SMM_MAGIC proves SMM code answered */
  UINT64  CallCount;      /* out: requests served; lives in SMRAM, survives between SMIs */
  UINT64  CpuIndex;       /* out: which CPU is running the SMI handler */
  UINT64  CpuCount;       /* out: CPUs that entered SMM */
  UINT64  SmramBase;      /* out: start of the SMRAM (TSEG) range */
  UINT64  SmramSize;      /* out */
  UINT64  HandlerAddress; /* out: where our handler code sits - inside SMRAM */
  UINT64  TotalSmis;      /* out: every SMI SMM has handled so far, ours or not */
  UINT64  Value;          /* out: INCREMENT_COUNTER result */
  UINT64  MmModel;        /* out: LAB_MM_MODEL_TRADITIONAL or LAB_MM_MODEL_STANDALONE */
  UINT8   Data[32];       /* out: READ_SMRAM result */
} LAB_SMM_MSG;
#pragma pack()

#endif

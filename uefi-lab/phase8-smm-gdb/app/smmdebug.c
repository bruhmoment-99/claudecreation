/* smmdebug.c - Phase 8: a target for debugging ring -2 with GDB/LLDB.
 *
 * Prints where our MM handler sits, waits so a debugger can attach to
 * QEMU's GDB server and set a breakpoint on LabGuidHandler, then sends one
 * PING with a recognisable argument. The debugger stops INSIDE SMM.
 * Runs on either firmware build (Traditional or Standalone MM).
 */
#include "efi.h"
#include "lib.h"
#include "../../phase4-smm/LabSmmPkg/Include/Guid/LabSmmComm.h"

#define DEBUG_MARKER 0x1234ABCD   /* look for this in Msg.Arg inside the debugger */
#define WAIT_SECONDS 15

static EFI_SMM_COMMUNICATION_PROTOCOL *gComm;
typedef struct { EFI_SMM_COMMUNICATE_HEADER Hdr; LAB_SMM_MSG Msg; } __attribute__((packed)) COMM_BUF;
static COMM_BUF *gBuf;

static __attribute__((noreturn)) void halt(void) { for (;;) __asm__ volatile("hlt"); }

static int smm_call(uint32_t cmd, uint64_t arg) {
    EFI_GUID g = LAB_SMM_COMM_GUID;
    memset(gBuf, 0, sizeof *gBuf);
    gBuf->Hdr.HeaderGuid = g;
    gBuf->Hdr.MessageLength = sizeof(LAB_SMM_MSG);
    gBuf->Msg.Command = cmd;
    gBuf->Msg.Arg = arg;
    UINTN size = sizeof *gBuf;
    if (EFI_ERROR(gComm->Communicate(gComm, gBuf, &size))) return 0;
    return gBuf->Msg.Magic == LAB_SMM_MAGIC;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    (void)self;
    gST = st;
    EFI_BOOT_SERVICES *BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 8: debug ring -2 with GDB ===\r\n\r\n");

    EFI_GUID comm_guid = EFI_SMM_COMMUNICATION_PROTOCOL_GUID;
    uint64_t page;
    if (EFI_ERROR(BS->LocateProtocol(&comm_guid, NULL, (void **)&gComm)) ||
        EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &page))) {
        print(u"MM communication not available\r\n"); halt();
    }
    gBuf = (COMM_BUF *)(uintptr_t)page;
    if (!smm_call(LAB_CMD_PING, 0)) { print(u"handler did not answer\r\n"); halt(); }

    print(u"[app] MM model: ");
    print(gBuf->Msg.MmModel == LAB_MM_MODEL_STANDALONE ? u"Standalone\r\n" : u"Traditional\r\n");
    print(u"[app] MMRAM "); print_hex(gBuf->Msg.SmramBase); print(u" + ");
    print_dec(gBuf->Msg.SmramSize / 1024); print(u" KiB\r\n");
    /* The debug scripts read this line to work out where to load symbols. */
    print(u"[app] DEBUG-HANDLER-ADDRESS="); print_hex(gBuf->Msg.HandlerAddress); print(u"\r\n");

    print(u"[app] Triggering an SMI in "); print_dec(WAIT_SECONDS);
    print(u" s: attach the debugger now (or press a key to go now)\r\n");
    EFI_INPUT_KEY k;
    for (int i = 0; i < WAIT_SECONDS * 10 && st->ConIn->ReadKeyStroke(st->ConIn, &k) != EFI_SUCCESS; i++)
        BS->Stall(100000);

    print(u"[app] SMI now: PING with Arg = "); print_hex(DEBUG_MARKER); print(u"\r\n");
    if (smm_call(LAB_CMD_PING, DEBUG_MARKER)) {
        print(u"[app] back in ring 0: request #"); print_dec(gBuf->Msg.CallCount);
        print(u", SMIs so far "); print_dec(gBuf->Msg.TotalSmis); print(u"\r\n");
    } else {
        print(u"[app] no answer\r\n");
    }

    print(u"[app] done, shutting down\r\n");
    BS->Stall(500000);
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    halt();
}

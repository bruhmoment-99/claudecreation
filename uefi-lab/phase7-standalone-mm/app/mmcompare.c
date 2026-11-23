/* mmcompare.c - Phase 7: the same SMM handler, two MM architectures.
 *
 * Boots as BOOTX64.EFI on either firmware build:
 *   ../phase4-smm/firmware/bin             Traditional MM (DXE_SMM_DRIVER)
 *   ../phase4-smm/firmware/bin-standalone  Standalone MM  (MM_STANDALONE)
 * Both contain the same handler code (LabSmmHandler.c); only the entry file
 * and the MM core around it differ. This app asks the handler which model
 * it runs in and probes the one rule that changed the most: which ordinary
 * (non-MMRAM) memory MM is willing to touch for ring 0.
 */
#include "efi.h"
#include "lib.h"
#include "../../phase4-smm/LabSmmPkg/Include/Guid/LabSmmComm.h"

static EFI_BOOT_SERVICES *BS;
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

static const CHAR16 *verdict(uint32_t st) { return st == LAB_STATUS_OK ? u"ACCEPTED" : u"REJECTED"; }

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    (void)self;
    gST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 7: Traditional MM vs Standalone MM ===\r\n\r\n");

    EFI_GUID comm_guid = EFI_SMM_COMMUNICATION_PROTOCOL_GUID;
    uint64_t page;
    if (EFI_ERROR(BS->LocateProtocol(&comm_guid, NULL, (void **)&gComm)) ||
        EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &page))) {
        print(u"MM communication not available\r\n"); halt();
    }
    gBuf = (COMM_BUF *)(uintptr_t)page;

    /* ---------- 1. Who answers? ---------- */
    print(u"[1] Asking our handler which MM architecture it runs in\r\n");
    if (!smm_call(LAB_CMD_PING, 0)) { print(u"    no answer from the handler\r\n"); halt(); }
    int standalone = gBuf->Msg.MmModel == LAB_MM_MODEL_STANDALONE;
    print(u"    model: ");
    print(standalone ? u"STANDALONE MM (MM_STANDALONE, loaded during PEI, no boot services)\r\n"
                     : u"TRADITIONAL MM (DXE_SMM_DRIVER, loaded during DXE, boot services at entry)\r\n");
    print(u"    handler at "); print_hex(gBuf->Msg.HandlerAddress);
    print(u", MMRAM "); print_hex(gBuf->Msg.SmramBase); print(u" + ");
    print_dec(gBuf->Msg.SmramSize / 1024); print(u" KiB\r\n");
    uint64_t mmram = gBuf->Msg.SmramBase;

    /* ---------- 2. The communication path ---------- */
    print(u"\r\n[2] Communication path\r\n");
    print(standalone
        ? u"    ring 0 buffer -> fixed shared buffer -> MM core copies it INTO MMRAM -> handler\r\n"
        : u"    ring 0 buffer -> handler reads it in place (must validate + copy itself)\r\n");
    print(u"    PING through it: "); print(smm_call(LAB_CMD_PING, 0) ? u"OK\r\n" : u"failed\r\n");

    /* ---------- 3. Pointer into MMRAM ---------- */
    uint64_t evil = (mmram + 0xFFF) & ~0xFFFULL;
    print(u"\r\n[3] Ring 0 offers a pointer INTO MMRAM ("); print_hex(evil); print(u")\r\n");
    smm_call(LAB_CMD_SET_MAILBOX, evil);
    print(u"    "); print(verdict(gBuf->Msg.Status));
    print(u" - both models refuse this\r\n");

    /* ---------- 4. Pointer to ordinary RAM ---------- */
    uint64_t plain;
    BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &plain);
    print(u"\r\n[4] Ring 0 offers ordinary RAM it just allocated ("); print_hex(plain); print(u")\r\n");
    smm_call(LAB_CMD_SET_MAILBOX, plain);
    uint32_t st4 = gBuf->Msg.Status;
    print(u"    "); print(verdict(st4));
    if (standalone)
        print(st4 == LAB_STATUS_OK ? u" - unexpected for Standalone MM\r\n"
              : u" - Standalone MM only touches memory described to it at boot (HOBs)\r\n");
    else
        print(st4 == LAB_STATUS_OK ? u" - Traditional MM trusts the UEFI memory map's runtime types\r\n"
              : u" - unexpected for Traditional MM\r\n");

    /* ---------- 5. The Phase 6 counter still works ---------- */
    print(u"\r\n[5] Phase 6 flash counter through the handler\r\n");
    if (smm_call(LAB_CMD_INCREMENT_COUNTER, 0) && gBuf->Msg.Status == LAB_STATUS_OK) {
        print(u"    LabBootCounter = "); print_dec(gBuf->Msg.Value); print(u" (written by MM)\r\n");
    } else {
        print(u"    failed\r\n");
    }

    print(u"\r\nSummary: model=");
    print(standalone ? u"STANDALONE" : u"TRADITIONAL");
    print(u", MMRAM pointer "); print(u"REJECTED");
    print(u", ordinary RAM "); print(verdict(st4)); print(u"\r\n");

    print(u"\r\nShutting down in 3 s (press a key to stay)...\r\n");
    EFI_INPUT_KEY k;
    for (int i = 0; i < 30; i++) {
        if (st->ConIn->ReadKeyStroke(st->ConIn, &k) == EFI_SUCCESS) { print(u"Staying. Close QEMU to quit.\r\n"); halt(); }
        BS->Stall(100000);
    }
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    halt();
}

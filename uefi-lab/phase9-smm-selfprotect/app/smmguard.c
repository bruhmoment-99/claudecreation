/* smmguard.c - Phase 9: how SMM protects ITSELF (boots as BOOTX64.EFI).
 *
 * Even code in ring -2 runs under rules. After boot, the firmware's SMM CPU
 * driver (PiSmmCpuDxeSmm / PiSmmCpuStandaloneMm) sets up SMM's own page
 * tables so that:
 *   - SMM code pages are READ-ONLY  (a bug can't be turned into "rewrite SMM")
 *   - SMM data and stack are NO-EXECUTE (injected bytes can't be run)
 *   - ordinary RAM is NO-EXECUTE or not mapped at all in SMM (SMM can never
 *     jump into OS-controlled code: the classic "SMM callout" is blocked)
 * This app asks our handler to read those page-table entries from inside SMM
 * and print them. Then, optionally, it asks SMM to write to its own code, to
 * show the firmware catching it.
 *
 * The live test halts the machine, so it only runs if the file
 * \EFI\lab\write-code-test exists on the disk, or you press W.
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

static void show_page(const CHAR16 *what, uint8_t f, const CHAR16 *meaning) {
    print(u"    "); print(what);
    if (!(f & LAB_PAGE_PRESENT)) { print(u"NOT MAPPED            "); }
    else {
        print((f & LAB_PAGE_WRITABLE) ? u"writable  " : u"READ-ONLY ");
        print((f & LAB_PAGE_NX) ? u"NO-EXECUTE  " : u"executable  ");
    }
    print(meaning); print(u"\r\n");
}

static int marker_file_exists(EFI_HANDLE self) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs; EFI_FILE_PROTOCOL *root, *f;
    if (EFI_ERROR(BS->HandleProtocol(self, &li_guid, (void **)&li))) return 0;
    if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs))) return 0;
    if (EFI_ERROR(fs->OpenVolume(fs, &root))) return 0;
    int found = !EFI_ERROR(root->Open(root, &f, u"\\EFI\\lab\\write-code-test", EFI_FILE_MODE_READ, 0));
    if (found) f->Close(f);
    root->Close(root);
    return found;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    gST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 9: how SMM protects itself ===\r\n\r\n");

    EFI_GUID comm_guid = EFI_SMM_COMMUNICATION_PROTOCOL_GUID;
    uint64_t page;
    if (EFI_ERROR(BS->LocateProtocol(&comm_guid, NULL, (void **)&gComm)) ||
        EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &page))) {
        print(u"MM communication not available\r\n"); halt();
    }
    gBuf = (COMM_BUF *)(uintptr_t)page;

    /* 1. Page tables, as SMM itself sees them. Arg = our own comm buffer page,
     *    i.e. ordinary ring 0 memory. */
    if (!smm_call(LAB_CMD_PAGE_INFO, page)) { print(u"handler did not answer\r\n"); halt(); }
    int standalone = gBuf->Msg.MmModel == LAB_MM_MODEL_STANDALONE;
    print(u"[1] SMM's own page tables, read from inside SMM (");
    print(standalone ? u"Standalone MM)\r\n" : u"Traditional MM)\r\n");
    uint8_t code = gBuf->Msg.Data[LAB_PAGE_IDX_CODE], data = gBuf->Msg.Data[LAB_PAGE_IDX_DATA];
    uint8_t stack = gBuf->Msg.Data[LAB_PAGE_IDX_STACK], ring0 = gBuf->Msg.Data[LAB_PAGE_IDX_ARG];
    uint64_t cpu = gBuf->Msg.Value;
    show_page(u"handler code   : ", code,  u"-> SMM can't rewrite its own code");
    show_page(u"handler data   : ", data,  u"-> variables can't be run as code");
    show_page(u"SMM stack      : ", stack, u"-> no running code from the stack");
    show_page(u"ring 0 page    : ", ring0, (ring0 & LAB_PAGE_PRESENT)
        ? u"-> SMM can never jump into OS code (no 'callouts')"
        : u"-> SMM can't even see it unless it was shared at boot");

    /* 2. CPU switches inside SMM. */
    print(u"\r\n[2] CPU protection switches while in SMM\r\n");
    print(u"    CR0.WP   : "); print((cpu & LAB_CPU_CR0_WP)   ? u"on  (read-only pages are enforced, even in SMM)\r\n" : u"off\r\n");
    print(u"    EFER.NXE : "); print((cpu & LAB_CPU_EFER_NXE) ? u"on  (no-execute bits are honoured)\r\n" : u"off\r\n");
    print(u"    CR4.SMEP : "); print((cpu & LAB_CPU_CR4_SMEP) ? u"on\r\n" : u"off (not needed: SMM runs everything as supervisor)\r\n");

    int protected_ok = (code & LAB_PAGE_PRESENT) && !(code & LAB_PAGE_WRITABLE) && (data & LAB_PAGE_NX) &&
                       (stack & LAB_PAGE_NX) && (!(ring0 & LAB_PAGE_PRESENT) || (ring0 & LAB_PAGE_NX)) &&
                       (cpu & LAB_CPU_CR0_WP) && (cpu & LAB_CPU_EFER_NXE);
    print(u"\r\nSummary: SMM self-protection "); print(protected_ok ? u"ON\r\n" : u"INCOMPLETE\r\n");

    /* 3. The live test. */
    print(u"\r\n[3] Live test: ask SMM to write to its own code page\r\n");
    int go = marker_file_exists(self);
    if (!go) {
        print(u"    This halts the machine. Press W within 5 s to run it, any other key to skip.\r\n");
        EFI_INPUT_KEY k;
        for (int i = 0; i < 50; i++) {
            if (st->ConIn->ReadKeyStroke(st->ConIn, &k) == EFI_SUCCESS) { go = (k.UnicodeChar == u'w' || k.UnicodeChar == u'W'); break; }
            BS->Stall(100000);
        }
    }
    if (go) {
        print(u"    Sending TRY_WRITE_CODE. Expect a #PF page fault from the firmware's SMM\r\n");
        print(u"    exception handler, then a halt. Close QEMU afterwards.\r\n");
        smm_call(LAB_CMD_TRY_WRITE_CODE, 0);
        print(u"    ...SMM returned: the write was NOT blocked.\r\n");
    } else {
        print(u"    skipped\r\n");
    }

    print(u"\r\nShutting down.\r\n");
    BS->Stall(500000);
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    halt();
}

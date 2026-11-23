/* smmtest.c - Phase 4 ring-0 test app (boots as BOOTX64.EFI).
 *
 * It pokes at ring -2 from the outside:
 *   1. ask firmware where SMRAM is and whether it is locked
 *   2. try to read SMRAM directly from ring 0 (the chipset returns FFs)
 *   3. talk to OUR SMM handler through EFI_SMM_COMMUNICATION_PROTOCOL
 *   4. try to give the handler a pointer INTO SMRAM (it must refuse)
 *   5. ExitBootServices, then raise a raw software SMI (port 0xB2) the way
 *      an OS kernel would, and get an answer from ring -2.
 * The SMM side lives in ../LabSmmPkg and is baked into ../firmware/bin/OVMF_CODE.fd.
 */
#include "efi.h"
#include "lib.h"
#include "../LabSmmPkg/Include/Guid/LabSmmComm.h"

static EFI_BOOT_SERVICES *BS;
static EFI_SMM_COMMUNICATION_PROTOCOL *gComm;

/* Communication buffer: header followed by our message, in ONE allocation. */
typedef struct {
    EFI_SMM_COMMUNICATE_HEADER Hdr;
    LAB_SMM_MSG                Msg;
} __attribute__((packed)) COMM_BUF;
static COMM_BUF *gBuf;

static const CHAR16 *status_name(uint32_t s) {
    return s == LAB_STATUS_OK ? u"OK" : s == LAB_STATUS_REJECTED ? u"REJECTED" : u"BAD_COMMAND";
}

static void hexbytes(const volatile uint8_t *p, int n) {
    static const CHAR16 hx[] = u"0123456789ABCDEF";
    CHAR16 b[4] = {0, 0, u' ', 0};
    for (int i = 0; i < n; i++) { b[0] = hx[p[i] >> 4]; b[1] = hx[p[i] & 15]; print(b); }
}

/* Send one message to our SMI handler. Returns 1 if SMM code answered. */
static int smm_call(uint32_t cmd, uint64_t arg) {
    EFI_GUID g = LAB_SMM_COMM_GUID;
    memset(gBuf, 0, sizeof *gBuf);
    gBuf->Hdr.HeaderGuid = g;
    gBuf->Hdr.MessageLength = sizeof(LAB_SMM_MSG);
    gBuf->Msg.Command = cmd;
    gBuf->Msg.Arg = arg;
    UINTN size = sizeof *gBuf;
    EFI_STATUS s = gComm->Communicate(gComm, gBuf, &size);
    if (EFI_ERROR(s)) { print(u"    Communicate failed: "); print_hex(s); print(u"\r\n"); return 0; }
    return gBuf->Msg.Magic == LAB_SMM_MAGIC;
}

/* SMM only accepts buffers in memory types it captured as "communication
 * safe" when SMRAM was locked. Try the usual candidates in order. */
static uint64_t alloc_comm_page(const CHAR16 *what) {
    static const uint32_t types[] = { EfiRuntimeServicesData, EfiACPIMemoryNVS, EfiReservedMemoryType };
    static const CHAR16 *names[]  = { u"EfiRuntimeServicesData", u"EfiACPIMemoryNVS", u"EfiReservedMemoryType" };
    for (int i = 0; i < 3; i++) {
        uint64_t addr;
        if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, types[i], 1, &addr))) continue;
        gBuf = (COMM_BUF *)(uintptr_t)addr;
        if (smm_call(LAB_CMD_PING, 0)) {
            print(u"    "); print(what); print(u" at "); print_hex(addr); print(u" ("); print(names[i]);
            print(u") accepted by SMM\r\n");
            return addr;
        }
        print(u"    "); print(names[i]); print(u" page not accepted, trying next type\r\n");
        BS->FreePages(addr, 1);
    }
    return 0;
}

static void show_msg(void) {
    LAB_SMM_MSG *m = &gBuf->Msg;
    print(u"    status "); print(status_name(m->Status));
    print(u", request #"); print_dec(m->CallCount);
    print(u", CPU "); print_dec(m->CpuIndex); print(u"/"); print_dec(m->CpuCount);
    print(u"\r\n");
}

static __attribute__((noreturn)) void halt(void) { for (;;) __asm__ volatile("hlt"); }

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    gST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 4: ring -2 (SMM) ===\r\n\r\n");

    /* ---------- 1. Where is SMRAM? ---------- */
    EFI_GUID acc_guid = EFI_SMM_ACCESS2_PROTOCOL_GUID;
    EFI_SMM_ACCESS2_PROTOCOL *acc;
    print(u"[1] SMRAM, according to EFI_SMM_ACCESS2_PROTOCOL\r\n");
    if (EFI_ERROR(BS->LocateProtocol(&acc_guid, NULL, (void **)&acc))) {
        print(u"    not found: this firmware was built without SMM. Use ../firmware/bin/OVMF_CODE.fd\r\n");
        halt();
    }
    EFI_SMRAM_DESCRIPTOR map[8]; UINTN msz = sizeof map;
    if (!EFI_ERROR(acc->GetCapabilities(acc, &msz, map))) {
        for (UINTN i = 0; i < msz / sizeof map[0]; i++) {
            print(u"    range "); print_hex(map[i].CpuStart); print(u" size ");
            print_dec(map[i].PhysicalSize / 1024); print(u" KiB  state:");
            if (map[i].RegionState & EFI_SMRAM_OPEN)   print(u" open");
            if (map[i].RegionState & EFI_SMRAM_CLOSED) print(u" closed");
            if (map[i].RegionState & EFI_SMRAM_LOCKED) print(u" LOCKED");
            print(u"\r\n");
        }
    }
    print(u"    LockState = "); print(acc->LockState ? u"locked" : u"unlocked");
    print(u" (locked means even firmware DXE code can't open it again until reset)\r\n");

    /* ---------- 3 (first, to learn addresses). Talk to our handler ---------- */
    EFI_GUID comm_guid = EFI_SMM_COMMUNICATION_PROTOCOL_GUID;
    if (EFI_ERROR(BS->LocateProtocol(&comm_guid, NULL, (void **)&gComm))) {
        print(u"EFI_SMM_COMMUNICATION_PROTOCOL not found\r\n"); halt();
    }
    print(u"\r\n[2] Finding a communication buffer SMM will accept\r\n");
    if (!alloc_comm_page(u"comm buffer")) { print(u"    no buffer accepted - is LabSmmHandler in this firmware?\r\n"); halt(); }
    uint64_t handler = gBuf->Msg.HandlerAddress;
    uint64_t smram   = gBuf->Msg.SmramBase;
    print(u"    our SMM handler code is at "); print_hex(handler);
    print(u" (inside SMRAM "); print_hex(smram); print(u" + ");
    print_dec(gBuf->Msg.SmramSize / 1024); print(u" KiB)\r\n");

    /* ---------- Ring 0 tries to look ---------- */
    print(u"\r\n[3] Ring 0 reads the handler's address directly\r\n    ");
    hexbytes((volatile uint8_t *)(uintptr_t)handler, 16);
    print(u"\r\n    -> all FF: the chipset hides SMRAM from anything not in SMM\r\n");

    /* ---------- Ring -2 looks at the same address ---------- */
    print(u"\r\n[4] Asking SMM (ring -2) for the same bytes\r\n");
    smm_call(LAB_CMD_PING, 0);        show_msg();
    if (smm_call(LAB_CMD_READ_SMRAM, 0)) {
        show_msg();
        print(u"    "); hexbytes(gBuf->Msg.Data, 16);
        print(u"\r\n    -> real x86 machine code: SMM sees what ring 0 can't\r\n");
    }

    /* ---------- 5. The confused-deputy test ---------- */
    print(u"\r\n[5] Pointer validation (the classic SMM bug class)\r\n");
    uint64_t evil = (smram + 0xFFF) & ~0xFFFULL;
    print(u"    SET_MAILBOX("); print_hex(evil); print(u") - an address INSIDE SMRAM:\r\n");
    smm_call(LAB_CMD_SET_MAILBOX, evil); show_msg();

    uint64_t mailbox = 0;
    {   /* A good mailbox: one more page of an accepted type. */
        COMM_BUF *saved = gBuf;
        uint64_t page = alloc_comm_page(u"mailbox page");  /* allocates + pings via that page */
        gBuf = saved;
        if (page) {
            mailbox = page;
            print(u"    SET_MAILBOX("); print_hex(mailbox); print(u") - normal memory:\r\n");
            smm_call(LAB_CMD_SET_MAILBOX, mailbox); show_msg();
            if (gBuf->Msg.Status != LAB_STATUS_OK) mailbox = 0;
        }
    }

    print(u"\r\nPress any key to ExitBootServices (auto in 3 s)...\r\n");
    EFI_INPUT_KEY k;
    for (int i = 0; i < 30 && st->ConIn->ReadKeyStroke(st->ConIn, &k) != EFI_SUCCESS; i++) BS->Stall(100000);

    /* ---------- 6. ExitBootServices, then a raw SMI like an OS would ---------- */
    print(u"\r\n[6] ExitBootServices, then a raw software SMI (out 0xB2, 0x5A)\r\n");
    UINTN size = 0, key, dsz; uint32_t dver;
    EFI_MEMORY_DESCRIPTOR *mm = NULL;
    BS->GetMemoryMap(&size, NULL, &key, &dsz, &dver);
    size += 8 * dsz;
    UINTN cap = size;
    BS->AllocatePool(EfiLoaderData, cap, (void **)&mm);
    EFI_STATUS s; int tries = 0;
    do {
        size = cap;
        if (EFI_ERROR(s = BS->GetMemoryMap(&size, mm, &key, &dsz, &dver))) break;
        s = BS->ExitBootServices(self, key);
    } while (s == EFI_INVALID_PARAMETER && ++tries < 8);
    if (EFI_ERROR(s)) { print(u"ExitBootServices failed\r\n"); halt(); }
    st->BootServices = NULL; st->ConOut = NULL; st->ConIn = NULL;

    serial_puts("[app] ExitBootServices done. No UEFI protocols exist any more.\n");
    if (!mailbox) { serial_puts("[app] no mailbox was accepted, skipping\n"); }
    else {
        volatile LAB_SMM_MSG *m = (volatile LAB_SMM_MSG *)(uintptr_t)mailbox;
        m->Command = LAB_CMD_PING; m->Magic = 0; m->Status = 0xFFFFFFFF;
        serial_puts("[app] ring 0: writing request to mailbox and triggering SMI...\n");
        outb(LAB_SMM_APM_CNT_PORT, LAB_SMM_SW_SMI_VALUE);   /* CPU enters SMM right here */
        if (m->Magic == LAB_SMM_MAGIC) {
            serial_puts("[app] ring 0 again: SMM answered, request #");
            serial_dec(m->CallCount);
            serial_puts(", status OK. The OS saw nothing but one OUT instruction.\n");
        } else {
            serial_puts("[app] no answer from SMM\n");
        }
    }
    serial_puts("[app] shutting down via RuntimeServices->ResetSystem\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    halt();
}

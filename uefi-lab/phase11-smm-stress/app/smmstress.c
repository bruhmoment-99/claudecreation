/* smmstress.c - Phase 11: stress-test our SMM handler with hostile input.
 *
 * Every byte an SMI handler receives is controlled by ring 0, so firmware
 * vendors throw huge amounts of malformed input at their handlers. This app
 * does the same to LabSmmHandler, reproducibly (fixed random seed):
 *
 *   Part A, through the MM Communication protocol (boot time):
 *     random commands and arguments, hostile pointers (inside SMRAM,
 *     unaligned, zero, huge), wrong message sizes, a wrong GUID, garbage.
 *   Part B, after ExitBootServices, through raw software SMIs (port 0xB2):
 *     random SMI command bytes, and garbage in the mailbox.
 *
 * Pass criteria: the handler always answers or cleanly ignores a message,
 * never accepts a bad pointer, its code is byte-for-byte unchanged at the
 * end, and the machine is still alive.
 *
 * Deliberately NOT fuzzed: TRY_WRITE_CODE (halts the machine by design,
 * Phase 9) and INCREMENT_COUNTER (writes flash, Phase 6).
 */
#include "efi.h"
#include "lib.h"
#include "../../phase4-smm/LabSmmPkg/Include/Guid/LabSmmComm.h"

#define SEED        0x5EEDC0FFEE123457ULL
#define ITERATIONS  3000
#define RAW_SMIS    400

static EFI_BOOT_SERVICES *BS;
static EFI_SMM_COMMUNICATION_PROTOCOL *gComm;
static uint8_t *gPage;                 /* one 4 KiB page used as the comm buffer */
static uint64_t gGoodPage;             /* a page we own: a legitimate mailbox    */
static uint64_t gSmramBase, gSmramSize;

static __attribute__((noreturn)) void halt(void) { for (;;) __asm__ volatile("hlt"); }

/* xorshift64*: tiny, fast, reproducible */
static uint64_t rng = SEED;
static uint64_t rnd(void) { rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27; return rng * 0x2545F4914F6CDD1DULL; }

static EFI_SMM_COMMUNICATE_HEADER *hdr(void) { return (EFI_SMM_COMMUNICATE_HEADER *)gPage; }
static LAB_SMM_MSG *msg(void) { return (LAB_SMM_MSG *)(gPage + sizeof(EFI_SMM_COMMUNICATE_HEADER)); }

/* Send whatever is in gPage. Returns the Communicate status. */
static EFI_STATUS send(UINTN msg_len) {
    hdr()->MessageLength = msg_len;
    UINTN size = sizeof(EFI_SMM_COMMUNICATE_HEADER) + msg_len;
    return gComm->Communicate(gComm, gPage, &size);
}

static void fill(uint32_t cmd, uint64_t arg) {
    EFI_GUID g = LAB_SMM_COMM_GUID;
    memset(gPage, 0, 4096);
    hdr()->HeaderGuid = g;
    msg()->Command = cmd;
    msg()->Arg = arg;
}

static int ping(uint64_t *calls) {
    fill(LAB_CMD_PING, 0);
    if (EFI_ERROR(send(sizeof(LAB_SMM_MSG))) || msg()->Magic != LAB_SMM_MAGIC) return 0;
    if (calls) *calls = msg()->CallCount;
    return 1;
}

static int overlaps_smram(uint64_t a, uint64_t len) {
    return a < gSmramBase + gSmramSize && a + len > gSmramBase;
}

/* A random command, never one of the two excluded on purpose. */
static uint32_t random_cmd(void) {
    uint32_t c = (uint32_t)(rnd() % 16);
    if (c == LAB_CMD_INCREMENT_COUNTER || c == LAB_CMD_TRY_WRITE_CODE) c = 0;
    if (rnd() % 8 == 0) c = (uint32_t)rnd();             /* sometimes any 32-bit value */
    if (c == LAB_CMD_INCREMENT_COUNTER || c == LAB_CMD_TRY_WRITE_CODE) c = 0;
    return c;
}

/* Pointers chosen to be hostile. */
static uint64_t evil_pointer(void) {
    switch (rnd() % 7) {
        case 0:  return gSmramBase;                                        /* start of SMRAM */
        case 1:  return (gSmramBase + (rnd() % gSmramSize)) & ~0xFFFULL;   /* inside SMRAM   */
        case 2:  return (gSmramBase + gSmramSize - 0x1000);                /* last SMRAM page */
        case 3:  return gGoodPage + 1 + rnd() % 0xFFF;                     /* unaligned      */
        case 4:  return 0;                                                 /* NULL           */
        case 5:  return 0xFFFFFFFFFFFFF000ULL;                             /* top of space   */
        default: return gSmramBase - 0x1000 + (rnd() % 2) * 0x1000;        /* straddles start */
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    gST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 11: stress-testing the SMM handler ===\r\n\r\n");

    EFI_GUID comm_guid = EFI_SMM_COMMUNICATION_PROTOCOL_GUID;
    uint64_t p1, p2;
    if (EFI_ERROR(BS->LocateProtocol(&comm_guid, NULL, (void **)&gComm)) ||
        EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &p1)) ||
        EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &p2))) {
        print(u"MM communication not available\r\n"); halt();
    }
    gPage = (uint8_t *)(uintptr_t)p1; gGoodPage = p2;

    fill(LAB_CMD_PING, 0);
    if (EFI_ERROR(send(sizeof(LAB_SMM_MSG))) || msg()->Magic != LAB_SMM_MAGIC) { print(u"handler did not answer\r\n"); halt(); }
    gSmramBase = msg()->SmramBase; gSmramSize = msg()->SmramSize;
    int standalone = msg()->MmModel == LAB_MM_MODEL_STANDALONE;

    /* Snapshot the handler's first 32 code bytes, to prove nothing changed them. */
    uint8_t code_before[32];
    fill(LAB_CMD_READ_SMRAM, 0); send(sizeof(LAB_SMM_MSG)); memcpy(code_before, msg()->Data, 32);

    print(u"Model: "); print(standalone ? u"Standalone MM" : u"Traditional MM");
    print(u", seed "); print_hex(SEED); print(u", "); print_dec(ITERATIONS); print(u" messages\r\n\r\n");

    /* ================= Part A: the Communicate path ================= */
    uint64_t answered = 0, rejected = 0, bad_cmd = 0, ignored = 0, comm_err = 0, violations = 0, last_calls = 0;
    uint64_t kinds[6] = {0};
    for (int i = 0; i < ITERATIONS; i++) {
        int kind = (int)(rnd() % 6);
        kinds[kind]++;
        UINTN len = sizeof(LAB_SMM_MSG);
        uint32_t cmd = 0; uint64_t arg = 0;
        switch (kind) {
            case 0:  /* random command, random argument */
                cmd = random_cmd(); arg = rnd(); fill(cmd, arg); break;
            case 1:  /* SET_MAILBOX with a hostile pointer */
                cmd = LAB_CMD_SET_MAILBOX; arg = evil_pointer(); fill(cmd, arg); break;
            case 2:  /* PAGE_INFO on random addresses (walks page tables in SMM) */
                cmd = LAB_CMD_PAGE_INFO; arg = rnd(); fill(cmd, arg); break;
            case 3:  /* wrong message length */
                cmd = LAB_CMD_PING; fill(cmd, 0);
                { static const UINTN lens[] = {0, 1, 4, sizeof(LAB_SMM_MSG) - 1, sizeof(LAB_SMM_MSG) + 1, 2048};
                  len = lens[rnd() % 6]; }
                break;
            case 4:  /* the whole message is random garbage */
                cmd = random_cmd(); fill(cmd, 0);
                for (UINTN k = 0; k < sizeof(LAB_SMM_MSG); k++) ((uint8_t *)msg())[k] = (uint8_t)rnd();
                msg()->Command = cmd; arg = msg()->Arg;
                break;
            default: /* right message, wrong GUID: nobody should answer */
                fill(LAB_CMD_PING, 0); hdr()->HeaderGuid.Data1 ^= 0x5A5A5A5A; cmd = LAB_CMD_PING; break;
        }
        msg()->Magic = 0;
        EFI_STATUS s = send(len);
        if (EFI_ERROR(s)) { comm_err++; }
        else if (msg()->Magic != LAB_SMM_MAGIC) { ignored++; }
        else {
            answered++;
            if (msg()->Status == LAB_STATUS_REJECTED) rejected++;
            if (msg()->Status == LAB_STATUS_BAD_COMMAND) bad_cmd++;
            /* The one thing that must never happen: a bad pointer accepted. */
            if (cmd == LAB_CMD_SET_MAILBOX && msg()->Status == LAB_STATUS_OK &&
                (arg == 0 || (arg & 0xFFF) || overlaps_smram(arg, 0x1000))) {
                violations++;
                print(u"    VIOLATION: SET_MAILBOX accepted "); print_hex(arg); print(u"\r\n");
            }
            /* A message with the wrong size must never be processed. */
            if (len != sizeof(LAB_SMM_MSG)) {
                violations++;
                print(u"    VIOLATION: processed a message of size "); print_dec(len); print(u"\r\n");
            }
        }
        if (kind == 5 && msg()->Magic == LAB_SMM_MAGIC) {
            violations++; print(u"    VIOLATION: answered a message with the wrong GUID\r\n");
        }
        /* Every 250 messages: is the handler still alive, and still counting? */
        if ((i + 1) % 250 == 0) {
            uint64_t calls;
            if (!ping(&calls) || calls <= last_calls) {
                violations++; print(u"    VIOLATION: handler stopped answering correctly\r\n");
            }
            last_calls = calls;
            if ((i + 1) % 1000 == 0) { print(u"    ... "); print_dec(i + 1); print(u" messages, handler alive\r\n"); }
        }
    }

    print(u"\r\n[A] Communicate path: "); print_dec(ITERATIONS); print(u" messages\r\n");
    print(u"    answered          : "); print_dec(answered); print(u"\r\n");
    print(u"      of which REJECTED (bad pointer)   : "); print_dec(rejected); print(u"\r\n");
    print(u"      of which BAD_COMMAND (unknown cmd): "); print_dec(bad_cmd); print(u"\r\n");
    print(u"    ignored by the handler (wrong size) : "); print_dec(ignored); print(u"\r\n");
    print(u"    no handler for that GUID            : "); print_dec(comm_err); print(u"\r\n");
    print(u"    violations                          : "); print_dec(violations); print(u"\r\n");

    uint8_t code_after[32];
    fill(LAB_CMD_READ_SMRAM, 0); send(sizeof(LAB_SMM_MSG)); memcpy(code_after, msg()->Data, 32);
    int code_same = 1; for (int k = 0; k < 32; k++) if (code_before[k] != code_after[k]) code_same = 0;
    print(u"    handler code after the storm        : "); print(code_same ? u"unchanged\r\n" : u"CHANGED!\r\n");
    if (!code_same) violations++;

    /* Register our good page as the mailbox for Part B (Traditional accepts it,
     * Standalone refuses ordinary RAM - Phase 7). */
    fill(LAB_CMD_SET_MAILBOX, gGoodPage); send(sizeof(LAB_SMM_MSG));
    int have_mailbox = msg()->Magic == LAB_SMM_MAGIC && msg()->Status == LAB_STATUS_OK;

    /* ================= Part B: raw SMIs after ExitBootServices ================= */
    print(u"\r\n[B] After ExitBootServices: raw SMIs through port 0xB2 (results on serial)\r\n");
    UINTN msz = 0, key, dsz; uint32_t dver; EFI_MEMORY_DESCRIPTOR *mm = NULL;
    BS->GetMemoryMap(&msz, NULL, &key, &dsz, &dver);
    msz += 8 * dsz; UINTN cap = msz;
    BS->AllocatePool(EfiLoaderData, cap, (void **)&mm);
    EFI_STATUS s; int tries = 0;
    do { msz = cap; if (EFI_ERROR(s = BS->GetMemoryMap(&msz, mm, &key, &dsz, &dver))) break;
         s = BS->ExitBootServices(self, key); } while (s == EFI_INVALID_PARAMETER && ++tries < 8);
    if (EFI_ERROR(s)) { print(u"ExitBootServices failed\r\n"); halt(); }

    volatile LAB_SMM_MSG *mb = (volatile LAB_SMM_MSG *)(uintptr_t)gGoodPage;
    uint64_t raw_answered = 0;
    for (int i = 0; i < RAW_SMIS; i++) {
        if (have_mailbox && (i % 2 == 0)) {
            /* Garbage in the mailbox, then OUR SMI value. */
            for (UINTN k = 0; k < sizeof(LAB_SMM_MSG); k++) ((volatile uint8_t *)mb)[k] = (uint8_t)rnd();
            uint32_t c = random_cmd();
            if (c == LAB_CMD_SET_MAILBOX) mb->Arg = evil_pointer();
            mb->Command = c; mb->Magic = 0;
            outb(LAB_SMM_APM_CNT_PORT, LAB_SMM_SW_SMI_VALUE);
            if (mb->Magic == LAB_SMM_MAGIC) raw_answered++;
            if (c == LAB_CMD_SET_MAILBOX && mb->Status == LAB_STATUS_OK) {
                violations++; serial_puts("    VIOLATION: mailbox SET_MAILBOX accepted a hostile pointer\n");
            }
        } else {
            /* A random SMI command byte that isn't ours: our root handler must ignore it. */
            uint8_t v = (uint8_t)rnd(); if (v == LAB_SMM_SW_SMI_VALUE) v ^= 1;
            outb(LAB_SMM_APM_CNT_PORT, v);
        }
    }
    serial_puts("[B] raw SMIs sent: "); serial_dec(RAW_SMIS);
    if (have_mailbox) { serial_puts(", mailbox requests answered: "); serial_dec(raw_answered); }
    else serial_puts(" (no mailbox on Standalone MM: only random SMI values)");
    serial_puts("\n[B] machine still alive after the raw SMI storm\n");

    serial_puts("\nSummary: "); serial_dec(ITERATIONS + RAW_SMIS); serial_puts(" hostile inputs, ");
    serial_dec(violations); serial_puts(" violations, handler code ");
    serial_puts(code_same ? "unchanged" : "CHANGED");
    serial_puts(violations == 0 ? " -> PASS\n" : " -> FAIL\n");
    serial_puts("Shutting down.\n");
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    halt();
}

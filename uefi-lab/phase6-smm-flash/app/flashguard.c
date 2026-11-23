/* flashguard.c - Phase 6: SMM guards the flash (boots as BOOTX64.EFI).
 *
 * The UEFI variable store (boot order, Secure Boot keys, settings) lives in
 * the SPI flash chip. If ring 0 malware could write that chip, it could
 * rewrite the firmware or its settings and survive a reinstall. So on real
 * PCs the chipset lets ONLY SMM write flash, and every variable write goes
 * through code in SMM that decides whether to allow it.
 *
 * This app tries three ways to change LabBootCounter from ring 0:
 *   [2] write the flash chip directly          -> the chipset drops the write
 *   [3] SetVariable() via runtime services     -> SMM refuses (read-only for ring 0)
 *   [4] ask OUR SMM handler to increment it    -> SMM writes it, and it persists
 * The SMM side is LabSmmHandler.c in ../phase4-smm (built into the firmware).
 */
#include "efi.h"
#include "lib.h"
#include "../../phase4-smm/LabSmmPkg/Include/Guid/LabSmmComm.h"

/* QEMU's 4 MiB OVMF flash ends at 4 GiB: OVMF_VARS (528 KiB) sits at the
 * bottom, OVMF_CODE above it. Real PCs map their SPI flash the same way. */
#define FLASH_BASE 0xFFC00000ULL

static EFI_BOOT_SERVICES *BS;
static EFI_SMM_COMMUNICATION_PROTOCOL *gComm;
typedef struct { EFI_SMM_COMMUNICATE_HEADER Hdr; LAB_SMM_MSG Msg; } __attribute__((packed)) COMM_BUF;
static COMM_BUF *gBuf;

static __attribute__((noreturn)) void halt(void) { for (;;) __asm__ volatile("hlt"); }

static int smm_call(uint32_t cmd) {
    EFI_GUID g = LAB_SMM_COMM_GUID;
    memset(gBuf, 0, sizeof *gBuf);
    gBuf->Hdr.HeaderGuid = g;
    gBuf->Hdr.MessageLength = sizeof(LAB_SMM_MSG);
    gBuf->Msg.Command = cmd;
    UINTN size = sizeof *gBuf;
    if (EFI_ERROR(gComm->Communicate(gComm, gBuf, &size))) return 0;
    return gBuf->Msg.Magic == LAB_SMM_MAGIC;
}
static uint64_t smi_count(void) { return smm_call(LAB_CMD_PING) ? gBuf->Msg.TotalSmis : 0; }

static EFI_STATUS read_counter(uint64_t *v) {
    EFI_GUID g = LAB_SMM_COMM_GUID; uint32_t attr; UINTN size = sizeof *v;
    return gST->RuntimeServices->GetVariable((CHAR16 *)LAB_SMM_COUNTER_NAME, &g, &attr, &size, v);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    (void)self;
    gST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 6: SMM guards the flash ===\r\n\r\n");

    EFI_GUID comm_guid = EFI_SMM_COMMUNICATION_PROTOCOL_GUID;
    uint64_t page;
    if (EFI_ERROR(BS->LocateProtocol(&comm_guid, NULL, (void **)&gComm)) ||
        EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &page))) {
        print(u"SMM communication not available: boot this with ../phase4-smm/firmware/bin\r\n"); halt();
    }
    gBuf = (COMM_BUF *)(uintptr_t)page;
    if (!smm_call(LAB_CMD_PING)) { print(u"LabSmmHandler did not answer\r\n"); halt(); }

    /* ---------- 1. Reading flash is allowed ---------- */
    volatile uint8_t *flash = (volatile uint8_t *)(uintptr_t)FLASH_BASE;
    print(u"[1] Ring 0 READS the flash chip (allowed)\r\n");
    int fvh = flash[0x28] == '_' && flash[0x29] == 'F' && flash[0x2A] == 'V' && flash[0x2B] == 'H';
    print(u"    variable store at "); print_hex(FLASH_BASE);
    print(fvh ? u": firmware volume signature \"_FVH\" found\r\n" : u": no _FVH signature?\r\n");

    /* ---------- 2. Writing flash directly is not ---------- */
    /* The variable store's size is in the firmware volume header (offset 0x20).
     * Its last byte is unused space, erased to 0xFF: a harmless target. */
    uint64_t fv_len = *(volatile uint64_t *)(flash + 0x20);
    volatile uint8_t *target = flash + fv_len - 1;
    print(u"\r\n[2] Ring 0 WRITES the flash chip directly\r\n");
    print(u"    target "); print_hex((uint64_t)(uintptr_t)target);
    print(u" holds "); print_hex(*target);
    print(u"\r\n    sending CFI 'program byte' (0x40) then 0x00 to it...\r\n");
    *target = 0x40;      /* flash command: program one byte */
    *target = 0x00;      /* the byte to program */
    *target = 0xFF;      /* flash command: back to read mode */
    uint8_t after = *target;
    print(u"    read back: "); print_hex(after);
    print(after == 0xFF ? u"  -> DROPPED: outside SMM the chipset ignores flash writes\r\n"
                        : u"  -> the write went through?! flash is NOT protected\r\n");

    /* ---------- 3. SetVariable from ring 0 ---------- */
    print(u"\r\n[3] Ring 0 calls SetVariable(LabBootCounter = 999)\r\n");
    uint64_t before_smis = smi_count();
    EFI_GUID g = LAB_SMM_COMM_GUID; uint64_t fake = 999;
    EFI_STATUS s = st->RuntimeServices->SetVariable((CHAR16 *)LAB_SMM_COUNTER_NAME, &g,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS, sizeof fake, &fake);
    uint64_t after_smis = smi_count();
    /* smi_count() itself costs one SMI, so subtract it. */
    print(u"    SMIs while it ran: "); print_dec(after_smis - before_smis - 1);
    print(u" (the variable driver lives in SMM, so the request went there)\r\n");
    print(u"    status "); print_hex(s);
    print(s == EFI_WRITE_PROTECTED ? u" = EFI_WRITE_PROTECTED: SMM refused an untrusted caller\r\n"
                                   : u" (expected EFI_WRITE_PROTECTED)\r\n");

    /* ---------- 4. Ask our SMM handler ---------- */
    uint64_t v = 0;
    s = read_counter(&v);
    print(u"\r\n[4] Ring 0 reads LabBootCounter: ");
    if (s == EFI_NOT_FOUND) print(u"not set yet (first boot)\r\n");
    else { print_dec(v); print(u"\r\n"); }
    print(u"    asking OUR SMM handler to increment it...\r\n");
    if (smm_call(LAB_CMD_INCREMENT_COUNTER) && gBuf->Msg.Status == LAB_STATUS_OK) {
        print(u"    SMM wrote it to flash: LabBootCounter = "); print_dec(gBuf->Msg.Value); print(u"\r\n");
        uint64_t check = 0;
        read_counter(&check);
        print(u"    ring 0 GetVariable reads back "); print_dec(check);
        print(check == gBuf->Msg.Value ? u" - stored\r\n" : u" - mismatch!\r\n");
    } else {
        print(u"    SMM could not write it, status "); print_hex(gBuf->Msg.Arg); print(u"\r\n");
    }

    print(u"\r\n[5] It lives in flash (ovmf_vars.fd): boot again and the count goes up.\r\n");
    print(u"    Delete ovmf_vars.fd to reset it.\r\n");
    print(u"\r\nShutting down in 3 s (press a key to stay)...\r\n");
    EFI_INPUT_KEY k;
    for (int i = 0; i < 30; i++) {
        if (st->ConIn->ReadKeyStroke(st->ConIn, &k) == EFI_SUCCESS) {
            print(u"Staying. Close QEMU to quit.\r\n"); halt();
        }
        BS->Stall(100000);
    }
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    halt();
}

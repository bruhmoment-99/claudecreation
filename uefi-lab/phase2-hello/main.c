/* Phase 2 - first UEFI application.
 *
 * Firmware loads this PE32+ image, then calls efi_main(ImageHandle, SystemTable).
 * Everything you can do - print, read keys, allocate memory, reboot - is reached
 * through that SystemTable pointer. There is no libc and no OS underneath. */
#include "efi.h"

static EFI_SYSTEM_TABLE *ST;

static void print(const CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, (CHAR16 *)s); }

static void print_hex(uint64_t v) {
    CHAR16 buf[19] = u"0x";
    for (int i = 0; i < 16; i++) {
        unsigned nib = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = nib < 10 ? u'0' + nib : u'A' + nib - 10;
    }
    buf[18] = 0;
    print(buf);
}

static void print_dec(uint64_t v) {
    CHAR16 buf[21]; int i = 20; buf[i] = 0;
    do { buf[--i] = u'0' + v % 10; v /= 10; } while (v);
    print(&buf[i]);
}

/* Clang may emit calls to these even with -ffreestanding. */
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }

static const CHAR16 *mem_type_name(uint32_t t) {
    static const CHAR16 *names[] = {
        u"Reserved", u"LoaderCode", u"LoaderData", u"BootServicesCode", u"BootServicesData",
        u"RuntimeServicesCode", u"RuntimeServicesData", u"Conventional", u"Unusable",
        u"ACPIReclaim", u"ACPINVS", u"MMIO", u"MMIOPortSpace", u"PalCode", u"Persistent"};
    return t < sizeof names / sizeof *names ? names[t] : u"Other";
}

static void dump_memory_map(void) {
    EFI_BOOT_SERVICES *BS = ST->BootServices;
    UINTN size = 0, key, desc_size; uint32_t desc_ver;
    EFI_MEMORY_DESCRIPTOR *map = NULL;

    /* Classic UEFI dance: ask for size, allocate (which can grow the map), retry. */
    BS->GetMemoryMap(&size, NULL, &key, &desc_size, &desc_ver);
    size += 2 * desc_size;
    if (EFI_ERROR(BS->AllocatePool(2 /* EfiLoaderData */, size, (void **)&map))) return;
    if (EFI_ERROR(BS->GetMemoryMap(&size, map, &key, &desc_size, &desc_ver))) { BS->FreePool(map); return; }

    uint64_t pages[16] = {0};
    UINTN count = size / desc_size;
    for (UINTN i = 0; i < count; i++) {
        /* Step by desc_size, NOT sizeof(EFI_MEMORY_DESCRIPTOR): firmware may pad entries. */
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + i * desc_size);
        pages[d->Type < 15 ? d->Type : 15] += d->NumberOfPages;
    }
    print(u"\r\nMemory map: "); print_dec(count); print(u" descriptors\r\n");
    for (uint32_t t = 0; t < 16; t++) {
        if (!pages[t]) continue;
        print(u"  "); print(mem_type_name(t)); print(u": ");
        if (pages[t] >= 256) { print_dec(pages[t] / 256); print(u" MiB\r\n"); }
        else                 { print_dec(pages[t] * 4);   print(u" KiB\r\n"); }
    }
    BS->FreePool(map);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ST = st;
    st->BootServices->SetWatchdogTimer(0, 0, 0, NULL);  /* firmware reboots after 5 min otherwise */
    st->ConOut->ClearScreen(st->ConOut);

    print(u"Hello from bare metal (well, QEMU)!\r\n\r\n");
    print(u"Firmware vendor : "); print(st->FirmwareVendor); print(u"\r\n");
    print(u"Firmware rev    : "); print_hex(st->FirmwareRevision); print(u"\r\n");
    print(u"UEFI revision   : "); print_dec(st->Hdr.Revision >> 16); print(u".");
    print_dec((st->Hdr.Revision & 0xFFFF) / 10); print(u"\r\n");
    print(u"Image handle    : "); print_hex((uint64_t)image); print(u"\r\n");
    print(u"Config tables   : "); print_dec(st->NumberOfTableEntries); print(u"\r\n");

    dump_memory_map();

    print(u"\r\nPress any key to shut down (auto in 10 s)...\r\n");
    EFI_INPUT_KEY key;
    for (int i = 0; i < 100; i++) {
        if (st->ConIn->ReadKeyStroke(st->ConIn, &key) == EFI_SUCCESS) break;
        st->BootServices->Stall(100000);
    }
    st->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;  /* not reached */
}

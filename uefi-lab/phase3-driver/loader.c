/* loader.c - BOOTX64.EFI for Phase 3. It behaves like a tiny OS loader:
 *
 *   1. Protocol discovery: ask firmware which handles carry which protocols.
 *   2. Load our driver from the ESP (LoadedImage -> SimpleFileSystem -> File),
 *      start it, then find its protocol by GUID and call it.
 *   3. The handoff: GetMemoryMap + ExitBootServices. After that, Boot
 *      Services and ConOut are gone. Only Runtime Services and raw hardware
 *      remain - exactly the world a kernel wakes up in.
 */
#include "lab_protocol.h"
#include "lib.h"

#define DRIVER_PATH u"\\EFI\\lab\\greeter.efi"

static EFI_BOOT_SERVICES *BS;

static void fail(const CHAR16 *what, EFI_STATUS s) {
    print(u"ERROR: "); print(what); print(u" -> status "); print_hex(s); print(u"\r\n");
}

/* ---------- 1. Protocol discovery ---------- */
static UINTN count_handles(EFI_GUID *guid) {
    UINTN n = 0; EFI_HANDLE *buf = NULL;
    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, guid, NULL, &n, &buf))) return 0;
    BS->FreePool(buf);
    return n;
}

static void discover(void) {
    EFI_GUID sfs = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, blk = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;

    UINTN all = 0; EFI_HANDLE *handles = NULL;
    BS->LocateHandleBuffer(AllHandles, NULL, NULL, &all, &handles);
    UINTN protocols = 0;
    for (UINTN i = 0; i < all; i++) {
        EFI_GUID **list; UINTN c;
        if (!EFI_ERROR(BS->ProtocolsPerHandle(handles[i], &list, &c))) { protocols += c; BS->FreePool(list); }
    }
    if (handles) BS->FreePool(handles);

    print(u"[1] Protocol discovery: the firmware handle database\r\n");
    print(u"    handles in database     : "); print_dec(all);       print(u"\r\n");
    print(u"    protocol instances      : "); print_dec(protocols); print(u"\r\n");
    print(u"    with BlockIo (disks)    : "); print_dec(count_handles(&blk)); print(u"\r\n");
    print(u"    with SimpleFileSystem   : "); print_dec(count_handles(&sfs)); print(u"\r\n");

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    if (!EFI_ERROR(BS->LocateProtocol(&gop_guid, NULL, (void **)&gop))) {
        print(u"    GOP framebuffer         : ");
        print_dec(gop->Mode->Info->HorizontalResolution); print(u"x");
        print_dec(gop->Mode->Info->VerticalResolution);   print(u" @ ");
        print_hex(gop->Mode->FrameBufferBase); print(u"\r\n");
    } else {
        print(u"    GOP                     : none\r\n");
    }

    /* Before our driver runs, nobody provides our protocol. */
    EFI_GUID lab = LAB_GREETER_PROTOCOL_GUID; void *p;
    print(u"    LAB_GREETER before load : ");
    print(EFI_ERROR(BS->LocateProtocol(&lab, NULL, &p)) ? u"not found (expected)\r\n" : u"found?!\r\n");
}

/* ---------- 2. Load + start the driver ---------- */
static EFI_STATUS read_file_from_my_volume(EFI_HANDLE self, CHAR16 *path, void **out, UINTN *out_size) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file; EFI_STATUS s;

    /* Which device did firmware load *me* from? Open the same FAT volume. */
    if (EFI_ERROR(s = BS->HandleProtocol(self, &li_guid, (void **)&li))) return s;
    if (EFI_ERROR(s = BS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs))) return s;
    if (EFI_ERROR(s = fs->OpenVolume(fs, &root))) return s;
    s = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    if (EFI_ERROR(s)) return s;

    /* File size trick from the spec: seek to 0xFFFF...FF = end of file. */
    uint64_t size = 0;
    file->SetPosition(file, ~0ULL);
    file->GetPosition(file, &size);
    file->SetPosition(file, 0);

    void *buf;
    if (EFI_ERROR(s = BS->AllocatePool(EfiLoaderData, size, &buf))) { file->Close(file); return s; }
    UINTN n = size;
    s = file->Read(file, &n, buf);
    file->Close(file);
    if (EFI_ERROR(s)) { BS->FreePool(buf); return s; }
    *out = buf; *out_size = n;
    return EFI_SUCCESS;
}

static LAB_GREETER_PROTOCOL *load_driver(EFI_HANDLE self) {
    void *img; UINTN size; EFI_HANDLE drv; EFI_STATUS s;
    print(u"\r\n[2] Loading driver "); print(DRIVER_PATH); print(u"\r\n");

    if (EFI_ERROR(s = read_file_from_my_volume(self, DRIVER_PATH, &img, &size))) { fail(u"read driver", s); return NULL; }
    print(u"    read "); print_dec(size); print(u" bytes, calling LoadImage + StartImage\r\n");

    /* LoadImage parses the PE, relocates it, and checks Secure Boot policy. */
    s = BS->LoadImage(0, self, NULL, img, size, &drv);
    BS->FreePool(img);   /* firmware made its own copy */
    if (EFI_ERROR(s)) { fail(u"LoadImage", s); return NULL; }
    if (EFI_ERROR(s = BS->StartImage(drv, NULL, NULL))) { fail(u"StartImage", s); return NULL; }

    EFI_GUID lab = LAB_GREETER_PROTOCOL_GUID; LAB_GREETER_PROTOCOL *g;
    if (EFI_ERROR(s = BS->LocateProtocol(&lab, NULL, (void **)&g))) { fail(u"LocateProtocol(LAB_GREETER)", s); return NULL; }
    print(u"    LocateProtocol(LAB_GREETER) -> interface at "); print_hex((uint64_t)g);
    print(u", revision "); print_hex(g->Revision); print(u"\r\n");
    return g;
}

/* ---------- 3. The handoff ---------- */
static const char *type_name(uint32_t t) {
    static const char *n[] = {"Reserved","LoaderCode","LoaderData","BootServicesCode","BootServicesData",
        "RuntimeServicesCode","RuntimeServicesData","Conventional","Unusable","ACPIReclaim","ACPINVS",
        "MMIO","MMIOPortSpace","PalCode","Persistent"};
    return t < EfiMaxMemoryType ? n[t] : "Other";
}

static __attribute__((noreturn)) void handoff(EFI_HANDLE self) {
    UINTN map_size = 0, key, desc_size; uint32_t desc_ver;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    EFI_STATUS s;

    print(u"\r\n[3] Handoff: GetMemoryMap + ExitBootServices\r\n");
    print(u"    (from here on output goes straight to the COM1 UART)\r\n\r\n");

    /* Allocate with slack: AllocatePool itself can add descriptors to the map. */
    BS->GetMemoryMap(&map_size, NULL, &key, &desc_size, &desc_ver);
    map_size += 8 * desc_size;
    UINTN capacity = map_size;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, capacity, (void **)&map))) for (;;) __asm__("hlt");

    /* The MapKey must match the CURRENT map. If anything changed it (a timer
     * event allocating, for instance) ExitBootServices fails with
     * EFI_INVALID_PARAMETER and we must re-read the map and retry. Between
     * the calls we may not allocate or print. */
    int attempts = 0;
    do {
        map_size = capacity;
        s = BS->GetMemoryMap(&map_size, map, &key, &desc_size, &desc_ver);
        if (EFI_ERROR(s)) break;
        attempts++;
        s = BS->ExitBootServices(self, key);
    } while (s == EFI_INVALID_PARAMETER && attempts < 8);

    if (EFI_ERROR(s)) { print(u"ExitBootServices failed\r\n"); for (;;) __asm__("hlt"); }

    /* ======== Boot Services are GONE. gST->ConOut, BS->*: all invalid. ======== */
    /* Firmware also zeroes these fields; touching BS now would be a bug. */
    gST->BootServices = NULL; gST->ConIn = NULL; gST->ConOut = NULL;

    serial_puts("[loader] ExitBootServices succeeded (attempt ");
    serial_dec(attempts);
    serial_puts("). We now own the machine.\n\n");

    /* What a kernel does next: walk the map and decide what memory it may reuse. */
    uint64_t reclaim = 0, keep_rt = 0, rt_regions = 0;
    uint64_t pages[EfiMaxMemoryType + 1] = {0};
    UINTN count = map_size / desc_size;
    for (UINTN i = 0; i < count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + i * desc_size);
        uint32_t t = d->Type < EfiMaxMemoryType ? d->Type : EfiMaxMemoryType;
        pages[t] += d->NumberOfPages;
        if (t == EfiBootServicesCode || t == EfiBootServicesData || t == EfiConventionalMemory ||
            t == EfiLoaderCode || t == EfiLoaderData)
            reclaim += d->NumberOfPages;
        if (d->Attribute & EFI_MEMORY_RUNTIME) { keep_rt += d->NumberOfPages; rt_regions++; }
    }
    serial_puts("[loader] final memory map, as a kernel sees it (");
    serial_dec(count); serial_puts(" descriptors):\n");
    for (uint32_t t = 0; t < EfiMaxMemoryType; t++) {
        if (!pages[t] || t == EfiReservedMemoryType || t == EfiMemoryMappedIO) continue;
        serial_puts("    "); serial_puts(type_name(t)); serial_puts(": ");
        serial_dec(pages[t] * 4); serial_puts(" KiB\n");
    }
    serial_puts("    => usable by the OS (incl. old boot-services memory): ");
    serial_dec(reclaim / 256); serial_puts(" MiB\n");
    serial_puts("    => must stay mapped for Runtime Services: ");
    serial_dec(rt_regions); serial_puts(" regions, "); serial_dec(keep_rt * 4); serial_puts(" KiB\n\n");

    /* Runtime Services still work: this call jumps into firmware code that
     * lives in EfiRuntimeServicesCode memory. (A real OS would first call
     * SetVirtualAddressMap and use virtual addresses.) */
    EFI_TIME t;
    if (!EFI_ERROR(gST->RuntimeServices->GetTime(&t, NULL))) {
        serial_puts("[loader] RuntimeServices->GetTime(): ");
        serial_dec(t.Year); serial_putc('-'); serial_dec2(t.Month); serial_putc('-'); serial_dec2(t.Day);
        serial_putc(' '); serial_dec2(t.Hour); serial_putc(':'); serial_dec2(t.Minute);
        serial_putc(':'); serial_dec2(t.Second); serial_puts(" (CMOS RTC)\n");
    }
    serial_puts("[loader] RuntimeServices->ResetSystem(Shutdown). Bye.\n");
    gST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    for (;;) __asm__("hlt");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    gST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 3: driver + protocols + handoff ===\r\n\r\n");

    discover();

    LAB_GREETER_PROTOCOL *g = load_driver(self);
    if (g) {
        g->Greet(g, u"ring 3 world");
        g->Greet(g, u"future SMM handler");
        print(u"    driver reports GetCallCount() = "); print_dec(g->GetCallCount(g)); print(u"\r\n");
    }

    print(u"\r\nPress any key to ExitBootServices (auto in 5 s)...\r\n");
    EFI_INPUT_KEY k;
    for (int i = 0; i < 50 && st->ConIn->ReadKeyStroke(st->ConIn, &k) != EFI_SUCCESS; i++) BS->Stall(100000);

    handoff(self);
}

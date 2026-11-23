/* loader.c - our own bootloader (BOOTX64.EFI).
 *
 * What every OS loader does - GRUB, Windows Boot Manager, systemd-boot:
 *   1. read the kernel file from disk           (UEFI file protocol)
 *   2. put its segments where it was linked     (parse ELF, AllocatePages)
 *   3. collect what the kernel needs to know    (framebuffer, memory map, ACPI)
 *   4. ExitBootServices                         (firmware steps aside)
 *   5. jump to the kernel's entry point         (point of no return)
 */
#include "efi.h"
#include "lib.h"
#include "../include/boot.h"

#define KERNEL_PATH u"\\kernel.elf"

static EFI_BOOT_SERVICES *BS;

static __attribute__((noreturn)) void die(const CHAR16 *what, EFI_STATUS s) {
    print(u"\r\n[loader] FATAL: "); print(what);
    print(u" (status "); print_hex(s); print(u")\r\n");
    for (;;) BS->Stall(1000000);
}

/* ---------- ELF64 (only the parts a loader needs) ---------- */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf64_Ehdr;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Elf64_Phdr;
#define PT_LOAD 1
#define EM_X86_64 62
#define ET_EXEC 2

/* ---------- 1. read the kernel file (same steps as phase 3) ---------- */
static void read_kernel(EFI_HANDLE self, void **out, UINTN *out_size) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li; EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *file; EFI_STATUS s;

    if (EFI_ERROR(s = BS->HandleProtocol(self, &li_guid, (void **)&li))) die(u"LoadedImage", s);
    if (EFI_ERROR(s = BS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs))) die(u"SimpleFileSystem", s);
    if (EFI_ERROR(s = fs->OpenVolume(fs, &root))) die(u"OpenVolume", s);
    if (EFI_ERROR(s = root->Open(root, &file, KERNEL_PATH, EFI_FILE_MODE_READ, 0))) die(u"open \\kernel.elf", s);
    root->Close(root);

    uint64_t size = 0;
    file->SetPosition(file, ~0ULL); file->GetPosition(file, &size); file->SetPosition(file, 0);
    void *buf;
    if (EFI_ERROR(s = BS->AllocatePool(EfiLoaderData, size, &buf))) die(u"AllocatePool", s);
    UINTN n = size;
    if (EFI_ERROR(s = file->Read(file, &n, buf))) die(u"read kernel", s);
    file->Close(file);
    *out = buf; *out_size = n;
}

/* ---------- 2. load the ELF segments at their physical addresses ---------- */
static uint64_t load_elf(const uint8_t *img, UINTN size, uint64_t *lo, uint64_t *hi) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    if (size < sizeof *eh || eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 /* 64-bit */ || eh->machine != EM_X86_64 || eh->type != ET_EXEC)
        die(u"kernel.elf is not an x86-64 ELF executable", 0);

    *lo = ~0ULL; *hi = 0;
    for (uint16_t i = 0; i < eh->phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(img + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;

        /* Ask firmware for EXACTLY the pages the kernel was linked for. */
        uint64_t start = ph->paddr & ~0xFFFULL;
        uint64_t end   = (ph->paddr + ph->memsz + 0xFFF) & ~0xFFFULL;
        uint64_t addr  = start;
        EFI_STATUS s = BS->AllocatePages(2 /* AllocateAddress */, EfiLoaderData, (end - start) / 4096, &addr);
        if (EFI_ERROR(s)) die(u"the kernel's link address is not free RAM", s);

        memcpy((void *)(uintptr_t)ph->paddr, img + ph->offset, ph->filesz);
        memset((void *)(uintptr_t)(ph->paddr + ph->filesz), 0, ph->memsz - ph->filesz);  /* .bss */

        print(u"    segment "); print_hex(ph->paddr); print(u"  ");
        print_dec(ph->memsz); print(u" bytes  ");
        print((ph->flags & 1) ? u"code" : u"data"); print(u"\r\n");
        if (start < *lo) *lo = start;
        if (end > *hi) *hi = end;
    }
    return eh->entry;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE self, EFI_SYSTEM_TABLE *st) {
    gST = st; BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);
    st->ConOut->ClearScreen(st->ConOut);
    print(u"=== Phase 5: our bootloader ===\r\n\r\n");

    /* 1 + 2 */
    void *img; UINTN img_size;
    read_kernel(self, &img, &img_size);
    print(u"[loader] read \\kernel.elf: "); print_dec(img_size); print(u" bytes\r\n");
    uint64_t lo, hi;
    uint64_t entry = load_elf(img, img_size, &lo, &hi);
    BS->FreePool(img);
    print(u"[loader] kernel entry point: "); print_hex(entry); print(u"\r\n");

    /* 3. Boot info: framebuffer, ACPI, runtime services. */
    BOOT_INFO *bi;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, sizeof *bi, (void **)&bi))) die(u"AllocatePool", 0);
    memset(bi, 0, sizeof *bi);
    bi->magic = BOOT_INFO_MAGIC;
    bi->kernel_phys_start = lo;
    bi->kernel_phys_end = hi;
    bi->rt_reset_system = (uint64_t)(uintptr_t)st->RuntimeServices->ResetSystem;

    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    bi->fb_format = FB_FORMAT_NONE;
    if (!EFI_ERROR(BS->LocateProtocol(&gop_guid, NULL, (void **)&gop)) && gop->Mode->Info->PixelFormat <= 1) {
        bi->fb_base   = gop->Mode->FrameBufferBase;
        bi->fb_size   = gop->Mode->FrameBufferSize;
        bi->fb_width  = gop->Mode->Info->HorizontalResolution;
        bi->fb_height = gop->Mode->Info->VerticalResolution;
        bi->fb_pitch  = gop->Mode->Info->PixelsPerScanLine;
        bi->fb_format = gop->Mode->Info->PixelFormat == 0 ? FB_FORMAT_RGBX : FB_FORMAT_BGRX;
        print(u"[loader] framebuffer: "); print_dec(bi->fb_width); print(u"x"); print_dec(bi->fb_height);
        print(u" at "); print_hex(bi->fb_base); print(u"\r\n");
    } else {
        print(u"[loader] no linear framebuffer, kernel will use serial only\r\n");
    }

    EFI_GUID acpi20 = {0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        const uint8_t *a = (const uint8_t *)&st->ConfigurationTable[i].VendorGuid, *b = (const uint8_t *)&acpi20;
        int eq = 1; for (int k = 0; k < 16; k++) if (a[k] != b[k]) eq = 0;
        if (eq) bi->acpi_rsdp = (uint64_t)(uintptr_t)st->ConfigurationTable[i].VendorTable;
    }
    print(u"[loader] ACPI RSDP: "); print_hex(bi->acpi_rsdp); print(u"\r\n");

    /* 4. The final memory map, then ExitBootServices. */
    UINTN map_size = 0, key, desc_size; uint32_t desc_ver;
    BS->GetMemoryMap(&map_size, NULL, &key, &desc_size, &desc_ver);
    UINTN cap = map_size + 16 * desc_size;
    EFI_MEMORY_DESCRIPTOR *map;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, cap, (void **)&map))) die(u"AllocatePool", 0);

    print(u"[loader] ExitBootServices and jumping to the kernel. Bye, firmware!\r\n");
    EFI_STATUS s; int tries = 0;
    do {
        map_size = cap;
        if (EFI_ERROR(s = BS->GetMemoryMap(&map_size, map, &key, &desc_size, &desc_ver))) break;
        s = BS->ExitBootServices(self, key);
    } while (s == EFI_INVALID_PARAMETER && ++tries < 8);
    if (EFI_ERROR(s)) die(u"ExitBootServices", s);

    bi->mmap = (uint64_t)(uintptr_t)map;
    bi->mmap_size = map_size;
    bi->mmap_desc_size = desc_size;

    /* 5. Jump. The kernel is an ELF built for the System V ABI (argument in
     * RDI), while this loader uses the Microsoft ABI (argument in RCX). The
     * sysv_abi attribute makes clang use the kernel's convention for this call. */
    typedef void (__attribute__((sysv_abi, noreturn)) *kernel_entry_t)(BOOT_INFO *);
    ((kernel_entry_t)(uintptr_t)entry)(bi);
}

/* boot.h - the handoff contract between OUR bootloader and OUR kernel.
 *
 * Every OS has one of these: Linux has "struct boot_params", Windows has the
 * "loader parameter block", Multiboot has its info struct. It's everything the
 * kernel can't find out by itself once firmware is gone: where the screen is,
 * which RAM is free, where the ACPI tables are.
 *
 * Shared by loader/loader.c (UEFI, Windows ABI) and the kernel (ELF, SysV ABI),
 * so it only uses fixed-size types and no pointers-to-functions-with-ABI. */
#pragma once
#include <stdint.h>

#define BOOT_INFO_MAGIC 0x4F464E49544F4F42ULL   /* "BOOTINFO" little-endian */

#define FB_FORMAT_RGBX  0   /* byte order R,G,B,x */
#define FB_FORMAT_BGRX  1   /* byte order B,G,R,x (most common) */
#define FB_FORMAT_NONE  2   /* no linear framebuffer */

typedef struct {
    uint64_t magic;

    /* Linear framebuffer from the UEFI Graphics Output Protocol. It stays
     * valid after ExitBootServices: it's just memory-mapped video RAM. */
    uint64_t fb_base;
    uint64_t fb_size;
    uint32_t fb_width, fb_height;
    uint32_t fb_pitch;           /* pixels per scan line (can be > width) */
    uint32_t fb_format;

    /* The FINAL UEFI memory map (the one passed to ExitBootServices).
     * Entries are EFI_MEMORY_DESCRIPTORs, mmap_desc_size bytes apart. */
    uint64_t mmap;
    uint64_t mmap_size;
    uint64_t mmap_desc_size;

    uint64_t acpi_rsdp;          /* ACPI 2.0 RSDP, 0 if not found */
    uint64_t kernel_phys_start;  /* where the loader put the kernel image */
    uint64_t kernel_phys_end;

    /* UEFI RuntimeServices->ResetSystem. Runtime services survive
     * ExitBootServices, so the kernel can still use it to power off.
     * It's a Microsoft-ABI function pointer: see kernel.c for the call. */
    uint64_t rt_reset_system;
} BOOT_INFO;

/* UEFI memory descriptor layout, repeated here so the kernel doesn't need
 * the UEFI headers. Natural alignment puts PhysicalStart at offset 8. */
typedef struct {
    uint32_t type;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;
    uint64_t attribute;
} BOOT_MEMORY_DESCRIPTOR;

enum {
    MEM_RESERVED, MEM_LOADER_CODE, MEM_LOADER_DATA, MEM_BS_CODE, MEM_BS_DATA,
    MEM_RT_CODE, MEM_RT_DATA, MEM_CONVENTIONAL, MEM_UNUSABLE, MEM_ACPI_RECLAIM,
    MEM_ACPI_NVS, MEM_MMIO, MEM_MMIO_PORT, MEM_PAL, MEM_PERSISTENT
};

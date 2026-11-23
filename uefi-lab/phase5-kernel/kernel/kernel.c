/* kernel.c - our own kernel. No UEFI, no OS, no libraries.
 *
 * The bootloader jumps to _start with a BOOT_INFO pointer in RDI. From there:
 *   1. switch to our own stack           (the firmware's is in "free" memory)
 *   2. load our own GDT and page tables  (same reason)
 *   3. install exception handlers        (so bugs show a message, not a reboot)
 *   4. read the memory map, set up a page allocator
 *   5. start the timer and keyboard interrupts
 *   6. run a tiny shell on the framebuffer
 */
#include <stdint.h>
#include <stddef.h>
#include "../include/boot.h"
#include "io.h"
#include "cpu.h"
#include "screen.h"

void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }

/* ---------------- entry ---------------- */
__attribute__((used)) uint8_t kernel_stack[64 * 1024] __attribute__((aligned(16)));
void kmain(BOOT_INFO *bi);

__attribute__((naked, section(".text._start"))) void _start(void) {
    __asm__ volatile(
        "cli\n"
        "leaq kernel_stack+65536(%rip), %rsp\n"   /* our own stack */
        "xorl %ebp, %ebp\n"
        "call kmain\n"                            /* RDI still holds BOOT_INFO* */
        "1: hlt\n jmp 1b\n");
}

/* ---------------- logging: serial + screen ---------------- */
static int g_screen;
static void out(const char *s) { for (; *s; s++) { if (*s == '\n') serial_putc('\r'); serial_putc(*s); if (g_screen) con_putc(*s); } }
static void out_dec(uint64_t v) { char b[21]; int i = 20; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); out(&b[i]); }
static void out_hex(uint64_t v) {
    char b[19] = "0x"; for (int i = 0; i < 16; i++) { unsigned n = (v >> ((15 - i) * 4)) & 15; b[2 + i] = n < 10 ? '0' + n : 'A' + n - 10; }
    b[18] = 0; out(b);
}

/* ---------------- memory ---------------- */
static BOOT_INFO *g_bi;
static uint64_t g_free_bytes, g_alloc_next, g_alloc_end;

/* After ExitBootServices, everything the firmware used for boot services
 * is ours to reuse. Memory we must NOT touch: runtime services, ACPI NVS,
 * MMIO, and the loader-data pages holding our kernel, BOOT_INFO and map. */
static int is_free(uint32_t t) { return t == MEM_CONVENTIONAL || t == MEM_BS_CODE || t == MEM_BS_DATA; }

static void memory_init(void) {
    uint64_t best = 0, best_len = 0;
    for (uint64_t off = 0; off < g_bi->mmap_size; off += g_bi->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d = (BOOT_MEMORY_DESCRIPTOR *)(uintptr_t)(g_bi->mmap + off);
        if (!is_free(d->type)) continue;
        uint64_t len = d->num_pages * 4096;
        g_free_bytes += len;
        if (d->type == MEM_CONVENTIONAL && d->phys_start >= 0x100000 && len > best_len) { best = d->phys_start; best_len = len; }
    }
    g_alloc_next = best; g_alloc_end = best + best_len;
}

/* The simplest allocator there is: hand out pages from the biggest free
 * region, never give them back. Real kernels use a bitmap or free lists. */
static uint64_t alloc_page(void) {
    if (g_alloc_next + 4096 > g_alloc_end) return 0;
    uint64_t p = g_alloc_next; g_alloc_next += 4096;
    memset((void *)(uintptr_t)p, 0, 4096);
    return p;
}

static const char *mem_type_name(uint32_t t) {
    static const char *n[] = {"RESERVED", "LOADER CODE", "LOADER DATA", "BOOT SVC CODE", "BOOT SVC DATA",
        "RUNTIME CODE", "RUNTIME DATA", "FREE", "UNUSABLE", "ACPI RECLAIM", "ACPI NVS", "MMIO", "MMIO PORTS",
        "PAL CODE", "PERSISTENT"};
    return t < 15 ? n[t] : "OTHER";
}

static void cmd_mem(void) {
    uint64_t pages[16] = {0};
    for (uint64_t off = 0; off < g_bi->mmap_size; off += g_bi->mmap_desc_size) {
        BOOT_MEMORY_DESCRIPTOR *d = (BOOT_MEMORY_DESCRIPTOR *)(uintptr_t)(g_bi->mmap + off);
        pages[d->type < 15 ? d->type : 15] += d->num_pages;
    }
    for (int t = 1; t < 15; t++) {
        if (!pages[t] || t == MEM_MMIO) continue;
        out("  "); out(mem_type_name(t)); out(": "); out_dec(pages[t] * 4); out(" KIB\n");
    }
    out("  USABLE BY THIS KERNEL: "); out_dec(g_free_bytes >> 20); out(" MIB\n");
}

/* ---------------- exceptions ---------------- */
void kernel_exception(unsigned vector, const char *name, uint64_t rip, uint64_t error, int fatal) {
    if (fatal && g_screen) screen_panic("KERNEL PANIC - CPU EXCEPTION");
    out(fatal ? "\n[kernel] EXCEPTION " : "[kernel] exception ");
    out_dec(vector); out(" ("); out(name); out(") at RIP "); out_hex(rip);
    if (error) { out(", error code "); out_hex(error); }
    if (vector == 14) { uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2)); out(", address "); out_hex(cr2); }
    out(fatal ? "\n[kernel] system halted. Close QEMU to quit.\n" : " - handled, continuing\n");
    if (fatal) for (;;) { cli(); hlt(); }
}

/* ---------------- power off through UEFI runtime services ---------------- */
static void power_off(void) {
    out("[kernel] calling UEFI RuntimeServices->ResetSystem(Shutdown)\n");
    /* The firmware function uses the Microsoft x64 ABI; ms_abi tells clang. */
    typedef void (__attribute__((ms_abi)) *reset_fn)(int type, uint64_t status, uint64_t size, void *data);
    cli();
    ((reset_fn)(uintptr_t)g_bi->rt_reset_system)(2 /* EfiResetShutdown */, 0, 0, 0);
}

/* ---------------- title bar ---------------- */
static void draw_title(void) {
    if (!g_screen) return;
    screen_fill_rect(0, 0, g_bi->fb_width, 36, RGB(40, 90, 170));
    screen_text_at(1, 0, "MY OWN KERNEL - NO OS, NO FIRMWARE", RGB(255, 255, 255), RGB(40, 90, 170));
}
static void draw_uptime(void) {
    if (!g_screen) return;
    uint64_t s = g_ticks / g_timer_hz;
    char b[32] = "UPTIME "; int n = 7;
    char d[21]; int i = 20; d[i] = 0; do { d[--i] = '0' + s % 10; s /= 10; } while (s);
    while (d[i]) b[n++] = d[i++];
    b[n++] = ' '; b[n++] = 'S'; b[n++] = ' '; b[n] = 0;
    screen_text_at(con_cols() - 16, 0, b, RGB(255, 230, 120), RGB(40, 90, 170));
}

/* ---------------- shell ---------------- */
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void run_command(const char *cmd) {
    if (!cmd[0]) return;
    if (streq(cmd, "help")) {
        out("  HELP    THIS LIST\n  MEM     MEMORY MAP FROM THE BOOTLOADER\n  UPTIME  TIMER TICKS\n"
            "  ALLOC   GRAB A FREE PAGE\n  INT3    TRIGGER A BREAKPOINT EXCEPTION (SURVIVABLE)\n"
            "  CRASH   DIVIDE BY ZERO (KERNEL PANIC)\n  CLEAR   CLEAR THE SCREEN\n  OFF     POWER OFF VIA UEFI RUNTIME SERVICES\n");
    } else if (streq(cmd, "mem")) { cmd_mem(); }
    else if (streq(cmd, "uptime")) {
        out("  "); out_dec(g_ticks); out(" TICKS AT "); out_dec(g_timer_hz); out(" HZ = "); out_dec(g_ticks / g_timer_hz); out(" S\n");
    } else if (streq(cmd, "alloc")) {
        uint64_t p = alloc_page(); out("  GOT PAGE "); out_hex(p); out("\n");
    } else if (streq(cmd, "int3")) { __asm__ volatile("int3"); }
    else if (streq(cmd, "crash")) {
        /* A real DIV instruction by zero. (In C, clang would rewrite 1/x
         * as a comparison and never divide, so we write the instruction.) */
        __asm__ volatile("xorl %%edx, %%edx\n movl $1, %%eax\n xorl %%ecx, %%ecx\n divl %%ecx" ::: "eax", "ecx", "edx");
    } else if (streq(cmd, "clear")) { screen_clear_console(); }
    else if (streq(cmd, "off")) { power_off(); }
    else { out("  UNKNOWN COMMAND. TRY HELP\n"); }
}

/* ---------------- main ---------------- */
void kmain(BOOT_INFO *bi) {
    g_bi = bi;
    out("\n[kernel] hello from our own kernel! BOOT_INFO at "); out_hex((uint64_t)(uintptr_t)bi); out("\n");
    if (bi->magic != BOOT_INFO_MAGIC) { out("[kernel] bad BOOT_INFO magic, halting\n"); for (;;) hlt(); }

    gdt_init();
    out("[kernel] GDT loaded (our own code/data segments)\n");
    paging_init();
    out("[kernel] page tables loaded: identity map of the first 4 GiB\n");
    idt_init();
    out("[kernel] IDT loaded: 32 exception handlers + timer + keyboard\n");

    g_screen = screen_init(bi);
    if (g_screen) {
        draw_title();
        screen_clear_console();
        out("[kernel] framebuffer "); out_dec(bi->fb_width); out("x"); out_dec(bi->fb_height);
        out(" at "); out_hex(bi->fb_base);
        /* Read a pixel back to prove the writes landed in video memory. */
        out(screen_get_pixel(4, 4) == RGB(40, 90, 170) ? " - drawing works\n" : " - readback mismatch\n");
    }
    out("[kernel] kernel image at "); out_hex(bi->kernel_phys_start); out(" - "); out_hex(bi->kernel_phys_end); out("\n");

    memory_init();
    out("[kernel] memory map: "); out_dec(g_free_bytes >> 20); out(" MiB free for the kernel to use\n");
    uint64_t p1 = alloc_page(), p2 = alloc_page();
    out("[kernel] page allocator: got "); out_hex(p1); out(" and "); out_hex(p2); out("\n");

    __asm__ volatile("int3");    /* proves the IDT works: handler runs and returns */

    interrupts_init(100);
    out("[kernel] interrupts on: PIT timer at 100 Hz, PS/2 keyboard on IRQ1\n");

    con_set_color(RGB(140, 230, 140));
    out("\nTYPE HELP AND PRESS ENTER.\n");
    con_set_color(RGB(230, 230, 230));
    out("> ");

    char line[64]; unsigned len = 0;
    uint64_t last_sec = 0;
    for (;;) {
        hlt();                                  /* sleep until the next interrupt */
        uint64_t sec = g_ticks / g_timer_hz;
        if (sec != last_sec) {
            last_sec = sec;
            draw_uptime();
            if (sec <= 3) { /* serial only, to keep the screen clean */
                const char *m = "[kernel] uptime "; while (*m) serial_putc(*m++);
                serial_putc('0' + (char)sec); m = " s\r\n"; while (*m) serial_putc(*m++);
            }
            if (sec == 3) {
                const char *m = "[kernel] boot checks complete\r\n"; while (*m) serial_putc(*m++);
                /* QEMU's isa-debug-exit device (added only by the self-test)
                 * quits the emulator on this write. Normally nothing listens. */
                outb(0xF4, 0);
            }
        }
        int c;
        while ((c = keyboard_getc()) >= 0) {
            if (c == '\n') { out("\n"); line[len] = 0; run_command(line); len = 0; out("> "); }
            else if (c == '\b') { if (len) { len--; out("\b"); } }
            else if (len < sizeof line - 1) { line[len++] = (char)c; char s[2] = {(char)c, 0}; out(s); }
        }
    }
}

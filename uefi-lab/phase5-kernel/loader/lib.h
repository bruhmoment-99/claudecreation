/* lib.h - tiny helpers shared by driver and loader (header-only, static). */
#pragma once
#include "efi.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

/* ---- Firmware console (valid only while Boot Services exist) ---- */
static EFI_SYSTEM_TABLE *gST;
static void print(const CHAR16 *s) { gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s); }

static void fmt_hex(CHAR16 *buf, uint64_t v, int digits) {
    buf[0] = u'0'; buf[1] = u'x';
    for (int i = 0; i < digits; i++) {
        unsigned n = (v >> ((digits - 1 - i) * 4)) & 0xF;
        buf[2 + i] = n < 10 ? u'0' + n : u'A' + n - 10;
    }
    buf[2 + digits] = 0;
}
static inline void print_hex(uint64_t v) { CHAR16 b[19]; fmt_hex(b, v, 16); print(b); }
static inline void print_dec(uint64_t v) {
    CHAR16 b[21]; int i = 20; b[i] = 0;
    do { b[--i] = u'0' + v % 10; v /= 10; } while (v);
    print(&b[i]);
}

/* ---- Raw 16550 UART on COM1 (0x3F8) ----
 * No firmware involved: this keeps working after ExitBootServices(), when
 * ConOut is gone. QEMU's -nographic / -serial stdio shows it. */
static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static void serial_putc(char c) {
    while (!(inb(0x3F8 + 5) & 0x20)) { }   /* LSR: transmit holding register empty */
    outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char *s) {
    for (; *s; s++) { if (*s == '\n') serial_putc('\r'); serial_putc(*s); }
}
static inline void serial_dec(uint64_t v) {
    char b[21]; int i = 20; b[i] = 0;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    serial_puts(&b[i]);
}
static inline void serial_dec2(unsigned v) { serial_putc('0' + v / 10 % 10); serial_putc('0' + v % 10); }

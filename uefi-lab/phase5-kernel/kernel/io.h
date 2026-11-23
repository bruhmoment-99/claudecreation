/* io.h - port I/O, MSRs and the serial port. All header-only. */
#pragma once
#include <stdint.h>
#include <stddef.h>

static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)           { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void)                { outb(0x80, 0); }   /* tiny delay for old chips */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return ((uint64_t)hi << 32) | lo;
}
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

/* COM1. Firmware already configured it (115200 8N1); we just use it. */
static inline void serial_putc(char c) {
    for (int i = 0; i < 100000 && !(inb(0x3F8 + 5) & 0x20); i++) { }
    outb(0x3F8, (uint8_t)c);
}

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

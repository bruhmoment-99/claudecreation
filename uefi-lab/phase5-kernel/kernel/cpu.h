/* cpu.h - taking over the CPU from firmware. */
#pragma once
#include <stdint.h>

void gdt_init(void);                 /* our own segment descriptors */
void paging_init(void);              /* our own page tables (identity map, 4 GiB) */
void idt_init(void);                 /* exception + IRQ handlers */
void interrupts_init(uint32_t hz);   /* PIC, PIT timer, keyboard; then STI */

extern volatile uint64_t g_ticks;    /* timer interrupts since interrupts_init */
extern uint32_t g_timer_hz;
int  keyboard_getc(void);            /* next typed character, or -1 */

/* Called by the exception handlers; implemented in kernel.c. */
void kernel_exception(unsigned vector, const char *name, uint64_t rip, uint64_t error, int fatal);

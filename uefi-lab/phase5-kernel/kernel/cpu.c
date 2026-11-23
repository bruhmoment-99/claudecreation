/* cpu.c - the CPU structures every x86-64 kernel sets up first.
 *
 * At entry we're still running on the firmware's GDT, page tables and
 * (disabled) interrupt setup. Those live in EfiBootServicesData memory,
 * which the memory map now says is FREE. So before using any of that
 * memory, a kernel replaces each of them with its own copy. */
#include "cpu.h"
#include "io.h"

/* ============================ GDT ============================
 * In 64-bit mode segmentation is mostly gone, but the CPU still needs a
 * code segment (with the "long mode" bit) and a data segment. */
static uint64_t gdt[3] = {
    0,                       /* 0x00: mandatory null descriptor        */
    0x00AF9A000000FFFFULL,   /* 0x08: kernel code, 64-bit, ring 0      */
    0x00CF92000000FFFFULL,   /* 0x10: kernel data, ring 0              */
};

void gdt_init(void) {
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr = { sizeof gdt - 1, (uint64_t)(uintptr_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        /* CS can't be loaded with MOV: do a far return to 0x08:label. */
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n movw %%ax, %%gs\n"
        :: "m"(gdtr) : "rax", "memory");
}

/* ============================ Paging ============================
 * 4-level paging: PML4 -> PDPT -> PD -> 2 MiB pages. We identity-map the
 * first 4 GiB (virtual address == physical address), which covers RAM,
 * the framebuffer and the local APIC. The 3-4 GiB window holds device
 * registers, so it's mapped uncached (PCD|PWT). */
#define PTE_P   0x001   /* present        */
#define PTE_W   0x002   /* writable       */
#define PTE_PWT 0x008   /* write-through  */
#define PTE_PCD 0x010   /* cache disable  */
#define PTE_PS  0x080   /* 2 MiB page     */

static uint64_t pml4[512]   __attribute__((aligned(4096)));
static uint64_t pdpt[512]   __attribute__((aligned(4096)));
static uint64_t pd[4][512]  __attribute__((aligned(4096)));

void paging_init(void) {
    for (int g = 0; g < 4; g++) {
        for (int i = 0; i < 512; i++) {
            uint64_t addr = ((uint64_t)g << 30) | ((uint64_t)i << 21);
            pd[g][i] = addr | PTE_P | PTE_W | PTE_PS | (g == 3 ? (PTE_PCD | PTE_PWT) : 0);
        }
        pdpt[g] = (uint64_t)(uintptr_t)pd[g] | PTE_P | PTE_W;
    }
    pml4[0] = (uint64_t)(uintptr_t)pdpt | PTE_P | PTE_W;
    /* Our tables are in our .bss, and the kernel runs identity-mapped, so
     * their virtual addresses ARE their physical addresses. */
    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)pml4) : "memory");
}

/* ============================ IDT ============================ */
struct __attribute__((packed)) idt_entry {
    uint16_t off0, sel; uint8_t ist, type; uint16_t off1; uint32_t off2, zero;
};
static struct idt_entry idt[256];

struct interrupt_frame { uint64_t rip, cs, rflags, rsp, ss; };

static void set_gate(int v, void *handler) {
    uint64_t a = (uint64_t)(uintptr_t)handler;
    idt[v] = (struct idt_entry){ (uint16_t)a, 0x08, 0, 0x8E /* present, ring 0, interrupt gate */,
                                 (uint16_t)(a >> 16), (uint32_t)(a >> 32), 0 };
}

static const char *EXC_NAMES[32] = {
    "DIVIDE ERROR", "DEBUG", "NMI", "BREAKPOINT", "OVERFLOW", "BOUND RANGE", "INVALID OPCODE",
    "DEVICE NOT AVAILABLE", "DOUBLE FAULT", "COPROCESSOR OVERRUN", "INVALID TSS", "SEGMENT NOT PRESENT",
    "STACK FAULT", "GENERAL PROTECTION FAULT", "PAGE FAULT", "RESERVED", "X87 FPU ERROR",
    "ALIGNMENT CHECK", "MACHINE CHECK", "SIMD ERROR", "VIRTUALIZATION", "CONTROL PROTECTION",
    "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "HYPERVISOR INJECTION",
    "VMM COMMUNICATION", "SECURITY", "RESERVED"
};

/* One tiny handler per exception vector, so each knows its own number.
 * __attribute__((interrupt)) makes clang save registers and end with IRETQ.
 * Only #BP (3, the INT3 instruction) is survivable here; the rest stop. */
#define EXC_NOERR(n) \
    __attribute__((interrupt)) static void exc##n(struct interrupt_frame *f) \
    { kernel_exception(n, EXC_NAMES[n], f->rip, 0, n != 3); }
#define EXC_ERR(n) \
    __attribute__((interrupt)) static void exc##n(struct interrupt_frame *f, unsigned long err) \
    { kernel_exception(n, EXC_NAMES[n], f->rip, err, 1); }

EXC_NOERR(0)  EXC_NOERR(1)  EXC_NOERR(2)  EXC_NOERR(3)  EXC_NOERR(4)  EXC_NOERR(5)
EXC_NOERR(6)  EXC_NOERR(7)  EXC_ERR(8)    EXC_NOERR(9)  EXC_ERR(10)   EXC_ERR(11)
EXC_ERR(12)   EXC_ERR(13)   EXC_ERR(14)   EXC_NOERR(15) EXC_NOERR(16) EXC_ERR(17)
EXC_NOERR(18) EXC_NOERR(19) EXC_NOERR(20) EXC_ERR(21)   EXC_NOERR(22) EXC_NOERR(23)
EXC_NOERR(24) EXC_NOERR(25) EXC_NOERR(26) EXC_NOERR(27) EXC_NOERR(28) EXC_ERR(29)
EXC_ERR(30)   EXC_NOERR(31)

static void *const EXC_TABLE[32] = {
    exc0, exc1, exc2, exc3, exc4, exc5, exc6, exc7, exc8, exc9, exc10, exc11, exc12, exc13, exc14, exc15,
    exc16, exc17, exc18, exc19, exc20, exc21, exc22, exc23, exc24, exc25, exc26, exc27, exc28, exc29, exc30, exc31
};

/* ---- hardware interrupts ---- */
volatile uint64_t g_ticks;
uint32_t g_timer_hz;

#define KBD_BUF 64
static volatile uint8_t kbd_buf[KBD_BUF];
static volatile unsigned kbd_head, kbd_tail;

static inline void pic_eoi(int irq) { if (irq >= 8) outb(0xA0, 0x20); outb(0x20, 0x20); }

__attribute__((interrupt)) static void irq_timer(struct interrupt_frame *f) {
    (void)f; g_ticks++; pic_eoi(0);
}
__attribute__((interrupt)) static void irq_keyboard(struct interrupt_frame *f) {
    (void)f;
    uint8_t sc = inb(0x60);                         /* must read, or no more IRQs */
    unsigned next = (kbd_head + 1) % KBD_BUF;
    if (next != kbd_tail) { kbd_buf[kbd_head] = sc; kbd_head = next; }
    pic_eoi(1);
}
__attribute__((interrupt)) static void irq_spurious(struct interrupt_frame *f) { (void)f; /* no EOI */ }

void idt_init(void) {
    for (int i = 0; i < 32; i++) set_gate(i, EXC_TABLE[i]);
    set_gate(0x20, irq_timer);       /* IRQ0 after PIC remap */
    set_gate(0x21, irq_keyboard);    /* IRQ1 */
    set_gate(0x27, irq_spurious);    /* IRQ7: PIC spurious */
    set_gate(0x2F, irq_spurious);    /* IRQ15: PIC spurious */
    set_gate(0xFF, irq_spurious);    /* local APIC spurious vector */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr = { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

/* ---- PS/2 keyboard: scancode set 1 -> ASCII (no shift; the font is caps only) ---- */
static const char KEYMAP[0x3A] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1A]='[',[0x1B]=']',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
    [0x27]=':',[0x28]='\'',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' ',
};

int keyboard_getc(void) {
    while (kbd_tail != kbd_head) {
        uint8_t sc = kbd_buf[kbd_tail];
        kbd_tail = (kbd_tail + 1) % KBD_BUF;
        if (sc & 0x80) continue;                     /* key release */
        if (sc < sizeof KEYMAP && KEYMAP[sc]) return KEYMAP[sc];
    }
    return -1;
}

static void i8042_wait_write(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { } }
static void i8042_wait_read(void)  { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) { } }

/* ---- PIC + PIT + local APIC virtual-wire mode ---- */
void interrupts_init(uint32_t hz) {
    /* 1. Remap the two 8259 PICs so IRQ0-15 land on vectors 0x20-0x2F
     *    (by default they collide with CPU exception vectors 8-15). */
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();   /* ICW1: init, expect ICW4 */
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();   /* ICW2: vector offsets */
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();   /* ICW3: cascade on IRQ2 */
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();   /* ICW4: 8086 mode */
    outb(0x21, 0xFC);                                           /* unmask IRQ0 (timer) + IRQ1 (keyboard) */
    outb(0xA1, 0xFF);

    /* 2. Let PIC interrupts reach the CPU through the local APIC: LINT0 in
     *    ExtINT mode ("virtual wire"). Firmware usually does this; we make sure. */
    uint64_t apic = rdmsr(0x1B) & ~0xFFFULL;
    volatile uint32_t *lapic = (volatile uint32_t *)(uintptr_t)apic;
    lapic[0xF0 / 4]  = 0x1FF;          /* enable APIC, spurious vector 0xFF */
    lapic[0x320 / 4] = 0x10000;        /* mask the APIC timer (we use the PIT) */
    lapic[0x350 / 4] = 0x700;          /* LINT0 = ExtINT: the 8259 PIC */
    lapic[0x360 / 4] = 0x400;          /* LINT1 = NMI */

    /* 3. Program the 8254 PIT, channel 0, as a periodic timer. */
    uint32_t div = 1193182 / hz;
    outb(0x43, 0x36);                  /* channel 0, lo/hi byte, mode 3 (square wave) */
    outb(0x40, (uint8_t)div); outb(0x40, (uint8_t)(div >> 8));
    g_timer_hz = hz;

    /* 4. PS/2 controller: drain stale bytes, enable the keyboard IRQ. The
     *    firmware's driver polls, so it often leaves IRQ1 switched off. */
    while (inb(0x64) & 1) (void)inb(0x60);
    i8042_wait_write(); outb(0x64, 0x20);                  /* read config byte */
    i8042_wait_read();  uint8_t cfg = inb(0x60);
    cfg |= 0x01;                                           /* IRQ1 on */
    cfg |= 0x40;                                           /* translate to scancode set 1 */
    i8042_wait_write(); outb(0x64, 0x60);                  /* write config byte */
    i8042_wait_write(); outb(0x60, cfg);

    sti();
}

/* linker.ld - where the kernel lives in memory.
 * The bootloader asks UEFI for exactly these physical pages
 * (AllocatePages with AllocateAddress), so this address must be free RAM. */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;                 /* 4 MiB */

    .text   : { *(.text._start) *(.text .text.*) }
    .rodata : ALIGN(4096) { *(.rodata .rodata.*) }
    .data   : ALIGN(4096) { *(.data .data.*) }
    .bss    : ALIGN(4096) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}

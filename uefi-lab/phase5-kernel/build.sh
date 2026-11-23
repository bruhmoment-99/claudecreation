#!/usr/bin/env bash
# Build our bootloader (BOOTX64.EFI, a PE/UEFI app) and our kernel (kernel.elf,
# a plain ELF). Same clang, two different targets.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p esp/EFI/BOOT
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
      -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -std=c11 \
      -nostdlib -fuse-ld=lld-link -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
      -o esp/EFI/BOOT/BOOTX64.EFI loader/loader.c loader/crt.c
# Kernel: ELF, no PIE, no SSE (-mgeneral-regs-only: interrupt handlers don't
# save SSE state). Compile with clang, link with ld.lld directly (for a bare
# ELF target clang would otherwise look for gcc). kernel/linker.ld fixes the
# physical address.
mkdir -p obj
for f in kernel cpu screen; do
  clang -target x86_64-unknown-none-elf -ffreestanding -fno-builtin -fno-stack-protector \
        -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -O2 -Wall -Wextra -std=c11 \
        -c -o obj/$f.o kernel/$f.c
done
ld.lld -T kernel/linker.ld -z max-page-size=0x1000 --build-id=none -nostdlib -static \
       -o esp/kernel.elf obj/kernel.o obj/cpu.o obj/screen.o
echo "built esp/EFI/BOOT/BOOTX64.EFI (bootloader) and esp/kernel.elf (kernel)"

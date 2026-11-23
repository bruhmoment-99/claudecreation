#!/usr/bin/env bash
# Build BOOTX64.EFI with plain clang + lld (Linux/macOS/WSL). Windows: use build.ps1.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p esp/EFI/BOOT
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
      -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -std=c11 \
      -nostdlib -fuse-ld=lld-link \
      -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
      -o esp/EFI/BOOT/BOOTX64.EFI main.c
echo "built esp/EFI/BOOT/BOOTX64.EFI"

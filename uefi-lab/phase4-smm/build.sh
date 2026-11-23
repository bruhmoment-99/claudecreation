#!/usr/bin/env bash
# Build the Phase 4 ring-0 test app. The SMM side is part of the firmware:
# see firmware/build-firmware.sh (prebuilt result in firmware/bin/).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p esp/EFI/BOOT
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
      -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -std=c11 \
      -nostdlib -fuse-ld=lld-link -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
      -o esp/EFI/BOOT/BOOTX64.EFI app/smmtest.c app/crt.c
echo "built esp/EFI/BOOT/BOOTX64.EFI"

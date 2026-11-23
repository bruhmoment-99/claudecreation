#!/usr/bin/env bash
# Build the Phase 6 test app. The SMM side is in ../phase4-smm (LabSmmPkg),
# already built into ../phase4-smm/firmware/bin/OVMF_CODE.fd.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p esp/EFI/BOOT
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
      -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -std=c11 \
      -nostdlib -fuse-ld=lld-link -Wl,-entry:efi_main -Wl,-subsystem:efi_application \
      -o esp/EFI/BOOT/BOOTX64.EFI app/flashguard.c app/crt.c
echo "built esp/EFI/BOOT/BOOTX64.EFI"

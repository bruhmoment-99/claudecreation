#!/usr/bin/env bash
# Build the Phase 10 Secure Boot lab: the lab app (BOOTX64.EFI) and two tiny
# test programs. Copies the test PK/KEK certificate onto the disk.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p esp/EFI/BOOT esp/EFI/lab
CFLAGS=(-target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone
        -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -std=c11
        -nostdlib -fuse-ld=lld-link -Wl,-entry:efi_main -Wl,-subsystem:efi_application)
clang "${CFLAGS[@]}" -o esp/EFI/BOOT/BOOTX64.EFI app/secureboot.c app/crt.c
clang "${CFLAGS[@]}" -o esp/EFI/lab/trusted.efi   app/trusted.c
clang "${CFLAGS[@]}" -o esp/EFI/lab/untrusted.efi app/untrusted.c
cp keys/LabTestPK.der esp/EFI/lab/
echo "built esp/EFI/BOOT/BOOTX64.EFI, esp/EFI/lab/trusted.efi, esp/EFI/lab/untrusted.efi"

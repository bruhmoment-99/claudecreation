#!/usr/bin/env bash
# Build the loader (BOOTX64.EFI, an application) and the driver (greeter.efi,
# a boot service driver). Only the PE subsystem flag differs between them.
set -euo pipefail
cd "$(dirname "$0")"
CFLAGS=(-target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone
        -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -std=c11
        -nostdlib -fuse-ld=lld-link -Wl,-entry:efi_main)
mkdir -p esp/EFI/BOOT esp/EFI/lab
clang "${CFLAGS[@]}" -Wl,-subsystem:efi_application    -o esp/EFI/BOOT/BOOTX64.EFI loader.c crt.c
clang "${CFLAGS[@]}" -Wl,-subsystem:efi_boot_service_driver -o esp/EFI/lab/greeter.efi driver.c crt.c
echo "built esp/EFI/BOOT/BOOTX64.EFI (loader) and esp/EFI/lab/greeter.efi (driver)"

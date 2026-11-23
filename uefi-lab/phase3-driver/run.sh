#!/usr/bin/env bash
# Boot the app in QEMU + OVMF. The esp/ folder is exposed as a FAT drive,
# so OVMF finds EFI/BOOT/BOOTX64.EFI and runs it automatically.
# Pass --headless for serial-only output in the terminal.
set -euo pipefail
cd "$(dirname "$0")"
OVMF_DIR=${OVMF_DIR:-/usr/share/OVMF}
cp -n "$OVMF_DIR/OVMF_VARS_4M.fd" ovmf_vars.fd 2>/dev/null || true
DISPLAY_ARGS=()
[[ "${1:-}" == "--headless" ]] && DISPLAY_ARGS=(-nographic)
exec qemu-system-x86_64 -machine q35 -m 256M -net none \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_DIR/OVMF_CODE_4M.fd" \
  -drive if=pflash,format=raw,file=ovmf_vars.fd \
  -drive format=raw,file=fat:rw:esp \
  "${DISPLAY_ARGS[@]}"

#!/usr/bin/env bash
# Boot Phase 6 on our SMM firmware (../phase4-smm/firmware/bin).
# ovmf_vars.fd here is the flash variable store: it keeps LabBootCounter
# between runs. Delete it to start over. --headless = console in this terminal.
set -euo pipefail
cd "$(dirname "$0")"
[[ -f esp/EFI/BOOT/BOOTX64.EFI ]] || ./build.sh
FW=../phase4-smm/firmware/bin
cp -n "$FW/OVMF_VARS.fd" ovmf_vars.fd 2>/dev/null || true
DISPLAY_ARGS=()
[[ "${1:-}" == "--headless" ]] && DISPLAY_ARGS=(-nographic)
# cfi.pflash01 secure=on is the flash protection under test: only SMM may write.
exec qemu-system-x86_64 -machine q35,smm=on -m 256M -net none \
  -global driver=cfi.pflash01,property=secure,value=on \
  -global ICH9-LPC.disable_s3=1 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$FW/OVMF_CODE.fd" \
  -drive if=pflash,format=raw,unit=1,file=ovmf_vars.fd \
  -drive format=raw,file=fat:rw:esp \
  "${DISPLAY_ARGS[@]}"

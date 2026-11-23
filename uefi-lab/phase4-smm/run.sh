#!/usr/bin/env bash
# Boot Phase 4 in QEMU on OUR firmware (firmware/bin/OVMF_CODE.fd, which has
# SMM enabled and LabSmmHandler built in). --headless = serial console here.
set -euo pipefail
cd "$(dirname "$0")"
[[ -f esp/EFI/BOOT/BOOTX64.EFI ]] || ./build.sh
cp -n firmware/bin/OVMF_VARS.fd ovmf_vars.fd 2>/dev/null || true
DISPLAY_ARGS=()
[[ "${1:-}" == "--headless" ]] && DISPLAY_ARGS=(-nographic)
# smm=on                 : emulate System Management Mode + SMRAM (TSEG)
# cfi.pflash01 secure=on : only SMM code may write the flash (variables)
# disable_s3=1           : no S3 resume path needed for this lab
exec qemu-system-x86_64 -machine q35,smm=on -m 256M -net none \
  -global driver=cfi.pflash01,property=secure,value=on \
  -global ICH9-LPC.disable_s3=1 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=firmware/bin/OVMF_CODE.fd \
  -drive if=pflash,format=raw,unit=1,file=ovmf_vars.fd \
  -drive format=raw,file=fat:rw:esp \
  "${DISPLAY_ARGS[@]}"

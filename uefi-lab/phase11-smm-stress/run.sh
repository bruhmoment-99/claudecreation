#!/usr/bin/env bash
# Boot the Phase 11 stress test on the Traditional MM firmware (default) or, with
# --standalone, on the Standalone MM build. --headless = console here.
# Takes a few seconds; results also go to the serial console.
set -euo pipefail
cd "$(dirname "$0")"
[[ -f esp/EFI/BOOT/BOOTX64.EFI ]] || ./build.sh
FW=../phase4-smm/firmware/bin
DISPLAY_ARGS=()
for a in "$@"; do
  case "$a" in
    --headless)   DISPLAY_ARGS=(-nographic) ;;
    --standalone) FW=../phase4-smm/firmware/bin-standalone ;;
  esac
done
cp "$FW/OVMF_VARS.fd" ovmf_vars.fd
exec qemu-system-x86_64 -machine q35,smm=on -m 256M -net none \
  -global mch.extended-tseg-mbytes=32 \
  -global driver=cfi.pflash01,property=secure,value=on \
  -global ICH9-LPC.disable_s3=1 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$FW/OVMF_CODE.fd" \
  -drive if=pflash,format=raw,unit=1,file=ovmf_vars.fd \
  -drive format=raw,file=fat:rw:esp \
  "${DISPLAY_ARGS[@]}"

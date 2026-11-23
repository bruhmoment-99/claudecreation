#!/usr/bin/env bash
# Boot the Phase 7 app on the Standalone MM firmware (default) or, with
# --traditional, on the Traditional MM build. --headless = console here.
set -euo pipefail
cd "$(dirname "$0")"
[[ -f esp/EFI/BOOT/BOOTX64.EFI ]] || ./build.sh
FW=../phase4-smm/firmware/bin-standalone
DISPLAY_ARGS=()
for a in "$@"; do
  case "$a" in
    --headless)    DISPLAY_ARGS=(-nographic) ;;
    --traditional) FW=../phase4-smm/firmware/bin ;;
  esac
done
cp -n "$FW/OVMF_VARS.fd" ovmf_vars.fd 2>/dev/null || true
# mch.extended-tseg-mbytes=32: Standalone MM keeps its whole heap in TSEG.
exec qemu-system-x86_64 -machine q35,smm=on -m 256M -net none \
  -global mch.extended-tseg-mbytes=32 \
  -global driver=cfi.pflash01,property=secure,value=on \
  -global ICH9-LPC.disable_s3=1 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$FW/OVMF_CODE.fd" \
  -drive if=pflash,format=raw,unit=1,file=ovmf_vars.fd \
  -drive format=raw,file=fat:rw:esp \
  "${DISPLAY_ARGS[@]}"

#!/usr/bin/env bash
# Boot the Phase 10 Secure Boot lab. First boot enrolls keys; boot again to
# see the firmware check this app first. --reset wipes the emulated flash
# (back to setup mode); needed after rebuilding the app. --headless = console here.
set -euo pipefail
cd "$(dirname "$0")"
[[ -f esp/EFI/BOOT/BOOTX64.EFI ]] || ./build.sh
FW=../phase4-smm/firmware/bin
DISPLAY_ARGS=()
for a in "$@"; do
  case "$a" in
    --headless) DISPLAY_ARGS=(-nographic) ;;
    --reset)    rm -f ovmf_vars.fd; echo "Emulated flash wiped: Secure Boot is back in setup mode." ;;
  esac
done
[[ -f ovmf_vars.fd ]] || cp "$FW/OVMF_VARS.fd" ovmf_vars.fd
exec qemu-system-x86_64 -machine q35,smm=on -m 256M -net none \
  -global driver=cfi.pflash01,property=secure,value=on \
  -global ICH9-LPC.disable_s3=1 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file="$FW/OVMF_CODE.fd" \
  -drive if=pflash,format=raw,unit=1,file=ovmf_vars.fd \
  -drive format=raw,file=fat:rw:esp \
  "${DISPLAY_ARGS[@]}"

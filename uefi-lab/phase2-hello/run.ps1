# Boot the app in QEMU + OVMF on Windows. Needs QEMU (winget install SoftwareFreedomConservancy.QEMU).
# QEMU for Windows ships edk2 firmware in its share\ folder; override with -Ovmf if yours differs.
param([string]$Ovmf = "C:\Program Files\qemu\share", [switch]$Headless)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$code = Join-Path $Ovmf "edk2-x86_64-code.fd"
if (-not (Test-Path ovmf_vars.fd)) { Copy-Item (Join-Path $Ovmf "edk2-i386-vars.fd") ovmf_vars.fd }
$disp = @(); if ($Headless) { $disp = @("-nographic") }
& "C:\Program Files\qemu\qemu-system-x86_64.exe" -machine q35 -m 256M -net none `
  -drive "if=pflash,format=raw,readonly=on,file=$code" `
  -drive "if=pflash,format=raw,file=ovmf_vars.fd" `
  -drive "format=raw,file=fat:rw:esp" @disp

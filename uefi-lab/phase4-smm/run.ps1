# Boot Phase 4 in QEMU on OUR firmware (firmware\bin, SMM enabled, LabSmmHandler inside).
#   run.cmd            -> QEMU window
#   run.cmd -Headless  -> console in this terminal; quit with Ctrl+A then X
param([switch]$Headless)
. "$PSScriptRoot\..\tools\common.ps1"
if (-not (Test-Path "$PSScriptRoot\esp\EFI\BOOT\BOOTX64.EFI")) { & "$PSScriptRoot\build.ps1" }
Start-LabVm -Dir $PSScriptRoot -FirmwareDir "$PSScriptRoot\firmware\bin" -Headless:$Headless

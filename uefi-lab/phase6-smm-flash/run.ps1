# Boot Phase 6 on our SMM firmware. ovmf_vars.fd in this folder is the flash
# variable store: LabBootCounter goes up on every run. Delete it to reset.
#   run.cmd            -> QEMU window
#   run.cmd -Headless  -> console in this terminal; quit with Ctrl+A then X
param([switch]$Headless)
. "$PSScriptRoot\..\tools\common.ps1"
if (-not (Test-Path "$PSScriptRoot\esp\EFI\BOOT\BOOTX64.EFI")) { & "$PSScriptRoot\build.ps1" }
Start-LabVm -Dir $PSScriptRoot -FirmwareDir "$PSScriptRoot\..\phase4-smm\firmware\bin" -Headless:$Headless

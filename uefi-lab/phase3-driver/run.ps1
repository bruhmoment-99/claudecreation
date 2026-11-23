# Boot this phase in QEMU + OVMF on Windows (builds first if needed).
#   run.cmd            -> QEMU window
#   run.cmd -Headless  -> console in this terminal; quit with Ctrl+A then X
param([switch]$Headless)
. "$PSScriptRoot\..\tools\common.ps1"
if (-not (Test-Path "$PSScriptRoot\esp\EFI\BOOT\BOOTX64.EFI")) { & "$PSScriptRoot\build.ps1" }
Start-LabVm -Dir $PSScriptRoot -Headless:$Headless

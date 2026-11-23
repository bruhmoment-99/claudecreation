# Boot our bootloader + kernel in QEMU on Windows (builds first if needed).
#   run.cmd            -> QEMU window: click into it and type HELP + Enter
#   run.cmd -Headless  -> serial log in this terminal; quit with Ctrl+A then X
param([switch]$Headless)
. "$PSScriptRoot\..\tools\common.ps1"
if (-not (Test-Path "$PSScriptRoot\esp\kernel.elf")) { & "$PSScriptRoot\build.ps1" }
Start-LabVm -Dir $PSScriptRoot -Headless:$Headless

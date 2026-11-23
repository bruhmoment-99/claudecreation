# Boot the Phase 9 app on the Traditional MM firmware (default) or, with
# -Standalone, on the Standalone MM firmware. Press W for the live write test.
#   run.cmd -Headless                -> Traditional MM, console in this terminal
#   run.cmd -Headless -Standalone    -> Standalone MM
param([switch]$Headless, [switch]$Standalone)
. "$PSScriptRoot\..\tools\common.ps1"
if (-not (Test-Path "$PSScriptRoot\esp\EFI\BOOT\BOOTX64.EFI")) { & "$PSScriptRoot\build.ps1" }
$fw = "$PSScriptRoot\..\phase4-smm\firmware\bin"
if ($Standalone) { $fw = "$PSScriptRoot\..\phase4-smm\firmware\bin-standalone" }
# Standalone MM keeps its whole heap in TSEG (SMRAM); QEMU's default 16 MiB
# is too small for its variable driver, so ask for 32 MiB.
Start-LabVm -Dir $PSScriptRoot -FirmwareDir $fw -Headless:$Headless -ExtraArgs @('-global', 'mch.extended-tseg-mbytes=32')

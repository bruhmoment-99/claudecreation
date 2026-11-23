# Boot the Phase 7 app on the Standalone MM firmware (default) or, with
# -Traditional, on the Traditional MM firmware. Compare the two outputs.
#   run.cmd -Headless                -> Standalone MM, console in this terminal
#   run.cmd -Headless -Traditional   -> Traditional MM
param([switch]$Headless, [switch]$Traditional)
. "$PSScriptRoot\..\tools\common.ps1"
if (-not (Test-Path "$PSScriptRoot\esp\EFI\BOOT\BOOTX64.EFI")) { & "$PSScriptRoot\build.ps1" }
$fw = "$PSScriptRoot\..\phase4-smm\firmware\bin-standalone"
if ($Traditional) { $fw = "$PSScriptRoot\..\phase4-smm\firmware\bin" }
# Standalone MM keeps its whole heap in TSEG (SMRAM); QEMU's default 16 MiB
# is too small for its variable driver, so ask for 32 MiB.
Start-LabVm -Dir $PSScriptRoot -FirmwareDir $fw -Headless:$Headless -ExtraArgs @('-global', 'mch.extended-tseg-mbytes=32')

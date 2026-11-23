# Boot the Phase 10 Secure Boot lab (Traditional MM firmware, which has
# Secure Boot support). The first boot enrolls keys; boot again to see the
# firmware check this app before running it.
#   run.cmd -Headless          -> console in this terminal (Ctrl+A then X quits)
#   run.cmd -Headless -Reset   -> wipe the emulated flash: back to setup mode
# After keys are enrolled, a REBUILT app has a new hash and won't boot:
# use -Reset after rebuilding.
param([switch]$Headless, [switch]$Reset)
. "$PSScriptRoot\..\tools\common.ps1"
if (-not (Test-Path "$PSScriptRoot\esp\EFI\BOOT\BOOTX64.EFI")) { & "$PSScriptRoot\build.ps1" }
$vars = Join-Path $PSScriptRoot 'ovmf_vars.fd'
if ($Reset -and (Test-Path $vars)) { Remove-Item $vars; Write-Host "Emulated flash wiped: Secure Boot is back in setup mode." }
Start-LabVm -Dir $PSScriptRoot -FirmwareDir "$PSScriptRoot\..\phase4-smm\firmware\bin" -Headless:$Headless

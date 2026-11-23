# Build the Phase 8 debug-target app on Windows. Or double-click build.cmd.
# The MM side is already built into ..\phase4-smm\firmware\bin (Traditional)
# and ..\phase4-smm\firmware\bin-standalone (Standalone).
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('app\smmdebug.c', 'app\crt.c')

# Build the Phase 11 stress-test app on Windows. Or double-click build.cmd.
# The MM side is already built into ..\phase4-smm\firmware\bin and bin-standalone.
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('app\smmstress.c', 'app\crt.c')

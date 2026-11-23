# Build the Phase 9 self-protection app on Windows. Or double-click build.cmd.
# The MM side is already built into ..\phase4-smm\firmware\bin and bin-standalone.
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('app\smmguard.c', 'app\crt.c')

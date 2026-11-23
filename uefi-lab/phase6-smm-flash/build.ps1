# Build the Phase 6 test app on Windows. Or double-click build.cmd.
# The SMM side is already in ..\phase4-smm\firmware\bin\OVMF_CODE.fd.
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('app\flashguard.c', 'app\crt.c')

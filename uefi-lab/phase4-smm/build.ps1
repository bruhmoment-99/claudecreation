# Build the Phase 4 ring-0 test app on Windows. Or double-click build.cmd.
# The SMM side is already built into firmware\bin\OVMF_CODE.fd (no EDK II needed).
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('app\smmtest.c', 'app\crt.c')

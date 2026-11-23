# Build the Phase 10 Secure Boot lab on Windows: the lab app (BOOTX64.EFI) and
# two tiny test programs. Copies the test PK/KEK certificate onto the disk.
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('app\secureboot.c', 'app\crt.c')
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\lab\trusted.efi'   -Subsystem 'efi_application' -Sources @('app\trusted.c')
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\lab\untrusted.efi' -Subsystem 'efi_application' -Sources @('app\untrusted.c')
Copy-Item (Join-Path $PSScriptRoot 'keys\LabTestPK.der') (Join-Path $PSScriptRoot 'esp\EFI\lab\') -Force

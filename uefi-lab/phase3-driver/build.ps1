# Build phase 3 on Windows: the loader (an application) and the driver (a boot
# service driver). Only the PE subsystem differs. Or double-click build.cmd.
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('loader.c', 'crt.c')
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\lab\greeter.efi' -Subsystem 'efi_boot_service_driver' -Sources @('driver.c', 'crt.c')

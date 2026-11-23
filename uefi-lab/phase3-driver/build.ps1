# Build loader + driver on Windows. Needs LLVM (winget install LLVM.LLVM).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$cflags = @('-target','x86_64-unknown-windows','-ffreestanding','-fshort-wchar','-mno-red-zone',
            '-fno-stack-protector','-fno-builtin','-O2','-Wall','-Wextra','-std=c11',
            '-nostdlib','-fuse-ld=lld-link','-Wl,-entry:efi_main')
New-Item -ItemType Directory -Force esp\EFI\BOOT, esp\EFI\lab | Out-Null
clang @cflags '-Wl,-subsystem:efi_application' -o esp\EFI\BOOT\BOOTX64.EFI loader.c crt.c
if ($LASTEXITCODE -ne 0) { throw "loader build failed" }
clang @cflags '-Wl,-subsystem:efi_boot_service_driver' -o esp\EFI\lab\greeter.efi driver.c crt.c
if ($LASTEXITCODE -ne 0) { throw "driver build failed" }
Write-Host "built esp\EFI\BOOT\BOOTX64.EFI (loader) and esp\EFI\lab\greeter.efi (driver)"

# Build BOOTX64.EFI on Windows. Needs LLVM (winget install LLVM.LLVM).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force esp\EFI\BOOT | Out-Null
clang -target x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone `
      -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -std=c11 `
      -nostdlib -fuse-ld=lld-link `
      "-Wl,-entry:efi_main" "-Wl,-subsystem:efi_application" `
      -o esp\EFI\BOOT\BOOTX64.EFI main.c
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built esp\EFI\BOOT\BOOTX64.EFI"

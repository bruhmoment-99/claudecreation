# Build Phase 5 on Windows: our bootloader (a UEFI .efi) and our kernel (an ELF).
# Or double-click build.cmd. Needs LLVM (clang + lld-link + ld.lld).
. "$PSScriptRoot\..\tools\common.ps1"
Invoke-EfiBuild -Dir $PSScriptRoot -Out 'esp\EFI\BOOT\BOOTX64.EFI' -Subsystem 'efi_application' -Sources @('loader\loader.c', 'loader\crt.c')

# The kernel: compile each file for a bare ELF target, then link with ld.lld
# at the fixed address in kernel\linker.ld.
$clang = Get-Clang
$ld = Get-LdLld
if (-not $ld) { throw "ld.lld.exe not found next to clang. Reinstall LLVM (winget install LLVM.LLVM)." }
$cflags = @('-target', 'x86_64-unknown-none-elf', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
            '-fno-pic', '-fno-pie', '-mno-red-zone', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-std=c11')
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force obj | Out-Null
    foreach ($f in @('kernel', 'cpu', 'screen')) {
        & $clang @cflags -c -o "obj\$f.o" "kernel\$f.c"
        if ($LASTEXITCODE -ne 0) { throw "clang failed on kernel\$f.c" }
    }
    & $ld -T 'kernel\linker.ld' -z 'max-page-size=0x1000' '--build-id=none' -nostdlib -static `
          -o 'esp\kernel.elf' 'obj\kernel.o' 'obj\cpu.o' 'obj\screen.o'
    if ($LASTEXITCODE -ne 0) { throw "ld.lld failed linking the kernel" }
    Write-Host "built esp\kernel.elf"
} finally { Pop-Location }

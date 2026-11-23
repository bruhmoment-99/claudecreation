# common.ps1 - shared helpers for the UEFI lab on Windows 11.
# Every phase's build.ps1 / run.ps1 dot-sources this file:
#     . "$PSScriptRoot\..\tools\common.ps1"
# Written for Windows PowerShell 5.1 (built into Windows 11) and PowerShell 7.

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

function Find-Exe([string]$Name, [string[]]$Fallbacks) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($f in $Fallbacks) { if ($f -and (Test-Path $f)) { return $f } }
    return $null
}

# The LLVM and QEMU installers do not always add themselves to PATH, so we
# also look in their default install folders.
function Get-Clang {
    Find-Exe 'clang.exe' @("$env:ProgramFiles\LLVM\bin\clang.exe",
                           "${env:ProgramFiles(x86)}\LLVM\bin\clang.exe")
}
function Get-Qemu {
    Find-Exe 'qemu-system-x86_64.exe' @("$env:ProgramFiles\qemu\qemu-system-x86_64.exe")
}

# OVMF = the open-source UEFI firmware QEMU boots. The official QEMU for
# Windows installer ships it in its share\ folder. Override with the
# LAB_OVMF_CODE / LAB_OVMF_VARS environment variables if yours is elsewhere.
function Get-Ovmf {
    $qemu = Get-Qemu
    $code = $env:LAB_OVMF_CODE
    $vars = $env:LAB_OVMF_VARS
    if ($qemu) {
        $share = Join-Path (Split-Path $qemu) 'share'
        if (-not $code) { $code = Join-Path $share 'edk2-x86_64-code.fd' }
        if (-not $vars) { $vars = Join-Path $share 'edk2-i386-vars.fd' }
    }
    if ($code -and $vars -and (Test-Path $code) -and (Test-Path $vars)) {
        return @{ Code = $code; Vars = $vars }
    }
    return $null
}

function Assert-Toolchain {
    if (-not (Get-Clang)) { throw "clang not found. Run setup.cmd in the uefi-lab folder first." }
}
function Assert-Emulator {
    if (-not (Get-Qemu)) { throw "QEMU not found. Run setup.cmd in the uefi-lab folder first." }
}

# Compile C straight to a PE32+ .efi image with clang + lld-link.
#   -target x86_64-unknown-windows : UEFI images use the Windows PE format
#   -ffreestanding -nostdlib       : no C runtime exists in firmware
#   -mno-red-zone                  : firmware interrupts may clobber the red zone
#   -fshort-wchar                  : CHAR16 strings (u"...")
#   -subsystem:...                 : 10 = application, 11 = boot service driver
function Invoke-EfiBuild {
    param([string]$Dir, [string]$Out, [string]$Subsystem, [string[]]$Sources)
    Assert-Toolchain
    $clang = Get-Clang
    $flags = @('-target', 'x86_64-unknown-windows', '-ffreestanding', '-fshort-wchar',
               '-mno-red-zone', '-fno-stack-protector', '-fno-builtin', '-O2',
               '-Wall', '-Wextra', '-std=c11', '-nostdlib', '-fuse-ld=lld-link',
               '-Wl,-entry:efi_main', "-Wl,-subsystem:$Subsystem")
    Push-Location $Dir
    try {
        New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
        & $clang @flags -o $Out @Sources
        if ($LASTEXITCODE -ne 0) { throw "clang failed while building $Out" }
        Write-Host "built $Out"
    } finally { Pop-Location }
}

# QEMU command line shared by interactive runs and the self-test. The esp\
# folder is shown to the VM as a FAT drive, so OVMF finds and boots
# esp\EFI\BOOT\BOOTX64.EFI. Paths are relative: QEMU runs inside the phase folder.
#
# -FirmwareDir: boot our own firmware build (OVMF_CODE.fd + OVMF_VARS.fd in
# that folder) instead of QEMU's, with SMM emulation on. Used by Phase 4.
function Get-QemuArgs([string]$Dir, [string]$FirmwareDir) {
    $machine = 'q35'
    $extra = @()
    if ($FirmwareDir) {
        $ovmf = @{ Code = (Join-Path $FirmwareDir 'OVMF_CODE.fd'); Vars = (Join-Path $FirmwareDir 'OVMF_VARS.fd') }
        if (-not (Test-Path $ovmf.Code)) { throw "Firmware not found: $($ovmf.Code)" }
        $machine = 'q35,smm=on'                                    # emulate SMM + SMRAM
        $extra = @('-global', 'driver=cfi.pflash01,property=secure,value=on',   # only SMM may write flash
                   '-global', 'ICH9-LPC.disable_s3=1')
    } else {
        $ovmf = Get-Ovmf
        if (-not $ovmf) { throw "OVMF firmware (edk2-x86_64-code.fd) not found next to QEMU. Set LAB_OVMF_CODE and LAB_OVMF_VARS." }
    }
    $vars = Join-Path $Dir 'ovmf_vars.fd'
    if (-not (Test-Path $vars)) { Copy-Item $ovmf.Vars $vars }   # writable copy of NVRAM
    return @('-machine', $machine, '-m', '256M', '-net', 'none') + $extra +
           @('-drive', "if=pflash,format=raw,unit=0,readonly=on,file=$($ovmf.Code)",
             '-drive', 'if=pflash,format=raw,unit=1,file=ovmf_vars.fd',
             '-drive', 'format=raw,file=fat:rw:esp')
}

# Interactive run. -Headless puts the firmware console in this terminal
# (quit QEMU with Ctrl+A then X).
function Start-LabVm([string]$Dir, [string]$FirmwareDir, [switch]$Headless) {
    Assert-Emulator
    $qemuArgs = Get-QemuArgs $Dir $FirmwareDir
    if ($Headless) { $qemuArgs += '-nographic' }
    Push-Location $Dir
    try { & (Get-Qemu) @qemuArgs } finally { Pop-Location }
}

# Unattended run: no window, firmware console captured to serial.log, then
# check that the log contains every expected line. Returns $true or $false.
function Test-LabVm([string]$Dir, [string[]]$Expect, [int]$TimeoutSec = 120, [string]$FirmwareDir) {
    Assert-Emulator
    $log = Join-Path $Dir 'serial.log'
    if (Test-Path $log) { Remove-Item $log }
    $qemuArgs = (Get-QemuArgs $Dir $FirmwareDir) + @('-display', 'none', '-serial', 'file:serial.log')
    $argLine = ($qemuArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath (Get-Qemu) -ArgumentList $argLine -WorkingDirectory $Dir `
                       -PassThru -NoNewWindow
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $p.Kill()
        Write-Host "  timed out after $TimeoutSec s" -ForegroundColor Yellow
    }
    $text = ''
    if (Test-Path $log) { $text = Get-Content $log -Raw }
    $ok = $true
    foreach ($e in $Expect) {
        if ($text -and $text.Contains($e)) { Write-Host "  [pass] $e" -ForegroundColor Green }
        else { Write-Host "  [FAIL] missing: $e" -ForegroundColor Red; $ok = $false }
    }
    return $ok
}

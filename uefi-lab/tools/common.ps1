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
# ld.lld links the Phase 5 kernel (an ELF file). It ships with LLVM, next to clang.
function Get-LdLld {
    $clang = Get-Clang
    if (-not $clang) { return $null }
    $ld = Join-Path (Split-Path $clang) 'ld.lld.exe'
    if (Test-Path $ld) { return $ld }
    Find-Exe 'ld.lld.exe' @()
}
# LLDB (Phase 8) ships with LLVM, but on Windows liblldb.dll is linked
# against ONE specific Python DLL (python3XX.dll). If that DLL isn't on PATH,
# lldb.exe can't even start. This finds out which one it needs.
function Get-LldbPythonDll {
    $bin = Split-Path (Get-Clang)
    $readobj = Join-Path $bin 'llvm-readobj.exe'
    $lib = Join-Path $bin 'liblldb.dll'
    if (-not (Test-Path $readobj) -or -not (Test-Path $lib)) { return $null }
    $m = & $readobj --coff-imports $lib | Select-String -Pattern 'Name: (python3\d+\.dll)' | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return $null
}

function Test-LldbRuns([string]$Lldb) {
    try { $null = & $Lldb --version; return ($LASTEXITCODE -eq 0) } catch { return $false }
}

# Returns the path to a WORKING lldb.exe, fixing PATH for this session if the
# Python it needs is installed somewhere standard. Returns $null (and explains
# why) if it can't be made to work.
function Get-Lldb([switch]$Quiet) {
    $clang = Get-Clang
    if (-not $clang) { return $null }
    $lldb = Join-Path (Split-Path $clang) 'lldb.exe'
    if (-not (Test-Path $lldb)) { if (-not $Quiet) { Write-Host "lldb.exe is not in your LLVM install." }; return $null }

    if (Test-LldbRuns $lldb) { return $lldb }

    $dll = Get-LldbPythonDll
    if (-not $dll) { if (-not $Quiet) { Write-Host "lldb.exe does not start, and no Python dependency was found." }; return $null }
    $digits = $dll -replace '^python3(\d+)\.dll$', '$1'          # python311.dll -> 11
    $roots = @("$env:LOCALAPPDATA\Programs\Python\Python3$digits",
               "$env:ProgramFiles\Python3$digits",
               "${env:ProgramFiles(x86)}\Python3$digits")
    $toolcache = "C:\hostedtoolcache\windows\Python"
    if (Test-Path $toolcache) {
        $roots += Get-ChildItem $toolcache -Directory -Filter "3.$digits.*" -ErrorAction SilentlyContinue |
                  ForEach-Object { Join-Path $_.FullName 'x64' }
    }
    foreach ($r in $roots) {
        if ($r -and (Test-Path (Join-Path $r $dll))) {
            $env:PATH = "$r;$env:PATH"
            if (Test-LldbRuns $lldb) {
                if (-not $Quiet) { Write-Host "lldb needs $dll; using the one in $r" }
                return $lldb
            }
        }
    }
    if (-not $Quiet) {
        Write-Host "lldb.exe needs $dll (Python 3.$digits), which isn't installed." -ForegroundColor Yellow
        Write-Host "Run setup.cmd again, or install it with: winget install Python.Python.3.$digits" -ForegroundColor Yellow
    }
    return $null
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
function Start-LabVm([string]$Dir, [string]$FirmwareDir, [switch]$Headless, [string[]]$ExtraArgs = @()) {
    Assert-Emulator
    $qemuArgs = (Get-QemuArgs $Dir $FirmwareDir) + $ExtraArgs
    if ($Headless) { $qemuArgs += '-nographic' }
    Push-Location $Dir
    try { & (Get-Qemu) @qemuArgs } finally { Pop-Location }
}

# Unattended run: no window, firmware console captured to serial.log, then
# check that the log contains every expected line. Returns $true or $false.
# -StopWhenFound ends QEMU as soon as every expected line is in the log (for
# tests that deliberately halt the machine). -FreshVars starts from a clean
# copy of the variable store.
function Test-LabVm([string]$Dir, [string[]]$Expect, [int]$TimeoutSec = 120, [string]$FirmwareDir,
                    [string[]]$ExtraArgs = @(), [switch]$StopWhenFound, [switch]$FreshVars) {
    Assert-Emulator
    $log = Join-Path $Dir 'serial.log'
    if (Test-Path $log) { Remove-Item $log }
    if ($FreshVars -and (Test-Path (Join-Path $Dir 'ovmf_vars.fd'))) { Remove-Item (Join-Path $Dir 'ovmf_vars.fd') }
    $qemuArgs = (Get-QemuArgs $Dir $FirmwareDir) + @('-display', 'none', '-serial', 'file:serial.log') + $ExtraArgs
    $argLine = ($qemuArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $p = Start-Process -FilePath (Get-Qemu) -ArgumentList $argLine -WorkingDirectory $Dir `
                       -PassThru -NoNewWindow

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $exited = $false
    while ((Get-Date) -lt $deadline) {
        if ($p.WaitForExit(500)) { $exited = $true; break }
        if ($StopWhenFound -and (Test-Path $log)) {
            $now = Get-Content $log -Raw -ErrorAction SilentlyContinue
            if ($now) {
                $all = $true
                foreach ($e in $Expect) { if (-not $now.Contains($e)) { $all = $false; break } }
                if ($all) { break }
            }
        }
    }
    if (-not $exited -and -not $p.HasExited) {
        $p.Kill()
        if (-not $StopWhenFound) { Write-Host "  timed out after $TimeoutSec s" -ForegroundColor Yellow }
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

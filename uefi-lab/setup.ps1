# setup.ps1 - install the UEFI lab toolchain on Windows 11 with winget.
# Installs LLVM (clang + lld-link) and QEMU (which includes OVMF firmware).
# Each installer asks for admin rights: accept the UAC prompt.
. "$PSScriptRoot\tools\common.ps1"

Write-Host "UEFI lab setup for Windows 11`n"

$packages = @(
    @{ Id = 'LLVM.LLVM';                       Name = 'LLVM / Clang'; Find = { Get-Clang } },
    @{ Id = 'SoftwareFreedomConservancy.QEMU'; Name = 'QEMU';         Find = { Get-Qemu } }
)

foreach ($pkg in $packages) {
    $found = & $pkg.Find
    if ($found) { Write-Host "[ok] $($pkg.Name): $found"; continue }

    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "$($pkg.Name) is missing and winget was not found. Install or update 'App Installer' from the Microsoft Store, then run setup.cmd again."
    }
    Write-Host "Installing $($pkg.Name) with winget. Accept the UAC prompt..." -ForegroundColor Cyan
    winget install --id $pkg.Id --exact --accept-package-agreements --accept-source-agreements
    if ($LASTEXITCODE -ne 0) { throw "winget could not install $($pkg.Id) (exit code $LASTEXITCODE)." }

    $found = & $pkg.Find
    if (-not $found) { throw "$($pkg.Name) installed but was not found. Close this window, open a new one and run setup.cmd again." }
    Write-Host "[ok] $($pkg.Name): $found"
}

$ovmf = Get-Ovmf
if (-not $ovmf) { throw "QEMU is installed but its OVMF firmware (share\edk2-x86_64-code.fd) is missing." }
Write-Host "[ok] OVMF firmware: $($ovmf.Code)"

# Phase 8 debugs SMM with LLDB, which needs one specific Python version on Windows.
if (Get-Lldb -Quiet) {
    Write-Host "[ok] LLDB debugger works"
} else {
    $dll = Get-LldbPythonDll
    if ($dll -and (Get-Command winget -ErrorAction SilentlyContinue)) {
        $digits = $dll -replace '^python3(\d+)\.dll$', '$1'
        Write-Host "LLDB needs Python 3.$digits ($dll). Installing it with winget. Accept the UAC prompt..." -ForegroundColor Cyan
        winget install --id "Python.Python.3.$digits" --exact --accept-package-agreements --accept-source-agreements
        if (Get-Lldb) { Write-Host "[ok] LLDB debugger works" }
        else { Write-Host "[warn] LLDB still can't start; only Phase 8 needs it. Open a new terminal and retry." -ForegroundColor Yellow }
    } else {
        Write-Host "[warn] LLDB can't start; only Phase 8 needs it." -ForegroundColor Yellow
        Get-Lldb | Out-Null
    }
}

Write-Host "`nToolchain ready. Next, run selftest.cmd to build and boot every phase." -ForegroundColor Green

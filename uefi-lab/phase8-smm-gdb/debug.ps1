# debug.ps1 - debug our ring -2 handler with LLDB (ships with LLVM) on Windows.
#
#   debug.cmd                    interactive: stops in LabGuidHandler, gives you the (lldb) prompt
#   debug.cmd -Demo              scripted tour, then exits; returns PASS/FAIL (used by the self-test)
#   debug.cmd -Standalone        use the Standalone MM firmware (default: Traditional)
#
# QEMU runs with its GDB server on 127.0.0.1:1234. The app prints where the
# handler is loaded (DEBUG-HANDLER-ADDRESS=...) and waits 15 s. We subtract
# LabGuidHandler's offset in the symbol file (llvm-nm) to get the load base,
# load the symbols there, break on the handler and let the SMI happen.
param([switch]$Demo, [switch]$Standalone)
. "$PSScriptRoot\..\tools\common.ps1"
Assert-Emulator

$fw  = Join-Path $PSScriptRoot '..\phase4-smm\firmware\bin'
$sym = 'LabSmmHandler.debug'
if ($Standalone) { $fw = Join-Path $PSScriptRoot '..\phase4-smm\firmware\bin-standalone'; $sym = 'LabMmHandlerStandalone.debug' }
$fw = (Resolve-Path $fw).Path
$symPath = Join-Path $fw "symbols\$sym"

$llvmBin = Split-Path (Get-Clang)
$lldb = Join-Path $llvmBin 'lldb.exe'
$nm   = Join-Path $llvmBin 'llvm-nm.exe'
if (-not (Test-Path $lldb)) { throw "lldb.exe not found in $llvmBin. It ships with LLVM (winget install LLVM.LLVM)." }

if (-not (Test-Path "$PSScriptRoot\esp\EFI\BOOT\BOOTX64.EFI")) { & "$PSScriptRoot\build.ps1" }

# Offset of LabGuidHandler inside the symbol file.
$nmLine = & $nm $symPath | Where-Object { $_ -match '\sLabGuidHandler$' } | Select-Object -First 1
if (-not $nmLine) { throw "LabGuidHandler not found in $symPath" }
$offset = [Convert]::ToUInt64(($nmLine -split '\s+')[0], 16)

# Start QEMU with the GDB server. Fresh variable store, serial to a file.
$vars = Join-Path $PSScriptRoot 'ovmf_vars.fd'
$log  = Join-Path $PSScriptRoot 'serial.log'
if (Test-Path $vars) { Remove-Item $vars }
if (Test-Path $log)  { Remove-Item $log }
$extra = @('-global', 'mch.extended-tseg-mbytes=32', '-serial', 'file:serial.log', '-gdb', 'tcp:127.0.0.1:1234')
if ($Demo) { $extra += @('-display', 'none') }
$qemuArgs = (Get-QemuArgs $PSScriptRoot $fw) + $extra
$argLine = ($qemuArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
$qemu = Start-Process -FilePath (Get-Qemu) -ArgumentList $argLine -WorkingDirectory $PSScriptRoot -PassThru -NoNewWindow

$failed = $false
try {
    # Wait for the app to report the handler address.
    $addr = $null
    for ($i = 0; $i -lt 600 -and -not $addr; $i++) {
        Start-Sleep -Milliseconds 200
        if (Test-Path $log) {
            $m = Select-String -Path $log -Pattern 'DEBUG-HANDLER-ADDRESS=0x([0-9A-F]+)' | Select-Object -First 1
            if ($m) { $addr = [Convert]::ToUInt64($m.Matches[0].Groups[1].Value, 16) }
        }
    }
    if (-not $addr) { throw "The app never printed the handler address (see serial.log)." }
    $base = $addr - $offset
    $addrHex = '0x{0:X}' -f $addr
    $baseHex = '0x{0:X}' -f $base
    Write-Host "LabGuidHandler is at $addrHex; symbols load at $baseHex ($sym)"

    $cmds = @(
        "target create --arch x86_64 `"$symPath`"",
        "target modules load --file $sym --slide $baseHex",
        'gdb-remote 127.0.0.1:1234'
    )
    if ($Demo) { $cmds += "memory read --size 1 --format x --count 8 $addrHex" }   # ring 0 view
    $cmds += @("breakpoint set --address $addrHex", 'process continue')
    if ($Demo) {
        $cmds += @(
            "memory read --size 1 --format x --count 8 $addrHex",                     # SMM view
            'expression -f x -- ((LAB_SMM_MSG *)CommBuffer)->Arg',
            'breakpoint delete --force',
            'breakpoint set --name ProcessMessage',
            'process continue',
            'frame variable Msg',
            'thread step-over',
            'breakpoint delete --force',
            'process detach')
    }
    $cmdFile = Join-Path $env:TEMP 'lab-smm.lldb'
    Set-Content -Path $cmdFile -Value $cmds -Encoding Ascii

    if (-not $Demo) {
        Write-Host "`nStopping in LabGuidHandler, inside SMM. Try: bt, register read rip, frame variable,"
        Write-Host "memory read `$pc, thread step-over, process continue. Quit with: process detach, then quit.`n"
        & $lldb -s $cmdFile
        return
    }

    $out = & $lldb -b -s $cmdFile | Out-String
    Write-Host $out
    $checks = [ordered]@{
        'ring 0 sees FF'               = "0xff 0xff 0xff 0xff"
        'SMM sees real code'           = "0xf3 0x0f 0x1e 0xfa"
        'stopped in LabGuidHandler'    = "LabGuidHandler at LabSmmHandler.c"
        'request marker visible'       = "0x000000001234abcd"
        'stepped in ProcessMessage'    = "ProcessMessage(Msg="
    }
    $ok = $true
    foreach ($k in $checks.Keys) {
        if ($out.Contains($checks[$k])) { Write-Host "  [pass] $k" -ForegroundColor Green }
        else { Write-Host "  [FAIL] $k (missing: $($checks[$k]))" -ForegroundColor Red; $ok = $false }
    }
    if (-not $ok) { $failed = $true }
} finally {
    if (-not $qemu.WaitForExit(30000)) { $qemu.Kill() }
}
if ($failed) { exit 1 }

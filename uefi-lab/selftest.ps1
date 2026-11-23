# selftest.ps1 - build every phase and boot each one in QEMU without a window,
# checking the firmware console output. Takes about a minute.
. "$PSScriptRoot\tools\common.ps1"

$results = [ordered]@{}

Write-Host "`n== Phase 2: hello world ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase2-hello\build.ps1"
$results['phase2-hello'] = Test-LabVm "$PSScriptRoot\phase2-hello" @(
    'Hello from bare metal',
    'Memory map:',
    'Conventional:')

Write-Host "`n== Phase 3: driver + protocols + handoff ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase3-driver\build.ps1"
$results['phase3-driver'] = Test-LabVm "$PSScriptRoot\phase3-driver" @(
    'LAB_GREETER before load : not found',
    'installed LAB_GREETER_PROTOCOL',
    'Hello, ring 3 world!',
    'ExitBootServices signalled',
    'ExitBootServices succeeded',
    'RuntimeServices->GetTime()')

Write-Host "`n== Phase 4: ring -2 (SMM) on our own firmware ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase4-smm\build.ps1"
$results['phase4-smm'] = Test-LabVm "$PSScriptRoot\phase4-smm" @(
    '[SMM] LabSmmHandler loaded into SMRAM',
    'LockState = locked',
    'accepted by SMM',
    'FF FF FF FF FF FF FF FF',
    'real x86 machine code',
    'status REJECTED',
    '[SMM] ring -2 here: software SMI 0x5A',
    'SMM answered') -TimeoutSec 180 -FirmwareDir "$PSScriptRoot\phase4-smm\firmware\bin"

Write-Host "`n== Phase 5: our bootloader + our kernel ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase5-kernel\build.ps1"
# isa-debug-exit lets the kernel end the VM after its 3-second boot check.
$results['phase5-kernel'] = Test-LabVm "$PSScriptRoot\phase5-kernel" @(
    'ExitBootServices and jumping to the kernel',
    '[kernel] hello from our own kernel',
    '[kernel] page tables loaded',
    'drawing works',
    'exception 3 (BREAKPOINT)',
    '[kernel] interrupts on',
    '[kernel] uptime 3 s',
    '[kernel] boot checks complete') -ExtraArgs @('-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04')

Write-Host "`n== Summary ==" -ForegroundColor Cyan
$allOk = $true
foreach ($k in $results.Keys) {
    if ($results[$k]) { Write-Host "  PASS  $k" -ForegroundColor Green }
    else { Write-Host "  FAIL  $k  (see $k\serial.log)" -ForegroundColor Red; $allOk = $false }
}
if (-not $allOk) { exit 1 }

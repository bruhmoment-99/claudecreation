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

Write-Host "`n== Summary ==" -ForegroundColor Cyan
$allOk = $true
foreach ($k in $results.Keys) {
    if ($results[$k]) { Write-Host "  PASS  $k" -ForegroundColor Green }
    else { Write-Host "  FAIL  $k  (see $k\serial.log)" -ForegroundColor Red; $allOk = $false }
}
if (-not $allOk) { exit 1 }

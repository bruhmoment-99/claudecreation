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

Write-Host "`n== Phase 6: SMM guards the flash ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase6-smm-flash\build.ps1"
$results['phase6-smm-flash'] = Test-LabVm "$PSScriptRoot\phase6-smm-flash" @(
    '[SMM] LabBootCounter is now read-only outside SMM',
    '"_FVH" found',
    'DROPPED: outside SMM the chipset ignores flash writes',
    'EFI_WRITE_PROTECTED: SMM refused an untrusted caller',
    'SMM wrote it to flash: LabBootCounter =',
    '- stored') -TimeoutSec 180 -FirmwareDir "$PSScriptRoot\phase4-smm\firmware\bin"

Write-Host "`n== Phase 7: Standalone MM ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase7-standalone-mm\build.ps1"
# Standalone MM keeps its whole heap in TSEG; 32 MiB gives the variable driver room.
$results['phase7-standalone-mm'] = Test-LabVm "$PSScriptRoot\phase7-standalone-mm" @(
    '[MM standalone] LabMmHandler (Standalone MM) loaded into MMRAM',
    'model: STANDALONE MM',
    'MM core copies it INTO MMRAM',
    'PING through it: OK',
    'model=STANDALONE, MMRAM pointer REJECTED, ordinary RAM REJECTED',
    '(written by MM)') -TimeoutSec 180 -FirmwareDir "$PSScriptRoot\phase4-smm\firmware\bin-standalone" `
    -ExtraArgs @('-global', 'mch.extended-tseg-mbytes=32')

Write-Host "`n== Phase 8: debugging the SMM handler with LLDB ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase8-smm-gdb\build.ps1"
& "$PSScriptRoot\phase8-smm-gdb\debug.ps1" -Demo
$results['phase8-smm-gdb'] = ($LASTEXITCODE -eq 0)

Write-Host "`n== Phase 9: how SMM protects itself ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase9-smm-selfprotect\build.ps1"
$p9 = "$PSScriptRoot\phase9-smm-selfprotect"
$tseg = @('-global', 'mch.extended-tseg-mbytes=32')
$p9marker = "$p9\esp\EFI\lab\write-code-test"
if (Test-Path $p9marker) { Remove-Item $p9marker }
$a = Test-LabVm $p9 @('Traditional MM)', 'handler code   : READ-ONLY executable', 'handler data   : writable  NO-EXECUTE',
    'ring 0 page    : writable  NO-EXECUTE', 'Summary: SMM self-protection ON') `
    -FirmwareDir "$PSScriptRoot\phase4-smm\firmware\bin" -ExtraArgs $tseg -FreshVars
$b = Test-LabVm $p9 @('Standalone MM)', 'handler code   : READ-ONLY executable', 'ring 0 page    : NOT MAPPED',
    'Summary: SMM self-protection ON') `
    -FirmwareDir "$PSScriptRoot\phase4-smm\firmware\bin-standalone" -ExtraArgs $tseg -FreshVars
# The live test halts the machine on purpose: stop as soon as the fault is reported.
New-Item -ItemType Directory -Force (Split-Path $p9marker) | Out-Null
Set-Content -Path $p9marker -Value 'run the live write test'
$c = Test-LabVm $p9 @('about to write to my own code page', '!!!! X64 Exception Type - 0E(#PF - Page-Fault)', 'W:1 P:1') `
    -FirmwareDir "$PSScriptRoot\phase4-smm\firmware\bin" -ExtraArgs $tseg -FreshVars -StopWhenFound -TimeoutSec 120
Remove-Item $p9marker
$results['phase9-smm-selfprotect'] = $a -and $b -and $c

Write-Host "`n== Phase 10: Secure Boot ==" -ForegroundColor Cyan
& "$PSScriptRoot\phase10-secure-boot\build.ps1"
$p10 = "$PSScriptRoot\phase10-secure-boot"
$fw10 = "$PSScriptRoot\phase4-smm\firmware\bin"
# Boot 1 from a clean flash: enroll keys, then test.
$a = Test-LabVm $p10 @('SetupMode = 1', 'untrusted.efi: ALLOWED', 'write PK : EFI_SUCCESS', 'SecureBoot = 1',
    'trusted.efi           (hash in db): ALLOWED', 'untrusted.efi         (not in db): REFUSED',
    '1 bit flipped     : REFUSED', 'add untrusted.efi''s hash to db (unsigned): EFI_SECURITY_VIOLATION',
    '-> Secure Boot works') -FirmwareDir $fw10 -FreshVars
# Boot 2 on the same flash: the firmware must check THIS app first.
$b = Test-LabVm $p10 @("The firmware checked THIS app's hash against db", '-> Secure Boot works') -FirmwareDir $fw10
$results['phase10-secure-boot'] = $a -and $b

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

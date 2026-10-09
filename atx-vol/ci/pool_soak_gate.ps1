<#
.SYNOPSIS
  CI gate (Task E3, sprint Step 2): the C2 8-thread SnapshotPool concurrency soak.

.DESCRIPTION
  `BacktestExec.SnapshotPoolConcurrentRunsMatchSerial` (atx-vol/tests/
  backtest_exec_test.cpp) races kThreads=8 concurrent `run_backtest` calls
  sharing one `SnapshotPool`, repeated kPoolSoakRepeats=20 times (a fresh pool
  per repeat), and asserts every concurrent run is bit-identical to the serial
  baseline plus exact pool-stats invariants (archive_opens/identity_probes/
  resident_entries == 6 every repeat, per invariants I1-I8). The test file's
  own comment already sizes 20 repeats as "what a per-commit gate can afford"
  (~1.8s) vs the fuller ~100-repeat soak (~8-9s) for deliberate investigation
  -- so the existing 20-repeat test IS this gate; no new C++ needed. This
  script is the re-runnable CI wrapper.

.PARAMETER Preset
  Build preset. Default 'dev' (Debug) -- this is a race/correctness soak, not
  a perf claim.
#>
param(
  [string] $Preset = "dev"
)
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Filter = "BacktestExec.SnapshotPoolConcurrentRunsMatchSerial"

Write-Host "[pool-soak-gate] building atx-vol-tests (-Preset $Preset)..." -ForegroundColor Cyan
& powershell.exe -NoProfile -File (Join-Path $RepoRoot "scripts\atx-build.ps1") build -Preset $Preset atx-vol-tests
if ($LASTEXITCODE -ne 0) {
  Write-Host "[pool-soak-gate] FAIL: build failed (exit $LASTEXITCODE)." -ForegroundColor Red
  exit 1
}

. (Join-Path $PSScriptRoot "_common.ps1")
$binaryDir = Get-PresetBinaryDir -RepoRoot $RepoRoot -Name $Preset
$exe = Join-Path $RepoRoot "$binaryDir\bin\atx-vol-tests.exe"
if (-not (Test-Path $exe)) {
  Write-Host "[pool-soak-gate] FAIL: binary not found at $exe." -ForegroundColor Red
  exit 1
}

Write-Host "[pool-soak-gate] running: --gtest_filter=$Filter (8 threads x 20 repeats)" -ForegroundColor Cyan
$output = & $exe "--gtest_filter=$Filter" 2>&1
$output | ForEach-Object { Write-Host "  $_" }
$exitCode = $LASTEXITCODE

$ran = [regex]::Match(([string]::Join("`n", $output)), '\[==========\]\s+(\d+)\s+tests? from')
$nRan = if ($ran.Success) { [int]$ran.Groups[1].Value } else { 0 }

if ($exitCode -ne 0) {
  Write-Host "[pool-soak-gate] FAIL: gtest exited $exitCode." -ForegroundColor Red
  exit 1
}
if ($nRan -ne 1) {
  Write-Host "[pool-soak-gate] FAIL: expected exactly 1 test to match the filter, got $nRan -- test renamed/removed?" -ForegroundColor Red
  exit 1
}

Write-Host "[pool-soak-gate] PASS: 8-thread SnapshotPool soak green (20 repeats, bit-identical to serial)." -ForegroundColor Green
Write-Host "[pool-soak-gate] NOTE: for a fuller soak (~100 repeats) during a specific race investigation, raise" -ForegroundColor DarkGray
Write-Host "  kPoolSoakRepeats in backtest_exec_test.cpp locally and rerun -- this per-commit gate deliberately" -ForegroundColor DarkGray
Write-Host "  stays at the fast 20-repeat budget (see that constant's own comment)." -ForegroundColor DarkGray
exit 0

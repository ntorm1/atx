<#
.SYNOPSIS
  CI gate (Task E3, sprint Step 2): n_threads 1-vs-N determinism, memcmp'd.

.DESCRIPTION
  Determinism invariants I1-I8 (docs/superpowers/specs/2026-07-21-atx-vol-
  backtest-review.md Sec 6) require the backtest engine to produce
  bit-identical results across n_threads and prefetch depth. This does not
  need new C++: `atx-vol/tests/backtest_exec_test.cpp` already carries three
  gtest cases that run the SAME strategy/corpus through `run_backtest` at
  n_threads=1 and a larger thread count and compare every result column via
  `bits_equal` (memcpy the double into a uint64_t and compare -- the exact
  memcmp-on-the-bit-pattern semantics the sprint plan's "n_threads 1 vs N
  memcmp" line asks for):

    - BacktestExec.HeldToExpiryDailyCohortsComposeAtScale   (1 vs 4 threads)
    - BacktestExec.StrategyLoopHedgeAndCohorts_Deterministic (1 vs N threads,
      B3+B4 composed: hedge ledger + overlapping cohorts)
    - BacktestExec.FixedBookComposedSubsetAndSettlement_Deterministic
      (1 vs N threads, B1+B2 composed: subset dedup + batched settlement)

  This script is a thin, re-runnable CI wrapper: build atx-vol-tests, run
  exactly this targeted filter (never the full suite -- controller
  directive), and fail loudly and non-zero if anything but a clean PASS comes
  back (including zero tests matched, which would mean the filter rotted).

.PARAMETER Preset
  Build preset. Default 'dev' (Debug) -- determinism is a correctness
  invariant checked on dev/rel, not a perf claim needing rel-avx2.
#>
param(
  [string] $Preset = "dev"
)
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Filter = "BacktestExec.HeldToExpiryDailyCohortsComposeAtScale:" +
          "BacktestExec.StrategyLoopHedgeAndCohorts_Deterministic:" +
          "BacktestExec.FixedBookComposedSubsetAndSettlement_Deterministic"

Write-Host "[determinism-gate] building atx-vol-tests (-Preset $Preset)..." -ForegroundColor Cyan
& powershell.exe -NoProfile -File (Join-Path $RepoRoot "scripts\atx-build.ps1") build -Preset $Preset atx-vol-tests
if ($LASTEXITCODE -ne 0) {
  Write-Host "[determinism-gate] FAIL: build failed (exit $LASTEXITCODE)." -ForegroundColor Red
  exit 1
}

. (Join-Path $PSScriptRoot "_common.ps1")
$binaryDir = Get-PresetBinaryDir -RepoRoot $RepoRoot -Name $Preset
$exe = Join-Path $RepoRoot "$binaryDir\bin\atx-vol-tests.exe"
if (-not (Test-Path $exe)) {
  Write-Host "[determinism-gate] FAIL: binary not found at $exe." -ForegroundColor Red
  exit 1
}

Write-Host "[determinism-gate] running: --gtest_filter=$Filter" -ForegroundColor Cyan
$output = & $exe "--gtest_filter=$Filter" 2>&1
$output | ForEach-Object { Write-Host "  $_" }
$exitCode = $LASTEXITCODE

$ran = [regex]::Match(([string]::Join("`n", $output)), '\[==========\]\s+(\d+)\s+tests? from')
$nRan = if ($ran.Success) { [int]$ran.Groups[1].Value } else { 0 }

if ($exitCode -ne 0) {
  Write-Host "[determinism-gate] FAIL: gtest exited $exitCode." -ForegroundColor Red
  exit 1
}
if ($nRan -ne 3) {
  # Fail-closed on a rotted filter (e.g. a test renamed/removed) rather than
  # silently passing on 0 or 1 matched tests.
  Write-Host "[determinism-gate] FAIL: expected exactly 3 tests to match the filter, got $nRan -- filter or test names rotted?" -ForegroundColor Red
  exit 1
}

Write-Host "[determinism-gate] PASS: $nRan/3 determinism gates green (n_threads 1 vs N, bit-identical)." -ForegroundColor Green
exit 0

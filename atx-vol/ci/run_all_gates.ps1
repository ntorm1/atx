<#
.SYNOPSIS
  Run every atx-vol Task E3 CI gate in sequence and report a summary.
  Non-zero exit if any gate fails.

.DESCRIPTION
  There is no .github/workflows or other CI runner infra in this repo today
  (checked at authoring time: no .github/, no ci/ dir anywhere else, no CI
  config in scripts/). These four gates are therefore implemented as plain,
  locally-invocable PowerShell scripts under atx-vol/ci/ -- runnable by hand
  today, and wireable into whatever CI runner shows up later by simply
  invoking this script (or the four scripts individually) as a build step.

  Gates, in the sprint plan's Step 2 order:
    1. determinism_gate.ps1        -- n_threads 1 vs N, memcmp (bits_equal)
    2. golden_replay_gate.ps1      -- 82-session golden NAV + economics tripwire (D1)
    3. lakehouse_off_link_gate.ps1 -- ATX_VOL_LAKEHOUSE=OFF still links
    4. pool_soak_gate.ps1          -- 8-thread SnapshotPool concurrency soak (C2)

  golden_replay_gate.ps1 fails closed (see its own header) when the real
  82-session SPY corpus is not available -- which is every git checkout
  today, since that corpus is real market data and is never committed. Set
  $env:ATX_VOL_GOLDEN_82_SESSION_CORPUS before running this script if you
  want that gate to actually exercise the tripwire rather than fail closed
  on a missing precondition.
#>
$ErrorActionPreference = "Continue"  # run every gate even if an earlier one fails; report all results

$gates = @(
  @{ Name = "determinism";        Script = "determinism_gate.ps1" },
  @{ Name = "golden-replay";      Script = "golden_replay_gate.ps1" },
  @{ Name = "lakehouse-off-link"; Script = "lakehouse_off_link_gate.ps1" },
  @{ Name = "pool-soak";          Script = "pool_soak_gate.ps1" }
)

$results = @()
foreach ($gate in $gates) {
  Write-Host ""
  Write-Host "==================== $($gate.Name) ====================" -ForegroundColor Magenta
  $scriptPath = Join-Path $PSScriptRoot $gate.Script
  & powershell.exe -NoProfile -File $scriptPath
  $exitCode = $LASTEXITCODE
  $results += [pscustomobject]@{ Gate = $gate.Name; ExitCode = $exitCode; Pass = ($exitCode -eq 0) }
}

Write-Host ""
Write-Host "==================== summary ====================" -ForegroundColor Magenta
$results | ForEach-Object {
  $color = if ($_.Pass) { "Green" } else { "Red" }
  $status = if ($_.Pass) { "PASS" } else { "FAIL (exit $($_.ExitCode))" }
  Write-Host ("  {0,-20} {1}" -f $_.Gate, $status) -ForegroundColor $color
}

$anyFailed = @($results | Where-Object { -not $_.Pass }).Count -gt 0
if ($anyFailed) {
  Write-Host ""
  Write-Host "[run-all-gates] one or more gates FAILED." -ForegroundColor Red
  exit 1
}
Write-Host ""
Write-Host "[run-all-gates] all gates PASSED." -ForegroundColor Green
exit 0

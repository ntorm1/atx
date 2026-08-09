<#
.SYNOPSIS
  CI gate (Task E3): the 82-session golden-replay economics tripwire (D1 Step 5,
  reassigned to E3 per the sprint plan). Fails the build if the corpus is absent
  (FAIL-CLOSED -- never a vacuous pass) or if the measured final_nav diverges
  from the pin in golden_pin.hpp, which would mean either an economics change
  landed without a kBacktestEconomicsRev bump, or an unrelated regression.

.DESCRIPTION
  golden_pin.hpp's `kGolden82SessionFinalNav` / `kGolden82SessionEconomicsRev`
  pair is already enforced at COMPILE time (a static_assert fails the build the
  moment kBacktestEconomicsRev is bumped without updating the paired literal --
  see that header). This script is the RUNTIME half: it actually replays the
  pinned 82-session SPY corpus through `atxvol_spy_dispersion_backtest
  run-surface-backtest` and compares the resulting final_nav bit-for-bit
  against the pin.

  The real 82-session corpus is real market data and is NOT checked into any
  git worktree (see tests/track_key_test.cpp's find_golden_82_session_corpus_root,
  which this script mirrors). This gate therefore has two honest outcomes:

    - Corpus found (env var or a well-known path)  -> run the replay, compare,
      PASS/FAIL on the actual numbers.
    - Corpus absent (every git checkout today)      -> FAIL LOUDLY. This is
      deliberate: a gate that silently no-ops when its precondition is missing
      is worse than no gate at all (sprint controller directive). If you want
      this gate to actually exercise the tripwire on a dev box that happens to
      have the corpus cached locally (e.g. C:/atx-data/spy-dispersion/runs/
      bt-sota-baseline), point -LakeCorpus (or $env:ATX_VOL_GOLDEN_82_SESSION_CORPUS)
      at a directory containing a `run_spec.tsv` in the `run-surface-backtest
      --run DIR` shape.

.PARAMETER LakeCorpus
  Path to a directory in the `run-surface-backtest --run DIR` shape
  (run_spec.tsv + definitions.tsv + universe_schedule.tsv, see
  atx-vol/tools/spy_dispersion_backtest.cpp). Overrides
  $env:ATX_VOL_GOLDEN_82_SESSION_CORPUS and the well-known repo-relative paths.

.PARAMETER Preset
  Build preset for atxvol_spy_dispersion_backtest. Default 'dev' (correctness
  gates run on dev/rel, never rel-avx2 -- this is a NAV-equality check, not a
  perf claim).

.EXAMPLE
  powershell.exe atx-vol/ci/golden_replay_gate.ps1
    # every git checkout today: corpus absent -> FAIL LOUDLY (by design)

.EXAMPLE
  $env:ATX_VOL_GOLDEN_82_SESSION_CORPUS = "C:/atx-data/spy-dispersion/runs/bt-sota-baseline"
  powershell.exe atx-vol/ci/golden_replay_gate.ps1
    # this dev host: corpus present -> real replay, real PASS/FAIL
#>
param(
  [string] $LakeCorpus = "",
  [string] $Preset = "dev"
)
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)  # atx-vol/ci -> atx-vol -> repo root
$GoldenPinHpp = Join-Path $RepoRoot "atx-vol\research\include\atx\vol\research\golden_pin.hpp"

function Fail-Closed([string]$msg) {
  Write-Host ""
  Write-Host "[golden-replay-gate] FAIL (closed): $msg" -ForegroundColor Red
  exit 1
}

if (-not (Test-Path $GoldenPinHpp)) {
  Fail-Closed "golden_pin.hpp not found at $GoldenPinHpp -- cannot read the pin."
}
$pinText = Get-Content $GoldenPinHpp -Raw
$navMatch = [regex]::Match($pinText, 'kGolden82SessionFinalNav\s*=\s*([0-9.eE+-]+)\s*;')
$revMatch = [regex]::Match($pinText, 'kGolden82SessionEconomicsRev\s*=\s*(\d+)\s*;')
if (-not $navMatch.Success -or -not $revMatch.Success) {
  Fail-Closed "could not parse kGolden82SessionFinalNav / kGolden82SessionEconomicsRev out of golden_pin.hpp -- header format changed?"
}
$pinnedNav = [double]::Parse($navMatch.Groups[1].Value, [System.Globalization.CultureInfo]::InvariantCulture)
$pinnedRev = [int]$revMatch.Groups[1].Value
Write-Host "[golden-replay-gate] pin: final_nav=$($navMatch.Groups[1].Value) economics_rev=$pinnedRev" -ForegroundColor Cyan

# ---- resolve the corpus (fail-closed if absent) ----
$candidate = $LakeCorpus
if (-not $candidate -and $env:ATX_VOL_GOLDEN_82_SESSION_CORPUS) {
  $candidate = $env:ATX_VOL_GOLDEN_82_SESSION_CORPUS
}
if (-not $candidate) {
  foreach ($rel in @("data\golden\82-session-spy", "..\data\golden\82-session-spy")) {
    $p = Join-Path $RepoRoot $rel
    if (Test-Path $p) { $candidate = $p; break }
  }
}
if (-not $candidate -or -not (Test-Path $candidate)) {
  Fail-Closed ("golden 82-session SPY corpus not found (checked -LakeCorpus, " +
    "`$env:ATX_VOL_GOLDEN_82_SESSION_CORPUS, and data\golden\82-session-spy under the repo root). " +
    "This corpus is real market data and is never checked into git -- see atx-vol/tests/track_key_test.cpp's " +
    "find_golden_82_session_corpus_root for the same search. Set `$env:ATX_VOL_GOLDEN_82_SESSION_CORPUS to a " +
    "run-surface-backtest-shaped directory (run_spec.tsv + definitions.tsv) to actually exercise this gate. " +
    "Refusing to pass vacuously: the pinned tripwire is final_nav=$($navMatch.Groups[1].Value) " +
    "economics_rev=$pinnedRev, unverified this run.")
}
Write-Host "[golden-replay-gate] corpus: $candidate" -ForegroundColor Cyan
if (-not (Test-Path (Join-Path $candidate "run_spec.tsv"))) {
  Fail-Closed "corpus directory '$candidate' has no run_spec.tsv -- not a run-surface-backtest-shaped directory."
}

# ---- build the CLI ----
Write-Host "[golden-replay-gate] building atxvol_spy_dispersion_backtest (-Preset $Preset)..." -ForegroundColor Cyan
& powershell.exe -NoProfile -File (Join-Path $RepoRoot "scripts\atx-build.ps1") build -Preset $Preset atxvol_spy_dispersion_backtest
if ($LASTEXITCODE -ne 0) { Fail-Closed "build of atxvol_spy_dispersion_backtest failed (exit $LASTEXITCODE)." }

# Resolve the binary from the preset's binaryDir the same way atx-build.ps1 does.
. (Join-Path $PSScriptRoot "_common.ps1")
$binaryDir = Get-PresetBinaryDir -RepoRoot $RepoRoot -Name $Preset
$exe = Join-Path $RepoRoot "$binaryDir\bin\atxvol_spy_dispersion_backtest.exe"
if (-not (Test-Path $exe)) { Fail-Closed "binary not found at $exe after a successful build?" }

# ---- copy the corpus to a scratch dir (NEVER mutate the original -- A3's discipline) ----
$scratch = Join-Path $RepoRoot "scratch-e3-golden-gate\bt-corpus-$([guid]::NewGuid().ToString('N').Substring(0,8))"
New-Item -ItemType Directory -Force -Path (Split-Path $scratch -Parent) | Out-Null
Copy-Item -Recurse -Path $candidate -Destination $scratch
try {
  $runSpec = Join-Path $scratch "run_spec.tsv"
  $specText = Get-Content $runSpec -Raw
  if ($specText -notmatch 'emit_tsv_diagnostics') {
    Add-Content -Path $runSpec -Value "emit_tsv_diagnostics`ttrue" -Encoding ascii
  }
  $tsvOut = Join-Path $scratch "surface_backtest.tsv"
  if (Test-Path $tsvOut) { Remove-Item $tsvOut -Force }

  Write-Host "[golden-replay-gate] running replay..." -ForegroundColor Cyan
  $stdout = & $exe run-surface-backtest --run $scratch 2>&1
  $stdout | ForEach-Object { Write-Host "  $_" }
  if ($LASTEXITCODE -ne 0) { Fail-Closed "run-surface-backtest exited $LASTEXITCODE." }

  if (-not (Test-Path $tsvOut)) {
    Fail-Closed "surface_backtest.tsv was not produced -- emit_tsv_diagnostics did not take effect?"
  }
  $lines = Get-Content $tsvOut
  if ($lines.Count -lt 2) { Fail-Closed "surface_backtest.tsv has no data rows." }
  $header = $lines[0] -split "`t"
  $navCol = [Array]::IndexOf($header, "nav")
  if ($navCol -lt 0) { Fail-Closed "surface_backtest.tsv header has no 'nav' column (schema changed?): $($lines[0])" }
  $lastRow = ($lines[$lines.Count - 1]) -split "`t"
  $measuredNavStr = $lastRow[$navCol]
  $measuredNav = [double]::Parse($measuredNavStr, [System.Globalization.CultureInfo]::InvariantCulture)

  $stdoutJoined = [string]::Join("`n", $stdout)
  $revOut = [regex]::Match($stdoutJoined, 'economics_rev=(\d+)')
  $measuredRev = if ($revOut.Success) { [int]$revOut.Groups[1].Value } else { $null }

  Write-Host ""
  Write-Host "[golden-replay-gate] measured: final_nav=$measuredNavStr economics_rev=$measuredRev" -ForegroundColor Cyan
  Write-Host "[golden-replay-gate] pinned:   final_nav=$($navMatch.Groups[1].Value) economics_rev=$pinnedRev" -ForegroundColor Cyan

  if ($null -ne $measuredRev -and $measuredRev -ne $pinnedRev) {
    # Should be unreachable: golden_pin.hpp's static_assert already ties
    # kGolden82SessionEconomicsRev to kBacktestEconomicsRev at compile time, so a
    # successful build cannot have a rev mismatch. Guarded anyway -- belt and
    # braces beats a silent pass.
    Fail-Closed "measured economics_rev=$measuredRev != pinned economics_rev=$pinnedRev (should be unreachable given golden_pin.hpp's compile-time static_assert -- investigate the build)."
  }

  if ($measuredNav -ne $pinnedNav) {
    Fail-Closed ("economics tripwire fired: measured final_nav=$measuredNavStr does not match the pin " +
      "$($navMatch.Groups[1].Value) at kBacktestEconomicsRev=$pinnedRev. Either economics changed without a " +
      "rev bump (bump kBacktestEconomicsRev, re-measure, re-pin BOTH constants in golden_pin.hpp together, and " +
      "add a CHANGELOG migration note -- never silently), or this is an unrelated regression to find and fix. " +
      "Do not re-pin without understanding which.")
  }

  Write-Host ""
  Write-Host "[golden-replay-gate] PASS: final_nav matches the pin exactly." -ForegroundColor Green
  exit 0
} finally {
  Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}

<#
.SYNOPSIS
  Reproducible CANONICAL-UNIVERSE acceptance run for the High-Quality Alpha Generation sprint
  (docs/superpowers/plans/2026-06-21-high-quality-alpha-generation-sprint.md -- "Acceptance").

  Runs the full pipeline with EVERY sprint flag composed together:
    load -> panel(canonical universe) -> sweep(strict, multi-seed; R1-R4 remediation gates: --typed-fields,
            --reject-price-scale, --dsr-subwindows, --deflate-selection; sweep-wide deflation via R1)
         -> combine(--conviction --capacity-floor --target-aum --walk-forward) -> optimize(daily, --cost-bps)
         -> report(net-of-cost portfolio OOS Sharpe + turnover + breadth N_eff)

  ACCEPTANCE METRICS printed at the end (plan "Acceptance" 1-4):
    1. portfolio_oos_sharpe NET of cost (held-out window, daily schedule) -- POSITIVE is the goal.
    2. oos_turnover (daily) below a tradeable target.
    3. breadth_effective_n materially above naive count-collapse.
    4. admission corrected for search-wide multiple testing (R1 cumulative-N deflation -- automatic in sweep).
    5. determinism intact (default flag-absent paths byte-identical -- proven by the unit suite, not this run).

.PREREQUISITE -- DATA (the run is BLOCKED without this)
  A MULTI-YEAR ORATS history zip (or directory of zips), passed via -OratsZip. The workspace currently has
  only a single-day strikes zip (Downloads/ORATS_SMV_Strikes_20240103.zip) which is NOT enough for discovery
  (one date => no time series). Supply the full history, OR point -PrebuiltPanel at a serialized multi-year
  panel.bin (e.g. the ATX_ALPHA101_PANEL the real-ORATS tests consume).

.EXAMPLE
  pwsh -File scripts/canonical-acceptance-run.ps1 -OratsZip C:\data\orats_history\ -Work work\accept `
       -Seeds 16 -TargetAum 5e8 -CapacityFloor 1e8 -CostBps 10

.EXAMPLE
  # Skip load/panel if you already have a canonical multi-year panel.bin:
  pwsh -File scripts/canonical-acceptance-run.ps1 -PrebuiltPanel work\accept\panel.bin -Work work\accept `
       -Seeds 16 -TargetAum 5e8 -CapacityFloor 1e8 -CostBps 10

.NOTES
  Stage-input flag names are inferred from atx-impl/src/config.cpp; if a stage rejects a flag, confirm with
  `build-rel\bin\atx-impl.exe <stage> --help`. oracle.hpp / search digest are untouched by every flag here
  (all sprint additions are opt-in). The run is exploratory: a non-positive OOS Sharpe is itself a decisive
  finding (the DSL/field set lacks net-of-cost OOS edge -> next sprint expands DATA, not the search).
#>
param(
  [string]$OratsZip = "",                 # raw ORATS history zip or dir (load input). Empty => requires -PrebuiltPanel.
  [string]$PrebuiltPanel = "",            # skip load+panel; use this canonical panel.bin directly.
  [string]$Work = "work\accept",          # working dir for all artifacts.
  [int]$Seeds = 16,                        # multi-seed sweep count (--sweep-runs).
  [double]$TargetAum = 5e8,               # --target-aum ($) for capacity sizing.
  [double]$CapacityFloor = 1e8,           # --capacity-floor ($ AUM; alphas with capacity < floor are faded).
  [double]$CostBps = 10,                  # --cost-bps round-trip transaction cost (net-of-cost charge).
  [double]$RobustHoldoutFrac = 0.5,       # --robust-holdout-frac (W4a weak sub-universe => robust factor live).
  [double]$MaxTurnover = 0.30,            # tightened --max-turnover gate (low-turnover goal).
  [double]$MaxPoolCorr = 0.30,            # tightened --max-pool-corr gate (breadth/diversity).
  [int]$WalkForward = 3,                   # --walk-forward folds (conviction-aware OOS telemetry, T7 NEW-1).
  [double]$RejectPriceScale = 0.30,        # --reject-price-scale (R2): reject a holdout book whose |time-avg corr(w, 1/price)| >= this. (0,1]; 1.0=off.
  [int]$DsrSubwindows = 3                   # --dsr-subwindows (R3): split the sealed holdout into K segments; require min-dsr on EACH. 0/absent=off; >=2 active.
)
$ErrorActionPreference = "Stop"
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$cli = Join-Path $repo "build-rel\bin\atx-impl.exe"
if (-not (Test-Path $cli)) { throw "atx-impl.exe not built. Run: cmake --build build-rel --target atx-impl" }
New-Item -ItemType Directory -Force $Work | Out-Null

$panel = if ($PrebuiltPanel) { $PrebuiltPanel } else { Join-Path $Work "panel.bin" }
$segs  = Join-Path $Work "segs"
$lib   = Join-Path $Work "_library"
$combo = Join-Path $Work "combo.bin"
$books = Join-Path $Work "books.bin"
$rep   = Join-Path $Work "report"

function Stage([string]$name, [scriptblock]$cmd) {
  Write-Host "=== $name ===" -ForegroundColor Cyan
  & $cmd
  if ($LASTEXITCODE -ne 0) { throw "STAGE FAILED: $name (exit $LASTEXITCODE)" }
}

if (-not $PrebuiltPanel) {
  if (-not $OratsZip) { throw "Provide -OratsZip (multi-year ORATS history) or -PrebuiltPanel." }
  # 1. load: raw ORATS zip -> .seg files.
  Stage "load" { & $cli load --zip $OratsZip --out $segs }
  # 2. panel: CANONICAL UNIVERSE -- single-name GICS, close>1, adv20>$25M, rolling, no count cap.
  # NOTE: --require-sector / --compact-universe are valueless in apply_flag but the parse loop consumes the
  # NEXT token as their (ignored) value -- so give them a dummy value and place them LAST, else they eat a real
  # flag. (Pre-existing CLI parse-loop bug: they are not in config.cpp's valueless-flag list at :284.)
  Stage "panel (canonical universe)" {
    & $cli panel --segs $segs --panel-out $panel `
      --min-price 1.0 --min-adv-usd 25000000 --adv-window 20 --top-n-by-adv 0 --require-sector 1 --compact-universe 1
  }
}

# 3. sweep: STRICT robust profile, multi-seed, gated accumulating library, walk-forward OOS, weak universe.
#    Sweep-wide multiple-testing deflation (R1 cumulative trial-count) is AUTOMATIC across the seeds.
#    PIPELINE-REMEDIATION GATES (all opt-in; every default-absent path is byte-identical -- proven by the
#    unit suite, NOT this run; oracle.hpp / F1 search digest untouched):
#      R1 --typed-fields       : exclude binary/low-cardinality/categorical fields from the numeric leaf pool
#                                (kills earnFlag-over-close-style type-confused alphas); raw gics -> group pool.
#      R2 --reject-price-scale : reject a holdout book that is a trivial 1/price (price-scale) dimensional tilt.
#      R3 --dsr-subwindows     : require the deflated-Sharpe bar on EACH of K holdout sub-windows (logical AND)
#                                -- kills a candidate that clears the aggregate bar on single-window luck.
#      R4 --deflate-selection  : the GA optimizes the DEFLATED Sharpe during search (per-generation running-N),
#                                not raw in-sample wq, so selection pressure tracks anti-overfit edge.
#    NOTE: R3 --pbo-hard-block (escalate a run-level CSCV-PBO breach past --max-pbo to a NON-ZERO EXIT) is
#          consumed by the SINGLE-RUN discover stage ONLY; the multi-run sweep does not act on it, so it is
#          intentionally omitted here. Run: discover --gated --max-pbo <p> --pbo-hard-block  to enforce it.
Stage "sweep (strict, multi-seed)" {
  & $cli sweep --panel $panel --library-dir $lib --seed 1 --sweep-runs $Seeds --gated `
    --min-dsr 0.5 --min-split-sharpe 0.0001 --max-pbo 0.5 `
    --oos-fraction 0.25 --oos-windows 3 `
    --typed-fields --reject-price-scale $RejectPriceScale --dsr-subwindows $DsrSubwindows --deflate-selection `
    --robust-holdout-frac $RobustHoldoutFrac --max-turnover $MaxTurnover --max-pool-corr $MaxPoolCorr
}

# 4. combine: conviction-weighted + real capacity at AUM + conviction-aware walk-forward telemetry.
Stage "combine (conviction + capacity)" {
  & $cli combine --panel $panel --library-dir $lib --combo-out $combo `
    --conviction --capacity-floor $CapacityFloor --target-aum $TargetAum --walk-forward $WalkForward --holdout-frac 0.25
}

# 5. optimize: DAILY rebalance + net-of-cost transaction charge.
Stage "optimize (daily, net-of-cost)" {
  & $cli optimize --panel $panel --combo $combo --books-out $books --rebalance daily --cost-bps $CostBps
}

# 6. report: net-of-cost portfolio OOS Sharpe + turnover + breadth.
Stage "report" { & $cli report --panel $panel --combo $combo --books $books --report-out $rep }

# ---- ACCEPTANCE METRICS ----
Write-Host "`n===== ACCEPTANCE METRICS =====" -ForegroundColor Green
$summary = Join-Path $rep "summary.txt"
if (Test-Path $summary) {
  Select-String -Path $summary -Pattern "portfolio_oos_sharpe|portfolio_sharpe|total_pnl_cost|total_pnl_net|oos_turnover|breadth_effective_n|breadth_implied_ic|walk_forward_oos_sharpe|capacity_min_alpha_aum|capacity_max_participation" |
    ForEach-Object { $_.Line }
} else {
  Write-Host "No summary.txt at $summary -- inspect $rep" -ForegroundColor Yellow
}
Write-Host "`nGoal: portfolio_oos_sharpe (NET) > 0 on the held-out daily window; oos_turnover low; breadth_effective_n high." -ForegroundColor Green

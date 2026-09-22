# atx-impl equity research book

`atx-impl` assembles the engine's signal VM, position transforms, portfolio
construction and daily cash/holdings replay into reproducible equity experiments.
The first fixed book is available through `equity-baseline`.

```powershell
build-equity/bin/atx-impl.exe equity-baseline `
  --panel C:/atx/data/tickerhistory_training_native_20260919/context.bin `
  --out C:/atx/data/equity_baseline_training_2013_20260919 `
  --evaluation-start 2013-04-04 --evaluation-end 2014-01-01 `
  --max-working-bytes 3000000000 --report-aum 100000000 `
  --replay-execution-delay 1 --replay-trade-bps 5 `
  --replay-annual-borrow-bps 365 --replay-day-basis 365
```

Choose a new output directory for each attempt. The end date is exclusive. The
input must be an identified, unaugmented TickerHistory context using the frozen
liquidity recipe: top 1,000 names, raw close above $5, 21-observation raw-dollar
ADV of at least $20m, and no sector requirement. Context preparation and native
construction are documented in the [platform progress record](../atx-engine/docs/PLATFORM_PROGRESS.md).

The command evaluates two fixed skipped-month momentum expressions with 126- and
252-observation lookbacks and five-observation smoothing. It retains observed
history before a name becomes eligible, requires both signals to be ready, and
excludes the 256-observation warmup from decisions and scored returns. Ranked
constituent positions are blended equally and shaped every five observations
toward gross 1 with a 1% name cap. These are fixed weights; a reported boundary
of zero does not describe held-out model selection.

Outputs include identified evaluation, combo and target-book panels; readiness
and target-exposure tables; and the engine's daily replay ledger, actual trades,
costs and liquidity diagnostics. A completed `manifest.json` binds the artifacts.
A failed attempt retains `failure.json` and its intermediate outputs. Missing
held marks reject the entire evaluation; no return is imputed.

This command is a training-only shaping diagnostic. Its summary distinguishes
execution completion from portfolio qualification. Exact neutrality, risk and
trade constraints, instrument eligibility, source economics and executable
capacity remain qualification work. The [declared book design](docs/EQUITY_BOOK_BASELINE.md)
sets the signal controls, research splits and next implementation priorities.

`equity-book` adds constrained allocation against execution-time marked holdings,
decision-time diagonal risk, immediate post-fee net/gross/name checks and a full-L1
20% trade budget. It consumes the original context and identified baseline
intermediates, including those retained after a failed baseline replay, and keeps
the original evaluation window. See the [constrained book guide](docs/EQUITY_BOOK_CONSTRAINED.md)
for the frozen profile, exact command, certificates and remaining data/execution
requirements. Explicit hold/close instructions now preserve units and remove
machine-scale dust under a fixed representation budget and full economic checks.
The startup allocation has 381 positions. The current v3 research profile adds
current-price availability constraints for unheld entries before optimization;
the helper API retains strict failure by default. The latest full-window attempt
has four independently checked allocations and 18 observable intervals, then
stops on a missing held PCS price on May 1, 2013. No investment result is accepted.
Current development/builds use the recovered isolated worktree
at `C:/atx/.worktrees/equity-platform`.

`equity-ic` evaluates the three deployed signals (`momentum_252`,
`momentum_126`, `blend_equal`) for cross-sectional forecast skill: Pearson and
rank IC, decay, block-bootstrap ICIR, decile spread and signal autocorrelation,
against a pre-registered, hash-chained trial ledger. It reads signals from the
published `equity-baseline` `combo.bin` and never recomputes them. See
[docs/EQUITY_IC.md](docs/EQUITY_IC.md) for the frozen recipe, exact command and
qualifications; results are sign-and-shape evidence only.

`equity-universe` builds the point-in-time dollar-volume universe (checkpoint
15): 84 monthly rebalances over 2013-2019, `top_n` {1000, 2000, 3000} x band
{0.00, 0.10} side by side, with churn, per-year coverage and union sizes, an
inferred delisting table and a survivorship lower bound, against the same
hash-chained ledger under a non-trial purpose. See
[docs/EQUITY_UNIVERSE.md](docs/EQUITY_UNIVERSE.md) for the frozen recipe, exact
command, output layouts and the 2016-2018 pre-holiday data-hole caveat; the
output is membership lists and counts only, not alpha.

`panel` gained three checkpoint 16 flags, `--universe-membership`,
`--universe-cut` and `--universe-eval-start` (all three or none), restricting
the compacted panel to the year-union allow-list from a checkpoint 15
`membership.bin`. They drove 13 (year x cut) cells over 2013-2019 on top-1000
and top-3000: all three signals FAIL the pre-registered bars, verdict NO
CANDIDATE. See the
[alpha scorecard](../atx-engine/reviews/2026-09-20-equity-alpha-scorecard-cp16.md),
its [design](../atx-engine/reviews/2026-09-20-iteration16-alpha-scorecard-design.md)
and [addendum 1](../atx-engine/reviews/2026-09-20-iteration16-alpha-scorecard-design-addendum-1.md);
membership is a year union, not as-of, and those `equity-baseline` outputs
commit signals-only and are not book baselines.

The engine's signed security transition and cash-claim payment planners
(checkpoint 12) are now built and natively validated on synthetic fixtures;
see the
[transition guide](../atx-engine/docs/SECURITY_TRANSITIONS.md) and the
[validation receipt](../atx-engine/reviews/2026-09-20-security-transition-validation.json).
The May 1, 2013 PCS price failure above is not yet resolved by this work.

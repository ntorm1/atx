# Recent-data DSL ensemble: active objective

The owner's 2026-09-26 pivot supersedes completing the entire original sprint as
the immediate objective. Build a reproducible strategy composed of many
atx-engine alpha DSL subalphas, targeting annualized net Sharpe >= 1, low
turnover, and a cross-sectional universe of thousands of stocks. Use 2020+
data. The target is not a measured result or a guarantee.

The original DAG remains the source/import/evidence index. Its outstanding
lanes are dependencies to prioritize, not prerequisites to finish wholesale.
The new instruction authorizes recent-data use and supersedes the older
pre-2020-only restriction. It does not authorize warehouse writes, broker
trading, pushes, or changes in the separately owned C:/atx checkout.

## Research contract

- Target universe: a dated liquid common-stock universe of approximately
  3,000 stocks. Report daily eligible counts and actual long/short holdings;
  do not equate a union of historical identifiers with daily coverage.
- Target return metric: annualized net Sharpe >= 1, using 252 sessions/year,
  after declared trading costs and borrow. Report gross/net, uncertainty,
  drawdown, annual performance, and sensitivity to costs.
- Working low-turnover target: one-way turnover <= 30% per month. Record the
  precise gross/NAV denominator and calendar aggregation. The owner confirmed
  **$1 billion NAV** for the cost and capacity model.
- First candidates use only evidenced fields available on the selected data:
  slow momentum, intermediate reversal, seasonality, low-risk, and liquidity
  families. Fundamental/industry families enter when their dated data is usable.
- Blend diversified families with train-only signs and coefficients. Apply
  delayed execution and portfolio-level turnover controls to the combined
  holdings; averaging standalone alpha Sharpes is not a portfolio backtest.
- Proposed date roles, pending coverage and prior-use inventory: 2020-2022
  development; 2023-2024 validation; 2025 through the latest complete available
  period reserved for final evaluation. Freeze exact boundaries before reading
  research performance. Prior use must be disclosed; a previously read period
  cannot be relabeled untouched. Warmup is historical input, not scored returns.
- Track every evaluated candidate and configuration. Fix a small candidate and
  blend budget before a run; do not search until Sharpe 1 appears or silently
  relax costs, coverage, universe, or turnover after seeing results.
- Begin with a bounded end-to-end run. Preserve source/data/config identities
  and useful intermediate signals so iterations do not redo loading, compilation,
  or unchanged numerical work. No automatic 25-45 minute workload.

## Implementation priorities

| Priority | Deliverable | Original sprint dependencies |
|---|---|---|
| 1 | Recent-data inventory and bounded date-aware loader with actual universe coverage | D5, D6, I1; D0/D1 evidence where used |
| 2 | Versioned library of diverse slow DSL subalphas with an executable research entry point | A1, A5, I3; existing VM and seed families |
| 3 | Combined holdings, cost-aware smoothing/rebalance controls, delayed net evaluation | A4, B1, R1, R3; relevant I4 combination work |
| 4 | Train-only combination/selection and walk-forward diagnostics with durable trials | E0a, E2, E6, E1; I4 |
| 5 | Broaden only the alpha/data/neutralization components that improve or correct this path | D3, D4, I2, L1/L2, R4 as evidenced |

Keep the raw multi-horizon IC screen conservative. The new same-residual screen
is experimental and disabled by default; its source review is not evidence of
useful pruning, speed, or recall. Do not make its completion block a supplied-DSL
ensemble baseline.

## Work allocation at pivot

- Root: runnable research contract, integration, source/evidence index, bounded
  builds and experiments. Keep private CPP boundaries, warm PCH/dependencies,
  and multiple compiler workers subject to available RAM.
- Pool 3: recent-data and metadata inventory, date roles, exact loading path.
  New shares/cap implementation stopped before coding.
- Pool 4: actual ensemble runner, cost/replay/turnover integration audit. D4
  source frozen after the repeated whole-payload hashing repair.
- Pool 5: DSL family library and current combination interfaces. Residual-screen
  source/fixtures frozen; no further screen research expansion.

## Existing bounded evidence retained

- Residual Fitness/Search: `26378979`, independent audit `8316939a`, 64 checks.
- Computed six-descriptor provider: `c1afb692`, five distinct checks after the
  test-only D6 parent-binding correction `cb9eda17`.
- D4 source is not imported or runtime-qualified at this checkpoint:
  `8eed1323`, `d5c4f393`, `c9ba9a60`; fixtures `cd886b07`/`d37cc7fd`.
- Experimental residual screen is not imported or runtime-qualified:
  `0fa86097`, fixtures `adb38ab0`, source review `e2e15b58`.

No recent-data strategy has yet been measured under this new contract.

## Initial metadata and runner findings

Source footer inspection reports TickerHistory3.parquet (32,323,644 rows,
262 row groups, 71 columns) extends through 2026-09-18. Every row group mixes
the full historical date range; extraction needs streaming projected batches,
not an assumed partition-pruned full-table read. Existing prepared recent
artifacts contain only 31 sessions and approximately 200 stocks. No recent
return statistics have been read for this strategy.

The existing equity-mine CLI accepts supplied DSL and an explicit date seal,
but its cost-aware DelayedSurfaceV2 path is programmatic only. Its flat-bps
legacy output does not establish the shared execution model or $1bn capacity.
Several CommonStockV2/PanelStore paths hard-code the old 2020 seal. Prefer a
small explicit recent research adapter and runner over silently bypassing those
contracts. Stock-type and source-vintage gaps must remain visible in results.

## Frozen first implementation packet

- Plan/integration pivot: `469972f4`.
- Candidate library source `eeb76477` -> integration `cf252c5b`, at
  `atx-impl/strategies/slow_price_volume_24_v1.json`; SHA256
  `1851cd07d41a68d3b47009875aa54703bbb59bf5335bd6e09e4dd426c9fff40b`.
  Twenty-four candidates, six families, four recipes per family. Exact fields
  are close/raw_close/volume. Native parsing/VM qualification is pending.
- Shared execution cadence source `54b4ec99` -> integration `34fcac6b`: every
  five sessions, partial decision-known dollar targets; unchanged defaults
  preserve prior V2 semantics. Runtime qualification is pending.
- Primary variant is weekly_partial25 (every five sessions, fraction0.25);
  weekly_full (fraction1) is a diagnostic. The name does not imply calendar-week
  alignment. Orientation is fit on primary TRAIN only and reused unchanged.
- Fixed trial budget: 24 x two TRAIN sign orientations + two combined TRAIN
  variants =50. Equal family allocation means fixed1/24 contributions. Missing
  candidate contribution is neutral zero, with no data-dependent renormalization.
  An unscorable required orientation fails visibly; it does not remove a family.
- Source/runtime guard `257ffd3b`: `scripts/run_bounded_research.py`. One owned
  process tree, immutable source/executable/config/log receipt, sampled RAM and
  wall limits. Smoke completed in0.265s; intentional1s timeout stopped the owned
  sleeper in1.109s. These verify the harness only, not strategy performance.
- Data/runner implementation uses explicit research artifacts, a lightweight
  header/privateCPP reader and a standalone executable. No broad CLI config
  migration or whole sprint compilation is required.

Warmup begins2018-06-01 for TRAIN and2021-06-01 for validation. The adapter must
provide at least320 usable membership-warmup sessions after the initial63 ADV
sessions before scoring. Its ID union includes every name eligible during
warmup and scoring, so delayed cross-sectional ranks cannot depend on future
role membership. The VM uses the dated cross-sectional mask on historical
operations while preserving raw time-series history.

The first bounded run is a wiring/data-quality observation, not the final
Sharpe claim. Missing held returns remain an error; the runner cannot invent
zero returns or silently drop an exposed name to make the backtest complete.

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

## Cash-claim rehearsal registration (before any completed strategy result)

The first two real attempts stopped on MDCO's missing 2020-01-06 mark; zero
orientations or portfolios completed. The exact pinned source identifies MDCO
and confirms its final January 3 print. This is an accounting/data dependency,
not evidence for selecting a candidate or changing its sign. The original
library, weights, date roles, costs, cadence, trial budget and failure records
remain fixed.

The next bounded rehearsal opts into
`atx.dsl-combined-execution/cash-claims-v2` using
`atx-impl/strategies/slow_price_volume_24_v1.cash_claims.json`, SHA256
`257c6d645292b9f9464e6401ed1f9adb5e41a66089fdc4f7e23db0724f6da510`.
Its five independently sourced fixed-cash rights are MDCO, WAIR, BOLD, ARQL
and THOR. Their identities, previous raw/adjusted prices and source-publication
clocks are evidence-bound. Publication clocks use the following minute when
the source supplies only minute precision. Historical delivery is unverified.

On known completion, stop new decisions and queued trading in the converted
line. Recognize the signed entitlement at the first eligible valuation mark,
using marked holdings divided by the evidenced previous raw price. Receivables
are included in valuation but cannot fund new positions; payables reserve
settled cash. Continue the last modeled short-borrow rate while payment remains
unknown. No payment date, cash receipt, stock conversion or missing return is
invented. VM and blend eligibility follow strict decision-time knowledge,
including warmup. These claims are used consistently in TRAIN orientations and
both combined role evaluations; pre-role claims create no opening position.

Actual calendar-month turnover sums actual filled dollars/pretrade NAV and
includes initial deployment, also disclosed separately. No turnover, Sharpe,
common-stock coverage or tradability target has yet been achieved. Stock
conversions and unexplained price gaps still fail strictly. A masks-only queue
contains 38 affected Q1 identifiers; absence alone is not event evidence. The
existing warehouse's event, terminal-return and universe-type tables are empty
under read-only inspection, so they cannot currently close this dependency.

Qualification first uses only the focused strategy executable/test targets,
warm equity-dev PCH/dependencies and RAM-admitted 2-4 compiler workers. The real
rehearsal retains its 180-second/1536-MiB sampled process-tree limit, 768-MiB
free-memory floor, 1024-MiB internal workspace and minimum 2000 names. It adds
the pinned claim configuration without expanding the candidate search.

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

## First native integration and rehearsal admission

The native runner and focused qualification target built at `36771680` in
88.873s with three workers, retaining the existing PCH and dependency trees.
Eighteen of nineteen checks passed in 0.711s; the DSL-library fixture alone
failed because `__FILE__` was relative to the build directory. The test-only
correction `87ff49cc` -> `d8cc7b65` uses the existing configured test-directory
macro. Runtime qualification of that correction follows before performance.

The first real rehearsal requires at least 2,000 usable names at a rebalance,
1,024 MiB admitted numerical workspace, and an external 180-second/1,536-MiB
sampled process-tree ceiling with 768 MiB minimum system free memory. These
limits are operational admission criteria, fixed before any strategy result.
TRAIN manifest SHA256 is
`900839a1ea8e21edc0f5edd5e9cd8f2bc7884a9295a86d5d6f2de19a79aed36b`;
the subsequent development-check manifest SHA256 is
`2129ed162ca3eb1e2bce3bd74f456b83ea11187c26729d06712677bbb6848e84`.
The latter has 524 sessions, 461 warmup sessions, 63 scored sessions and 4,101
union identifiers. It is 2020Q2 development evidence, despite the runner's
generic role label `validation`; 2023-2024 validation remains unread.

Independent mask audit `7a69b22a` -> `3f9e1749` verified the first role's axes,
warmup membership union and hashes. Ten cadence-five name-decisions may lack
an entry or endpoint bar. This is a data diagnostic, not permission to remove
names using future presence or invent zero returns. Failed evaluation attempts
remain in the research ledger.

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

The first real attempt and diagnostic retry both failed on a held price gap,
with zero completed trials. Source inspection confirms that security39621's
archive ends on2020-01-03. W2-D2 terminal/merger evidence is therefore pulled
forward: identify the dated event, value any evidenced signed claim explicitly,
and preserve cash-receipt/availability qualifications. No forward price fill,
future membership filter, Shumway gain for shorts or hypothetical settlement
may be silently introduced to produce a Sharpe. See report480af2d7 and its
strict-diagnostic addendum. Candidate/sign/blend selection recipe is unchanged.

## Development rehearsal registered before any strategy performance

The first real execution rehearsal uses TRAIN2020Q1 and a subsequent2020Q2
check, both inside the declared2020-2022 development period. This preserves
2023-2024 validation and2025+ final evaluation from the rehearsal. The primary
variant,24expressions,familyweights,costscenario and sign-fitting algorithm do
not change after seeing the rehearsal. Its metrics are development evidence.

Budget: one development rehearsal (50 TRAIN orientations/blends +2 subsequent
combined checks), followed by one full declared experiment (50 TRAIN +2
validation), at most104 completed role evaluations across these two stages.
Every failed/retried attempt is additionally recorded; the count does not reset
when a run fails. The existing recipe's52 count is per complete run. No extra
candidates/variants or selection using the rehearsal check is authorized by
this registration. The independent synthetic checks are correctness evidence.

First recent source extraction completed in39.203s, sampled process-tree peak
795,824,128bytes. It accepted16,035,158of16,237,003 selected rows. Projection
manifest SHA25625a96be8611bf97eddccea3b254220aa750a66b98953069c51cd392d30f0e446.
TRAIN2020Q1 data preparation completed in6.391s;461sessions including399warmup,
3,950union identifiers,62scored sessions, dailyeligible count2872..3000
(median2943). This establishes a thousands-name research cohort, not verified
common-stock status or achieved portfolio performance.

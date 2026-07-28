# Quant strategy research and implementation pipeline

This module turns the projection-backed BacktestDb into a reproducible
research-to-implementation system. It is designed first for systematic equity
option dispersion and long/short volatility valuation, but its stage contracts
are strategy-neutral.

The implementation is intentionally broker-neutral. It can emit versioned,
dry-run basket and hedge intents with typed algorithm parameters. Sending those
intents to a broker is an adapter responsibility outside `atx-vol`.

## Design sources

The design follows the responsibilities described by SpiderRock for systematic
dispersion and valuation portfolios: live analytics and theoretical surfaces,
risk-constrained basket orders, Greek/scenario balancing, dynamic hedging, and
pluggable execution instructions:

- <https://spiderrock.net/platform/>

The stage separation follows the same reusable flow as QuantConnect's Algorithm
Framework:

```text
point-in-time universe
  -> features and signals
  -> target portfolio
  -> risk-adjusted target
  -> execution and hedge intents
```

- <https://www.quantconnect.com/docs/v2/writing-algorithms/algorithm-framework/overview>

Research promotion is deliberately out-of-sample and multiple-testing aware:

- Bailey et al., *The Probability of Backtest Overfitting*:
  <https://papers.ssrn.com/sol3/Papers.cfm?abstract_id=2326253>
- Bailey and López de Prado, *The Deflated Sharpe Ratio*:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551>
- Newey and West, heteroskedasticity/autocorrelation-consistent inference:
  <https://www.nber.org/papers/t0055>

## Modules

### Existing market and backtest substrate

The pipeline reuses, rather than duplicates:

- `BacktestDb` for projection-backed P&L histories, exact source identities,
  continuation checkpoints, mmap reads, and suffix recomputation;
- `SurfaceDb` and `PricedSurface` for point-in-time theoretical values;
- `DispersionStrategy` and `DispersionBook` for exact constituent dispersion,
  implied-correlation telemetry, and vega/gamma/theta sizing;
- `PortfolioPricer` for deterministic multi-name first- and second-order Greeks;
- `ScenarioGrid` for uniform book shocks and exact/Taylor routing;
- the listed-dispersion route as an independent executable-quote and
  reconciliation check.

The new component-scenario path complements `ScenarioGrid`: it permits a
different spot and volatility shock for every UID. A component move can
therefore be conditioned on an index shock through beta plus a specified
residual, while sector or name-specific volatility responses remain explicit.

### Point-in-time research observations

Every observation carries four distinct clocks:

```text
observed_ts <= available_ts <= decision_ts < execution_ts
```

It also records the end of the outcome interval. A feature is inadmissible if it
was not available at the decision time. By default a signal calculated from a
stored close can first execute on the next stored observation; the system does
not silently fill it at the close that created it.

Feature and outcome dependency intervals are used during validation. Training
rows whose feature or label intervals overlap a test interval are purged, and an
explicit post-test embargo can be applied. Rows sharing a decision timestamp
form one indivisible panel group.

### Signal mining and validation

The initial miner supports causal identity, difference, and rolling z-score
transforms, explicit lags and lookbacks, both trade directions, and deterministic
parameter grids. New transforms should preserve the same contract:

- fit or warm up only from the training view;
- never normalize from a full sample;
- emit only stitched out-of-sample returns for selection;
- count every attempted configuration in the sealed trial family;
- use stable parameter and tie ordering.

Reported research statistics use dimensionless returns with an explicit positive
lagged-capital denominator. They include Newey-West inference, probabilistic and
deflated Sharpe probabilities, drawdown, and multiplicity-adjusted p-values.
Operational BacktestDb P&L and the existing tearsheet remain available, but
their dollar P&L is not relabeled as a research return.

The first research catalog is intentionally small and economically motivated:

- implied-correlation level/change/rolling z-score for dispersion;
- constituent-minus-index variance and correlation-risk spreads;
- realized-minus-implied volatility for cross-sectional long/short volatility;
- call/put implied-volatility spread, skew, curvature, term slope and event
  variance from the existing surface analytics;
- delta-hedged option carry and its interaction with volatility regimes.

These are hypotheses, not built-in claims of profitability. The dispersion
decomposition and the possibility that realistic frictions consume the apparent
correlation premium are documented in the literature:

- <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=673425>
- <https://arxiv.org/abs/1004.0125>

The realized-minus-implied cross-sectional valuation sort follows the hypothesis
studied by Goyal and Saretto:

- <https://ideas.repec.org/a/eee/jfinec/v94y2009i2p310-326.html>

Every transform, horizon, threshold and direction is still an attempted trial.
No paper result bypasses point-in-time construction, transaction-cost stress,
purged out-of-sample validation, or the promotion gates.

### Strategy construction and risk

Two concrete constructors are supplied:

1. **Dispersion.** Exact point-in-time index constituents are represented as a
   strict basket. A dirty basket must declare each proxy mapping, its knowledge
   timestamp, source identity, and exposure inputs. Existing vega/gamma/theta
   matching remains the pricing authority.
2. **Long/short valuation.** Option opportunities are ranked by a caller-owned
   theoretical valuation edge. Long and short sleeves are selected
   deterministically and scaled onto a common gross-vega budget.

The common risk overlay aggregates delta, gamma, vega, theta and gross notional,
evaluates heterogeneous component scenarios, and scales or rejects a target
against configured limits. It does not hide an infeasible relative exposure:
scalar scaling can reduce absolute risk, but cannot manufacture neutrality that
the constructor failed to create.

### Execution intents

A risk-approved target is converted to target-minus-current quantities. Output
is a versioned `BasketOrderIntent` plus optional `HedgeInstruction` records.
Algorithm parameters are typed values rather than unparsed strings.

All intents default to research-only and dry-run. `atx-vol` contains no broker
credentials, session management, network transport, or order-send operation.

## Research persistence

BacktestDb v1 remains byte- and reader-compatible. Research uses a companion
store below the same root:

```text
<backtest-db>/
  manifest.atxbtdb
  partitions/
  research/
    manifest.atxqrdb
    objects/
      <kind>-<sha256>.atxrun
```

Research objects are immutable and content addressed. The manifest contains an
artifact catalog, dependency lineage, and a current head per
`(artifact-kind, logical-id)`. Publication writes and validates the object first,
then atomically publishes the next manifest generation. A stale expected head is
rejected, preventing two workers from silently overwriting each other's result.

Artifact kinds cover:

- strategy definitions;
- point-in-time signal segments;
- candidates and parameter sets;
- validation trials and metrics;
- risk and component-scenario snapshots;
- execution and hedge intents.

Trials reference the exact BacktestDb run identity and source lineage instead of
copying aggregate P&L. Re-running a daily BacktestDb build first performs its
existing suffix extension; the research pipeline then publishes only artifacts
whose dependencies changed. Historical corrections produce a new immutable
suffix/head while prior evidence remains auditable.

## Promotion lifecycle

Research evidence moves through explicit stages:

```text
draft -> sealed discovery -> validated -> holdout -> paper -> approved
```

Promotion fails closed when any mandatory item is missing or non-finite.
Configurable gates include:

- complete point-in-time source and universe lineage;
- minimum out-of-sample folds and observations;
- HAC significance and deflated-Sharpe probability;
- multiplicity-adjusted significance;
- fold/path stability and drawdown;
- exposure, scenario-loss, turnover, and concentration limits;
- spread, impact, financing, and borrow stress;
- parameter-neighborhood stability;
- one-time untouched holdout;
- listed-contract feasibility/reconciliation sample;
- paper-trading duration and live-versus-model drift.

Passing these gates approves research evidence. It does not authorize an
external order send.

## Command-line research run

`atx-vol-quant-research` mines one stored BacktestDb signal using deterministic
purged walk-forward validation, then publishes the stitched out-of-sample
evidence as an immutable ResearchDb trial:

```powershell
.\build\bin\atx-vol-quant-research.exe `
  --db C:\data\backtests `
  --template long-40d-3m-strangle `
  --symbol SPY `
  --signal implied_correlation `
  --capital 1000000 `
  --min-train 252 --test 63 --step 63 `
  --lookbacks 5,20,60
```

The capital input is required and is used only as the lagged denominator that
turns future P&L into a dimensionless research return. The command tests
identity, change, and rolling z-score transforms in both directions, seals the
full attempted family before selection, and prints the selected candidate's
out-of-sample statistics and immutable artifact identity.

For implementation, `build_strategy_implementation_plan` composes a target,
current holdings, scalar limits, and per-UID conditional scenarios into one
risk decision and a dry-run basket/hedge intent. Existing
`DispersionStrategy` output can be mapped without re-sizing through
`dispersion_named_positions`; the dispersion engine remains authoritative for
its vega/gamma/theta-matched quantities.

## Cross-sectional listed-options bridge

The first executable cross-sectional slice now lives in
`atx-options-engine`. It bridges `ResearchObservation` and actual listed
contracts into the mature `atx-engine` cross-sectional stack instead of
reimplementing rank transforms, neutralization, optimization, execution, and
ledger accounting inside `atx-vol`.

The bridge adds the contracts that a scalar research row cannot express:
permanent contract identity, definition and quote clocks, multiplier, bid/ask
and displayed sizes, interval volume, lagged open interest, ADV, vega, return
volatility, initial/maintenance margin, explicit tradability status, and
separate definition/feature/execution/outcome lineage. It materializes a
canonical bounded date-by-contract `Dataset`; non-tradable cells are masked and
their engine signal is `NaN`. Ex-post forward-P&L labels are excluded from that
decision dataset and available only through a separately typed outcome view.

`make_option_target_book` then maps engine weights into whole listed contracts
using either premium-notional or per-contract-vega units. A lagged-ADV position
fraction and per-name limits apply before a documented conservative
independent-contract margin gate. This is research margin, not order
participation, SPAN, STANS, or broker portfolio margin.

The generic stock `WeightPolicy::reconcile` is intentionally not reused for
option sizing because it assumes one share per price unit. The next replay
stage will reconcile these option-aware contract targets into next-slice
orders, consume shared observed L1 liquidity, and commit partial fills through
the existing portfolio ledger. Full contracts, fidelity limits, and primary
research sources are documented in
[`atx-options-engine/README.md`](../../atx-options-engine/README.md).

The XS-2 `OptionExecutionReplay` kernel provides deterministic Consolidated-L1
partial fills, shared selected-participant liquidity, internally exact
multiplier-aware modeled cash, and effective-dated fee ledgers.

XS-3A adds `OptionExecutionSession`, a bounded persistent state machine that
preloads immutable market evidence once and merges future-effective dynamic
orders and cancellations across decision frontiers. Every frontier exposes
realized positions, scheduled/working/pending-cancel leaves, projected
exposure, new fills, and an append-only lifecycle transition ledger. Commands
cannot consume the same-timestamp quote that produced them, invalid baskets
reject atomically, and successful lifecycle calls allocate no memory after
workspace creation.

This closes the execution-state seam for adaptive research. The next
date-major coordinator must run the point-in-time signal and option target
policy after each observation and reconcile against position plus live leaves,
not position alone. The design contract and primary sources are in the
[`adaptive execution-session research note`](../../atx-options-engine/docs/adaptive-execution-session-research-2026-07-26.md).

## Extension rules

- New feature producers must declare observation and availability time, warmup,
  definition fingerprint, and exact source identities.
- New portfolio constructors emit targets; they do not mutate the backtest lot
  book directly.
- New risk models adjust or reject targets before intent generation.
- New execution adapters consume versioned intents and return explicit fills;
  they must not be linked into the research persistence layer.
- Any incompatible artifact encoding gets a new payload version and schema salt.
  Existing immutable objects are never rewritten in place.

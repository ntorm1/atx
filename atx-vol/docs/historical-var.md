# Historical VaR (`api/risk/historical_var.hpp`)

Design of record: `docs/superpowers/specs/2026-10-24-atx-vol-risk-engine-design.md` §5.6 and
decision D13. Engine tour: `docs/risk-engine.md`. This replaces the deleted
`src/analytics/var.*` / `historical_projection.*` pipeline (see "Migration" below; the
CHANGELOG `BREAKING — historical VaR rebuilt on the risk engine` entry has the details).

## What it computes

One-period historical-simulation VaR / ES of a **reference book** — relative option templates
(`TheoLeg`: a `TheoContract` with a relative maturity and a strike coordinate, plus a signed
qty) and share legs (`ShareLeg`) — over (base_i → shifted_i) pairs of **surface-backed**
`MarketState`s (`market_state_from_surfaces`).

Per scenario i:

1. Every template is resolved on base_i's surface for its uid (`resolve_theo_many` with
   `config.projection`): a fixed delta is re-solved, a fixed moneyness held, a relative
   maturity re-anchored at base_i's instant.
2. The resolved contracts, with the templates' quantities (never re-sized), and the share legs
   form one `RiskBook`; both states are evaluated with `config.kernel` (the sigma-family kernel
   by default, the cold kernel as oracle) and `explain(base_i → shifted_i, config.explain)`
   gives the scenario's P&L with its Greek split. The contract struck on base_i is the one held
   through shifted_i.
3. If the pair moves a rate while `config.needs.rho` is off, the base frame is re-evaluated
   with rho so the move is attributed to `pnl_rho` rather than left unexplained
   (`pnl_total` is `P_t − P_b` either way).

`HistoricalVarFrame` per scenario: the explain totals (`pnl_total` is THE P&L), templates
resolved / unresolved, and `admitted`.

## Failure policy

| case | outcome |
|---|---|
| a template's uid has no surface in base_i, delta unreachable, maturity unavailable … | counted in `n_legs_unresolved`; the scenario runs with the rest |
| a state with **no surface at all** (marks-only) as base or shifted, book has templates | scenario refused (not admitted): marks cannot host theoretical contracts (D13) |
| no resolved template and no share leg | not admitted |
| an explain exclusion (rolled, refused, numeric error lane, a share or option leg whose uid is missing from a state) | admitted iff at least one leg is in the totals, every excluded leg's premium is KNOWN, and `excluded_premium < max_excluded_premium_frac · |pv_base|` (strictly; default 0: any exclusion drops the scenario) |
| an excluded leg whose BASE value is unavailable (explain books it at premium 0) | never admitted, whatever the fraction: its premium is unknown, not zero |
| the state's clock refuses an expiry or the step | not admitted (checked before the kernel runs, with the kernel's and explain's own tests) |
| empty book, null state pointer, confidence outside (0, 1), invalid kernel policy, non-finite qty, negative `max_excluded_premium_frac` | `InvalidArgument` for the call |
| any other evaluate / explain failure (structural: a workspace or frame bound to another book …) | the call's error, never a silently dropped scenario |

Scenario-level failures never fail the call; structural ones always do. Each scenario has its
own `RiskBook`, so the one workspace is re-bound with `RiskWorkspace::reserve` before every
evaluation; that re-bind is what `reserve` does today, not what its header promises, and is
recorded at the call site in `historical_var.cpp`.

## Quantiles

Over the admitted scenarios: loss_i = −pnl_total_i, sorted ascending (ties keep scenario
order); nearest rank: index ⌈c·n⌉ − 1. VaR = the loss at that index; ES = the Neumaier mean of
the losses from that index up; `tail_mean` = the field-wise Neumaier mean of the tail
scenarios' `ExplainTotals` (the attribution of the tail; the two counts are rounded means), so
`expected_shortfall == −tail_mean.pnl_total` exactly. `reference_pv` = the mean `pv_base` over
the admitted scenarios. With no admitted scenario every point has NaN VaR / ES and
`tail_scenarios == 0`, and `reference_pv` is NaN.

`summarize_historical_var(frames, confidences)` is that quantile half on its own: a caller that
evaluates a long history in windows (one `historical_var` per window, `confidences` empty)
summarizes the concatenated frames once, and an artifact verifier recomputes published points
from stored frames — both bit-identical to one `historical_var` over the whole history. It
refuses (`InvalidArgument`) an ADMITTED frame whose `pnl_total` or `pv_base` is not finite: a
NaN loss has no rank. Frames that are not admitted are never read.
`config.keep_resolved_legs` keeps every template's resolution on every base state in
`resolved_legs` (scenario-major).

## Threading and determinism

Scenarios are independent. When the kernel's own fan-out is off (`family.n_threads == 1` /
`cold.n_threads == 1`) they are fanned over the pricing executor with `config.n_threads`;
otherwise they run serially and the kernel parallelizes (nested dispatches run inline). Each
scenario writes only its own slots and every reduction is serial in a fixed order, so the
result is bit-identical for any thread count (`HistoricalVar.DeterministicAcrossThreads`).

## Dispersion: `dispersion_book_var`

`research/listed_dispersion_pipeline.hpp`. The as-of book's positions become templates (same
uid / side / multiplier / qty, ATM-forward strike `StrikeCoord::log_moneyness(0)`, the caller's
relative maturity — `days(N)` as a fixed N-day year fraction) and the scenarios are the
CONSECUTIVE pairs of the historical surface sets (N sets → N − 1 scenarios). Any incomplete pair
(an unresolved template or a pair the engine did not admit) is `Unavailable`. The bounded form
takes an evaluator that supplies the history in overlapping windows, so
`dispersion_run_projected_var` loads each archive exactly once. Its loose TSV triple
(`projected_var.tsv`, `projected_risk_scenarios.tsv` — one row per pair with the explain totals —
and `projected_risk_legs.tsv` — the base-session resolutions) is recomputed bit-for-bit by
`verify_projected_var_artifacts`, which also requires `n_scenarios` = sessions − 1 and
re-derives every row's admission from its counts (a leg in the totals, no excluded leg: the
route runs at `max_excluded_premium_frac` 0) instead of trusting the stored `admitted` bit.
`dispersion_run_projected_var` feeds overlapping 16-session windows, turns the family kernel's
own fan-out off (`family.n_threads = 1`) and fans the scenarios over `fit.workers` instead.

Compared with the deleted engine the numbers move on purpose: a scenario is now the one-period
P&L of a pair (it was each set's value minus the LAST set's value), so N sets give N − 1
scenarios, and the published reference value is the mean `pv_base` over the admitted pairs (it
was the last scenario's value). The scenario table changed from 10 columns per session to 29 per
pair (28 before lane E's `pnl_carry` column, which follows `excluded_premium`); a run directory
written before either change fails verification on that header.

## Migration from the deleted pipeline

| deleted | now |
|---|---|
| `run_historical_var(SurfaceDb, VarPosition…, VarRunConfig)` | per session a `MarketState` from that session's surfaces (`market_state_from_surfaces`), consecutive sessions as `HistoricalVarScenario` pairs, `historical_var(book, pairs, config)` |
| `VarOptionPosition{underlier, time_to_expiry, target_abs_delta, side, qty, multiplier}` | `TheoLeg{TheoContract{uid, MaturitySpec::…, StrikeCoord::delta(±abs_delta, DeltaConvention::American), side, multiplier}, qty}` (the old "absolute delta" was the American spot delta) |
| `VarStockPosition{underlier, shares}` | `ShareLeg{id, uid, shares}` |
| `PreparedHistoricalProjection` + `projected_historical_var` (reference = last scenario's value) | `historical_var` / `summarize_historical_var` (one-period pair P&L) |
| `var_report` (per-scenario TSV, per-underlier rollup) | `ExplainResult::by_uid` per scenario via `explain`; no packaged report yet |
| `var_validation` (Kupiec POF, Christoffersen) | not carried; a breach-sequence validator over `HistoricalVarFrame`s is a follow-up |
| `bench/var_bench.cpp` (`var/prepared/…` rows of `atx-vol-projection-bench`), `bench/plot_var_scenario_pnl.py`, `bench/run_historical_projection_gate.ps1` | removed; the projection rows of `atx-vol-projection-bench` stay. No historical-VaR bench yet |
| `docs/historical-var-engine-status.md` | this page |

All of the deleted code was internal (`src/analytics/`); no api/ header and no Python binding
went with it.

## Tests

- `tests/risk_historical_var_test.cpp`, ctest prefix `risk/HistoricalVar.`:
  - pipeline: `FixedDeltaTemplatesReSolvePerScenario`, `FixedMoneynessTemplatesHoldK`,
    `SharesInScenarioPnl`;
  - quantiles: `NearestRankQuantilesMatchHandComputation` (5 scenarios, VaR / ES at 0.6 and 0.8
    by hand), `NearestRankTiesKeepScenarioOrder`, `NearestRankAtOneScenarioAndConfidenceNearOne`,
    `TailMeanIsAttributionOfTail`, `SummarizeRefusesAnAdmittedNonFiniteFrame`;
  - failure policy: `UnresolvedTemplateIsCountedNotFatal`, `NoSurfaceStateIsRefused`,
    `ExcludedLegNeedsAKnownPremiumStrictlyBelowTheFraction` (incl. a known zero premium at
    fraction 0), `ClockThatRefusesAnExpiryDropsOnlyThatScenario`,
    `ForeignWorkspaceIsAnErrorNotADroppedScenario`, `InvalidArgumentsAreRefused`;
  - kernels: `FamilyAndColdAgreeWithinTolerance` (family economics off so the family route really
    runs, witnessed by `FrameCounters`; bounds derived from the 1e-6 relative price gate, plus
    |ΔVaR| ≤ 1e-6 · reference_pv), `DeterministicAcrossThreads`.
- Dispersion: `dispersion/ListedDispersionPipeline.*` (numeric pins of the pair P&L and VaR / ES;
  the windowed evaluator bit-equal to one span, with the covered-twice / uncovered / past-the-end
  refusals; a timed surface off its snapshot's instant refused even next to an untimed one),
  `dispersion/DispersionRun*` and `dispersion/DispersionProjectedVar*` (artifact schema, verifier,
  sessions − 1 rule, admission re-derived from the counts, and an 18-session history that crosses the route's 16-pair window seam with
  every archive opened once). On the listed-pipeline 3-set fixture the pair P&Ls are
  −36.0787 / +74.7856, so VaR95 = ES95 = 36.0787.

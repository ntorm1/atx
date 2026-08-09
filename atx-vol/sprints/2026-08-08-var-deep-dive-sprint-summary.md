# VaR Deep-Dive Review & Findings Sprint — Summary

**Date:** 2026-08-08 (single-day sprint)
**Branch:** `var` (worktree `C:\atx-wt\var`), merged to `main` at `9a6c612`
**Plan:** `docs/superpowers/plans/2026-08-07-var-deep-dive-findings.md` · **Ledger:** `.superpowers/sdd/2026-08-07-var-deep-dive-findings/progress.md` (var worktree)
**Method:** five-agent parallel deep-dive review (P&L forensics, solver correctness, projection correctness, performance, feature gaps), consolidated into an 8-task plan, executed subagent-driven — fresh implementer per task, independent spec+quality reviewer per task, fix loops, final whole-branch review with a must-fix gate.

## Trigger

The cumulative P&L artifact for the SP100 dispersion historical replay
(`sp100_dispersion_ytd_cumulative_pnl.png`) climbed a suspicious near-45-degree
straight line to +$6.5M over 106 scenarios. Question: genuine, methodology
artifact, or engine defect?

## The 45-degree line, answered

**Follow-up correction: there were two defects.**

1. The engine projected each terminal option to historical |delta| and relative
   expiry correctly, but then re-sized its quantity to preserve reference dollar
   delta on every base date. Stock hedge shares were re-sized the same way. That
   is a characteristic-target strategy replay, not the final held portfolio.
   Historical projection now keeps every accepted option quantity and hedge
   share count exactly equal to the terminal reference holding; historical
   dollar delta is an output, not a sizing constraint.
2. The chart summed mutually exclusive historical VaR scenarios. Such a sum is
   not an equity curve, even after fixing quantities. The cumulative and
   "held-profile drift" presentation has been removed in favor of one-day P&L
   bars and a P&L distribution with VaR cutoffs.

The production-scale rerun passed a direct fixed-holdings invariant over 2,928
accepted positions and 106 scenarios. At 99%, VaR is **$941,857.72** and ES is
**$950,796.42**, versus the superseded re-sized result of about $1.014M and
$1.027M. The arithmetic sum of the corrected scenario P&Ls is still +$6.31M;
that persistence is evidence that summing the alternative scenarios was itself
the source of the 45-degree visual, not evidence of a realizable return stream.

The fixture remains a replayable subset rather than all terminal lots: 6,666
source options lack full-history underlier coverage, 45 sit at unsupported delta
boundaries, and 360 fail at least one replay scenario. Those exclusions are
reported and must not be described as the full 9,966-lot terminal portfolio.

## Review findings (input to the plan)

- **P&L forensics:** 2 Critical (presentation), 3 Important, 3 Minor — all presentation/disclosure; engine P&L path verified clean.
- **Solver:** 0 Critical, 1 Important (tolerance/2 acceptance assumed an unenforced 5e-8 laned-vs-scalar kernel bound), 7 Minor, 8 claimed-but-untested invariants.
- **Projection:** 0 Critical, 5 Important — all silent-bias class (no session-gap policy, fast-mark leak via `QueryExecution::Configured` in `PreparedHistoricalProjection`, unsurfaced tenor extrapolation, wing-extrapolated restrike roots, unreported exclusions), 10 Minor.
- **Performance:** base-mark harvest (~18–22% core time), dynamic scheduling (~10–20% t8 wall), measured-gamma slope (parked), bench integrity items.
- **Feature gaps:** no backtest validation statistics, single confidence/horizon/weights, reporting trapped in bench code, no attribution.

## What was built (8 tasks, 19 commits, 6 fix rounds)

1. **P&L presentation** (`82b2776`, `5acbb86`, superseded in follow-up): the interim cumulative decomposition was removed. Historical scenarios are now presented only as one-day P&L observations and a loss distribution.
2. **Solver/error-path hardening** (`2987bd8`): laned-vs-scalar delta bound pinned ≤5e-8 at the solver's exact request shape; `checked_row_count` uint32 overflow guard; `VarScenarioStatus::ArchiveError` — corrupt archives fail the run even under `ExcludeFromDistribution`; timestamp-fault reclassification. Also restored the recurring dropped VAR CMake registrations.
3. **Projection safety knobs + telemetry** (`93d84b5`, `1b3beda`, `a7ee48d`): `max_session_gap_days`, `max_excluded_fraction`, `max_restrike_abs_log_moneyness` (defaults preserve behavior); cold-execution knob on `PreparedHistoricalProjection::evaluate_into` with VaR-path callers forced cold; `VarLegFrame::diagnostic_flags` (tenor extrapolation bits, wing-proximity bit); `n_gap_skipped` / `n_excluded_from_distribution` / `n_tenor_extrapolated_legs` counters populated on both routes; comparator field-coverage gap closed.
4. **Engine reporting API** (`1df442e`, `e8d7641`): `var_report.{hpp,cpp}` — byte-stable per-scenario TSV writer extracted from the bench, `attribute_by_underlier` (worst-first, uid-identity-verified against positional trust).
5. **Risk metrics + validation statistics** (`12af897`, `55869d7`): BRW/EWMA-weighted VaR/ES (`VarWeighting`), `historical_var_curve`, `kupiec_pof`, and `christoffersen` remain reusable APIs. The benchmark's in-sample `validation:` use was removed in follow-up because its threshold came from the same observations it tested; the bench now labels the numbers an in-sample distribution summary, not a VaR backtest.
6. **Invariant test backfill** (`68b6d0f`, `e4683dc`): pack-composition invariance, genuine-row-failure whole-scenario downgrade (bit-identical to forced scalar), fallback-tail-through-engine (mechanistic proof via evaluations > pass cap), thread-invariance with a mixed downgraded/normal fixture — mutation-verified.
7. **Base-mark harvest, guarded** (`0de0fd0`, `1d6bf17`): solver-pass cold marks reused for base valuation behind `VarBaseMarkSource` (default `DedicatedPricePass`). Measured: dedicated mark lanes −50.0% exactly (306,870 eliminated), Greek-route counters byte-identical, fallback tail 0.41%. **Default NOT flipped**: harvested price differs from the scalar-route price by ≤8.74e-14 relative (pre-existing kernel entry-point gap: `american_greeks_al().price` analytic=true vs `american_price` analytic=false — on this fixture the dedicated pass is all-scalar because groups average 1.87 lanes, under the 4-lane pack threshold). Flip evidence assembled: parity gates zero both knob states, YTD P&L drift $0.00097 on $6.54M.
8. **Dynamic scenario scheduling** (`dcc0998`): replay loop moved to `PricingExecutor::run_dynamic` under pinned bit-invariance (3 dynamic-vs-static bit-identity tests, mutation-verified); static path demoted to test-only; bench emits `resolved_workers`; loader loop deliberately kept contiguous (snapshot chaining, IO-bound).

**Final whole-branch review:** APPROVE-WITH-MUST-FIX-LIST → must-fix landed
(`27db849`): archive faults now classified from *both* snapshot loads (the
base-NotFound + shifted-corrupt combination previously slipped through as
excludable). Gate after fix: **98/98 green**
(`Var.*:ContractProjection.*:HistoricalProjection.*:VarReport.*:VarValidation.*`).

## Superseded rerun and corrected result

Post-sprint rerun at `27db849` (release, terminal SP100 case, deterministic —
bit-identical across t1/t8 process reruns):

- The old **$6,541,876.61 cumulative P&L** is retired. Cumulative scenario P&L is not a valid baseline.
- The old Kupiec/Christoffersen "all pass" claim is retired. The breach threshold was estimated from the same sample, so it was not an out-of-sample VaR backtest.
- Corrected fixed-unit distribution: 99% VaR **$941,857.72**, 99% ES **$950,796.42**, worst one-day P&L **−$959,735.12**, over 106 observations.
- Corrected artifacts: `C:\atx\artifacts\var\sp100_dispersion_ytd_pnl_fixed_units.tsv` and `C:\atx\artifacts\var\sp100_dispersion_ytd_one_day_pnl_fixed_units.png`.

## Validation state

The follow-up fixed-holdings gate is 72/72 green
(`Var.*:VarReport.*:VarValidation.*`), the plot helper is 6/6 green, and the
real SP100 run passed the exact scenario-units-equal-reference-units invariant
with aggregate/retained parity counters all zero. The prior binding gate was
98/98 green at `27db849` (pre-merge). Full unfiltered suite
intentionally not run (user directive; the two known pre-existing failures —
`SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`,
`VolUmbrella.TierCountsMatchTheReadmeTable` — predate the branch and touch no
branch-owned file). Merge `9a6c612` also folded in main's concurrent
stress-day fit-fidelity sprint; post-merge gate rerun in progress at write time
— result to be recorded in the ledger.

## Open items

- **Decisions:** define an explicit policy for missing-history underliers if the full 9,966-lot terminal portfolio must be replayed; decide whether contracts crossing expiry inside a scenario should settle from an expiry fixing rather than remain excluded; flip `VarBaseMarkSource` default only after re-measuring under fixed-unit economics.
- **Follow-up tickets (final-review triage, class b):** out-of-sample rolling VaR backtest; triple-combination pin test (harvest + dynamic + mixed downgrade); share `var_frame_qualifies` with the unweighted statistics path; grouped-call fallback probe; `resolved_worker_count` dedup; TSV name-escaping caveat; ExpiredBeforeShift settlement policy; 2026-06-29 restrike-anchor anomaly investigation.

## Commit map (var branch, base `82c6417`)

`fdb4783` plan → `84c0d2c` north star → `82b2776`/`5acbb86` P&L labeling (+`1fc42a4` plan fix) → `2987bd8` solver hardening → `93d84b5`/`1b3beda`/`a7ee48d` projection safety + telemetry → `1df442e`/`e8d7641` reporting API → `12af897`/`55869d7` risk metrics + validation → `68b6d0f`/`e4683dc` invariant backfill → `0de0fd0`/`1d6bf17` base-mark harvest → `dcc0998` dynamic scheduling → `27db849` final-review must-fix → merged `9a6c612`.

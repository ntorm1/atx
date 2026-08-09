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

**No engine defect.** The engine implements the confirmed methodology exactly:
each position is defined by (underlier, call/put, |delta| moneyness, relative
time to expiry); every historical observation independently restrikes it on the
base surface to those characteristics, re-sizes to the reference dollar delta,
and reprices cold on the shifted surface; per-scenario P&L = shifted − base.
The per-scenario P&L distribution — the VaR object — is clean (P&L identity
verified to 1e-8, no hedge double-count, no look-ahead, cold marks both sides).

The straight line is a property of *cumulating* those P&Ls: every scenario
re-arms theta on a freshly restruck book, so the cumulative track is the
frictionless equity curve of "sell a fresh dispersion book every session," not
a held book. Exact telescoping decomposition of the accepted trace:

| Component | Amount | Share |
|---|---|---|
| Re-basing resets (same-day, theta re-arm) | +$4.87M | 74% |
| Re-basing resets (at the 12 history breaks) | +$0.96M | 15% |
| Held-profile revaluation drift | +$0.72M | 11% |
| **Cumulative total** | **+$6.55M** | |

The artifact now discloses this: retitled "Characteristic-Restruck Historical
Replay," semantics subtitle, and a second series plotting held-profile drift
(ends ≈ $723K). The smoothness is mechanical too — adjacent scenarios share the
middle surface (lag-1 autocorrelation −0.5), so spike losses are clawed back by
the next restruck scenario.

## Review findings (input to the plan)

- **P&L forensics:** 2 Critical (presentation), 3 Important, 3 Minor — all presentation/disclosure; engine P&L path verified clean.
- **Solver:** 0 Critical, 1 Important (tolerance/2 acceptance assumed an unenforced 5e-8 laned-vs-scalar kernel bound), 7 Minor, 8 claimed-but-untested invariants.
- **Projection:** 0 Critical, 5 Important — all silent-bias class (no session-gap policy, fast-mark leak via `QueryExecution::Configured` in `PreparedHistoricalProjection`, unsurfaced tenor extrapolation, wing-extrapolated restrike roots, unreported exclusions), 10 Minor.
- **Performance:** base-mark harvest (~18–22% core time), dynamic scheduling (~10–20% t8 wall), measured-gamma slope (parked), bench integrity items.
- **Feature gaps:** no backtest validation statistics, single confidence/horizon/weights, reporting trapped in bench code, no attribution.

## What was built (8 tasks, 19 commits, 6 fix rounds)

1. **P&L truth-in-labeling** (`82b2776`, `5acbb86`): plot decomposition (`compute_decomposition`, pytest-covered), disclosure title/subtitle, status-doc update. Two plan defects found and fixed by workers during execution (decomposition formula; Christoffersen counts).
2. **Solver/error-path hardening** (`2987bd8`): laned-vs-scalar delta bound pinned ≤5e-8 at the solver's exact request shape; `checked_row_count` uint32 overflow guard; `VarScenarioStatus::ArchiveError` — corrupt archives fail the run even under `ExcludeFromDistribution`; timestamp-fault reclassification. Also restored the recurring dropped VAR CMake registrations.
3. **Projection safety knobs + telemetry** (`93d84b5`, `1b3beda`, `a7ee48d`): `max_session_gap_days`, `max_excluded_fraction`, `max_restrike_abs_log_moneyness` (defaults preserve behavior); cold-execution knob on `PreparedHistoricalProjection::evaluate_into` with VaR-path callers forced cold; `VarLegFrame::diagnostic_flags` (tenor extrapolation bits, wing-proximity bit); `n_gap_skipped` / `n_excluded_from_distribution` / `n_tenor_extrapolated_legs` counters populated on both routes; comparator field-coverage gap closed.
4. **Engine reporting API** (`1df442e`, `e8d7641`): `var_report.{hpp,cpp}` — byte-stable per-scenario TSV writer extracted from the bench, `attribute_by_underlier` (worst-first, uid-identity-verified against positional trust).
5. **Risk metrics + validation statistics** (`12af897`, `55869d7`): BRW/EWMA-weighted VaR/ES (`VarWeighting`), `historical_var_curve` (monotonicity guaranteed by construction), `kupiec_pof`, `christoffersen` (independence + conditional coverage) — reviewer independently recomputed every pinned value to float precision; bench `validation:` block.
6. **Invariant test backfill** (`68b6d0f`, `e4683dc`): pack-composition invariance, genuine-row-failure whole-scenario downgrade (bit-identical to forced scalar), fallback-tail-through-engine (mechanistic proof via evaluations > pass cap), thread-invariance with a mixed downgraded/normal fixture — mutation-verified.
7. **Base-mark harvest, guarded** (`0de0fd0`, `1d6bf17`): solver-pass cold marks reused for base valuation behind `VarBaseMarkSource` (default `DedicatedPricePass`). Measured: dedicated mark lanes −50.0% exactly (306,870 eliminated), Greek-route counters byte-identical, fallback tail 0.41%. **Default NOT flipped**: harvested price differs from the scalar-route price by ≤8.74e-14 relative (pre-existing kernel entry-point gap: `american_greeks_al().price` analytic=true vs `american_price` analytic=false — on this fixture the dedicated pass is all-scalar because groups average 1.87 lanes, under the 4-lane pack threshold). Flip evidence assembled: parity gates zero both knob states, YTD P&L drift $0.00097 on $6.54M.
8. **Dynamic scenario scheduling** (`dcc0998`): replay loop moved to `PricingExecutor::run_dynamic` under pinned bit-invariance (3 dynamic-vs-static bit-identity tests, mutation-verified); static path demoted to test-only; bench emits `resolved_workers`; loader loop deliberately kept contiguous (snapshot chaining, IO-bound).

**Final whole-branch review:** APPROVE-WITH-MUST-FIX-LIST → must-fix landed
(`27db849`): archive faults now classified from *both* snapshot loads (the
base-NotFound + shifted-corrupt combination previously slipped through as
excludable). Gate after fix: **98/98 green**
(`Var.*:ContractProjection.*:HistoricalProjection.*:VarReport.*:VarValidation.*`).

## Rerun & new baseline question

Post-sprint rerun at `27db849` (release, terminal SP100 case, deterministic —
bit-identical across t1/t8 process reruns):

- **Cumulative P&L $6,541,876.61** vs prior accepted $6,546,715.73 — **−$4,839.12 (−0.074%)**, caused by a 9-lot swing in `replay_excluded_option_lots` (351→360) attributed to the sprint's intentional classification fixes; surface DB unchanged. Not yet bisected to a single commit; prior trace preserved as `sp100_dispersion_ytd_pnl_cross_pre_rerun_backup.tsv`.
- **Validation (first ever for this engine): all pass** — 1 breach vs 1.06 expected at 99% over 106 scenarios; Kupiec p=0.953; Christoffersen p_ind=0.890, p_cc=0.989.
- **HTML report:** `C:\atx\artifacts\var\sp100_dispersion_ytd_var_report.html` (self-contained; leads with the baseline-discrepancy banner).

## Validation state

Binding gate 98/98 green at `27db849` (pre-merge). Full unfiltered suite
intentionally not run (user directive; the two known pre-existing failures —
`SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`,
`VolUmbrella.TierCountsMatchTheReadmeTable` — predate the branch and touch no
branch-owned file). Merge `9a6c612` also folded in main's concurrent
stress-day fit-fidelity sprint; post-merge gate rerun in progress at write time
— result to be recorded in the ledger.

## Open items

- **Decisions:** re-accept $6,541,876.61 as the P&L baseline or bisect the 9-lot exclusion swing; flip `VarBaseMarkSource` default (evidence assembled); post-merge full-suite sweep.
- **Follow-up tickets (final-review triage, class b):** triple-combination pin test (harvest + dynamic + mixed downgrade); share `var_frame_qualifies` with the unweighted statistics path; bench validation-block all-Ok guard; grouped-call fallback probe; `resolved_worker_count` dedup; plot-script edge tests; plan-doc stale Interfaces block; TSV name-escaping caveat; ExpiredBeforeShift ordering (M9); 2026-06-29 restrike-anchor anomaly ticket (base/shifted ~$7M off-level on the 06-29-anchored restrike, same-surface control normal, P&L unaffected — investigation recipe in the final review output) + PNG annotation.

## Commit map (var branch, base `82c6417`)

`fdb4783` plan → `84c0d2c` north star → `82b2776`/`5acbb86` P&L labeling (+`1fc42a4` plan fix) → `2987bd8` solver hardening → `93d84b5`/`1b3beda`/`a7ee48d` projection safety + telemetry → `1df442e`/`e8d7641` reporting API → `12af897`/`55869d7` risk metrics + validation → `68b6d0f`/`e4683dc` invariant backfill → `0de0fd0`/`1d6bf17` base-mark harvest → `dcc0998` dynamic scheduling → `27db849` final-review must-fix → merged `9a6c612`.

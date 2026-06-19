# Sprint S10 — Conviction Sizing & Regime-Adaptive Integration — Progress

**Worktree:** `atx-wt/s10-conviction-regime` (canonical `scripts/new-worktree.ps1` location;
the source plan proposed `.claude/worktrees/...` — changed at kickoff to use the dev-preset
tooling, see Plan adjustments)
**Branch:** `worktree-s10-conviction-regime`
**Base:** main @ `8a34a9e` (Merge sprint-8 — superseded the plan's stale `767c08b`)
**Started:** 2026-06-18
**Source plan:** [`../../research/rentech-improvement-sprint-plan.md`](../../research/rentech-improvement-sprint-plan.md)
**Source research:** [`../../research/rentech-structure-signals-domain-mapping.md`](../../research/rentech-structure-signals-domain-mapping.md)
**Prior progress:** sprint-8 (optimizer; `sprint-8-progress.md`)

## Plan adjustments vs. the source plan

Five adjustments were frozen at kickoff against the source implementation plan:

1. **Scope cut to S10-0…S10-5** (six units). The user deferred **S10-6** (walk-forward adaptation
   harness) and **S10-7** (cross-source data-validation gate) to a later sprint. They remain
   specified in the source plan and are lifted to the ROADMAP backlog at close.
2. **Base + worktree corrected.** The plan's `767c08b` predated the sprint-8 merge; the real base is
   `8a34a9e`. The worktree lives in `atx-wt/` (the `new-worktree.ps1` / `dev`-preset convention) rather
   than the plan's proposed `.claude/worktrees/` path, so clangd + sccache + shared deps wire up
   automatically.
3. **Test framework is GoogleTest**, not the `ATS_TEST` macro the generic `plans/docs/sprint.md`
   mentions. atx-engine tests use `TEST(Suite, Case)` / `EXPECT_*` in `tests/<group>/*_test.cpp`
   (auto-globbed, one exe per group: `atx-engine-<group>-tests`). Test counts are `Suite total/fail/skip`.
4. **AlphaMetrics has no IS/OOS Sharpe field** (it carries sharpe, turnover, returns, drawdown, margin,
   fitness, holding_days). The S10-1 conviction function therefore takes the OOS/IS stability ratio as
   an explicit caller-supplied scalar rather than reading it off AlphaMetrics.
5. **S10-4 capacity input is caller-supplied.** Wiring a full `ExecutionSimulator` ADV curve into the
   combiner is out of proportion for the unit; crowding takes a per-name remaining-capacity vector as
   input (the caller computes it from `cost/capacity.hpp` upstream), keeping the unit deterministic and
   self-contained. The correlation de-correlation core is unchanged.

Realistic scope for this sprint:

1. **S10-0** — Marker + ledger + scaffold (`combine/conviction.hpp`, `risk/kelly_sizing.hpp` fwd-decls).
2. **S10-1** — Conviction score: continuous [0,1] confidence from DSR / (1−PBO) / stability / explainability.
3. **S10-2** — Fractional-Kelly sizing: explicit, conviction-scaled, covariance-aware leverage over the factor model.
4. **S10-3** — Regime-conditioned combination: HMM posterior → per-regime combine weights (byte-identical fallback).
5. **S10-4** — Crowding / capacity-aware de-correlation at the combine step.
6. **S10-5** — Breadth instrumentation: effective-N + IR = IC·√breadth, report wiring.

Defer to a later sprint (lifted to ROADMAP backlog at close):

- **S10-6** — Walk-forward adaptation harness (re-fit/re-admit + decay). Deferred by the user.
- **S10-7** — Cross-source data-validation gate in ORATS ingest. Deferred by the user.

## Per-unit ledger

| Unit  | Status | Commit  | Notes |
|-------|--------|---------|-------|
| S10-0 | done   | `<sha>` | Opened this ledger; added forward-decl scaffold headers `combine/conviction.hpp` (ExplainFlag, ConvictionConfig, ConvictionScore) and `risk/kelly_sizing.hpp` (KellyConfig, KellyWeights), plus a scaffold smoke test that proves both compile/link under `/W4 /permissive- /WX`. No logic. `ConvictionScaffold 1/0/0`. Baseline `atx-engine-combine-tests` built green (130s, full engine lib) before scaffold. |
| S10-1 | done   | `<sha>` | Conviction score. `conviction()` blends four already-computed inputs into a continuous confidence in [0,1]: `clamp(dsr.dsr)` (DSR probability, weight 0.40), `clamp(1−pbo.pbo)` (less-overfit, weight 0.35), and `clamp(oos_is_ratio)` (OOS/IS Sharpe stability, weight 0.25) — the three weights sum to 1.0 so `base` ∈ [0,1] — then multiplies by an explainability factor (Explained 1.0, PartlyExplained 0.75, HeadScratcher 0.50 discount: "trade unexplained at reduced size"). It recomputes nothing, takes the OOS/IS ratio as a caller-supplied scalar (AlphaMetrics has no IS/OOS field), and returns the score plus its four component terms for report attribution. Fail-closed: a NaN DSR/PBO/ratio ATX_ASSERTs (abort) in debug. Pure, no RNG, order-fixed reduction (dsr→pbo→stability). `Conviction 9/0/0` (8 Conviction + 1 ConvictionDeathTest); `ConvictionScaffold 1/0/0` still green. |
| S10-2 | done   | `<sha>` | Fractional-Kelly sizing. `kelly_size()` computes the full-Kelly target `f* = V^{-1}mu` over the FACTORED covariance via `FactorModel::apply_inverse` (Woodbury — never materializes the MxM inverse), scales it by `cfg.kelly_fraction` (default quarter-Kelly: full Kelly overbets — Samuelson), scales each name by its conviction in [0,1] (a zero-conviction name ⇒ exactly 0 weight), then clamps gross leverage `Sum|w|` to `cfg.max_gross` (uniform scale preserving relative tilt; `scale_applied` records the factor, 1.0 when slack/disabled). It is the GP optimizer's TARGET — not a replacement QP (the cost-to-go machinery is untouched). Fail-closed: a non-finite `expected_alpha`/`conviction`, a length mismatch, a conviction ∉ [0,1], or a negative `kelly_fraction` ATX_ASSERTs (abort) in debug. Pure, no RNG, order-fixed (ascending instrument i throughout). `KellySizing 8/0/0` (6 KellySizing + 2 KellySizingDeathTest); full risk group 246/0/0 stays green. |
| S10-3 | pending | — | Regime-conditioned combination. |
| S10-4 | pending | — | Crowding / capacity-aware de-correlation. |
| S10-5 | pending | — | Breadth instrumentation. |

## Sprint S10 commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `<sha>` | S10-0 | ConvictionScaffold 1/0/0 |
| `<sha>` | S10-1 | Conviction 9/0/0 |
| `<sha>` | S10-2 | KellySizing 8/0/0 |
| `<sha>` | S10-3 | RegimeCombine … |
| `<sha>` | S10-4 | Crowding … |
| `<sha>` | S10-5 | Breadth … |
| `<sha>` | close | docs(s10-close): close sprint-10 — N units, M tests |

## What Sprint S10 proves / Next sprint priorities

S10 turns atx from "binary-admit, statically-combined" into "**conviction-sized, regime-adaptive,
breadth-instrumented**" — closing the precise gaps where the verified RenTech mapping (G1–G5) exceeded
the current implementation, without rebuilding the already-mature discovery, validation, and optimization
spines. The deferred **S10-6** (walk-forward adaptation) and **S10-7** (cross-source data validation)
remain the open RenTech gaps (G6, G7) for the next sprint. Hands the fund-level track a conviction-scaled,
regime-aware combined book and a breadth metric to allocate across sleeves.

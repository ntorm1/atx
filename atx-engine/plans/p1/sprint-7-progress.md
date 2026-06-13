# Sprint 7 — Portfolio Construction & Production Lifecycle — Implementation Progress

**Status:** 🔄 IN PROGRESS — S7-0 open
**Worktree:** in-place (branch `feat/atx-engine-book`; S7 does not use a dedicated worktree — explicit pathspecs only, no push)
**Branch:** `feat/atx-engine-book`
**Base:** `feat/atx-core-stdlib` @ `6445b5c` (branch cut from main @ 6445b5c)
**Started:** 2026-06-13
**Source plan:** [`sprint-7-portfolio-lifecycle-implementation-plan.md`](sprint-7-portfolio-lifecycle-implementation-plan.md)

---

## Plan adjustments vs. source / as-built reconciliation

The controller's kickoff recon against `main` @ `6445b5c` found the following corrections (these supersede plan §0.1/§0.9):

- **D1 — S6 cost layer MERGED.** The entire `cost/` layer is now in `main` (commit `e23daf8 merge: S6 cost-calibration & capacity`). Present & verified: `cost::cost_aware_knobs(const CalibratedCost&, f64 ref_participation, f64 ref_sigma, f64 horizon_days) -> CostKnobs{f64 kappa; combine::GateConfig gate; f64 fitness_cost_floor;}` (cost/cost_aware.hpp:151); `cost::capacity_point(std::span<const risk::CapacityPoint>) -> f64` zero-crossing (cost/capacity.hpp:63); `cost::should_trade(f64 expected_edge_bps, f64 predicted_cost_bps, f64 safety) -> bool` (cost/temp_perm.hpp:153). => S7 programs against `cost::` DIRECTLY; the §0.1 hard merge prerequisite is SATISFIED (no blocker). `book::CostInputs` is retained as the clean optimizer-facing scalar seam, not a fallback.

- **D2 — capacity_point present** (cost/capacity.hpp:63) => the local `book::detail::zero_crossing` fallback from §0.9 is UNNEEDED; use `cost::capacity_point` directly.

- **D3 — no Library positions accessor (CRITICAL).** `library::Library` (library.hpp) exposes public read-passthroughs `n_alphas, n_segments, segment_path, get, pnl, state_as_of, master_seeds, snapshot, mark, admit` but NO public `store()` or `positions()`. The underlying `LibraryStore::positions(AlphaId g, usize period) -> std::span<const f64>` exists (store.hpp:173). => S7-3 will add a 1-line `Library::positions(AlphaId, usize period)` read-passthrough mirroring the existing `pnl()` at library.hpp:227 (a SECOND additive engine-source touch besides FactorModelBuilder::build_components).

- **D4 — no admitted_ids/ids_in_state.** => the pipeline (§4.6) enumerates `for a in [0, n_alphas): if state_as_of({a}, t) == State` via a book/-local helper (no library edit).

- **D5 — build keys on PanelView.** `FactorModelBuilder::build(const PanelView&, usize window, std::span<const f64> market_cap, std::span<const u32> group_id) -> Result<FactorModel>` returns `FactorModel::create(X[0].x, F, D, /*fit_begin*/0, /*fit_end*/window)`. => `build_components` returns `FactorComponents{X[0].x, F, D, fit_end=window}` (fit_begin 0).

- **D6 — deflated_sharpe dsr==psr.** `eval::deflated_sharpe(f64 sr, usize T, f64 skew, f64 exkurt, usize N, std::optional<f64> var) -> DsrResult{f64 psr; f64 sr_star; f64 dsr; f64 haircut_sharpe;}` sets `dsr == psr` as-built (deflated_sharpe.hpp). => the decay gate's `dsr < dsr_admit` is effectively a PSR comparison; document it.

---

## Kickoff risks

### (a) Four Pattern-B / cross-module edges

1. **L7 QP / projected-gradient refinement → atx-core.** Ship on the as-built `risk::PortfolioOptimizer` projected/proximal loop; true OSQP-style ADMM is the recorded lift to atx-core.
2. **Symmetric eigensolver for dead-factor extraction → already in Eigen.** `Eigen::SelfAdjointEigenSolver` with a fixed sign convention; no atx-core request needed.
3. **The S6 `cost/` layer → MERGED (D1); no longer a prerequisite.** S7 programs against `cost::` directly.
4. **eRank / effective-breadth helper `(Σλ)²/Σλ²` → atx-core L6 residual.** Ship engine-local in `book/`.

### (b) S6-merge prerequisite check — SATISFIED

S6 cost-calibration branch merged into `main` at commit `e23daf8` (confirmed 2026-06-13 by controller recon). All D1–D6 as-built deltas recorded above. No merge blocker remains; S7 may proceed directly against `cost::` and the merged engine source.

### (c) Shared-branch / explicit-pathspec discipline

Branch `feat/atx-engine-book` is in the main worktree (no dedicated worktree). Explicit-pathspec commits only (`git add -- <paths>`; never `git add -A`); `git show HEAD --stat` after each commit to verify only the intended files appear; NEVER touch `atx-core/*` or `atx-tsdb/*`; do not push.

---

## Per-unit status

| Unit  | Title                                                                            | Status      | Commit SHA(s) | Tests | Notes |
|-------|----------------------------------------------------------------------------------|-------------|---------------|-------|-------|
| S7-0  | Marker + ledger + book scaffold + S6-merge prerequisite check                   | 🔄 in prog  |               | —     | ledger + `book/fwd.hpp` forward decls; S6 merge confirmed (D1). |
| S7-1  | Multi-period optimizer (`risk::MultiPeriodOptimizer`)                            | ⬜ pending   |               | —     | |
| S7-2  | Alpha-decay monitor (`book::DecayMonitor` + `book::DecayController`)            | ⬜ pending   |               | —     | |
| S7-3  | Dead-alpha recycler → risk factors (`risk::DeadFactorExtractor`)                 | ⬜ pending   |               | —     | Engine touch: `Library::positions` passthrough (D3). |
| S7-4  | Kelly / fractional-Kelly allocator (`book::AllocationConfig` + allocate())      | ⬜ pending   |               | —     | |
| S7-5  | End-to-end `book::BookPipeline` + `book::BookReport` capstone                   | ⬜ pending   |               | —     | |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| _(this commit)_ | S7-0 | docs(s7-0): open sprint-7 portfolio-lifecycle ledger + book scaffold |

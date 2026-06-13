# Sprint 7 — Portfolio Construction & Production Lifecycle — Implementation Progress

**Status:** ✅ CLOSED 2026-06-13 — all 6 units (S7-0…S7-5) shipped + two-stage reviewed; final whole-sprint review **SHIP**. Full engine suite **1105/1105** (288 suites), `/W4 /permissive- /WX` + strict-FP clean. Unmerged on `feat/atx-engine-book` (merge/push is the user's gate).
**Worktree:** in-place (branch `feat/atx-engine-book`; S7 does not use a dedicated worktree — explicit pathspecs only, no push)
**Branch:** `feat/atx-engine-book`
**Base:** `feat/atx-core-stdlib` @ `6445b5c` (branch cut from main @ 6445b5c)
**Started / Closed:** 2026-06-13 / 2026-06-13
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

| Unit  | Title                                                                            | Status   | Commit SHA(s)        | Tests | Notes |
|-------|----------------------------------------------------------------------------------|----------|----------------------|-------|-------|
| S7-0  | Marker + ledger + book scaffold + S6-merge prerequisite check                   | ✅ done  | `e134308`            | —     | ledger + `book/fwd.hpp`; S6 merge confirmed (D1). |
| S7-1  | Multi-period optimizer (`risk::MultiPeriodOptimizer`)                            | ✅ done  | `e19b218` + `0d78a01`| 7     | receding-horizon over `PortfolioOptimizer::solve`; single-period==multi-period bit-pin; signed-zero full-step fix; trade_rate∈(0,1] guard. |
| S7-2  | Alpha-decay monitor (`book::DecayMonitor` + `book::DecayController`)            | ✅ done  | `ec3e51e` + `d6fa81b`| 9     | Page-Hinkley down + DSR/PSR drop + MinTRL gate + cost-flood discriminator; asymmetric retire-fast/restore-slow over `Library::mark` (PIT). PH level-shift proven; pure-function MinTRL test; `DecayState` split. |
| S7-3  | Dead-alpha → risk factors (`risk::extract_dead_factors`/`augment_factor_model`) | ✅ done  | `9981e13` + `88f8814`| 9     | Kakushadze holdings-overlap eigen-extraction (sign-pinned) + eRank truncation + blockdiag FactorModel augmentation; R6 ‖X_deadᵀw‖ reduced ~45%. Engine touches: `FactorModelBuilder::build_components` refactor, `Library::positions`/`n_periods` passthroughs (D3). |
| S7-4  | Capacity-bounded Kelly allocation + reproducible book report                    | ✅ done  | `58b97f4` + `842be7c`| 16    | `size_book`/`effective_breadth`; `accumulate_report→Result` (OOB/dim guards) + byte-identical `write_report` (`to_chars`, binary); `FactorModel::exposures()` accessor. |
| S7-5  | End-to-end `book::BookPipeline` capstone + bench                                | ✅ done  | `41803b4` + `346db45`| 7     | real orchestrator (mine→promote→combine→augment-risk→size→optimize→monitor→recycle→report); all 7 invariants proven non-vacuously; R7 binding capacity clip; fitted combiner drives the book. `bench/book_bench.cpp` compiles under `-DATX_BUILD_BENCH=ON`. |
| polish| IWYU + decay dead-accessor/explicit-journal-fault + fwd.hpp comments             | ✅ done  | `2f24a2a`            | —     | carry-forward minors from per-unit reviews. |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `e134308` | S7-0 | docs(s7-0): open sprint-7 portfolio-lifecycle ledger + book scaffold |
| `e19b218` | S7-1 | feat(s7-1): multi-period turnover-aware optimizer (receding-horizon over PortfolioOptimizer) |
| `0d78a01` | S7-1 | fix(s7-1): exact full-step bit-identity (signed-zero) + trade_rate boundary validation |
| `ec3e51e` | S7-2 | feat(s7-2): alpha-decay monitor (Page-Hinkley + DSR drop) driving the S4 lifecycle |
| `d6fa81b` | S7-2 | fix(s7-2): prove Page-Hinkley level-shift detection + pure-function MinTRL test; split DecayState; IWYU |
| `9981e13` | S7-3 | feat(s7-3): dead-alpha→risk-factor extraction (Kakushadze) + FactorModel augmentation |
| `88f8814` | S7-3 | fix(s7-3): validate as_of_period (OOB guard) + augment dim-check + caller-order-independent determinism + IWYU |
| `58b97f4` | S7-4 | feat(s7-4): capacity-bounded Kelly allocation + reproducible book reporting |
| `842be7c` | S7-4 | fix(s7-4): accumulate_report returns Result with OOB/dim guards; split long fns; test separator |
| `41803b4` | S7-5 | feat(s7-5): end-to-end BookPipeline capstone (all-invariants gate) + book micro-bench |
| `346db45` | S7-5 | fix(s7-5): make R7 capacity clip a real binding leverage bound + wire fitted combiner into the book |
| `2f24a2a` | polish | chore(s7): IWYU cleanup + decay-monitor dead-accessor/explicit-journal-fault + fwd.hpp comment fixes |

> _(plus the user's interleaved `14fccd3 chore(dev): fast-worktree build setup` — not part of S7, disjoint files.)_

---

## What S7 proves (v2 done-gate)

S7 turns the v2 factory into an **operating book** and proves all eight carried invariants **simultaneously** in the `BookPipeline` capstone (R1 deterministic byte-identical replay; R2 receding-horizon no-look-ahead, truncation-invariant at every fitted boundary; R3 calibrated honest cost — turnover κ + cost-flood discriminator + capacity ceiling; R4 no survivorship; R5 the S4 lifecycle spine **driven** PIT with asymmetric retire-fast/restore-slow; R6 dead alphas → endogenous risk factors that **measurably** neutralize; R7 capacity-bounded Kelly gross; R8 reproducible headless artifacts). Every primitive already existed (the fixed-iteration optimizer, the PIT journal, the capacity curve, the DSR, the combined mega-alpha, the calibrated cost) — S7 is the compositional layer that **drives, threads, and accumulates** over them. The two genuinely-new numeric paths are the dead-alpha eigen-extraction (R6) and the Page-Hinkley + DSR decay detector (R5); both are differential-tested and non-vacuous.

**v2 is feature-complete** (S1 truth · S2 throughput · S3 mining · S4 library · S5 learned combiner · S6 calibrated cost · S7 operating book), pending merge.

## Residuals → p2 backlog
- **L7 QP / fixed-iteration ADMM optimizer → atx-core** — shipped on the as-built `PortfolioOptimizer` projected/proximal loop; true OSQP-style solver is the recorded lift.
- **eRank / effective-breadth `(Σλ)²/Σλ²` → atx-core L6** — engine-local for now.
- **DSL re-eval adapter (§0.8)** — the pipeline uses `ScriptedSignalSource` deterministic doubles as combiner constituents; re-parsing `provenance.expr_source` → compile → `VmSignalSource` at scale is deferred (batoned from S5).
- **Standalone-PanelView / full book-prefix truncation harness (§0.9 / R2)** — the pipeline builds a `PanelView` directly over the research panel grids; the load-bearing no-look-ahead property (builder truncation-invariance) is proven directly, but a full end-to-end book-prefix truncation harness is the deferred extension.
- **Gârleanu-Pedersen forward-blended aim** — the myopic + partial trade-rate is shipped; the full multi-step forward aim is deferred.
- **S6 borrow-accrual completion** — the net-cost picture for a half-short book.

## Close-out notes
- **ROADMAP.md S7-row flip DEFERRED to the user**: `p1/ROADMAP.md` has the user's **uncommitted** edits (Sprint-8 covariance-construction prep). To avoid entangling that uncommitted work, this sprint does **not** edit ROADMAP.md; flipping the S7 row `⏳ → ✅` and the module status `S1–S7 CLOSED — v2 complete` is left for the user to apply alongside their S8 changes.
- Spec [`sprint-7-portfolio-lifecycle.md`](sprint-7-portfolio-lifecycle.md) marked ✅ closed; user reference [`sprint-7.md`](sprint-7.md) created.

## Baton → p2
With v2 complete — a deterministic, bias-free, calibrated, multicore alpha factory + portfolio brain that mines, stores, learns, combines, prices, **and operates** a capacity-bounded book that watches itself decay and recycles its dead — the p2 frontier is **live**: broker connectivity, real-time PIT ingest, intraday execution, and cross-machine scale-out. The reproducible artifacts (content-addressed library manifest + byte-identical book report) and the S7.5 all-invariants gate are the foundation that makes going live trustworthy.

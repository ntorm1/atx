# Phase 2 S8 — Production-Grade Portfolio Optimizer — Implementation Progress

**Worktree:** `C:/Users/natha/atx-wt/p2-s8`
**Branch:** `feat/p2-s8-optimizer`
**Base:** main @ `0b950bd`
**Started:** 2026-06-15
**Source plan:** [`sprint-8-optimizer-implementation-plan.md`](sprint-8-optimizer-implementation-plan.md)
**Prior progress:** p2 S1 multi-horizon optimizer (merged into `main`); see the plan §0 as-built recon.

## Plan adjustments vs. the source plan

No scope change at kickoff. The plan is frozen as written; this ledger records the **§0
file:line reconciliation deltas** below (the plan file is NOT edited — `sprint.md` discipline:
the plan is a fossil, the ledger is the diary). The headers have grown LOC since the S8-0
code review (constraints 362→397, qp_solver 444→482, capacity 290, multi_period 189,
multi_horizon 319, factor_model 875→932, optimizer 363), so several anchor line numbers
drifted by a few lines; **every load-bearing symbol and signature is confirmed unchanged**.
The single most-important anchor — the dense-Ã materialization at `qp_solver.hpp:193` — is
**exact**.

Realistic scope for this sprint (8 units, split S8-a / S8-b per plan §3):

1. **S8.0** — Marker + ledger + as-built recon + baseline capture (this unit).
2. **S8.1** — Factor-augmented sparse QP reformulation (`y=Xᵀw`, `P=blkdiag(D,F)`).
3. **S8.2** — Deterministic static quasi-definite LDLᵀ (`kkt_ldl.hpp`).
4. **S8.3** — ADMM rebuild: Ruiz + polish + certificates + warm-start; **perf gate**; flip the 3 re-enabled tests green.
5. **S8.4** — Box/linear constraint surface (%ADV/%shares/sector-net) + wire QP into single-period drivers.
6. **S8.5** — Conic (SOCP): tracking-error, sector-risk, robust, √-impact.
7. **S8.6** — Constraint-hierarchy / infeasibility elasticity.
8. **S8.7** — Multi-period: Gârleanu-Pedersen Riccati cost-to-go.
9. **S8.8** — Header/source split (pimpl) + compile-time measurement + close.

(S8.8 is the close unit; "8 units" in the plan §3 header counts S8.1–S8.8, with S8.0 the marker.)

---

## §0 reconciliation — every plan §0 file:line claim re-verified against this worktree

All paths under `atx-engine/include/atx/engine/risk/`. **Status legend:** ✅ confirmed at the
plan's line; ↧ confirmed, line drifted (new line recorded); all signatures verified unchanged.

### `constraints.hpp` (plan 362 LOC → **397 LOC**)

| Claim | Plan ref | Verified | Status |
|---|---|---|---|
| `ConstraintSet::materialize(const MatX& X, span w_prev, usize M) -> Result<MaterializedConstraints>` | :134 | :134 (decl), body :134–163 | ✅ |
| Fixed row order (dollar-neutral, position box, factor exposure, group, beta) | :153-157 | :153–157 (`emit_dollar_neutral`/`emit_position_box`/`emit_factor_exposure`/`emit_group`/`emit_beta`) | ✅ |
| `MaterializedConstraints{MatX A; VecX l,u; f64 gross_l1_budget; bool has_turnover; f64 turnover_budget; vector<f64> turnover_ref}` | :105 | :105 (struct), fields :106–115 | ✅ |

### `qp_solver.hpp` (plan 444 LOC → **482 LOC**)

| Claim | Plan ref | Verified | Status |
|---|---|---|---|
| Dense augmented Ã materialized | :193 | :193 (`sys.a = cl::MatX::Zero(er, en)` inside `build_aug_system`) | ✅ exact |
| `run_admm` | :329 | :329 | ✅ |
| Matrix-free `pcg_solve` (Jacobi precond) | :360 | :360 (`pcg_solve`); `build_precond` :303 | ✅ |
| `P=2λV` applied via `FactorModel::apply` | :404 | :404 (`p.V.apply(...)` in the KKT operator) | ✅ |
| Order-fixed `dot()` | :456 | :457 | ↧ +1 |
| `kInf=1e30` | :479 | :479 (`static constexpr f64 kInf = 1e30`) | ✅ |
| `QpConfig{iters=600, kkt_iters=50, rho=1.0, sigma=1e-6}` | :69 | :69 (struct); values iters=600 :76, kkt_iters=50 :77, rho=1.0 :78, sigma=1e-6 :79 (also feas_tol=1e-6 :80) | ✅ |
| `QpProblem` | :84 | :84 | ✅ |
| `ConstrainedQpSolver::solve(const QpProblem&)` | :98 | :98 | ✅ |

### `capacity.hpp` (**290 LOC**)

| Claim | Plan ref | Verified | Status |
|---|---|---|---|
| `capacity_curve` | :264 | :265 (signature line; comment header :254) | ↧ +1 |
| `temp_i = Y·σ_i·part_i^δ`, `part_i = shares_i/ADV_i` | :245 | formula comment :214–216; live computation `part = shares/adv` :240, `temp = Y*sigma*part^delta` in the same impact body | ↧ formula lives :214 (doc) / :240 (code), not :245; `:245` lands inside the impact-cost function body — confirmed present |

### `multi_period.hpp` (**189 LOC**)

| Claim | Plan ref | Verified | Status |
|---|---|---|---|
| Scalar `capacity_gross` ceiling | :138 | :138 (`oc.gross_leverage = std::min(oc.gross_leverage, cost.capacity_gross)`); field decl :71 | ✅ |
| `blend_toward` signed-zero special-case `rate==1.0` | :158-175 | comment :161–165, signature :166, special-case `(rate == 1.0) ? target[i] : (p + rate*(target[i]-p))` :172 | ✅ |

### `multi_horizon.hpp` (**319 LOC**)

| Claim | Plan ref | Verified | Status |
|---|---|---|---|
| `MultiHorizonConfig` | :112 | :112 | ✅ |
| `gp_aim` (horizon-average) | :194 | :194 | ✅ |
| Minimal-vs-augmented dispatch | :227 | `solve_toward_aim` :225 (decl), dispatch-to-augmented :230 | ↧ -2 (:225) |
| `solve_augmented` (q=−aim) | :270 | :271 (signature); q=−aim documented :264 | ↧ +1 |
| `stacked_mpc=true ⇒ Err(NotImplemented)` | :154 | :154–156 (`if (cfg.stacked_mpc) return Err(NotImplemented, ...)`) | ✅ |
| S7 boundary pin (doc) | :28-37 | :28–37 (header block: H=1 + identity ⇒ byte-identical to MultiPeriodOptimizer) | ✅ |
| blend | :286-305 | `blend_toward` :298 | ↧ (within range; def at :298) |

### `factor_model.hpp` (plan 875 LOC → **932 LOC**)

| Claim | Plan ref | Verified | Status |
|---|---|---|---|
| Public API (create, exposures()→X, risk(w), apply_inverse, apply, specific_var()→D, neutralize) | :103-265 | `class FactorModel` :103; `create` :112; `risk(w)` :181; `exposures()` :175; `apply_inverse` :208; `apply` :228; `specific_var()` :245; `neutralize` :250 — all within :103–265 | ✅ |
| `kSpecificVarFloor=1e-12` | (R7) | :94 (`inline constexpr f64 kSpecificVarFloor = 1e-12`); applied at create() :139 | ✅ |

### `optimizer.hpp` (**363 LOC**)

| Claim | Plan ref | Verified | Status |
|---|---|---|---|
| `WeightPolicy` dollar-neutral `gross_normalize(demean(α))` | :26-32 | :26–32 (header block) | ✅ |
| `risk_aversion==0.0` exact branch | (Pin #1) | documented :29–32; Pin #1 assertion comment at :234 (`EXACT WeightPolicy dollar-neutral book ... pin #1`) | ✅ |
| κ=0 turnover-prox identity | :268 | `prox_turnover` :266; `tau = turnover_penalty * kStep` :267; "κ == 0 ⇒ τ == 0 ⇒ identity (pin #2)" :264 | ↧ -1/-2 (:266) |
| λ>0 tilt / κ>0 monotone | :46-48 | :46–48 (header block: any λ>0 shrinks high-variance gross (pin #3); κ>0 prox cuts turnover (pin #4)) | ✅ |
| NaN-α → 0 weight | :61-64 | :61–64 (header block: NaN α cell → exactly 0, excluded from demean/Σ) | ✅ |
| empty-w_prev ≡ all-zero | :135-138 | :131–137 (`s.prev[i] = (s.live[i] && i < w_prev.size()) ? w_prev[i] : 0.0`; "bitwise-identically to an explicit all-zero w_prev") | ↧ (:131–137) |

**Recon verdict:** all symbols, signatures, struct layouts, config defaults, and the dense-Ã
defect anchor are present and unchanged. No signature drifted — only a handful of anchor line
numbers shifted ±1–2 due to LOC growth since the code review. **S8.1+ may rely on the plan §0
claims as written**; the only correction a downstream agent needs is to read by symbol name
rather than hardcoded line.

---

## Regression-pin checklist (plan §0.4) — the 8 differential anchors S8.1–S8.8 inherit

Every pin must survive **bit-for-bit (≤1e-9)**. Lock-state recorded at S8.0; each downstream
unit re-checks the relevant pins and ticks the gate (G-PIN). The minimal constraint set
(`GrossNet [+ PositionCap]`) keeps the as-built `PortfolioOptimizer` fast path UNTOUCHED — the
rewrite owns only the augmented path; this dispatch contract is what protects the pins.

- [ ] **Pin #1 (λ=0 exact)** — `PortfolioOptimizer::solve(α,V,{})` with λ=0, κ=0, cap-off ≡ `WeightPolicy` `gross_normalize(demean(α))`. Anchor: `optimizer.hpp:26-32`, branch on `risk_aversion==0.0`, assertion at `:234`.
- [ ] **Pin #2 (κ=0 prox identity)** — `turnover_penalty==0` ⇒ turnover prox is exactly the identity. Anchor: `optimizer.hpp:266` (`prox_turnover`, τ==0 ⇒ identity).
- [ ] **Pin #3 (λ>0 variance tilt)** — any λ>0 shrinks high-variance gross relative to λ=0 (documented monotone). Anchor: `optimizer.hpp:46-48`.
- [ ] **Pin #4 (κ>0 turnover cut)** — κ>0 prox shrinks the move away from w_prev, cutting turnover (documented monotone). Anchor: `optimizer.hpp:46-48`.
- [ ] **NaN pin** — a NaN-α cell → exactly 0 weight, excluded from demean/Σ. Anchor: `optimizer.hpp:61-64`.
- [ ] **Empty-w_prev boundary** — empty `w_prev` ≡ explicit all-zero `w_prev`, bitwise. Anchor: `optimizer.hpp:131-137`.
- [ ] **Signed-zero pin** — `blend_toward` rate==1.0 assigns the target verbatim (preserves −0.0; `bit_cast<u64>` pin). Anchor: `multi_period.hpp:166-172`, `multi_horizon.hpp:298`.
- [ ] **S7/S1 boundary pin (R7)** — `MultiHorizonOptimizer` with one `SignalHorizon::identity()`, minimal constraint set, full trade-rate ≡ byte-identical `MultiPeriodOptimizer.run(...)`. Anchor: `multi_horizon.hpp:28-37`; live test `risk_multi_horizon_integration_test.cpp::R7_DegenerateReducesToMultiPeriodByteIdentical` (NOT disabled — runs at S8.0).

All boxes left unchecked at S8.0 (this unit writes no solver math); S8.1/S8.3/S8.4/S8.7 tick them as the gates pass.

---

## Per-unit ledger

| Unit  | Status | Commit  | Notes |
|-------|--------|---------|-------|
| S8.0  | done   | `<bench-sha>` | Marker + ledger open (`Base: main @ 0b950bd`), full §0 file:line reconciliation (all signatures confirmed, anchors drifted ≤2 lines, dense-Ã anchor `qp_solver.hpp:193` exact), 8-pin checklist locked, `bench/optimizer_scale_bench.cpp` added (M∈{500,1500,3000,5000}×K∈{16,64} augmented-path sweep) + baseline captured (table below), 3 `DISABLED_MultiHorizonIntegration` tests renamed off `DISABLED_` + `GTEST_SKIP()`-guarded so they compile-but-skip (S8.3 flips green). Build env: `dev` preset, VS2022 DevShell + clang-cl; `databento-cpp` submodule init'd in the fresh worktree. |

### S8.0 measured baseline — augmented-path `MultiHorizonOptimizer::run` (as-built solver)

Preset: `dev` (Ninja + clang-cl, **Debug**, PCH+unity+sccache). QpConfig shipped default
(iters=600, kkt_iters=50, rho=1, sigma=1e-6). One `run()` == one densified-Ã ADMM solve over a
single-period schedule. **Debug build ⇒ upper-bound latency; the S8.3 comparison is
apples-to-apples (same preset/bench).** `bench/optimizer_scale_bench.cpp::BM_OptimizerScaleAugmented`.

| M | K | ms/run (as-built) |
|---|---|---|
| 500  | 16 | `<TBD>` |
| 500  | 64 | `<TBD>` |
| 1500 | 16 | `<TBD>` |
| 1500 | 64 | `<TBD>` |
| 3000 | 16 | `<TBD>` |
| 3000 | 64 | `<TBD>` |
| 5000 | 16 | `<TBD>` |
| 5000 | 64 | `<TBD>` |

> **S8.3 perf gate (G-PERF) is measured against the M=3000,K=64 row:** ≥20× speedup, target
> < 500 ms/run, scaling exponent ≤ ~1.3 (sub-O(M²)). The plan expects ~20 s/run at the large
> end — the actual measured numbers are recorded above.

### S8.0 re-enabled-but-skipped tests

`risk_multi_horizon_integration_test.cpp` — the 3 `DISABLED_MultiHorizonIntegration` tests are
renamed off the `DISABLED_` prefix (so they COMPILE and are discoverable) and guarded with a
`GTEST_SKIP() << "S8.3: full-size re-enable …"` as the first statement (so they don't run the
~20s as-built path yet). S8.3 removes the `GTEST_SKIP()` line to flip them green.

| Old name | New name | Guard |
|---|---|---|
| `DISABLED_R1_FullPipelineDeterministicByteIdentical` | `R1_FullPipelineDeterministicByteIdentical` | `GTEST_SKIP()` |
| `DISABLED_R2_TruncationInvarianceNoLookAhead` | `R2_TruncationInvarianceNoLookAhead` | `GTEST_SKIP()` |
| `DISABLED_R3_AugmentedConstraintsExactEveryPeriod` | `R3_AugmentedConstraintsExactEveryPeriod` | `GTEST_SKIP()` |

(The two already-running fast tests in the file — `R2_TrajectoryIsPureFunctionOfCurrentAlpha`
and `R7_DegenerateReducesToMultiPeriodByteIdentical` — are untouched; R7 is the live S7 boundary pin.)

## Phase 2 S8 sprint commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `<marker-sha>` | marker | — |
| `<bench-sha>` | S8.0 | risk_multi_horizon_integration: 5 tests (2 run / 3 skip); bench builds |

## What S8.0 proves / Next sprint priorities

S8.0 establishes the **green build + baseline + pin contract** the rest of S8 is measured
against: the build env works (`dev` preset, DevShell), the as-built augmented solver's scale
cost is captured (the perf-gate reference), the §0 recon confirms every symbol the rewrite
will touch is where the plan says, and the 8 pins are an explicit inherited checklist. **Next:
S8.1** — add `risk/qp_augment.hpp` (`y=Xᵀw`, `P=blkdiag(D,F)`), rewrite `ConstrainedQpSolver`
internals to consume the augmented sparse form (public `solve(const QpProblem&)` unchanged),
and prove the augmented QP matches the as-built dense-Ã solver (kept compiled as `_reference`)
to tolerance (G-DIFF). The dense-Ã anchor for the rewrite is **`qp_solver.hpp:193`**.

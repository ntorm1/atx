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
| S8.0  | done   | `0bd6389` | Marker + ledger open (`Base: main @ 0b950bd`), full §0 file:line reconciliation (all signatures confirmed, anchors drifted ≤2 lines, dense-Ã anchor `qp_solver.hpp:193` exact), 8-pin checklist locked, `bench/optimizer_scale_bench.cpp` added (M∈{500,1500,3000,5000}×K∈{16,64} augmented-path sweep) + baseline captured (table below), 3 `DISABLED_MultiHorizonIntegration` tests renamed off `DISABLED_` + `GTEST_SKIP()`-guarded so they compile-but-skip (S8.3 flips green). Build env: `dev` preset, VS2022 DevShell + clang-cl; `databento-cpp` submodule init'd in the fresh worktree. |
| S8.2  | done   | `b78ad99`→`a1bd42b` | Deterministic no-pivot quasi-definite LDLᵀ. 3 commits: `b78ad99` `kkt_ldl.hpp` (`QuasiDefiniteLdl` — `factor_symbolic` = AMD ordering via `Eigen::AMDOrdering<int>` + elimination tree + L pattern; `factor_numeric` = QDLDL-style no-pivot numeric pass, no sqrt, mixed-sign D; `solve` = fwd-L/D⁻¹/bwd-Lᵀ, division-free) + `risk_kkt_ldl_test.cpp`, `5867092` wire into `run_admm` (factor once per solve, reuse every iteration; dropped `<Eigen/SparseCholesky>` + the interim `SimplicialLDLT`; public API unchanged), `a1bd42b` relax the augmented diff gate to 1e-8 + fold in review nits. **Tests:** `RiskKktLdl` 7/7 green; `RiskQpSolver` 13/13; `RiskMultiHorizon` 15/15; `RiskQpAugment` 6/6 (the diff-gate battery re-confirmed at the relaxed 1e-8 — see the regression note below). QDLDL algorithm reimplemented from scratch (NOT linked — osqp/qdldl is Apache-2.0, idea vendored only). |
| S8.1  | done   | `16b00de`→`ff304d2` | Factor-augmented sparse QP reformulation. 6 commits: `16b00de` preserve as-built dense-Ã solver as `qp_solver_reference.hpp` (the diff oracle — verified byte-for-byte identical to as-built bar the `risk`→`risk::reference` namespace), `edd49a3` `qp_augment.hpp` assembly (`y=Xᵀw` factor-definition rows, `P=blkdiag(2λD,2λF)`, all-sparse Ã — no dense materialization), `484747a` rewrite `ConstrainedQpSolver` onto the augmented sparse KKT (interim `Eigen::SimplicialLDLT`, tagged for S8.2 replacement; public `solve(const QpProblem&)` unchanged), `950e824`+`6b1508f` diff-gate test (`risk_qp_augment_test.cpp`), `ff304d2` review nits. **Tests:** `RiskQpAugment` 6/6 green — 3 structural (Hessian block-diag-DF, factor-definition rows, gross/turnover sparse aux rows), `DollarNeutralRecoversAnalyticOptimum`, `TwoSolvesByteIdentical` (same-path determinism via `bit_cast`), and `MatchesDenseOracleAcrossBattery` (the G-DIFF gate). `RiskQpSolver` 13/13 still green. Built via `ninja` CI preset (PCH-off non-unity; the `dev` preset has a pre-existing unrelated ODR clash in `data_*_test.cpp`, not ours). |

### S8.0 measured baseline — augmented-path `MultiHorizonOptimizer::run` (as-built solver)

Preset: `dev` (Ninja + clang-cl, **Debug**, PCH+unity+sccache). One `run()` == one densified-Ã
ADMM solve over a single-period schedule. `bench/optimizer_scale_bench.cpp::BM_OptimizerScaleAugmented`,
run `--benchmark_min_time=1x` (exactly one execution/case). `--benchmark_out` JSON at
`build/s8_baseline.json`.

**Fixed iteration budget = iters=4, kkt_iters=4 (16 dense-Ã PCG matvecs/solve) — NOT the
shipped iters=600/kkt_iters=50 (30,000 matvecs).** The shipped budget is intractable as a
Debug baseline: at iters=80/kkt=20 a single **M=500** solve already measured **~83 s**, and the
dense Ã is ~M×3M so the per-solve cost is **O(M²) in M** — M=5000 at the shipped budget would be
**many hours**. Wall time is **linear in the matvec count**, so the shipped-budget equivalent ≈
measured × (30000/16) ≈ **measured × 1875**. Holding iters FIXED isolates exactly what the S8.3
gate measures — the per-iteration dense-Ã cost and its O(M²) scaling — without the iteration count
confounding it. **S8.3 re-runs THIS bench at THIS SAME fixed budget**, so the ≥20× comparison is
apples-to-apples (the rewrite's win is the per-iteration matvec going O(M²)→O(M·K²)).

| M | K | ms/run @ 4×4 budget (measured) | ≈ shipped-budget equiv (×1875) |
|---|---|---|---|
| 500  | 16 | 1,660.8   | ~0.86 h |
| 500  | 64 | 1,535.6   | ~0.80 h |
| 1500 | 16 | 16,318.8  | ~8.5 h |
| 1500 | 64 | 14,847.0  | ~7.7 h |
| 3000 | 16 | 58,337.6  | ~30 h |
| 3000 | 64 | 62,033.7  | ~32 h |
| 5000 | 16 | 167,241.3 | ~87 h |
| 5000 | 64 | 180,094.5 | ~94 h |

**Scaling (real_time, K=64, the dominant column):** M 500→1500 (3×) is 1.54s→14.85s ≈ **9.6×**;
1500→3000 (2×) is 14.85s→62.03s ≈ **4.2×**; 3000→5000 (1.67×) is 62.03s→180.09s ≈ **2.9×**. Each
ratio tracks **M² almost exactly** (3²=9, 2²=4, 1.67²=2.8) — empirical confirmation of the
as-built **O(M²) dense-Ã defect** (§0.2). The log-log fit exponent is **≈ 2.0**.

> **S8.3 perf gate (G-PERF), measured against the M=3000,K=64 row (62,033.7 ms @ 4×4 budget):**
> ≥ 20× speedup, target < 500 ms/run, scaling exponent ≤ ~1.3 (sub-O(M²)). The plan's "~20 s/run"
> estimate was at M≤1000 (the old `multi_horizon_bench` scale) and the shipped 600×50 budget; at
> M=3000 the O(M²) dense path is dramatically worse (~30+ h projected at the shipped budget). The
> actual measured per-(M,K) numbers (at the documented fixed baseline budget) are recorded above —
> S8.3 compares like-for-like against them.

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

### S8.1 G-DIFF gate result + review gates

**G-DIFF (the augmented solver matches the as-built dense-Ã oracle at the optimum).**
`RiskQpAugment.MatchesDenseOracleAcrossBattery` — 11-case M×K battery (M∈{50,120}, K∈{4,16,64},
every constraint family: box / factor / group / beta / gross / turnover, alone and combined),
both solvers run to the same per-case converged fixed ADMM budget. Asserted
`‖w_new − w_ref‖∞ ≤ kDiffTol = 1e-11`; achieved battery-wide worst is **2.9e-15** (every case at
1e-15…1e-16 machine precision). The gate is **agreement-at-optimum, NOT bit-identity** between the
PCG oracle and the direct-KKT augmented solver — the two solvers target the identical QP (same ρ=1,
σ=1e-6, feas_tol, α=1 no over-relaxation) and converge to the same unique minimizer, but mid-iteration
iterates differ. **PASS** (the dense O(M²) oracle makes this a ~24-min Debug run; the in-test M is
capped at 120 because the differential cost is dominated by the oracle, and the reformulation's
correctness is M-independent — a wider M-spread to 800 was verified out-of-test to the same bound).

**Diff-gate tolerance plan-text correction:** the plan §3 S8.1 line phrases the target as
"bit-identical on the minimal set" with a ≤1e-10/1e-8 fallback. The implemented gate is
agreement-at-optimum at 1e-11 (PCG vs direct-KKT cannot be bit-identical mid-iteration). This is the
correct invariant for S8.1; **minimal-set bit-identity is an S8.4 fast-path-dispatch pin, not an S8.1
deliverable** (no `GrossNet`-only case is in the S8.1 battery — every case runs the augmented path).

**Review gates (both two-stage subagent reviews — reserved for the solver-math units):**
- **Spec/math review — VERDICT: SHIP-WITH-NITS, zero blockers.** 9/9 claims PASS: P=blkdiag(2λD,2λF)
  with the factor-of-2 and λ matching the as-built `P=2λV` convention; factor rows encode `y−Xᵀw=0`
  exactly (correct sign, correct Xᵀ orientation); no dense Ã on the augmented path; oracle is a
  faithful byte-for-byte copy; diff gate uses a genuinely non-diagonal SPD F; determinism (R1) holds
  with no RNG and fixed iteration counts; public API unchanged; interim Eigen LDLT clearly tagged for
  S8.2; all 6 constraint families correctly encoded with gross/turnover L1-split rows byte-identical
  between augment and oracle.
- **Code-quality review — VERDICT: SHIP-WITH-NITS, zero blockers.** Compiles clean under the real
  `-W4 -WX` flags (zero warnings); triplet emission fully deterministic (no unordered containers,
  fixed ascending traversal); index/width arithmetic safe at the stated scale; no needless
  MatX/SparseMatrix copies (RVO + `std::move`); const/noexcept consistent with the risk module; the
  `build_kkt` per-column σ-insertion is correct even at λ=0.

**Nit dispositions:**
- FIXED (`ff304d2`): dropped the unused `#include <utility>` in `qp_augment.hpp`; corrected the
  stale "≤ kDiffTol (1e-9)" docstring to 1e-11 in the battery test.
- DEFERRED to S8.2: `qp_solver.hpp` KKT index widths are `int` (Eigen default `StorageIndex`), safe
  at n≈3·M scale but unguarded — S8.2 (which owns the production factorization) should add a debug
  `ATX_CHECK` on `dim` width. The interim `kkt_iters` config field is a no-op in the rewritten solver
  (retained for API compat).
- ACCEPTED as-is: the `GrossAndTurnoverSplitsAreSparseAuxRows` sparsity assertion is soft
  (`nnz/rows < n`, near-tautological); the two value-exact assembly tests (Hessian block-diag,
  factor-definition rows) carry the structural coverage, so this is adequate.
- DOC drift (plan, not edited — `sprint.md` discipline): plan §2 file-map lists the test under
  `tests/src/`; the actual file lives in `atx-engine/tests/` (the root), which is what the
  `CONFIGURE_DEPENDS *_test.cpp` glob picks up. Cosmetic; the gate compiles and runs.

**Pins:** S8.1 ticks none of the 8 regression pins directly (they live on the as-built fast path,
which the augmented rewrite does not touch — the dispatch contract protects them). The full-pipeline
pin checks (the 3 re-enabled `R1/R2/R3` tests) remain `GTEST_SKIP`-guarded until S8.3.

### S8.2 result — deterministic no-pivot quasi-definite LDLᵀ + the diff-gate regression it forced

**The factorization (G-DIFF for the LDL kernel itself, R11).** `risk_kkt_ldl_test.cpp` — `RiskKktLdl`
7/7. Reconstruction `‖PᵀLDLᵀP − K‖∞/‖K‖∞`: well-conditioned synthetic battery ≤ 1e-12; real
augmented KKTs (σ=1e-6 ⇒ κ~1e6) worst ≈ 2.3e-10 (gate 1e-9). Inertia cross-check: the count of
NEGATIVE D pivots equals the constraint-row count `r` on every case (real + synthetic) — confirms the
quasi-definite sign structure held with no pivot reordering. Solve accuracy `‖Kx−rhs‖∞ ≤ 1e-9`,
cross-checked `‖x − x_dense_LDLT‖∞ ≤ 1e-9` vs a dense (pivoting) `Eigen::LDLT` reference.

**G-DET (determinism — first unit to tick this gate).** Same K factored twice ⇒ byte-identical
perm/Lp/Li/Lx/D (`bit_cast<u64>`); the full augmented book is byte-identical across
`Eigen::setNbThreads({1,2,4,8})`. The factorization is purely serial + order-fixed (ascending CSC
reductions, no unordered container governs accumulation, AMD ordering is a pure function of the
sparsity pattern), and the zero-pivot path returns `Err` (R3) rather than dividing. **G-DET: PASS.**

**Review gates (both two-stage subagent reviews — this was flagged the riskiest unit of the sprint):**
- **Spec/math review — VERDICT: SHIP-WITH-NITS, zero blockers.** 9/9 claims PASS. The reviewer ran an
  INDEPENDENT 2000-trial randomized adversarial battery (varied coupling, skewed block sizes):
  reconstruction ≤ 5.5e-16 well-conditioned, inertia exact every trial, zero failures. It independently
  adjudicated the σ=1e-6 reconstruction gap as HONEST conditioning, not a bug: recon error (3.1e-10 at
  κ up to 1.6e7) sits BELOW κ·eps (3.5e-9) — normwise-backward-stable; Eigen's dense LDLᵀ reconstructs
  tighter (4.4e-12) only because it pivots (Bunch-Kaufman), the accepted determinism-for-pivoting
  tradeoff.
- **Code-quality review — VERDICT: SHIP-WITH-NITS, zero blockers.** The hand-rolled sparse-kernel index
  arithmetic is sound: every L-column write is bounded by `Lnz_[c]` (reach-count invariant verified),
  workspaces reset per consumed column (no stale read), no retained pointer to K after factor, no
  unordered container / float-sort governing order. Warning-clean under clang-cl `/W4 /permissive- /WX`.

**THE DIFF-GATE REGRESSION (key decision record).** Swapping the ADMM x-update from the interim
`Eigen::SimplicialLDLT` to the production no-pivot `QuasiDefiniteLdl` regressed the S8.1 augmented diff
gate (`RiskQpAugment.MatchesDenseOracleAcrossBattery`), which was asserting agreement-with-oracle at
**1e-11**. Full-battery diagnosis (every case's gap captured): only **3 of 11** cases exceed 1e-11 —
the gross/turnover **L1-split** cases, whose σ=1e-6-only aux-column diagonals make the KKT
ill-conditioned (κ~1e6) — sitting in a tight band **3.7e-10…5.9e-10** (battery-wide worst **5.9e-10**);
the other 8 cases stay at machine precision. This is honest no-pivot conditioning at the
backward-stability floor (~κ·eps ≈ 1e-9), NOT a defect: the interim Eigen LDLT hit 2.9e-15 only because
it PIVOTS — precision deliberately traded for determinism (R1/R5), and the factor's correctness is
proven independently by the kkt_ldl gates above. **Resolution:** relax `kDiffTol` 1e-11 → **1e-8** (the
plan §3 S8.1 target; ~17× margin over the achieved worst). The 1e-11 was an S8.1 over-tightening that
measured Eigen's pivoting precision rather than the reformulation's correctness. **S8.3's planned
deterministic polish (iterative refinement on the KKT solve) is the lever to tighten this back** if a
sub-1e-8 augmented-vs-oracle agreement is later wanted. Committed in `a1bd42b`.

**Nit dispositions (both reviews):**
- FIXED (`a1bd42b`): dropped the write-only `perm_inv_` member (`solve()` applies `perm_` both
  directions); added a `solve()` span-length precondition `assert`; corrected the `build_permuted_upper`
  comment (assembly is duplicate-free, the diagonal is seeded by assignment not summed); formatted the
  `RecordProperty` bounds in scientific notation (`std::to_string` rounded ~1e-10 to `"0.000000"`).
- DEFERRED: exact-zero pivot guard (`D_[k]==0.0`) could use a relative-magnitude floor for robustness on
  near-singular non-QD input — out of contract for the regularized KKT (σ,ρ>0 bound pivots away from 0),
  both reviews call it optional. Cross-solve symbolic caching (keyed by constraint sparsity pattern on a
  mutable solver member) is deferred — the clean `factor_symbolic`/`factor_numeric` seam ships and
  `WarmSymbolicMatchesColdFactorByteForByte` proves a re-numeric over an existing symbolic is bit-identical
  to a cold factor, so caching is addable later without API churn.

## Phase 2 S8 sprint commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `8a312b6` | marker | — |
| `0bd6389` | S8.0 | risk_multi_horizon_integration: 5 tests (2 passed / 3 skipped / 0 failed); atx-engine-bench builds + runs |
| `16b00de`→`ff304d2` (6) | S8.1 | RiskQpAugment 6/6 (incl. G-DIFF battery PASS @ 1e-11, worst 2.9e-15 — gate later moved to 1e-8 at S8.2); RiskQpSolver 13/13; both review gates SHIP-WITH-NITS, zero blockers |
| `b78ad99`→`a1bd42b` (3) | S8.2 | RiskKktLdl 7/7 (recon ≤2.3e-10, inertia exact, G-DET bit-identical across {1,2,4,8} threads); RiskQpAugment 6/6 (G-DIFF battery re-confirmed @ 1e-8, worst 5.9e-10 — no-pivot honest conditioning); RiskQpSolver 13/13; RiskMultiHorizon 15/15; both review gates SHIP-WITH-NITS, zero blockers (riskiest unit; independent 2000-trial adversarial battery clean) |

## What S8.0 proves / Next sprint priorities

S8.0 establishes the **green build + baseline + pin contract** the rest of S8 is measured
against: the build env works (`dev` preset, DevShell), the as-built augmented solver's scale
cost is captured (the perf-gate reference), the §0 recon confirms every symbol the rewrite
will touch is where the plan says, and the 8 pins are an explicit inherited checklist. **Next:
S8.1** — add `risk/qp_augment.hpp` (`y=Xᵀw`, `P=blkdiag(D,F)`), rewrite `ConstrainedQpSolver`
internals to consume the augmented sparse form (public `solve(const QpProblem&)` unchanged),
and prove the augmented QP matches the as-built dense-Ã solver (kept compiled as `_reference`)
to tolerance (G-DIFF). The dense-Ã anchor for the rewrite is **`qp_solver.hpp:193`**.

# `atx::engine::risk` — Risk Model + Portfolio Optimizer

This module is the risk and portfolio-construction engine. It has two halves that
meet at one object, the `FactorModel`:

1. **Estimation** — turn raw panel returns + exposures into a *factored* covariance
   model `V = X F Xᵀ + D` (Barra-style: factor loadings `X`, factor covariance `F`,
   idiosyncratic diagonal `D`).
2. **Optimization** — given an alpha signal `α`, the factored `V`, a previous book
   `w_prev`, and a set of constraints, produce the target book `w`.

The covariance is **never** materialized as a dense `M×M` matrix. Everything the
optimizer needs from `V` (variance, `V⁻¹` apply) is computed in factor space via the
Woodbury identity, so a solve is `O(iters·(M·K + K³))`, never `O(M²)`.

> **For future agents:** the load-bearing invariants of this module are
> **determinism** (R1) and **byte-identical boundary behavior** (R10). Read
> [Design rules](#design-rules) and [Determinism](#determinism--testing) before
> changing anything in the optimizer or solver. A change that flips a regression pin
> is a bug, not a feature.

---

## File map

### Spine / shared
| File | Role |
|---|---|
| [fwd.hpp](atx-engine/include/atx/engine/risk/fwd.hpp) | Forward declarations of the risk spine (`FactorModel`, `PortfolioOptimizer`, `StyleFactor`, …) so headers that only *name* these types avoid the heavy includes. |
| [factor_model.hpp](atx-engine/include/atx/engine/risk/factor_model.hpp) + [src/risk/factor_model.cpp](atx-engine/src/risk/factor_model.cpp) | `FactorModel` (the factored `V`, pimpl) + `FactorModelBuilder` (estimation entry). |
| [reference_data.hpp](atx-engine/include/atx/engine/risk/reference_data.hpp) | `CapacityRef` — borrowed %ADV / %shares / price / NAV inputs for participation & ownership caps. |

### Estimation (builds `V`)
| File | Role |
|---|---|
| [exposures.hpp](atx-engine/include/atx/engine/risk/exposures.hpp) | `StyleFactor`, `FactorModelConfig`, `CovarianceConfig`, `build_exposures` → exposure matrix `X`. |
| [stat_factor_model.hpp](atx-engine/include/atx/engine/risk/stat_factor_model.hpp) | APCA (Connor-Korajczyk) latent statistical factors. |
| [cov_ewma.hpp](atx-engine/include/atx/engine/risk/cov_ewma.hpp) | Split-half-life EWMA factor covariance `F` + Newey-West HAC + SPD floor. |
| [shrinkage.hpp](atx-engine/include/atx/engine/risk/shrinkage.hpp) | Ledoit-Wolf constant-correlation shrinkage + Marchenko-Pastur clip (standalone). |
| [specific_risk.hpp](atx-engine/include/atx/engine/risk/specific_risk.hpp) | Idiosyncratic diagonal `D` (EWMA + NW + structural blend). |
| [eigen_adjust.hpp](atx-engine/include/atx/engine/risk/eigen_adjust.hpp) | Menchero-Wang-Orr Monte-Carlo eigenfactor de-biasing (seed-pinned). |
| [psd_repair.hpp](atx-engine/include/atx/engine/risk/psd_repair.hpp) | Higham nearest-correlation + eigenvalue clip (standalone). |
| [vol_regime.hpp](atx-engine/include/atx/engine/risk/vol_regime.hpp) | Volatility-regime λ² rescale of `F`,`D`. |
| [dead_factor.hpp](atx-engine/include/atx/engine/risk/dead_factor.hpp) | Recycle retired alphas' holdings into orthogonal risk factors. |
| [horizon_blend.hpp](atx-engine/include/atx/engine/risk/horizon_blend.hpp) | Convex blend of short/long-horizon covariances. |
| [capacity.hpp](atx-engine/include/atx/engine/risk/capacity.hpp) | AUM → net-edge capacity curve (research report). |

### Optimization (uses `V`)
| File | Role |
|---|---|
| [optimizer.hpp](atx-engine/include/atx/engine/risk/optimizer.hpp) | `PortfolioOptimizer` — single-period solve; the **dispatch** between the fast path and the augmented QP. |
| [constraints.hpp](atx-engine/include/atx/engine/risk/constraints.hpp) | `ConstraintSet` descriptors + `materialize()` → `MaterializedConstraints` (linear rows + cone/elasticity metadata). |
| [cone.hpp](atx-engine/include/atx/engine/risk/cone.hpp) | Second-order-cone descriptors + deterministic SOC/ball projections (tracking-error, sector-risk, robust-alpha). |
| [qp_augment.hpp](atx-engine/include/atx/engine/risk/qp_augment.hpp) | `build_augmented` — assemble the sparse factor-augmented QP (`AugmentedQp`). |
| [qp_solver.hpp](atx-engine/include/atx/engine/risk/qp_solver.hpp) | `ConstrainedQpSolver` — deterministic ADMM (Ruiz + KKT + cone z-projection + polish + certificate). |
| [kkt_ldl.hpp](atx-engine/include/atx/engine/risk/kkt_ldl.hpp) | `QuasiDefiniteLdl` — no-pivot quasi-definite LDLᵀ, factored once per solve. |
| [qp_solver_reference.hpp](atx-engine/include/atx/engine/risk/qp_solver_reference.hpp) | Frozen dense-Ã ADMM oracle — **test-only** differential anchor. |
| [elasticity.hpp](atx-engine/include/atx/engine/risk/elasticity.hpp) | `solve_elastic` — minimize-violation relaxation of elastic constraints by priority. |

### Multi-period
| File | Role |
|---|---|
| [multi_period.hpp](atx-engine/include/atx/engine/risk/multi_period.hpp) | `MultiPeriodOptimizer` — receding-horizon walk; reuses the single-period solve verbatim. |
| [multi_horizon.hpp](atx-engine/include/atx/engine/risk/multi_horizon.hpp) + [src/risk/multi_horizon.cpp](atx-engine/src/risk/multi_horizon.cpp) | `MultiHorizonOptimizer` — constrained multi-horizon driver (forecast → GP aim → dispatch → first-move). |
| [horizon.hpp](atx-engine/include/atx/engine/risk/horizon.hpp) | Decay-weighted forecast trajectory `α_t → alpha[h][i]`. |
| [garleanu_pedersen.hpp](atx-engine/include/atx/engine/risk/garleanu_pedersen.hpp) + [src/risk/garleanu_pedersen.cpp](atx-engine/src/risk/garleanu_pedersen.cpp) | `gp_aim_and_value` — Gârleanu-Pedersen cost-to-go (scalar-Λ reduction). |

---

## The covariance model — `FactorModel`

[factor_model.hpp](atx-engine/include/atx/engine/risk/factor_model.hpp) holds `V`
**factored**: exposures `X` (`M×K`), factor covariance `F` (`K×K`), specific variance
diagonal `D` (`M`). It precomputes the Cholesky of the **capacitance** matrix
`C = F⁻¹ + Xᵀ D⁻¹ X` so the Woodbury inverse is a `K×K` solve, not an `M×M` one.

Key methods (everything the optimizer touches `V` through):
- `n_instruments()` / `n_factors()` → `M`, `K`.
- `exposures()` / `factor_cov()` / `specific_var()` → read `X`, `F`, `D`.
- `risk(w)` → portfolio variance `(Xᵀw)ᵀ F (Xᵀw) + Σ Dᵢ wᵢ²`, `O(MK + K²)`.
- `apply_inverse(in, out)` → `V⁻¹·in` via cached Cholesky, `O(MK + K³)`. **No dense `V`.**
- `neutralize(signal)` → residualize a signal on the exposures (ridge `1e-10`).

**Pimpl (S8.8a):** the Eigen members (`x_`, `f_`, `d_`, `dinv_`, `cap_llt_`) live in
`struct Impl` behind a `unique_ptr` in [src/risk/factor_model.cpp](atx-engine/src/risk/factor_model.cpp).
The class keeps **value semantics** (deep-copying copy/assign, noexcept move) because
`std::optional<FactorModel>` and by-value `factor_override` consumers depend on it. This
split is a pure compile refactor — zero behavior change.

### Estimation control flow

```
build_exposures(panel, cfg, row, market_cap, group_id)
    → ExposureMatrix X            (z-scored styles + sector dummies, PIT)

FactorModelBuilder::build_components(panel, window, …)
    → Pass A: OLS equal-weight → bootstrap specific d0
    → Pass B: WLS (weight 1/d0) → factor-return series f, residuals u
    → covariance pipeline:
        ewma_factor_covariance(f, vol_hl, corr_hl, nw_lags) → F (K×K SPD)
        specific_risk_blend(x0, u, …)                       → D (M > 0)
        [opt] eigen_adjust(F, sims, amplify, seed)          → F̂
        [opt] vol_regime_multiplier(f, F)                   → λ², rescale F,D
        [opt] horizon_blend(F_short, F_long, w)             → F
    → FactorComponents{X, F, D, fit_end}

[opt] augment_factor_model(components, dead_factors)        → hstack X, blockdiag F

FactorModel::create(X, F, D, fit_begin, fit_end)
    → floor D, cache D⁻¹, build + Cholesky capacitance C
    → FactorModel  (ready for the optimizer)
```

Every reduction is order-fixed (ascending row / instrument / factor / lag). The only
RNG is in `eigen_adjust`, and it is seed-pinned with a canonical draw order, so the
whole estimation is byte-identical across runs and threads.

---

## The optimizer engine

The single-period entry is `PortfolioOptimizer::solve(alpha, V, w_prev)` in
[optimizer.hpp](atx-engine/include/atx/engine/risk/optimizer.hpp). It objective is

```
maximize_w   αᵀw − λ·wᵀVw − κ·‖w − w_prev‖₁
subject to   Σ w = 0,   Σ|w| ≤ L,   |wᵢ| ≤ cap   (+ optional richer constraints)
```

### Dispatch — two engines behind one call

`solve()` routes by what constraints are attached:

| Attached `constraints` | Route | Why |
|---|---|---|
| none, or **minimal** (`GrossNet` + optional `PositionCap`) | **fast path** `solve_fast` | This is exactly the algebra the projected/proximal loop expresses; the regression pins exercise this verbatim. |
| **non-minimal** (any factor / group / beta / turnover / participation / ownership / sector / cone row) | **augmented QP** `solve_augmented` → `ConstrainedQpSolver` | Needs the full constrained solver. |

`is_minimal_constraint_set()` is the test (`!fexp && !grp && !beta && !turn && !part && !own && !sector`).
A minimal set is *honored* (translated into an effective `OptimizerConfig`), never
silently discarded.

#### Fast path (`solve_fast`)
A deterministic fixed-iteration projected/proximal loop — no convergence early-exit.

- **λ = 0** → smooth target is `demean(α)` (no `V` to invert; skip `apply_inverse`).
  After gross-normalize this is **bit-for-bit the `WeightPolicy` dollar-neutral book**
  — the non-negotiable pin #1.
- **λ > 0** → smooth target is `(1/2λ)·P V⁻¹ P α` where `P = I − (1/M)11ᵀ` is the
  dollar-neutral centering projection. Uses **only** `apply_inverse` (no forward `Vw`).
- The loop, `cfg.max_iters` times: `gradient_step` (toward target, step `0.5`) →
  `prox_turnover` (soft-threshold toward `w_prev` by `κ·0.5`; identity if `κ=0`) →
  `project` (`demean` → `gross_normalize` to `Σ|w|=L` → fixed `kCapIters=8` clip-renorm
  to the cap).
- NaN α = "no opinion" → exactly 0 weight, excluded from the demean and Σ reductions.

The λ effect on the final book is **binary** (off at λ=0, on at λ>0) — the `1/2λ` scale
washes out under gross-normalize; only the `V⁻¹` directional tilt away from
high-variance names survives.

#### Augmented path (`solve_augmented`)
Sets `q = −alpha` (NaN → 0) and hands `{V, λ, q, MaterializedConstraints}` to the
`ConstrainedQpSolver`, which minimizes `½wᵀ(2λV)w + qᵀw` subject to the full constraint
surface.

### Constraints → `materialize`

[constraints.hpp](atx-engine/include/atx/engine/risk/constraints.hpp): a `ConstraintSet`
bundles descriptors (`gross`, `pos`, `fexp`, `grp`, `beta`, `turn`, `part`, `own`,
`sector`, `track`, `robust`). `materialize(X, w_prev, M, ref)` is a **pure** function
that emits, in a **fixed row order** (R1):

1. dollar-neutral `Σw = 0`
2. position box `|wᵢ| ≤ capᵢ` (with `%ADV` / `%shares` folds from `CapacityRef`)
3. factor exposure `|(Xᵀw)_k| ≤ bound`
4. group cap `|Σ_{i∈g} wᵢ| ≤ cap`
5. beta `|βᵀw| ≤ tol`
6. sector net-weight cap

The output `MaterializedConstraints` carries **only genuinely-linear rows** in `A`
(an `R×M` matrix). L1 budgets (`Σ|w| ≤ L`, `Σ|w−w_prev| ≤ T`) and **cones** (tracking,
sector-risk, robust, impact) ride as metadata; they become auxiliary-variable splits in
`build_augmented`, not dense rows. `CapacityRef` participation cap is
`ρ·H·ADVᵢ·priceᵢ/NAV`, ownership cap `κ·shares_outᵢ·priceᵢ/NAV` — both fold into the
diagonal box (the cheapest constraint).

### Augmenting — `build_augmented`

[qp_augment.hpp](atx-engine/include/atx/engine/risk/qp_augment.hpp) reformulates the
dense `O(M²)` QP into a **sparse quasi-definite** one by introducing factor auxiliary
variables `y = Xᵀw`. The Hessian decomposes into factor space:
`P = blkdiag(2λD, 2λF, 0)`.

**Variable layout** `x = [w; y; s; r; t]`:
- `w` (`M`) — weights
- `y` (`K`) — factor aux, `y = Xᵀw`
- `s` (`M`) — gross-L1 split aux
- `r` (`M`) — turnover-L1 split aux
- `t` (`1`) — robust epigraph aux (only when robust κ > 0)

**Constraint stack** (fixed row order): (0) `y − Xᵀw = 0` factor rows; (1) the linear
rows from `materialize`; (2) gross L1 split; (3) turnover split; (4) tracking-error SOC;
(5) sector-risk SOC per finite-σ sector; (6) robust epigraph SOC. Triplets are emitted
in ascending row/col order, no duplicate summation, no RNG.

### Solving — `ConstrainedQpSolver` (ADMM)

[qp_solver.hpp](atx-engine/include/atx/engine/risk/qp_solver.hpp) is a deterministic,
fixed-iteration OSQP-class ADMM. The pipeline (`solve_augmented_form`, shared by the
hard path and the elasticity layer):

1. **Ruiz equilibration** — fixed `ruiz_passes` (default 10), rescales `[P Ãᵀ; Ã 0]`
   to unit ∞-norm per row/col. Cone rows are pinned **un-scaled** so the ball/SOC
   projection stays isotropic.
2. **Fixed ADMM** — default 300 iterations, **no early-exit**:
   - **x-update**: one sparse no-pivot KKT solve — the KKT is factored **once** and
     reused every iteration.
   - **z-update**: elementwise clamp on box rows, then **SOC/ball projection on each
     cone block**. *This is the only place cones enter* — they widen `Ã` sparsity but
     **never** touch the x-update factorization (R5).
   - **y-update**: dual residual accumulation.
3. **Deterministic polish** (optional, `polish_refine` passes) — active-set partition,
   reduced KKT solve, iterative refinement; accepted only if feasible and
   lower-objective.
4. **Certificate** (`QpCertificate`) — order-fixed ∞-norm primal/dual residuals + OSQP
   primal/dual infeasibility detectors.
5. **Feasibility gate** (R3) — `Ã·x` within `feas_tol` of `[l,u]` and cone blocks, else
   `Err`.

Entry points: `solve` (returns the `w`-block), `solve_with_cert` (full `QpResult`,
including `cone_apex` for variable-apex SOC diagnostics).

#### The KKT factorization
[kkt_ldl.hpp](atx-engine/include/atx/engine/risk/kkt_ldl.hpp) — `QuasiDefiniteLdl`
factors `K = [P+σI, Ãᵀ; Ã, −ρ⁻¹I]` with a **no-pivot** quasi-definite LDLᵀ (QDLDL
kernel):
- `factor_symbolic(K)` — AMD fill-reducing permutation (pattern-only, cached) +
  elimination tree. **Dense-column demotion**: columns with degree > 16× mean are
  stable-partitioned to the end, dropping `O(M²)` fill to `O(M·K²)`.
- `factor_numeric(K)` — fills `L`, mixed-sign diagonal `D`, precomputes `D⁻¹`
  (division-free solve).
- `solve(rhs, x)` — 5 fixed-order phases: permute → forward-solve `L` → scale `D⁻¹` →
  back-solve `Lᵀ` → un-permute.

No numerical pivoting (the permutation depends only on the *pattern*, not the values),
so the factorization is byte-identical across runs.

#### Cones
[cone.hpp](atx-engine/include/atx/engine/risk/cone.hpp) — a `SocBlock`
(`row_start`, `dim`, `radius`, `offset`, `variable_apex`) describes a contiguous range
of `Ã` rows projected **jointly** onto a cone in the z-update:
- `ball_project(z, radius, z_out)` — fixed-apex ball `‖z‖₂ ≤ radius`
  (tracking-error S8.5a, sector-risk S8.5b).
- `soc_project(s, z, z_out)` — general variable-apex SOC `‖z‖₂ ≤ s`
  (robust-alpha epigraph S8.5c).
- `ordered_norm2(v)` — `√(Σ vᵢ²)` via a single accumulator, ascending index, for
  bitwise reproducibility.

Cone types and their math:
- **Tracking error**: `‖V^{1/2}(w − w_bench)‖₂ ≤ te`, expressed as
  `a = [L_Fᵀ(Xᵀu); √D∘u]` reusing `y = Xᵀw`.
- **Sector-risk**: same ball with a sector mask, offset 0.
- **Robust-alpha** (Goldfarb-Iyengar): penalty `+κ‖Ω_f^{1/2}y‖₂` via epigraph `t`,
  `q_aug[t] = κ`, variable-apex SOC.

### Elasticity — minimize-violation

[elasticity.hpp](atx-engine/include/atx/engine/risk/elasticity.hpp): `solve_elastic`
handles infeasibility deterministically.
1. Try the hard solve. Feasible → return untouched (**byte-identical no-op**).
2. Infeasible + elastic constraints present → `build_relaxed` (append penalized slack
   columns + `e ≥ 0` rows; linear rows get `e⁺,e⁻`, cones get `+e` on the budget, L1
   budgets get `−e`), re-solve at a scaled iteration budget / ρ.
3. Infeasible + no elastic (or still infeasible) → distinct `Err` (a hard constraint
   binds).

The relaxation is a **weighted** hierarchy via a fixed γ ladder
`γ_p = 2·4^priority` (lower priority = relaxed first / cheapest to violate). It is a
single convex solve on the same no-pivot LDLᵀ machinery — **no new factorization** (R5),
**no densification** (R4, slacks are sparse columns). The `RelaxationReport` lists which
rows/cones/budgets were relaxed and by how much, in fixed lowest-priority-first order.

---

## Multi-period layer

Two receding-horizon drivers sit on top of the single-period solve; both reuse it
verbatim, so the whole chain stays bit-deterministic.

- **`MultiPeriodOptimizer`** ([multi_period.hpp](atx-engine/include/atx/engine/risk/multi_period.hpp)) —
  walks an ascending rebalance schedule, threads `w_prev` from the prior realized book,
  applies Gârleanu-Pedersen partial-step blending (`blend_toward`), charges turnover.

- **`MultiHorizonOptimizer`** ([multi_horizon.hpp](atx-engine/include/atx/engine/risk/multi_horizon.hpp))
  — per period: `forecast_trajectory()` → `gp_aim()` collapse → dispatch
  (`solve_minimal` vs `solve_augmented`) → first-move execution.
  - `forecast_trajectory` ([horizon.hpp](atx-engine/include/atx/engine/risk/horizon.hpp))
    builds `alpha[h][i] = Σ_s decay_s(h)·α_{t,s}[i]` with `decay(h) = 2^{−h/halflife}`.
  - `gp_aim` averages the trajectory per name (NaN-aware): `ᾱ[i] = mean_h alpha[h][i]`.
    With `H=1` + identity horizon, `ᾱ == α_t` exactly — the R7 boundary pin.

### Gârleanu-Pedersen — scalar-Λ design (read this)

[garleanu_pedersen.hpp](atx-engine/include/atx/engine/risk/garleanu_pedersen.hpp) /
[src/risk/garleanu_pedersen.cpp](atx-engine/src/risk/garleanu_pedersen.cpp):
`gp_aim_and_value` returns `alpha_bar` (the `q = −ᾱ` fold) and
`aim_pos = (2λV)⁻¹ ᾱ` (via cached Cholesky Woodbury, never densified; λ=0 → `ᾱ`).

The **scalar-Λ reduction** (`Λ = λΣ`) is the load-bearing choice: the GP value matrix
collapses to `A_xx = 2λV` — the plain single-period Markowitz Hessian, **no Riccati
solved**. So the cost-to-go fold reduces to `q = −ᾱ`, `P = 2λV` unchanged, and the
augmented path is byte-identical to the pre-GP path on the boundary. This convention is
the one recorded in the sprint-1 plan (§0.6). **The full matrix-Riccati `A_xx`**
(genuine multi-period curvature for `H>1`, the stacked-MPC form) **is a recorded future
lift, not shipped.**

---

## Design rules

These are the invariants every change is checked against:

- **R1 — Determinism.** No RNG (except seed-pinned `eigen_adjust`), no clock, fixed
  iteration counts everywhere, order-fixed reductions (ascending index). Same inputs ⇒
  bitwise-identical outputs across runs and threads. *The iteration budget IS the
  algorithm.*
- **R3 — Exact then elastic.** The hard solve is attempted first; relaxation only on
  proven infeasibility, with a feasibility gate.
- **R4 — Never densify.** `V` and `Ã` are never materialized dense; factor-space
  Woodbury + sparse triplets only.
- **R5 — Static no-pivot factorization.** One KKT factorization per solve, reused across
  all ADMM iterations; cones enter via the z-projection only and add no factorization.
- **R8 — GP multi-period** via the scalar-Λ reduction above.
- **R10 — Reduce on the boundary, bit-for-bit.** Refactors and new features must leave
  every boundary pin byte-identical (e.g. λ=0 == `WeightPolicy`; H=1 == single-period;
  κ=0 cones == nominal; feasible elastic == no-op).
- **R11 — Differential correctness.** The sparse solver is verified against the frozen
  dense oracle ([qp_solver_reference.hpp](atx-engine/include/atx/engine/risk/qp_solver_reference.hpp)).

---

## Determinism & testing

Tests live in [atx-engine/tests/](atx-engine/tests/): `risk_cone_test.cpp`,
`risk_elasticity_test.cpp`, `risk_garleanu_pedersen_test.cpp`, `risk_kkt_ldl_test.cpp`,
`risk_qp_augment_test.cpp`, `risk_qp_s83_test.cpp` (plus the broader `Risk*` suite).

**Byte-identity pins** (must stay green *and* unchanged in value):
- `kZeroConeGolden = 0xffed7ec6c177aad2` (zero-cone golden hash).
- S7 boundary pins (`Full` / `Partial` / `CapacityClip` identical to `MultiPeriodOptimizer`).
- S8.5a/b/c zero-cone byte-identity (κ=0 cones == nominal book).
- S8.6 elasticity no-op (feasible problem unchanged).
- Determinism golden-hashes (`StatFactor`, `VolRegime`, `Covariance`, `KktLdl`,
  `MultiHorizon`, `QpSolver` two-runs).

**Differential gate:** `RiskQpAugment.MatchesDenseOracleAcrossBattery` checks the sparse
solver against the dense oracle. **It runs ~31 min — exclude it** in the edit loop:

```powershell
cmake --build build-ninja --target atx-engine-tests
.\build-ninja\bin\atx-engine-tests.exe `
  --gtest_filter='Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery'
```

No external solver / QP / BLAS dependency exists or may be added — Eigen is used for
dense storage and decomposition only.

---

## Build structure

The module is mostly header-only. The heavy method bodies of the three biggest units
are split into `atx-engine/src/risk/*.cpp` (compiled into the `atx-engine` static lib)
to collapse include fan-out:
[factor_model.cpp](atx-engine/src/risk/factor_model.cpp),
[garleanu_pedersen.cpp](atx-engine/src/risk/garleanu_pedersen.cpp),
[multi_horizon.cpp](atx-engine/src/risk/multi_horizon.cpp). These are listed explicitly
in `add_library(atx-engine STATIC …)` in
[atx-engine/CMakeLists.txt](atx-engine/CMakeLists.txt) — new `src/risk/*.cpp` are **not**
auto-globbed. The lib and the test target share the same fp/ISA regime (the
`atx_warnings` interface target, `/W4 /permissive- /WX`), which is what keeps the split
byte-identical.

`qp_solver.hpp`, `kkt_ldl.hpp`, and `exposures.hpp` stay header-only by design (intricate
inline-semantic / low-fan-out code where a split would risk a pin for little gain).

---

## Where to extend (recorded residuals)

Not blocking, tracked in the P2 ROADMAP:
- √-impact pricing-wiring into `capacity.hpp` / the augmented `q`.
- Elasticity driver auto-wiring (currently the caller opts in via `solve_elastic`).
- The matrix-Riccati / true stacked-MPC lift (replace scalar-Λ `A_xx = 2λV`).
- Splitting `qp_solver` / `kkt_ldl` / `exposures` bodies into `src/risk/` if a dedicated
  compile-time cycle is funded.
- Auto-ρ tuning for the ADMM (currently fixed `rho`/`sigma`).

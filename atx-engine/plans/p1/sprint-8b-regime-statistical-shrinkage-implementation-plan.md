# Sprint S8-b — Regime, Statistical Factors, Shrinkage & Integration: FROZEN implementation plan

> **For agentic workers:** this is the **frozen *how*** for S8-b (units S8.5–S8.8). The
> **what** lives in the sprint spec [`sprint-8-risk-covariance-construction.md`](sprint-8-risk-covariance-construction.md);
> the research grounding is [`../../research/covariance-matrix-construction-massive-universe-deep-dive.md`](../../research/covariance-matrix-construction-massive-universe-deep-dive.md).
> Execute task-by-task via **superpowers:subagent-driven-development**: fresh implementer per unit, TDD,
> then spec-compliance review, then code-quality review. Steps use checkbox (`- [ ]`) syntax.
> S8-a (S8.1–S8.4) is **shipped** ([`sprint-8a-progress.md`](sprint-8a-progress.md)); S8-b builds on it.

**Goal:** finish the vendor-grade risk model — add volatility-regime responsiveness (VRA), the statistical
(APCA) factor path that retires the `n_stat_factors` `NotImplemented`, a model-free shrinkage / RMT /
PSD-repair toolkit, and a short/long-horizon blend — then prove the whole deepened estimation pipeline
end-to-end (integration + bench + close), **without changing the `FactorModel` apply interface**
(`risk`/`apply_inverse`/`neutralize` stay verbatim) and **without weakening any carried-forward invariant**
(determinism, no look-ahead, PSD-by-construction).

**Architecture:** continue the S8-a pattern — new estimators land as focused new headers under
`include/atx/engine/risk/` (one responsibility each); all new *build-path* behavior is **opt-in via the
existing `CovarianceConfig`** (extended additively), whose **defaults reproduce the S8-a / P4 path
bit-for-bit** so every existing risk test stays green. The S8.7 toolkit is **standalone** (not auto-wired
into `build`) — a `risk::cov` statistical utility usable as a model-free covariance / optimizer bootstrap.
atx-core gets no new primitives this sprint — nonlinear shrinkage (QIS) + `apca` + `nearest_corr` are the
recorded Pattern-B lifts; ship engine-local fallbacks only.

**Tech stack:** C++20 header-only, Eigen (column-major `MatX`/`VecX`), GoogleTest, Google Benchmark
(`bench/*_bench.cpp`), Ninja + clang-cl, `atx::core` linalg (`symmetric_eig`/`ols`/`wls`/`solve_spd`) +
`atx::core::random::Xoshiro256pp`.

---

## §0 — Recon: the as-built state S8-b modifies (verified at kickoff)

Exact anchors (read these before touching anything):

| Concern | Location | As-built behavior |
|---|---|---|
| Factored apply `V=XFXᵀ+D` | [`risk/factor_model.hpp:100-257`](../../include/atx/engine/risk/factor_model.hpp#L100) | `FactorModel::create` / `risk` / `apply_inverse` (Woodbury) / `neutralize`. **DO NOT CHANGE THE INTERFACE.** `create(MatX x, MatX f, VecX d, usize fit_begin, usize fit_end) -> Result<FactorModel>` floors D, requires F SPD, rejects K>256. |
| Builder pipeline | [`factor_model.hpp:427-512`](../../include/atx/engine/risk/factor_model.hpp#L427) | `build`: `x0=build_exposures(row 0)` → `accumulate_ols`→`d0` → Pass B (`accumulate_robust` if `cov.robust_regression` else `accumulate_wls`) → `fkept` (compacted, row 0 newest) → **factor-cov switch** (`LedoitWolfSingle`/`EwmaNeweyWest`) → **eigen_adjust** (if `eigen_adjust_sims>0`) → `specific_variances` → `create`. |
| **The stat/dead rung** | [`factor_model.hpp:430-434`](../../include/atx/engine/risk/factor_model.hpp#L430) | `if (cfg.n_stat_factors>0 \|\| cfg.n_dead_factors>0) return Err(NotImplemented,…)`. **S8.6 replaces the `n_stat_factors>0` branch**; `n_dead_factors>0` stays `NotImplemented` (S7.3). |
| Factor cov (LW) | [`factor_model.hpp:357-374`](../../include/atx/engine/risk/factor_model.hpp#L357) | `detail::factor_covariance(fseries, cfg_shrink)`: column-demean → MLE cov S (÷T) → canonical scaled-identity LW. |
| Factor cov (EWMA+NW) | [`risk/cov_ewma.hpp`](../../include/atx/engine/risk/cov_ewma.hpp) | `ewma_factor_covariance(fseries, vol_hl, corr_hl, nw_lags) -> MatX` (SPD, eigen-floored). `detail::{ewma_weights,ewma_means,ewma_cov,recombine_split,newey_west,psd_floor}`. Row 0 = newest; `H==0`⇒equal weights. **S8.8 reuses `ewma_factor_covariance` at two half-life sets.** |
| Eigen-adjust | [`risk/eigen_adjust.hpp`](../../include/atx/engine/risk/eigen_adjust.hpp) | `eigen_adjust(f, sims, amplify, seed) -> Result<MatX>` (MWO, seeded, sims==0⇒no-op). |
| Specific risk | [`risk/specific_risk.hpp`](../../include/atx/engine/risk/specific_risk.hpp) | `specific_risk_blend(x0, u_by_inst, window, spec_hl, spec_nw_lags, structural) -> SpecificRisk{variances, isc(empty)}`. `detail::{ewma_var, nw_inflation}` (scalar). **S8.8 reuses at two half-life sets.** |
| `specific_variances` | [`factor_model.hpp:672-691`](../../include/atx/engine/risk/factor_model.hpp#L672) | member; exhaustive switch on `cov.specific_method`. Returns `VecX` length M (X[0] order). |
| Config | [`risk/exposures.hpp:120-153`](../../include/atx/engine/risk/exposures.hpp#L120) | `FactorCovMethod`/`SpecificRiskMethod` enums + `CovarianceConfig` (S8-a fields) + `FactorModelConfig{sector_factors, style_mask, n_stat_factors, n_dead_factors, factor_cov_shrink, cov}`. **S8-b extends `CovarianceConfig` additively (§3).** |
| Per-date returns | [`factor_model.hpp:299-318`](../../include/atx/engine/risk/factor_model.hpp#L299) | `detail::date_returns(panel, s, xm, keep)` (listwise NaN-drop) + `detail::select_rows`. **S8.6 reuses for the APCA return panel.** |

Reusable atx-core (no edge — reuse verbatim; verified signatures):
- `core::linalg::symmetric_eig(const MatX&) -> Result<EigResult{VecX values /*ASCENDING*/, MatX vectors /*cols*/}>` — [`decompose.hpp:127`](../../../atx-core/include/atx/core/linalg/decompose.hpp#L127).
- `core::linalg::ols(X,y)` / `wls(X,y,w)` / `ridge(X,y,λ)` → `Result<OlsResult{VecX beta, f64 r2, VecX residuals}>` — [`regression.hpp`](../../../atx-core/include/atx/core/linalg/regression.hpp) (ols@72, ridge@90, wls@112).
- `core::linalg::solve_spd(const MatX& A, const VecX& b) -> Result<VecX>` (LLT) — `spd.hpp`/`solve.hpp`.
- `core::random::Xoshiro256pp{u64 seed}`; `.normal()` deterministic standard normal — [`random.hpp:95,184`](../../../atx-core/include/atx/core/random.hpp#L95).
- `combine::detail::ledoit_wolf_intensity(const MatX& s, const MatX& centered) -> f64` — [`combiner.hpp:254`](../../include/atx/engine/combine/combiner.hpp#L254). Closed form for the **scaled-identity** target `μ·I`; **S8.7 generalizes the *target*, not the kernel** (see §4).

Apply/optimize interface S8.8 integration must flow through UNCHANGED:
- `PortfolioOptimizer::solve(span<const f64> alpha, const FactorModel& V, span<const f64> w_prev) -> Result<vector<f64>>` — [`optimizer.hpp:111`](../../include/atx/engine/risk/optimizer.hpp#L111). `OptimizerConfig{risk_aversion, turnover_penalty, gross_leverage, name_cap, dollar_neutral, max_iters}`. Uses `V.apply_inverse` only.
- `MultiPeriodOptimizer::run(sched, alpha_at, model_at, cost) -> Result<MultiPeriodResult>` — [`multi_period.hpp:117`](../../include/atx/engine/risk/multi_period.hpp#L117).

Test/bench infra (verified):
- Tests: `tests/CMakeLists.txt` `file(GLOB … CONFIGURE_DEPENDS "*_test.cpp")` → **drop a `*_test.cpp`, rebuild, no CMake edit.**
- Bench: `bench/CMakeLists.txt` `file(GLOB … CONFIGURE_DEPENDS "*_bench.cpp")` into `atx-engine-bench` (links `benchmark::benchmark`); pattern = [`bench/cost_bench.cpp`](../../bench/cost_bench.cpp) (`#include <benchmark/benchmark.h>`, `static void BM_x(benchmark::State& st){…}`, `Xoshiro256pp` synthetic data). **Drop a `*_bench.cpp`, no CMake edit.**

---

## §1 — Rules (every unit obeys; non-negotiable gates)

**Build/test — the EXACT incantation.** This shell is *not* a VS dev shell by default; every build/test
command MUST prepend the prelude (each tool call is a fresh shell):

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:VCPKG_ROOT='C:\Users\natha\vcpkg'; $env:ATX_DEPS_DIR='C:\atx-cache\deps'
Set-Location 'C:\Users\natha\atx-wt\s8'
# build the test target then run just the new suite (fast iterate):
cmake --build --preset ninja --target atx-engine-tests
ctest --preset ninja -R <SuiteRegex> --output-on-failure
# bench (S8.8 only): cmake --build --preset ninja --target atx-engine-bench
```
(The `'vswhere.exe' is not recognized` line is BENIGN — the env is still set, `INCLUDE` becomes set.
The 2 ctest "failures" `atx-core-tests_NOT_BUILT` / `atx-tsdb-tests_NOT_BUILT` are EXPECTED sentinels —
engine-only build — IGNORE them. Baseline after S8-a: **1096/1098 real tests pass**.)

- **TDD always.** Failing GoogleTest first (`#include <gtest/gtest.h>`, `TEST(Subject_Condition_Expected)`),
  watch it fail for the right reason, then implement. Tests are flat under `atx-engine/tests/*_test.cpp`,
  **CMake-globbed** — drop the file, rebuild, do **not** hand-edit `CMakeLists.txt`. Match the style of
  [`risk_cov_ewma_test.cpp`](../../tests/risk_cov_ewma_test.cpp) (anonymous namespace, `using` aliases,
  synthetic `PanelView` fixtures, byte-identity via `std::memcmp`/`bit_cast`).
- **Gates (a unit is done only when ALL pass):** `/W4 /permissive- /WX` clean (any warning fails),
  `/fp:precise`, clang-format clean (`clang-format -i` on YOUR OWN new/edited files ONLY — **never** the
  umbrella `--target clang-format`, which reformats the databento submodule), the unit's new suite green,
  **and the full `atx-engine-tests` suite still green** (esp. `RiskFactorBuilder*`/`RiskFactorModel*`/`RiskCovEwma*`/
  `RiskEigenAdjust*`/`RiskSpecificRisk*`). **Do NOT run clang-tidy** (disabled repo-wide).
- **Determinism (carried invariant #1).** No clock, no map iteration, no unseeded RNG. All reductions in
  canonical ascending order (instrument, then factor; ascending lag/date). **S8 has exactly ONE RNG site —
  S8.3 `eigen_adjust` (already shipped).** No S8-b unit introduces a new RNG in the build path. (Tests/bench
  may seed `Xoshiro256pp` for synthetic data — that is test scaffolding, not a model RNG.) Prove byte-identity
  where a unit's output feeds the model (`eigen_adjust` replay covered by S8.3; VRA/APCA/blend are RNG-free).
- **No look-ahead (carried invariant #2).** Every estimate is fit on the trailing window only. The test is
  **truncation-invariance**: appending older rows beyond the window+lookback must not change the estimate.
- **Backward-compat.** New build-path behavior is opt-in (see §3). `CovarianceConfig{}` default + `n_stat_factors==0`
  ⇒ the S8-a / P4 path, byte-for-byte. Existing risk tests are part of every unit's done-gate.
- **Pattern B.** No new general-purpose primitive in the engine. S8.6 needs `apca`, S8.7 needs
  `nonlinear_shrinkage`/`nearest_corr` from atx-core (recorded in spec + ROADMAP); ship the engine-side
  fallback only, marked with a `// PATTERN-B:` comment naming the atx-core lift.
- **Commit per unit:** `feat(s8-N): <summary>` with trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Stage **explicit pathspecs**
  (never `git add -A`). **Do not push.** This worktree is on its own branch `feat/s8` (no shared-ref race).
  **Commit-message files MUST be BOM-free** (S8.4's `cb01c07` carries a stray BOM from a BOM-encoded temp
  file — do not repeat): write the message with a BOM-free editor / `git commit -F <file>` where `<file>`
  is plain UTF-8, or use multiple `-m` flags.

---

## §2 — Files (S8-b)

| Unit | Create | Modify |
|---|---|---|
| S8.5 | `risk/vol_regime.hpp`, `tests/risk_vol_regime_test.cpp` | `risk/exposures.hpp` (`vra_halflife` field — §3); `risk/factor_model.hpp` (`build` applies λ² to F and D when `vra_halflife>0`) |
| S8.6 | `risk/stat_factor_model.hpp`, `tests/risk_stat_factor_test.cpp` | `risk/exposures.hpp` (`apca_gls_reweight` field — §3); `risk/factor_model.hpp` (`build` dispatches the `n_stat_factors>0` branch to APCA instead of `NotImplemented`) |
| S8.7 | `risk/shrinkage.hpp`, `risk/psd_repair.hpp`, `tests/risk_shrinkage_psd_test.cpp` | (none required — standalone toolkit) |
| S8.8 | `risk/horizon_blend.hpp`, `tests/risk_covariance_integration_test.cpp`, `bench/risk_covariance_bench.cpp`, `sprint-8.md` (user ref) | `risk/exposures.hpp` (horizon-blend fields — §3); `risk/factor_model.hpp` (`build` blends short/long F and D when `horizon_blend`); `plans/p1/ROADMAP.md`, `plans/p1/sprint-8b-progress.md` (close) |

All new headers: `#pragma once`, namespace `atx::engine::risk`, `detail` sub-namespace for kernels,
`[[nodiscard]]`, `noexcept` on leaf math, `Result<T>` at fallible boundaries, exhaustive switches,
functions ≤ ~60 lines. Document the contract (§9 of the C++ profile), `// SAFETY:` every deviation.

---

## §3 — `CovarianceConfig` extension (additive; every default reproduces S8-a/P4)

Add these fields to `CovarianceConfig` in [`risk/exposures.hpp`](../../include/atx/engine/risk/exposures.hpp)
(after the S8.4 block, before the closing brace). **Each default is the no-op path.** Add the FULL S8-b set
in S8.5 even though later fields wire in later units — landing them up front avoids re-touching every
including TU (the same rationale the S8-a struct used).

```cpp
  // --- S8.5 Volatility Regime Adjustment (VRA) ---
  atx::usize vra_halflife = 0;        // 0 ⇒ no VRA (λ²≡1); e.g. 42 (short) / 168 (long) — USE4
  // --- S8.6 statistical factors (APCA); activates when FactorModelConfig.n_stat_factors > 0 ---
  bool apca_gls_reweight = true;      // 2nd APCA pass (GLS residual-variance reweight); false ⇒ 1-pass
  // --- S8.8 short/long-horizon blend (the long-horizon half-life set + convex weight) ---
  bool horizon_blend = false;         // false ⇒ single-horizon (P4 / S8.2 path), byte-identical
  atx::f64 horizon_blend_weight = 0.5;// w in F = w·F_short + (1−w)·F_long (clamped [0,1])
  atx::usize vol_halflife_long = 0;   // long-horizon vol HL  (the short HL is the existing vol_halflife)
  atx::usize corr_halflife_long = 0;  // long-horizon corr HL (short = existing corr_halflife)
  atx::usize spec_halflife_long = 0;  // long-horizon specific HL (short = existing spec_halflife)
```

> S8.7's toolkit takes NO `CovarianceConfig` field — `shrinkage.hpp` / `psd_repair.hpp` are standalone
> utilities (model-free covariance / optimizer bootstrap), not auto-wired into `build` this sprint.

---

## §4 — Algorithms (the reference math each kernel implements)

### S8.5 — Volatility Regime Adjustment (VRA). *Research §4.4 (USE4); citation: MSCI Barra USE4 Methodology Notes Tables 4.1/5.1; Menchero-Wang-Orr.*
Given the kept factor-return series `fkept` (T×K, row 0 newest) and the model's forecast factor covariance
`F` (the diagonal `F_jj` is the forecast factor variance σ_j²):
- **Factor cross-sectional bias statistic** per kept date `t`:
  `B_t = √( (1/K) · Σ_{j=1..K} ( f_{t,j} / σ_j )² )`, with `σ_j = √F_jj`. `E[B_t²]=1` if the forecast is
  unbiased; `B_t²>1` in a high-vol regime the fixed half-life under-forecast. A factor with `σ_j≤0`
  (degenerate column) is excluded from the K-average (and K reduced for that date) — guard divide-by-zero.
- **Regime multiplier** `λ² = EWMA(B_t² ; vra_halflife)` = `Σ_t w_t B_t² / Σ_t w_t`, `w_t = 2^(−t/H)`
  (row 0 newest, the SAME weighting as `cov_ewma.hpp`; `H==0`⇒equal weights — but `vra_halflife==0` is the
  no-VRA sentinel, so the kernel is only ever called with `H≥1`). `λ²>0` always.
- **Rescale** `F ← λ²·F` (PSD-preserving — positive scalar). **D analogue:** `D ← λ²·D` (apply the SAME
  market-wide vol-regime multiplier to the specific variances; a documented simplification — the regime
  signal is market-wide; per-name specific VRA needs a dense specific panel, recorded as a backlog residual).
- **Diagnostic:** the kernel RETURNS the `B_t` series (the statistical-analysis surface). Acceptance tests
  the kernel directly; it is NOT threaded into `FactorModel` (the apply interface stays unchanged).
- **No-op:** `vra_halflife==0` ⇒ `build` skips VRA entirely (F, D byte-identical). A stationary panel gives
  `λ²≈1` within tolerance (the bias stat averages to ~1).

Kernel signature (in `vol_regime.hpp`):
```cpp
struct RegimeAdjust { atx::f64 lambda2; atx::core::linalg::VecX bias_stats; }; // bias_stats length == T kept dates
[[nodiscard]] RegimeAdjust vol_regime_multiplier(const atx::core::linalg::MatX& fseries,
                                                 const atx::core::linalg::MatX& f_forecast,
                                                 atx::usize vra_halflife);
```
Order-fixed (ascending date, then factor). PURE, RNG-free, `noexcept` not required (no alloc-critical path).

### S8.6 — Statistical factor model (APCA, Connor-Korajczyk 2-pass). *Research §3.2; citation: Axioma AXUS4; Connor & Korajczyk.*
Replace the `n_stat_factors>0` `NotImplemented` with a statistical `FactorModel`. Build the return panel
`R` (N×T) over the trailing window: N = the current cross-section instruments (X[0] order — the apply M),
T = window dates; `R[n,t] = step_return(panel, t, inst_n)`. Drop instruments WITHOUT full T-length clean
history (complete-case rows — the asymptotic argument needs a rectangular block); call the surviving count
N (must satisfy `N > T` and `N > K` and `T > K`, else `Err(InvalidArgument)`). Column-demean each
instrument's row over its T returns (subtract the time-mean per asset).

**Pass 1 (equal-weighted):**
1. `Ω = (1/N)·Rᵀ·R` (**T×T Gram** — the asymptotic trick for N≫T; NOT the N×N cov — atx-core `pca()`
   forms N×N and is therefore WRONG here. `// PATTERN-B: atx-core apca (T×T Gram) is the L7 lift; engine
   forms the Gram explicitly + symmetric_eig.`).
2. `symmetric_eig(Ω)` → eigenvalues ascending; the **top-K** eigenvectors (largest K eigenvalues, i.e. the
   LAST K columns) are the factor-return matrix `Fhat` (T×K), each column a factor's return series.
3. Exposures `B = R·Fhat·(Fhatᵀ·Fhat)⁻¹` (N×K) — regress each asset's return series on the factor returns
   (use `solve_spd` on the K×K `Fhatᵀ Fhat`, or OLS per asset). Specific variance `s_n = var(R_n − B_n·Fhatᵀ)`.

**Pass 2 (GLS, opt-in via `apca_gls_reweight`, default true):** reweight rows of `R` by `1/√s_n` →
`R_w[n,:] = R[n,:]/√s_n`; recompute `Ω_gls=(1/N)R_wᵀR_w`, eigendecompose → final `Fhat`; recover final `B`
and `s_n` from the UN-weighted `R` (exposures/specific are reported in the original return scale).

**Assemble the `FactorModel`:** `X = B` (N×K statistical exposures for the current cross-section),
`F = ` the K×K covariance of the factor-return series `Fhat` (column-demean → MLE cov ÷T; reuse
`detail::factor_covariance(Fhat, cfg.factor_cov_shrink)` so the LW shrink keeps F SPD), `D = s_n` (length N).
`create(X, F, D, fit_begin=0, fit_end=window)`. Sign/ordering of eigenvectors is canonical (symmetric_eig
is deterministic); **fix the factor sign** so a column's first non-zero entry is positive (eigenvector sign
is otherwise arbitrary — pin it for determinism). `n_dead_factors>0` STILL returns `NotImplemented`.

### S8.7 — Model-free shrinkage + RMT + PSD repair. *Research §7 (Ledoit-Wolf 2004/2020, MP), §8 (Higham 2002).*

**Constant-correlation Ledoit-Wolf** (`shrinkage.hpp`). Generalize the canonical LW *target* (the kernel
math is reused, the target changes from `μ·I` to the constant-correlation matrix):
- Sample cov `S` (N×N, MLE ÷T) from `centered` (T×N). `s_i=√S_ii`. Sample correlation `C_ij=S_ij/(s_i s_j)`.
  `r̄ = ` mean of the off-diagonal `C_ij` (i<j, order-fixed). **Target** `F`: `F_ii=S_ii`,
  `F_ij = r̄·s_i·s_j` (i≠j).
- **Intensity** `δ = clamp( (π̂ − ρ̂) / (T·γ̂), 0, 1 )` where:
  - `π̂ = Σ_ij (1/T) Σ_t ( (r_{t,i})(r_{t,j}) − S_ij )²` (the SAME `Σ_t ‖r_t r_tᵀ − S‖²_F` sum the canonical
    kernel computes — reuse it: `π̂ = T · b̄²_canonical · T`? NO — compute `π̂` directly as the order-fixed
    `Σ_t‖r_t r_tᵀ−S‖²_F` *without* the `/T` the canonical helper folds in; expose it cleanly).
  - `γ̂ = ‖F − S‖²_F` (Frobenius sq-distance, target misspecification).
  - `ρ̂` (constant-correlation target, Ledoit-Wolf 2004 "Honey, I Shrunk…" Appendix A):
    `ρ̂ = Σ_i π̂_ii + Σ_{i≠j} (r̄/2)·( √(s_jj/s_ii)·ϑ_{ii,ij} + √(s_ii/s_jj)·ϑ_{jj,ij} )`, with
    `ϑ_{ii,ij} = (1/T) Σ_t ( (r_{t,i}² − S_ii)(r_{t,i} r_{t,j} − S_ij) )` and `π̂_ij` the per-entry asymptotic
    variance `(1/T) Σ_t ( r_{t,i} r_{t,j} − S_ij )²`. (Document the formula; it is the published closed form.)
- **Shrunk** `Σ̂ = δ·F + (1−δ)·S`. PSD by construction (convex combo of PSD S + the PD-ish const-corr F).
- **DRY mandate:** do not fork a second LW intensity for the scaled-identity case — the const-corr path is a
  generalization; the existing `combine::detail::ledoit_wolf_intensity` STAYS the scaled-identity home.
  `shrinkage.hpp` adds the const-corr target + its `ρ̂`. Note the shared structure in a comment.

**Marchenko-Pastur eigenvalue clipping** (`shrinkage.hpp`). Input a correlation matrix `C` (N×N) + ratio
`q=N/T`:
- `λ₊ = (1+√q)²` (the noise-bulk upper edge for unit-variance noise). Eigendecompose `C=UΛUᵀ`
  (`symmetric_eig`). Eigenvalues `Λ_k < λ₊` are "noise"; replace ALL of them with their average
  `λ̄ = (Σ_{noise} Λ_k)/n_noise` (**trace-preserving** — the sum of clipped eigenvalues is unchanged).
  Reassemble `C_clean = U·diag(Λ_clipped)·Uᵀ`; **renormalize the diagonal to 1** (divide by √(diag) outer
  product) so it stays a correlation matrix. Returns the cleaned correlation matrix.

**Higham nearest-correlation** (`psd_repair.hpp`). Alternating projections (Higham 2002):
- Input symmetric `A` (near-correlation, possibly indefinite), `max_iter` (fixed, default ~100), `tol`.
  `Y=A`, `ΔS=0`. Loop (bounded `max_iter`): `R=Y−ΔS`; `X=proj_PSD(R)` (`symmetric_eig`, clip eigenvalues
  `<0` to 0, reassemble); `ΔS=X−R`; `Y=proj_unit_diag(X)` (set `Y_ii=1`, off-diagonal = X). Exit when
  `‖Y−X‖_F < tol`. Return `Y` (PSD, unit-diagonal, Frobenius-closest). `Result<MatX>` (Err if `symmetric_eig`
  fails). FIXED upper iteration bound (JPL bounded-loop rule).
- **`eigenvalue_clip(A, eps)`** (cheap PSD repair, also `psd_repair.hpp`): eigendecompose, clip eigenvalues
  to `≥ eps`, reassemble. For covariances (loses unit-diagonal — not for correlations). `MatX`.
- `// PATTERN-B: atx-core nearest_corr + nonlinear_shrinkage (QIS) are the L7 lifts; engine ships
  const-corr LW + MP clip + Higham here. Nonlinear (QIS/QuEST) shrinkage is NOT built this sprint.`

### S8.8 — Short/long-horizon blend. *Research §9, §11 (USE4S/L, AXUS4 SH/MH half-life tables).*
- `blend_factor_cov(const MatX& f_short, const MatX& f_long, f64 w) -> MatX` = `w·F_short + (1−w)·F_long`,
  `w` clamped [0,1]. PSD-preserving (convex combo of SPD). `blend_specific(const VecX& d_short,
  const VecX& d_long, f64 w) -> VecX` likewise. (`horizon_blend.hpp`.)
- `build` (when `cov.horizon_blend`): build `F_short = ewma_factor_covariance(fkept, vol_halflife,
  corr_halflife, nw_lags)` and `F_long = ewma_factor_covariance(fkept, vol_halflife_long, corr_halflife_long,
  nw_lags)`; `F = blend(F_short, F_long, horizon_blend_weight)`. Same for D via two `specific_risk_blend`
  calls at `spec_halflife` / `spec_halflife_long` (only when `specific_method==EwmaNeweyWestStructural`;
  else D is single-horizon pop-variance). Blend BEFORE eigen_adjust / VRA in the pipeline (blend is the
  factor-cov construction; eigen_adjust + VRA then clean/scale the blended F). `horizon_blend==false` ⇒
  single-horizon path, byte-identical. Recommended config sets (research §11, document in comment):
  USE4S vol 84 / corr 504 / vra 42; USE4L vol 252 / corr 504 / vra 168; AXUS4 SH 60/125, MH 125/250.
  (No universal blend weight exists in the research — vendors ship two models; `w=0.5` is our neutral default.)

---

## §5 — Per-unit task breakdown

### Task S8.5 — Volatility Regime Adjustment (VRA)

**Files:** Create `risk/vol_regime.hpp`, `tests/risk_vol_regime_test.cpp`. Modify `exposures.hpp` (config — add
the FULL §3 S8-b set now), `factor_model.hpp` (apply λ² in `build`). Research §4.4.

- [ ] **Step 1 — failing test (no-op on stationary + λ²>1 on spike).** In `risk_vol_regime_test.cpp`: (a) a
  stationary factor-return series whose realized vols ≈ the forecast diag ⇒ `λ²≈1` (within ~5%); (b) a series
  with the NEWEST rows carrying an injected vol spike (returns scaled ×k) while the forecast diag reflects the
  calmer full-window vol ⇒ `λ²>1` materially. Assert the `bias_stats` series length == T and is order-fixed
  deterministic (call twice → byte-identical). Run → fails (header absent).
- [ ] **Step 2 — implement `vol_regime.hpp`.** `vol_regime_multiplier(fseries, f_forecast, vra_halflife)` per
  §4 (B_t, EWMA λ², return `RegimeAdjust{lambda2, bias_stats}`). Guard `σ_j≤0` (exclude that factor from B_t's
  K-average). Order-fixed.
- [ ] **Step 3 — wire `build`.** Add the §3 fields to `CovarianceConfig`. In `build`, AFTER `eigen_adjust`
  and AFTER `specific_variances`, when `cfg.cov.vra_halflife>0` compute the multiplier ONCE from `fkept` +
  the post-eigen-adjust `f`, then `f *= λ²` and `d *= λ²`. Default `vra_halflife==0` ⇒ skip (F,D byte-identical).
- [ ] **Step 4 — truncation-invariance + determinism + full green.** Add a truncation-invariance test (extra
  older rows beyond the window don't change λ²) and a build-level determinism test (same panel/config twice →
  byte-identical F,D digest). Build + run new suite + **full `atx-engine-tests`** (S8-a suites stay green
  because `vra_halflife` defaults 0). All gates.
- [ ] **Step 5 — commit** `feat(s8-5): volatility regime adjustment (VRA bias-stat factor cov/specific rescale)`.

**Acceptance:** injected-spike panel ⇒ `λ²>1` inflates the forecast and reverts on a later calm window;
`λ²≈1` on a stationary panel (within tolerance); bias-stat series order-fixed deterministic; existing tests green.

### Task S8.6 — Statistical factor model (APCA) — fills `n_stat_factors`

**Files:** Create `risk/stat_factor_model.hpp`, `tests/risk_stat_factor_test.cpp`. Modify `exposures.hpp`
(`apca_gls_reweight`), `factor_model.hpp` (`build` dispatch). Research §3.2.

- [ ] **Step 1 — failing test (recover planted latent factors).** Synthetic panel: K planted latent factors
  drive N≫T instruments (`R = B_true·Fᵀ_true + noise`); build with `n_stat_factors=K`. Assert: (a) the top-K
  Gram eigenvalues dominate (a clear gap to the (K+1)-th); (b) each recovered factor-return series correlates
  highly (|corr|>0.9) with a planted one (up to sign/permutation); (c) the resulting `FactorModel` applies
  through `apply_inverse`/`risk` (V SPD — `create` succeeds). Run → fails (`NotImplemented`).
- [ ] **Step 2 — implement `stat_factor_model.hpp`.** `build_stat_factor_model(panel, window, market_cap,
  group_id, n_stat, gls, factor_cov_shrink) -> Result<FactorModel>` per §4 (complete-case N×T panel,
  Pass 1 T×T Gram + top-K, exposures B, specific s_n; Pass 2 GLS reweight when `gls`; factor cov via
  `detail::factor_covariance`; sign-pinned eigenvectors; `create`). `Err(InvalidArgument)` if `N≤T` or
  `T≤K` or `N≤K` or too few complete-case names. `// PATTERN-B: atx-core apca (T×T Gram) is the L7 lift.`
- [ ] **Step 3 — wire `build` dispatch.** In `factor_model.hpp build`, change the rung: when
  `cfg.n_stat_factors>0` (and `n_dead_factors==0`) RETURN `build_stat_factor_model(...)` instead of
  `NotImplemented`; `n_dead_factors>0` STILL `NotImplemented`. (Place the dispatch before the fundamental
  pipeline — the statistical path is a distinct model variant.)
- [ ] **Step 4 — determinism + truncation-invariance + green.** Same panel twice → byte-identical V digest;
  truncation-invariance (older rows beyond window don't change it). The fundamental path
  (`n_stat_factors==0`) is untouched ⇒ existing suite green. All gates.
- [ ] **Step 5 — commit** `feat(s8-6): APCA statistical factor model (T×T Gram, GLS reweight) — retires n_stat_factors NotImplemented`.

**Acceptance:** planted latent structure recovered (top-K eigenvalues dominate; recovered factor returns
correlate highly with planted); statistical V SPD + applies through the existing Woodbury path; the
`n_stat_factors` `NotImplemented` is retired; `n_dead_factors>0` still `NotImplemented`; existing tests green.

### Task S8.7 — Model-free shrinkage + RMT + PSD-repair toolkit

**Files:** Create `risk/shrinkage.hpp`, `risk/psd_repair.hpp`, `tests/risk_shrinkage_psd_test.cpp`. Research §7, §8.

- [ ] **Step 1 — failing tests (LW closed form + MP trace + Higham nearest).** In
  `risk_shrinkage_psd_test.cpp`: (a) on a small hand-built `(S, centered)` with a known constant-correlation
  structure, assert `constant_correlation_shrinkage` returns `δ·F+(1−δ)·S` with `δ` matching a hand-computed
  `(π̂,ρ̂,γ̂)` value (within ULP-bounded tol) and F SPD; (b) `mp_clip` on a correlation matrix with planted
  noise eigenvalues inside `[λ₋,λ₊]` removes them (replaces with their average) and PRESERVES the trace
  (Σλ unchanged within tol); (c) `nearest_correlation` on a known near-PSD invalid matrix (one negative
  eigenvalue, off-diagonal > 1) returns a valid correlation matrix (unit diagonal, all eigenvalues ≥ 0,
  Frobenius-closer than the input). Run → fails (headers absent).
- [ ] **Step 2 — implement `shrinkage.hpp`.** `constant_correlation_target(const MatX& s) -> MatX`;
  `constant_correlation_shrinkage(const MatX& s, const MatX& centered) -> MatX` (compute `r̄`, target,
  `π̂`,`ρ̂`,`γ̂`, `δ`, return `δF+(1−δ)S`); `mp_clip(MatX corr, atx::f64 q) -> MatX` (λ₊ clip, average,
  renormalize diagonal). Order-fixed, PSD by construction. `// PATTERN-B: atx-core nonlinear_shrinkage (QIS).`
- [ ] **Step 3 — implement `psd_repair.hpp`.** `nearest_correlation(const MatX& a, usize max_iter, atx::f64
  tol) -> Result<MatX>` (Higham alternating projections, FIXED `max_iter` bound); `eigenvalue_clip(const
  MatX& a, atx::f64 eps) -> MatX`. `// PATTERN-B: atx-core nearest_corr.`
- [ ] **Step 4 — invertibility + green.** Add an N>T test: `constant_correlation_shrinkage` on a window with
  N>T returns a POSITIVE-DEFINITE (invertible — Cholesky succeeds) matrix where the raw sample cov is
  singular. The toolkit is standalone (no `build` wiring) ⇒ existing suite trivially green. All gates.
- [ ] **Step 5 — commit** `feat(s8-7): model-free shrinkage (const-corr LW + MP clip) + PSD repair (Higham + eigen-clip)`.

**Acceptance:** const-corr LW intensity matches the closed form on a known setup, PD-guaranteed at N>T;
MP clip removes the `[λ₋,λ₊]` bulk and preserves trace; Higham repair → nearest valid correlation
(unit diagonal, eigenvalues ≥ 0); existing tests green.

### Task S8.8 — Short/long-horizon blend + integration + bench + close

**Files:** Create `risk/horizon_blend.hpp`, `tests/risk_covariance_integration_test.cpp`,
`bench/risk_covariance_bench.cpp`, `sprint-8.md`. Modify `exposures.hpp` (S8.8 fields already added in S8.5 §3),
`factor_model.hpp` (blend dispatch), `ROADMAP.md`, `sprint-8b-progress.md`. Research §9, §10, §11, §6.

- [ ] **Step 1 — failing test (blend convexity + integration end-to-end).** In
  `risk_covariance_integration_test.cpp`: (a) `blend_factor_cov(F_s, F_l, w)` equals the convex combo and is
  SPD for SPD inputs; `w=1`⇒F_s, `w=0`⇒F_l; (b) the ALL-FEATURES-ON integration: build with
  `robust_regression=true, factor_cov_method=EwmaNeweyWest, horizon_blend=true, eigen_adjust_sims>0 (seeded),
  vra_halflife>0, specific_method=EwmaNeweyWestStructural, structural_blend=true` on a synthetic panel; run
  `PortfolioOptimizer::solve(alpha, model, w_prev)`; assert (i) build succeeds + V SPD, (ii) BYTE-IDENTICAL
  replay (same config twice → identical solve output, incl. the seeded eigen-adjust), (iii)
  truncation-invariance at the fit boundary. Run → fails (`horizon_blend.hpp` absent).
- [ ] **Step 2 — implement `horizon_blend.hpp` + wire `build`.** `blend_factor_cov` / `blend_specific`
  (clamp w, convex combo). In `build`, when `cfg.cov.horizon_blend`, replace the single `ewma_factor_covariance`
  with two-HL blend (short = existing HLs, long = `*_long` HLs) BEFORE eigen_adjust; blend D analogously when
  `specific_method==EwmaNeweyWestStructural`. Default `horizon_blend==false` ⇒ single-horizon, byte-identical.
- [ ] **Step 3 — bench.** Create `bench/risk_covariance_bench.cpp` (Google Benchmark, `cost_bench.cpp` style):
  `BM_FactorCovBuild` over a representative synthetic universe (e.g. N≈500–1000, window≈250) reporting build
  time; report the condition number of F for the P4 single-MLE path vs the full S8 path (the §10 compute
  profile — log `cond` as a benchmark counter). Build `atx-engine-bench` to confirm it compiles + runs.
  (Keep N modest so the bench finishes in seconds; document that 3k–5k is the production target.)
- [ ] **Step 4 — close ceremony.** Write `sprint-8.md` (user reference: what S8 proves — robust regression,
  EWMA+NW, eigenfactor de-bias, specific blend, VRA, APCA, shrinkage/PSD, horizon blend; the apply interface
  unchanged; determinism/no-look-ahead/PSD invariants held). Update `ROADMAP.md` (S8 header → shipped; unit
  table S8.5–S8.8 → ✅ with SHAs). Close `sprint-8b-progress.md` (residuals → backlog: ISC Woodbury wiring,
  per-name specific VRA, atx-core L7 lifts `apca`/`nonlinear_shrinkage`/`nearest_corr`/`huber_irls`).
- [ ] **Step 5 — full green + commit** `feat(s8-8): short/long-horizon blend + S8 integration + covariance bench + close`.
  (Do NOT merge `feat/s8` — leave the branch for the controller / user to finish.)

**Acceptance:** the integration test runs data → robust regression → EWMA+NW (horizon-blended) cov →
eigenfactor clean → VRA → specific-risk blend → assemble V → optimize, asserting determinism (byte-identical
replay incl. the seeded eigenfactor step), no look-ahead (truncation-invariance), and SPD V; bench reports
build time + condition-number improvement vs the P4 baseline; ROADMAP + ledger + `sprint-8.md` updated.

---

## §6 — Exit criteria (S8-b)

- S8.5–S8.8 each: TDD, both reviews passed, committed, all gates green, **full `atx-engine-tests` green**.
- Every new build-path behavior opt-in; `CovarianceConfig{}` + `n_stat_factors==0` reproduces S8-a/P4
  (proven by the untouched existing risk suite staying green at every unit).
- Determinism: no new RNG site (S8.3 remains the only one); the integration test proves byte-identical replay
  of the full all-features-on pipeline incl. the seeded eigen-adjust.
- No look-ahead: each new estimator has a truncation-invariance test.
- `FactorModel` apply interface unchanged; `risk`/`apply_inverse`/`neutralize` and their tests untouched.
  The deepened V (and the statistical-factor variant) flow through `PortfolioOptimizer` unchanged.
- The `n_stat_factors` `NotImplemented` is retired (S8.6); `n_dead_factors` stays `NotImplemented` (S7.3).
- Shrinkage/PSD toolkit is PD-guaranteed and usable model-free; integration + bench prove the end-to-end build.
- Ledger `sprint-8b-progress.md` updated per unit; residuals recorded to ROADMAP backlog. **S8 CLOSED**
  (the spec's "merge" step is left as a controller/user decision — do NOT auto-merge `feat/s8`).

## §7 — References

Research deep-dive: §3.2 (APCA 2-pass, T×T Gram), §4.4 (VRA bias stat + λ² EWMA; DVA NOT built), §7
(const-corr LW + MP clip; nonlinear QIS = Pattern-B lift), §8 (Higham nearest-correlation + eigen-clip),
§9 (short/long blend), §11 (USE4S/L, AXUS4 SH/MH half-life tables), §10 (compute profile, bench). Sprint spec
units S8.5–S8.8. Carried-forward invariants: ROADMAP "Carried-forward invariants".

## §8 — Self-review (done after drafting; fixed inline)

- Spec coverage: S8.5↔§4.4 ✓, S8.6↔§3.2 ✓, S8.7↔§7+§8 ✓, S8.8↔§9+§10+§11 ✓ — all four S8-b units mapped.
- Placeholder scan: acceptance tolerances concrete (VRA λ²≈1±5% / λ²>1; APCA |corr|>0.9 + eigenvalue gap;
  LW closed-form match; MP trace-preserved; Higham unit-diag + eig≥0; integration byte-identical replay). ✓
- Type consistency: `CovarianceConfig` extended additively (§3); `RegimeAdjust{lambda2, bias_stats}`,
  `build_stat_factor_model(...) -> Result<FactorModel>`, `constant_correlation_shrinkage`/`mp_clip`/
  `nearest_correlation`/`eigenvalue_clip`/`blend_factor_cov`/`blend_specific` signatures fixed here. `FactorModel`
  interface untouched. ✓
- Backward-compat: every new default = no-op (vra_halflife 0, horizon_blend false, n_stat_factors 0);
  existing suite is each unit's gate. ✓
- Pattern B: `apca` (S8.6), `nonlinear_shrinkage`+`nearest_corr` (S8.7) recorded as atx-core lifts; engine
  ships fallbacks only, `// PATTERN-B:` marked. ✓

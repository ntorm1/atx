# Sprint S8-a — Covariance Construction Core: FROZEN implementation plan

> **For agentic workers:** this is the **frozen *how*** for S8-a (units S8.1–S8.4). The
> **what** lives in the sprint spec [`sprint-8-risk-covariance-construction.md`](sprint-8-risk-covariance-construction.md);
> the research grounding is [`../../research/covariance-matrix-construction-massive-universe-deep-dive.md`](../../research/covariance-matrix-construction-massive-universe-deep-dive.md).
> Execute task-by-task via **superpowers:subagent-driven-development**: fresh implementer per unit, TDD,
> then spec-compliance review, then code-quality review. Steps use checkbox (`- [ ]`) syntax.

**Goal:** deepen the as-built P4 factor risk model from "a correct factored covariance" to "the
covariance a risk shop would actually trade" — robust regression, EWMA+Newey-West factor covariance,
Monte-Carlo eigenfactor de-biasing, and a real specific-risk model — **without changing the
`FactorModel` apply interface** (`risk`/`apply_inverse`/`neutralize` stay verbatim) and **without
weakening any carried-forward invariant** (determinism, no look-ahead, PSD-by-construction).

**Architecture:** new estimators land as focused new headers under `include/atx/engine/risk/` (one
responsibility each); all new behavior is **opt-in via a new `CovarianceConfig`** carried on
`FactorModelConfig`, whose **defaults reproduce the P4 path bit-for-bit** so every existing risk test
stays green. The builder dispatches on the config. atx-core gets no new primitives this sprint —
genuine kernel needs (Huber IRLS) ship engine-local as a fixed-iteration fallback with the Pattern-B
lift recorded.

**Tech stack:** C++20 header-only, Eigen (column-major `MatX`/`VecX`), GoogleTest, Ninja + clang-cl,
`atx::core` linalg (`ols`/`wls`/`symmetric_eig`) + `atx::core::random::Xoshiro256pp`.

---

## §0 — Recon: the as-built state S8-a modifies

Exact anchors (read these before touching anything):

| Concern | Location | As-built behavior |
|---|---|---|
| Factored apply `V=XFXᵀ+D` | [`risk/factor_model.hpp:92-249`](../../include/atx/engine/risk/factor_model.hpp#L92-L249) | `FactorModel::create` / `risk` / `apply_inverse` (Woodbury) / `neutralize`. **DO NOT CHANGE THE INTERFACE.** |
| Builder entry | [`factor_model.hpp:377-429`](../../include/atx/engine/risk/factor_model.hpp#L377-L429) | `build(panel, window, market_cap, group_id)`; rejects `n_stat_factors>0 || n_dead_factors>0` → `NotImplemented` (line 380). |
| Pass A (OLS bootstrap) | [`factor_model.hpp:442-471`](../../include/atx/engine/risk/factor_model.hpp#L442-L471) | `accumulate_ols` → `d0_i = var(OLS resid)`. |
| Pass B (WLS) | [`factor_model.hpp:478-513`](../../include/atx/engine/risk/factor_model.hpp#L478-L513) | `accumulate_wls`, weights `1/d0_i`, emits `fseries` (window×K) + `u_by_inst`. |
| Factor cov `F` | [`factor_model.hpp:349-366`](../../include/atx/engine/risk/factor_model.hpp#L349-L366) | `detail::factor_covariance`: column-demean → MLE cov `S` (÷T) → single scaled-identity LW via `combine::detail::ledoit_wolf_intensity`. |
| Specific var `D` | [`factor_model.hpp:518-528`](../../include/atx/engine/risk/factor_model.hpp#L518-L528) | `specific_variances`: plain `detail::pop_variance` of WLS residuals. |
| **Existing Huber kernel** | [`cost/robust_ls.hpp:124`](../../include/atx/engine/cost/robust_ls.hpp#L124) | **`cost::irls_huber(X, y, RobustCfg{huber_k=1.345, max_iter=50, tol})` → `RobustFit{beta, r2, residuals, iters}`** (S6-1). Deterministic (RNG-free, order-fixed `eval::median` MAD scale, byte-identical; `huber_k=+inf`→OLS). Starts from OLS — **no prior-weight param**. `cost::detail::{huber_weight, mad_scale}`. **S8.1 reuses this — do NOT write a second IRLS kernel.** |
| Config | [`risk/exposures.hpp:108-114`](../../include/atx/engine/risk/exposures.hpp#L108-L114) | `FactorModelConfig{sector_factors, style_mask, n_stat_factors, n_dead_factors, factor_cov_shrink}`. |
| Exposures + `ColumnTag` | [`risk/exposures.hpp:103-135`](../../include/atx/engine/risk/exposures.hpp#L103-L135) | `StyleFactor` enum, `ColumnTag{Kind::{Style,Sector}}`, `ExposureMatrix{x, instrument_rows, columns}`. |

Reusable atx-core (no edge — reuse verbatim):
- `combine::detail::ledoit_wolf_intensity(const MatX& s, const MatX& centered) -> f64` — [`combine/combiner.hpp:254`](../../include/atx/engine/combine/combiner.hpp#L254).
- `core::linalg::ols(X, y)` / `wls(X, y, w)` → `Result<{beta, residuals}>` — [`atx-core/.../linalg/regression.hpp`](../../../atx-core/include/atx/core/linalg/regression.hpp).
- `core::linalg::symmetric_eig(A) -> Result<EigResult{VecX values /*ascending*/, MatX vectors}>` — [`decompose.hpp:127`](../../../atx-core/include/atx/core/linalg/decompose.hpp#L127).
- `core::random::Xoshiro256pp{seed}`; `.normal()` = deterministic standard normal, copy-reproducible — [`atx-core/.../random.hpp:83`](../../../atx-core/include/atx/core/random.hpp#L83).

---

## §1 — Rules (every unit obeys; non-negotiable gates)

**Build/test — the EXACT incantation.** This shell is *not* a VS dev shell by default; every build/test
command MUST prepend the prelude (each tool call is a fresh shell):

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:VCPKG_ROOT='C:\Users\natha\vcpkg'; $env:ATX_DEPS_DIR='C:\atx-cache\deps'
Set-Location 'C:\Users\natha\atx-wt\s8'
# build one test target then run just that suite (fast iterate):
cmake --build --preset ninja --target atx-engine-tests
ctest --preset ninja -R <SuiteRegex> --output-on-failure
```
(The `'vswhere.exe' is not recognized` line is benign — the env is still set. `INCLUDE` becomes set.)

- **TDD always.** Failing GoogleTest first (`#include <gtest/gtest.h>`, `TEST(Subject_Condition_Expected)`),
  watch it fail for the right reason, then implement. Tests are flat under `atx-engine/tests/*_test.cpp`,
  **CMake-globbed** — drop the file, rebuild, do **not** hand-edit `CMakeLists.txt`. Match the style of
  [`risk_factor_builder_test.cpp`](../../tests/risk_factor_builder_test.cpp) (anonymous namespace, `using`
  aliases, synthetic `PanelView` fixtures).
- **Gates (a unit is done only when all pass):** `/W4 /permissive- /WX` clean (any warning fails),
  `/fp:precise`, clang-format clean, the unit's new suite green, **and the full `atx-engine-tests` suite
  still green** (esp. `RiskFactorBuilder*`/`RiskFactorModel*`). **Do NOT run clang-tidy** (disabled repo-wide).
- **Determinism (carried invariant #1).** No clock, no map iteration, no unseeded RNG. All reductions in
  canonical ascending order (instrument, then factor; ascending lag). **S8.3 is the ONLY new RNG site** —
  its `Xoshiro256pp` seed is a config field, **recorded** so a re-run is byte-identical, and its
  simulation loop is **fixed-count** (no convergence early-exit). Prove byte-identity in the test.
- **No look-ahead (carried invariant #2).** Every estimate is fit on the trailing window only. The test is
  **truncation-invariance**: appending older rows beyond the window+lookback must not change the estimate.
- **Backward-compat.** New behavior is opt-in (see §3). `CovarianceConfig{}` default ⇒ the P4 path,
  byte-for-byte. Existing risk tests are part of every unit's done-gate.
- **Pattern B.** No new general-purpose primitive in the engine. S8.1 needs Huber IRLS from atx-core
  (recorded in spec + ROADMAP); ship the engine-side fixed-iteration fallback **only**, marked with a
  `// PATTERN-B:` comment naming the atx-core lift.
- **Commit per unit:** `feat(s8-N): <summary>` with trailer
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Stage **explicit pathspecs**
  (never `git add -A`). **Do not push.** This worktree is on its own branch `feat/s8` (no shared-ref race).

---

## §2 — Files (S8-a)

| Unit | Create | Modify |
|---|---|---|
| S8.1 | `tests/risk_robust_regression_test.cpp` | `risk/exposures.hpp` (expose √-cap weight + cap-weighted industry-sum-to-zero constraint descriptor); `risk/factor_model.hpp` (robust IRLS in Pass A/B, opt-in) |
| S8.2 | `risk/cov_ewma.hpp`, `tests/risk_cov_ewma_test.cpp` | `risk/exposures.hpp` (`CovarianceConfig` fields — §3); `risk/factor_model.hpp` (`build` dispatches to EWMA+NW when enabled) |
| S8.3 | `risk/eigen_adjust.hpp`, `tests/risk_eigen_adjust_test.cpp` | `risk/exposures.hpp` (`eigen_adjust_*` fields); `risk/factor_model.hpp` (`build` applies adjustment to `F` when enabled) |
| S8.4 | `risk/specific_risk.hpp`, `tests/risk_specific_risk_test.cpp` | `risk/exposures.hpp` (`spec_*`, `structural_blend` fields); `risk/factor_model.hpp` (`specific_variances` → blended estimator when enabled) |

All new headers: `#pragma once`, namespace `atx::engine::risk`, `detail` sub-namespace for kernels,
`[[nodiscard]]`, `noexcept` on leaf math, `Result<T>` at fallible boundaries, exhaustive switches,
functions ≤ ~60 lines. Document the contract (§9 of the C++ profile), `// SAFETY:` every deviation.

---

## §3 — `CovarianceConfig` (the one config change; added in S8.2, extended per unit)

Add to [`risk/exposures.hpp`](../../include/atx/engine/risk/exposures.hpp) (before `FactorModelConfig`),
and add a member `CovarianceConfig cov{};` to `FactorModelConfig`. **Every default reproduces P4.**

```cpp
// Half-lives are in observations (rows). 0 / the default enum value selects the P4 path so existing
// estimates are unchanged. All EWMA/NW reductions are order-fixed (ascending lag, then factor).
enum class FactorCovMethod : atx::u8 { LedoitWolfSingle /*P4 default*/, EwmaNeweyWest };
enum class SpecificRiskMethod : atx::u8 { PopVariance /*P4 default*/, EwmaNeweyWestStructural };

struct CovarianceConfig {
  // --- S8.1 robust regression ---
  bool robust_regression = false;     // false ⇒ plain inverse-d0 WLS (P4)
  atx::f64 huber_c = 1.345;           // Huber tuning constant (95% Gaussian efficiency)
  atx::usize robust_iters = 5;        // FIXED IRLS iterations (determinism: no convergence exit)
  bool cap_weight = false;            // √-cap instrument weighting (needs market_cap)
  bool industry_sum_to_zero = false;  // cap-weighted industry-dummy sum-to-zero constraint
  // --- S8.2 EWMA + Newey-West factor covariance ---
  FactorCovMethod factor_cov_method = FactorCovMethod::LedoitWolfSingle;
  atx::usize vol_halflife = 0;        // fast HL for variances   (0 ⇒ unused; e.g. 60 short-horizon)
  atx::usize corr_halflife = 0;       // slow HL for correlations (0 ⇒ unused; e.g. 125)
  atx::usize nw_lags = 0;             // Newey-West Bartlett lags (0 ⇒ no serial-corr adjustment)
  // --- S8.3 eigenfactor risk adjustment (the ONLY new RNG) ---
  atx::usize eigen_adjust_sims = 0;   // 0 ⇒ no adjustment; e.g. 1000 sims when enabled
  atx::f64 eigen_adjust_amplify = 1.0;// a in γ(k)=a(v(k)−1)+1. DEFAULT 1.0 (NOT the paper's 1.4)
  atx::u64 eigen_adjust_seed = 0;     // recorded; same seed ⇒ byte-identical F̂
  // --- S8.4 specific risk ---
  SpecificRiskMethod specific_method = SpecificRiskMethod::PopVariance;
  atx::usize spec_halflife = 0;       // EWMA HL for specific-return variance (0 ⇒ unused)
  atx::usize spec_nw_lags = 0;        // Newey-West lags for specific autocorrelation
  bool structural_blend = false;      // blend thin-history names toward ln-vol-on-exposures model
};
```

---

## §4 — Algorithms (the reference math each kernel implements)

**EWMA weights (S8.2/S8.4).** For half-life `H`, the weight on the observation `k` steps back
(`k=0` newest) is `w_k = 2^(−k/H)`. Normalize: `Σ_k w_k`. EWMA mean `μ = (Σ w_k x_k)/(Σ w_k)`; EWMA
covariance entry `Cov_ij = (Σ_k w_k (x_{i,k}−μ_i)(x_{j,k}−μ_j)) / (Σ_k w_k)`. **Split half-lives:**
variances use `vol_halflife`, correlations use `corr_halflife`; recombine `F_ij = ρ_ij·σ_i·σ_j` with
`σ_i=√Var_i(vol HL)` and `ρ_ij` from the `corr_halflife` EWMA correlation. Closed-form check: with a
constant series the EWMA mean equals the constant; the lag-`k` weight ratio is exactly `2^(−1/H)`.

**Newey-West (S8.2/S8.4).** Bartlett-kernel serial-correlation adjustment to a covariance estimated from
a (factor-return or specific-return) series:
`F_NW = Γ_0 + Σ_{d=1..L} (1 − d/(L+1)) (Γ_d + Γ_dᵀ)`, where `Γ_d` is the lag-`d` autocovariance
`Γ_d = (1/T) Σ_t (x_t−μ)(x_{t−d}−μ)ᵀ` (order-fixed ascending `d`, then factor). For a scalar AR(1) with
autocorrelation `φ`, NW inflates the long-run variance toward `σ²·(1+2Σ_{d}(1−d/(L+1))φ^d)` — the
analytic target the test asserts. Re-symmetrize and (if it lost PSD) defer to S8.7's repair; for S8-a a
small ridge / eigenvalue floor to keep `FactorModel::create`'s Cholesky succeeding is acceptable, noted.

**Robust IRLS + √-cap (S8.1) — REUSE `cost::irls_huber`, do not reinvent.** The engine already ships a
tested, deterministic IRLS-Huber kernel ([`cost/robust_ls.hpp`](../../include/atx/engine/cost/robust_ls.hpp),
S6-1). S8.1 composes √-cap *prior* weighting with that Huber kernel rather than writing a second IRLS loop:
- **Preferred:** minimally + additively generalize `cost::irls_huber` to accept an optional prior-weight
  vector `w0` (default `Ones` ⇒ existing behavior + S6 tests unchanged): the per-iteration weight becomes
  `ω_i = w0_i · huber_weight(r_i/s, k)`. S8.1 passes `w0_i = rootcap_i` (and/or `1/d0_i` to keep the
  inverse-specific-variance WLS character). `rootcap_i = √(cap_i)/mean_j √(cap_j)` (down-weights mega-caps
  without letting them dominate; `Ones` when `cap_weight=false`). This keeps **one** Huber kernel (honors
  "no second general-purpose primitive"), reuses the order-fixed MAD scale + byte-identity guarantee.
- The IRLS retains its deterministic convergence exit (RNG-free, order-fixed ⇒ byte-identical run-to-run,
  proven by S6-1's C5 test); `robust_iters` maps to `RobustCfg::max_iter`. No new RNG, no parallelism exit.
- Industry-sum-to-zero: resolve the market/industry-dummy collinearity via a cap-weighted sum-to-zero
  constraint descriptor emitted by `exposures.hpp`; enforce by mean-centering the cap-weighted industry
  dummy columns (the as-built `kNeutralizeRidge` is the current crutch). Document the chosen mechanism.
- **PATTERN-B:** the atx-core `huber_irls` (L7) remains the recorded lift; until it lands, the engine's
  `cost::irls_huber` (generalized) is the home. Mark the risk→cost include + the generalization
  `// PATTERN-B: reuse S6-1 cost::irls_huber; atx-core huber_irls is the eventual L7 home.`

**Eigenfactor risk adjustment (S8.3) — Menchero-Wang-Orr.** Given `F` (K×K SPD):
1. `symmetric_eig(F) → U (cols=eigenvectors), D (ascending eigenvalues)`. Treat `F=U D Uᵀ` as truth.
2. For `m = 1..M` (`eigen_adjust_sims`, fixed): draw `b_m ∈ ℝ^{K×T}` with `b_m[k,t] ~ N(0, D[k])` using
   the seeded `Xoshiro256pp` (`σ_k·normal()`); simulated factor-return history `f_m = U b_m` (K×T).
   Re-estimate the simulated covariance `F̃_m = (1/T) f_m f_mᵀ`. **Eigendecompose the SAMPLE covariance**
   `F̃_m = Û_m·D̂_m·Û_mᵀ` (its OWN sample eigenbasis Û_m — NOT the true U). The per-eigenfactor bias is the
   ratio of the *true* variance along the sample direction to the sample eigenvariance the model reports:
   accumulate `v(k) += √( û_{m,k}ᵀ·F·û_{m,k} / D̂_m[k] )`.
   > **DO NOT** project onto the true basis (`diag(Uᵀ F̃_m U)`) — that estimator is *unbiased* (→ `v(k)≈1`
   > for all k) and cannot correct anything. The de-bias must be measured in the SAMPLE eigenbasis, which is
   > what one actually estimates from data (Menchero-Wang-Orr USE4 App. B; research §4.3). Empirically the
   > sample-basis form reproduces the research anchor `v_small≈1.37, v_large≈0.76` at T/K=4; the true-basis
   > form gives `v≈0.999` everywhere. *(This corrects the original frozen-plan formula — confirmed in the
   > S8.3 spec review.)*
3. `v(k) = (1/M) Σ_m √( û_{m,k}ᵀ·F·û_{m,k} / D̂_m[k] )` (simulated vol bias); `γ(k) = a·(v(k)−1)+1`,
   `a = eigen_adjust_amplify` (**default 1.0**). Rescale `D̂(k) = γ(k)²·D(k)`; rotate back
   `F̂ = U D̂ Uᵀ`. **PSD-preserving** (γ²>0). `T` for the inner sim is a fixed deterministic function of K
   (as-built: `T = max(100·K, 200)` — the apply path never sees the fit window); fix it and document.
   `a=0 ⇒ γ≡1 ⇒ F̂≡F` (identity — a test case).

**Specific-risk blend (S8.4).** Per current-cross-section instrument:
`σ_n^TS = √(EWMA-var(spec returns, spec_halflife) · NW-inflation(spec_nw_lags))`. Structural model:
regress `ln σ_n^TS` on the instrument's exposure row over names with clean full history (via
`core::linalg::ols`), predict `ln σ_n^STR` for all; `σ_n^STR = exp(·)`. Blend
`σ_n = γ_n·σ_n^TS + (1−γ_n)·σ_n^STR`, with `γ_n → 1` for full clean history (≥ window residuals) and
`γ_n → 0` for thin/violated history (few residuals). `D_n = σ_n²`, floored (`create` re-floors anyway).
ISC (issuer-specific covariance) is **interface-only** this sprint — define a `std::vector<{i,j,cov}>`
off-diagonal carrier on the result struct, leave it empty, do not wire it into the Woodbury path.

---

## §5 — Per-unit task breakdown

### Task S8.1 — Robust cross-sectional factor regression (√-cap + Huber)

**Files:** Modify `risk/exposures.hpp`, `risk/factor_model.hpp`. Create `tests/risk_robust_regression_test.cpp`.
Research §3.1.

- [ ] **Step 1 — failing test (closed-form Huber down-weight).** Build a synthetic single-date panel `r = X·f_true + u`
  with all residuals zero except ONE planted fat-tailed outlier instrument. Add the `cov` config with
  `robust_regression=true, robust_iters=5`. Assert: the robust factor-return vector is **materially closer**
  (smaller L2 distance) to `f_true` than the plain-WLS factor return on the same panel. Run → fails (robust path absent).
- [ ] **Step 2 — implement (reuse `cost::irls_huber`).** Add the `CovarianceConfig` robust fields (§3).
  Generalize `cost::irls_huber` to take an optional prior-weight vector `w0` (default `Ones` — S6 tests
  must stay green). In `factor_model.hpp` add an `accumulate_robust` path (guarded by
  `cfg.cov.robust_regression`; default keeps `accumulate_ols`/`_wls`) that builds `w0_i = rootcap_i`
  (and/or `1/d0_i`) per kept instrument and calls the generalized `cost::irls_huber`. Mark
  `// PATTERN-B: reuse S6-1 cost::irls_huber; atx-core huber_irls is the eventual L7 home.`
  Confirm the S6-1 `RobustLs*` suite still passes (the `w0=Ones` default path is unchanged).
- [ ] **Step 3 — √-cap + constraint.** In `exposures.hpp` expose the √-cap weight helper and a
  cap-weighted industry-sum-to-zero constraint descriptor; wire `cap_weight` / `industry_sum_to_zero`.
- [ ] **Step 4 — determinism + green.** Add a byte-identity test (same inputs twice → identical factor
  returns; the IRLS is RNG-free). Build + run new suite + **full `atx-engine-tests`** (`RiskFactorBuilder*`
  must stay green because `robust_regression` defaults false). All gates.
- [ ] **Step 5 — commit** `feat(s8-1): robust √-cap + Huber IRLS cross-sectional factor regression`.

**Acceptance:** planted-outlier panel → robust factor returns measurably closer to the clean-panel factor
returns than plain WLS (Huber down-weight suppresses the outlier's leverage); determinism digest stable;
existing `risk_factor_builder_test.cpp` green. (Exact L2 tolerance: assert `dist_robust < dist_wls` and
`dist_robust < 0.5·dist_wls` on the constructed fixture — tune the planted-outlier magnitude so the
strict inequality holds with margin.)

### Task S8.2 — EWMA factor covariance (split vol/corr half-lives) + Newey-West

**Files:** Create `risk/cov_ewma.hpp`, `tests/risk_cov_ewma_test.cpp`. Modify `exposures.hpp` (config),
`factor_model.hpp` (dispatch). Research §4.1–4.2.

- [ ] **Step 1 — failing test (EWMA weight closed form).** In `risk_cov_ewma_test.cpp` assert the EWMA
  variance of a known series equals the closed-form `Σ w_k (x_k−μ)² / Σ w_k`, `w_k=2^(−k/H)`; and the
  lag-`k`/lag-`k+1` weight ratio `== 2^(−1/H)` exactly. Fails (header absent).
- [ ] **Step 2 — implement `cov_ewma.hpp`.** `ewma_factor_covariance(fseries, vol_hl, corr_hl, nw_lags)`:
  EWMA variances at `vol_hl`, EWMA correlations at `corr_hl`, recombine `F_ij=ρ_ij σ_i σ_j`, then add the
  Newey-West Bartlett sum (§4). Order-fixed. Return `MatX` (SPD; eigen-floor if NW broke PSD, documented).
- [ ] **Step 3 — Newey-West test.** Construct a factor-return series with known AR(1) `φ`; assert the NW
  long-run variance matches the analytic `σ²(1+2Σ_d(1−d/(L+1))φ^d)` within ULP-bounded tolerance.
- [ ] **Step 4 — conditioning test + dispatch.** On a `T≈K` window assert `cond(F_ewma_split) < cond(F_mle_single)`.
  Wire `build` to call EWMA when `factor_cov_method==EwmaNeweyWest`; default path unchanged.
- [ ] **Step 5 — green + commit** `feat(s8-2): EWMA split-half-life factor covariance + Newey-West`.

**Acceptance:** EWMA lag-k weight `==2^(−k/H)`; AR(1) series → analytic NW inflation; `F` SPD (Cholesky
succeeds); split half-lives better-conditioned than single-window MLE at `T≈K`; existing tests green.

### Task S8.3 — Eigenfactor risk adjustment (Monte-Carlo de-biasing)

**Files:** Create `risk/eigen_adjust.hpp`, `tests/risk_eigen_adjust_test.cpp`. Modify `exposures.hpp`
(config), `factor_model.hpp` (apply to `F`). Research §4.3.

- [ ] **Step 1 — failing test (identity at a=0).** `eigen_adjust(F, sims=100, amplify=0.0, seed=…)` must
  return `F` unchanged (γ≡1). Fails (header absent).
- [ ] **Step 2 — implement `eigen_adjust.hpp`.** The MWO algorithm (§4) using `symmetric_eig` +
  `Xoshiro256pp{seed}.normal()`. Fixed `sims` loop. Return `Result<MatX>` (SPD).
- [ ] **Step 3 — bias-correction test.** Construct `F` with a known small-eigenvalue direction; assert the
  smallest eigenfactor's measured `v(k)>1` and the adjustment **inflates** its eigenvariance (the §1
  under-forecast is corrected); assert `F̂` SPD.
- [ ] **Step 4 — seed determinism (the invariant crux).** Same seed twice → **byte-identical** `F̂`
  (`bit_cast<u64>` element compare); two different seeds → different `F̂`. Wire `build` to apply when
  `eigen_adjust_sims>0`; record the seed used. Default (`sims=0`) ⇒ no-op.
- [ ] **Step 5 — green + commit** `feat(s8-3): Monte-Carlo eigenfactor risk adjustment (seeded, a=1.0)`.

**Acceptance:** `a=0` is identity; smallest eigenfactor bias `v(k)>1` and its eigenvariance is inflated;
`F̂` SPD; **same seed ⇒ byte-identical**, different seed ⇒ differs; default off; existing tests green.

### Task S8.4 — Specific-risk model: EWMA + Newey-West + structural blend

**Files:** Create `risk/specific_risk.hpp`, `tests/risk_specific_risk_test.cpp`. Modify `exposures.hpp`
(config), `factor_model.hpp` (`specific_variances` dispatch). Research §5.

- [ ] **Step 1 — failing test (thin-history → structural).** Two instruments: one full clean history, one
  with only a few residuals. With `specific_method=EwmaNeweyWestStructural, structural_blend=true`, assert
  the thin-history name gets `γ≈0` and inherits the structural estimate (NOT a noisy near-zero variance).
  Fails (path absent).
- [ ] **Step 2 — implement `specific_risk.hpp`.** EWMA variance (`spec_halflife`) × NW inflation
  (`spec_nw_lags`) → `σ^TS`; `ln σ^TS`-on-exposures OLS → `σ^STR`; blend by `γ_n` (history depth). Define
  the **interface-only** ISC off-diagonal carrier (empty; not wired). 
- [ ] **Step 3 — recency + recovery tests.** EWMA weights recent residuals more (a late variance shift
  moves the estimate more than an equally-sized early shift); a full-history i.i.d.-residual instrument
  recovers ≈ population variance (within tolerance); `D` stays `> 0` (floored).
- [ ] **Step 4 — dispatch + green.** Wire `build`'s `specific_variances` to the blended estimator when
  enabled; default `PopVariance` path byte-identical. Full suite green.
- [ ] **Step 5 — commit** `feat(s8-4): specific-risk EWMA + Newey-West + structural blend`.

**Acceptance:** thin-history name → `γ→0`, structural value (no spurious near-zero); EWMA recency-weights;
full-history i.i.d. recovers ≈ pop variance; `D>0`; existing tests green.

---

## §6 — Exit criteria (S8-a)

- S8.1–S8.4 each: TDD, both reviews passed, committed, all gates green, **full `atx-engine-tests` green**.
- Every new behavior opt-in; `CovarianceConfig{}` default reproduces P4 (proven by the untouched existing
  risk suite staying green at every unit).
- Determinism: S8.3 seed recorded, fixed-count sims, byte-identical replay proven.
- No look-ahead: each estimator has a truncation-invariance test.
- `FactorModel` apply interface unchanged; `risk`/`apply_inverse`/`neutralize` and their tests untouched.
- Ledger `sprint-8a-progress.md` updated per unit; residuals (NW-PSD repair, ISC wiring, `huber_irls`/
  `apca` atx-core lifts) recorded to ROADMAP backlog for S8-b / atx-core. **Hand off to S8-b** (VRA, APCA,
  shrinkage/PSD toolkit, short/long blend + integration + bench + close).

## §7 — References

Research deep-dive sections: §1 (sample-cov failure), §3.1 (robust regression), §4.1–4.2 (EWMA + NW),
§4.3 (eigenfactor MWO — **a=1.0 caveat**), §5 (specific risk). Sprint spec units S8.1–S8.4.

## §8 — Self-review (done after drafting; fix inline)

- Spec coverage: S8.1↔§3.1, S8.2↔§4.1–4.2, S8.3↔§4.3, S8.4↔§5 — all four S8-a units mapped. ✓
- Placeholder scan: acceptance tolerances are concrete (S8.1 `<0.5·dist_wls`; S8.2 weight `==2^(−k/H)`,
  AR(1) analytic; S8.3 byte-identity; S8.4 `γ→0`/pop-var recovery). ✓
- Type consistency: one `CovarianceConfig` (§3) extended additively per unit; member `cov` on
  `FactorModelConfig`; enums `FactorCovMethod`/`SpecificRiskMethod`. `FactorModel` interface untouched. ✓
- Backward-compat: every default = P4; existing suite is each unit's gate. ✓

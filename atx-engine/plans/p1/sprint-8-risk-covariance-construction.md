# Sprint S8 — Vendor-Grade Risk Model: Covariance Construction & Cleaning (sprint spec)

**Status:** ⏳ proposed (not open). **Consumes** P4 (`risk::FactorModel` + `FactorModelBuilder` + `PortfolioOptimizer`) and S6 (calibrated cost for the optimizer turnover term). **No** S3/S4/S5 dependency — runs on the *truth/throughput/cost* track, not the factory track.
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md) · **Quality gate:** [`../docs/implementation-quality.md`](../docs/implementation-quality.md)
**Grounded in:** [`../../research/covariance-matrix-construction-massive-universe-deep-dive.md`](../../research/covariance-matrix-construction-massive-universe-deep-dive.md)
(the deep-research deep-dive: MSCI/Barra USE4 + Axioma/Qontigo AXUS4/AXWW4 methodology, Ledoit-Wolf shrinkage, RMT/eigenfactor cleaning, EWMA+Newey-West, PSD enforcement, short/long-horizon blending — 25/25 adversarially-verified claims, primary vendor + peer-reviewed sources). Section references below (e.g. *research §4.3*) point into that file.

---

## Why this sprint

P4 shipped a **correct but minimal** Barra-style risk model: [`risk/factor_model.hpp`](../../include/atx/engine/risk/factor_model.hpp) keeps the covariance in factored form `V = X F Xᵀ + D`, applies it via Woodbury (`apply_inverse`, O(MK+K³), never a dense M×M), and `FactorModelBuilder::build` estimates `(X, F, D)` from a trailing window. But the *estimation quality* is where commercial risk shops spend their entire methodology budget, and the as-built has explicit gaps the code itself flags:

- **Factor covariance `F`** = a single MLE sample covariance shrunk once toward `m·I` (scaled-identity Ledoit-Wolf, [`factor_model.hpp:349`](../../include/atx/engine/risk/factor_model.hpp#L349)). No separate volatility/correlation half-lives, no EWMA decay, no Newey-West serial-correlation adjustment, no eigenfactor cleaning — the four layers every vendor applies (*research §4*).
- **Specific variance `D`** = plain population variance of WLS residuals ([`factor_model.hpp:518`](../../include/atx/engine/risk/factor_model.hpp#L518)). No EWMA, no Newey-West, no structural blend for thin-history names, no issuer-specific covariance (*research §5*).
- **Statistical factors** (`cfg.n_stat_factors > 0`) → hard `NotImplemented` ([`factor_model.hpp:380`](../../include/atx/engine/risk/factor_model.hpp#L380)) — the APCA path (*research §3.2*) is unbuilt.
- **Regression** = inverse-specific-variance WLS only; no robust Huber re-weighting and no √-cap weighting, so a single fat-tailed return date tilts the factor returns (*research §3.1*).
- **No regime responsiveness** (VRA/DVA, *research §4.4*), **no model-free shrinkage path** (constant-correlation LW / nonlinear / RMT, *research §7*), **no explicit PSD repair** beyond the construction Cholesky (*research §8*), **no short/long-horizon blend** (*research §9*).

At a 3,000–5,000-stock universe (the research's target scale) these are not cosmetic: the sample factor covariance's smallest eigenfactors under-forecast realized risk by ~40% (*research §1, §4.3*), and an optimizer fed that biased `V` systematically over-loads the directions it thinks are safe. **S8 closes the gap between "a correct factored covariance" and "the covariance a risk shop would actually trade."**

This sprint deepens the *risk / optimization / statistical-analysis* layer so atx-engine matches Barra/Axioma-grade construction, while preserving every carried-forward invariant — determinism (the Monte-Carlo eigenfactor step is the one new RNG site, seeded and recorded), no look-ahead (every EWMA/NW/structural fit is trailing-window, truncation-invariant), and PSD-by-construction.

---

## Relationship to S7

S8 has **no hard dependency on S7** and S7 has none on S8 — but they share a seam and S8 **strengthens** S7. Specifically:

- S7.1's `MultiPeriodOptimizer` ([`risk/multi_period.hpp`](../../include/atx/engine/risk/multi_period.hpp)) calls `PortfolioOptimizer::solve` with a `FactorModel` per period. S8 improves the *quality* of that `FactorModel` without changing the optimizer's interface — `apply_inverse`/`risk` are untouched, so the multi-period driver consumes the cleaned `V` for free.
- S7.3 (dead-alpha → risk-factor) and S8.6 (statistical factors) both fill the **same reserved factor-block extension** (`cfg.n_dead_factors` / `cfg.n_stat_factors`, the twin `NotImplemented` rungs). S8.6 builds the statistical-factor machinery (APCA + endogenous factor append); S7.3 reuses that machinery to append dead-alpha directions. **Recommendation: open S8 before or alongside S7** so S7's optimizer trades the deepened covariance and S7.3 inherits S8.6's append path.

---

## Scope — units

> This is a **9-unit body** — it exceeds the 4–7-unit sprint ceiling ([`../docs/sprint.md`](../docs/sprint.md) §"What a sprint is"), so it **splits at kickoff** into:
> - **S8-a** — *Covariance construction core*: S8.1–S8.4 (robust regression, EWMA+NW factor cov, eigenfactor cleaning, specific-risk model). The "build the matrix right" half.
> - **S8-b** — *Regime, statistical factors, shrinkage & integration*: S8.5–S8.8 (VRA, APCA statistical factors, model-free shrinkage+PSD toolkit, short/long blend + integration + close).
>
> Both sub-sprints get their own ledger (`sprint-8a-progress.md`, `sprint-8b-progress.md`) and frozen implementation plan at their respective kickoffs. The unit numbering (`S8-1`…`S8-8`) is continuous across the split.

New estimation kernels land as **focused new headers** under [`include/atx/engine/risk/`](../../include/atx/engine/risk/) (one responsibility each, matching the existing `exposures.hpp`/`factor_model.hpp`/`optimizer.hpp` split), and the config grows on `FactorModelConfig` (or a new `CovarianceConfig` carried alongside it). Every code unit ships a test file under [`atx-engine/tests/`](../../tests/) (flat, CMake-globbed, `ATS_TEST(...)`, no bare `return 1`).

### S8.0 — Marker + ledger
Open `sprint-8a-progress.md`, freeze scope, record `Base: master @ <SHA>`. Point [`ROADMAP.md`](ROADMAP.md) at the new ledger.

### S8.1 — Robust cross-sectional factor regression (√-cap + Huber)
*Grounds: research §3.1 (Axioma single-stage root-cap, Huber-weighted, industry-sum-to-zero regression).*
Upgrade `FactorModelBuilder`'s Pass A/B ([`factor_model.hpp:442-513`](../../include/atx/engine/risk/factor_model.hpp#L442-L513)) from inverse-specific-variance WLS to a **robust IRLS** factor regression: weight each instrument by `huberᵢ · √capᵢ` (√-cap down-weights mega-caps without letting them dominate; Huber de-sensitizes fat-tailed return dates), iterating the Huber weights to a **fixed iteration count** (determinism — no convergence early-exit, the §3.2 rule the optimizer already follows). Add the **cap-weighted industry-sum-to-zero constraint** to resolve the market-vs-industry-dummy collinearity (currently handled only by the `kNeutralizeRidge` guard).
- **Files:** modify [`risk/exposures.hpp`](../../include/atx/engine/risk/exposures.hpp) (expose √-cap weight + emit a constraint descriptor) and [`risk/factor_model.hpp`](../../include/atx/engine/risk/factor_model.hpp) (`accumulate_ols`/`accumulate_wls` → `accumulate_robust`). **Create:** `tests/risk_robust_regression_test.cpp`.
- **atx-core (Pattern B):** **L7 robust/Huber IRLS least-squares** (`huber_irls` over the existing `wls`). Engine-side fallback: a fixed-iteration IRLS loop wrapping `core::linalg::wls` (shippable now; the dedicated kernel is the lift).
- **Acceptance:** on a panel with one planted outlier return date, robust factor returns are materially closer to the clean-panel factor returns than plain WLS is (the Huber down-weight measurably suppresses the outlier's leverage); determinism digest stable across runs; existing `risk_factor_builder_test.cpp` stays green. (Exact tolerance fixed in the frozen kickoff impl-plan.)

### S8.2 — EWMA factor covariance with split vol/correlation half-lives + Newey-West
*Grounds: research §4.1 (EWMA `W_{t−k}=2^{−k/H}`, longer correlation half-life than volatility), §4.2 (Newey-West Bartlett-kernel serial-correlation adjustment).*
Replace the single MLE-cov → scaled-identity-LW path ([`detail::factor_covariance`](../../include/atx/engine/risk/factor_model.hpp#L349)) with the vendor recipe: (1) EWMA **variances** at the fast `vol_halflife`; (2) EWMA **correlations** at the slow `corr_halflife` (so effective obs ≫ K → `F` stays well-conditioned); (3) recombine `F_ij = ρ_ij·σ_i·σ_j`; (4) **Newey-West** add `Σ_{d=1..L}(1−d/(L+1))(Γ_d+Γ_dᵀ)` with `nw_lags` Bartlett lags. All reductions order-fixed (ascending lag, then factor). The existing single-shrink LW remains available as a config fallback for short windows.
- **Files:** **Create** `risk/cov_ewma.hpp` (the EWMA+NW factor-covariance kernel). Modify `FactorModelConfig` (add `vol_halflife`, `corr_halflife`, `nw_lags`; sensible defaults from research §11 — e.g. short-horizon 60/125/2). Modify `FactorModelBuilder::build` to call it. **Create:** `tests/risk_cov_ewma_test.cpp`.
- **atx-core:** none new (order-fixed reductions + existing `linalg`).
- **Acceptance:** EWMA weight on lag k equals `2^{−k/H}` (closed-form check); a known AR(1) factor-return series produces the analytically-expected Newey-West inflation; `F` is SPD (Cholesky succeeds); split half-lives produce a better-conditioned `F` (lower condition number) than the single-window MLE on a `T≈K` window.

### S8.3 — Eigenfactor risk adjustment (Monte-Carlo de-biasing)
*Grounds: research §4.3 (Menchero-Wang-Orr USE4 eigenfactor adjustment — the replicable RMT cleaning algorithm).*
The headline cleaning unit. Diagonalize `F = U·D·Uᵀ`; treating `F` as truth, **simulate** M return histories `f_m = U·b_m` with `b_m[k,:] ~ N(0, D[k])` using a **seeded** `core::random::Xoshiro256pp`; re-estimate + project each simulated covariance onto the original eigenbasis; measure per-eigenfactor volatility bias `v(k) = (1/M)Σ√(D̃_m(k)/D_m(k))`; rescale `D̂(k) = γ²(k)·D(k)` with `γ(k)=a·(v(k)−1)+1`; rotate back `F̂ = U·D̂·Uᵀ` (**PSD-preserving** since `γ²>0`).
- **CRITICAL (research §4.3 caveat):** default `a = 1.0` (the un-amplified shipping-USE4 value), **not** the paper's `a = 1.4` — the amplified version induces small pure-factor biases. `a` is a config knob.
- **DETERMINISM:** this is the **only new RNG site** in S8. The seed is a config field, is **recorded in the FactorModel artifact**, and the M-simulation loop runs a fixed count. Same seed → byte-identical `F̂`. This is the carried-forward invariant #1's exact stress point.
- **Files:** **Create** `risk/eigen_adjust.hpp`. Reuse `core::linalg::symmetric_eig` (eigenvalues ascending) + `core::random::Xoshiro256pp`. Modify `FactorModelConfig` (`eigen_adjust_sims`, `eigen_adjust_amplify=1.0`, `eigen_adjust_seed`). **Create:** `tests/risk_eigen_adjust_test.cpp`.
- **Acceptance:** on a synthetic `F` with known eigenstructure, the smallest eigenfactor's bias `v(k) > 1` and the adjustment inflates its eigenvariance (the §1 under-forecast is corrected); `F̂` is SPD; `a=0` is the identity transform; seed-determinism proof (same seed == byte-identical, two seeds differ).

### S8.4 — Specific-risk model: EWMA + Newey-West + structural blend
*Grounds: research §5 (USE4/Axioma specific risk — EWMA of specific returns, NW autocorrelation, structural-model blend for thin history, ISC for linked issuers).*
Replace plain `pop_variance` of residuals ([`specific_variances`](../../include/atx/engine/risk/factor_model.hpp#L518)) with: (1) **EWMA** variance of the per-instrument specific-return series at `spec_halflife`; (2) **Newey-West** autocorrelation correction; (3) a **structural model** `ln σ_n^STR = Σ_k X_nk b_k` (regress log specific-vol on exposures — reuse `core::linalg::ols`); (4) **blend** `σ_n = γ_n·σ_n^TS + (1−γ_n)·σ_n^STR`, `γ_n=1` for clean full history, `→0` for thin/violated (IPOs, gaps). Optional **ISC hook**: a sparse off-diagonal list of same-issuer security-line covariances (the one place `D` is not strictly diagonal) — **interface only this sprint**, populated when a multi-share-class universe exists.
- **Files:** **Create** `risk/specific_risk.hpp`. Modify `FactorModelBuilder` to call it; extend the `D` carrier to optionally hold ISC off-diagonal pairs (the `FactorModel` Woodbury path must accept a block-diagonal `D` — a bounded extension, or keep diagonal + a separate small correction). Modify `FactorModelConfig` (`spec_halflife`, `spec_nw_lags`, `structural_blend`). **Create:** `tests/risk_specific_risk_test.cpp`.
- **Acceptance:** a thin-history instrument (few residuals) gets `γ→0` and inherits the structural estimate (not a noisy near-zero variance); EWMA recency-weights recent residuals more; full-history instrument with i.i.d. residuals recovers ≈ the population variance; `D` stays floored > 0.

### S8.5 — Volatility Regime Adjustment (VRA)
*Grounds: research §4.4 (USE4 VRA; Axioma DVA — DVA is patented, VRA is the open analogue).*
Add a regime multiplier on top of the EWMA half-lives so `F` (and `D`) react to sudden vol regimes the fixed half-life can't track. Compute the **factor cross-sectional bias statistic** `B_t = √((1/K)Σ_j (f_{j,t}/σ_{j,t})²)` (realized vs predicted factor moves; `E[B_t²]=1` if unbiased), EWMA `B_t²` over `vra_halflife` → multiplier `λ²`, rescale `F ← λ²·F` (and the specific-risk analogue for `D`). Emit `B_t` as a **risk diagnostic** (the statistical-analysis surface: a bias time series the user can inspect). Note DVA (overlapping-segment dispersion rescaling with the `0.9 ≤ δ_n/δ_{n−1} ≤ 1.1` ratio constraint + cubic-spline) as the documented proprietary alternative, not built.
- **Files:** **Create** `risk/vol_regime.hpp`. Modify `FactorModelConfig` (`vra_halflife`). **Create:** `tests/risk_vol_regime_test.cpp`.
- **Acceptance:** on a panel with an injected vol spike, `λ²>1` inflates the forecast in the high-vol window and reverts after; `λ²≡1` (the no-op) on a stationary panel within tolerance; the bias-stat series is monotone-sensible and order-fixed deterministic.

### S8.6 — Statistical factor model (APCA) — fills `n_stat_factors`
*Grounds: research §3.2 (Axioma 2-pass Asymptotic Principal Components, residual-variance-weighted; fundamental-vs-statistical tradeoff).*
Build the statistical-factor path that [`factor_model.hpp:380`](../../include/atx/engine/risk/factor_model.hpp#L380) currently rejects with `NotImplemented`. **2-pass APCA** (Connor-Korajczyk): extract factors as the top-K eigenvectors of the **`T×T` cross-product** `Ω=(1/N)RᵀR` (the asymptotic trick for `N≫T` — *not* the `N×N` covariance), regress assets on factor returns for exposures, GLS-reweight by estimated residual variance, repeat. Emit a statistical-factor `FactorModel` variant (same `V=XFXᵀ+D` apply path). Also runs as a **completeness check** on the fundamental model's residual structure (statistical factors should explain little of the fundamental residual if the fundamental model is well-specified — a diagnostic).
- **Files:** **Create** `risk/stat_factor_model.hpp`. Reuse `core::linalg::symmetric_eig` / `core::linalg::pca` ([`pca.hpp`](../../../atx-core/include/atx/core/linalg/pca.hpp)). Modify `FactorModelBuilder::build` to dispatch on `cfg.n_stat_factors`. **Create:** `tests/risk_stat_factor_test.cpp`.
- **atx-core (Pattern B):** the canonical `pca()` forms the `N×N` sample covariance — **wrong for `N≫T`**. Request **L7 `apca` (asymptotic PCA on the `T×T` Gram)**; engine-side fallback uses `symmetric_eig` on the explicitly-formed `T×T` Gram (shippable now).
- **Acceptance:** on a panel with K planted latent factors, APCA recovers ≈ K significant eigenvalues and the recovered factor returns correlate highly with the planted ones; the statistical `V` is SPD and applies through the existing Woodbury path; reuses the fundamental model's seam without a parallel optimizer.

### S8.7 — Model-free shrinkage + RMT cleaning + PSD-repair toolkit
*Grounds: research §7 (Ledoit-Wolf constant-correlation & nonlinear shrinkage, Marchenko-Pastur eigenvalue clipping), §8 (Higham nearest-correlation, eigenvalue clipping).*
A reusable `risk::cov` statistical toolkit, usable both inside the factor model and as a **model-free covariance** (the optimizer bootstrap + permanent cross-check overlay — research §14 takeaway #4):
- **Constant-correlation Ledoit-Wolf target** (`f_ij=r̄·√(s_ii·s_jj)`) — the equity workhorse, alongside the existing scaled-identity target ([`combiner.hpp:254`](../../include/atx/engine/combine/combiner.hpp#L254) `ledoit_wolf_intensity`, which S8 generalizes — **one canonical LW**, the 4b mandate).
- **Nonlinear / analytical shrinkage** (Ledoit-Wolf 2020) and **Marchenko-Pastur eigenvalue clipping** (`λ± = (1±√(N/T))²`, clip the noise bulk) — the RMT path.
- **PSD repair**: Higham nearest-correlation (alternating projections via `symmetric_eig`) + cheap eigenvalue clipping, for matrices that lose PSD after NW symmetrization or pairwise/missing-data patching.
- **Files:** **Create** `risk/shrinkage.hpp` (+ possibly `risk/psd_repair.hpp` if it grows). Generalize the canonical LW helper rather than fork it. **Create:** `tests/risk_shrinkage_psd_test.cpp`.
- **atx-core (Pattern B):** **L7 analytical nonlinear shrinkage (QIS/QuEST)** + **Marchenko-Pastur clip**; Higham nearest-correlation engine-side via `symmetric_eig` (shippable now; `core::linalg::nearest_corr` is the optional lift).
- **Acceptance:** LW intensity matches the closed form on a known `(π,ρ,γ)` setup; MP clip removes eigenvalues inside `[λ₋,λ₊]` and preserves trace; Higham repair turns a near-PSD invalid matrix into the nearest valid correlation matrix (Frobenius-closest, unit diagonal, all eigenvalues ≥ 0); shrinkage path is PD-guaranteed and invertible at `N>T`.

### S8.8 — Short/long-horizon blend + integration + bench + close
*Grounds: research §9 (short/long-horizon models), §10 (full pipeline), §6 (Woodbury — already in P4).*
Wire the deepened estimation into a coherent model and prove it end-to-end. Add **short- and long-horizon** half-life config sets (research §11 tables) and an optional convex blend in factor space (`F_blend = w·F_short + (1−w)·F_long`, `D` likewise — both PSD-preserving). Confirm the cleaned `V` flows through `PortfolioOptimizer::solve` and `MultiPeriodOptimizer::run` **unchanged** (S8 touches estimation, not the apply interface). Add a **risk-attribution diagnostic** (decompose portfolio variance into factor vs specific contributions — the statistical-analysis deliverable). **Bench** the full build at a 3k–5k-name universe (build time, condition number, memory — the research §10 compute profile). Close ceremony: residuals → ROADMAP backlog, status table, `Last reviewed`, "What S8 proves", user reference `sprint-8.md`, merge.
- **Files:** **Create** `risk/horizon_blend.hpp` (or fold into `cov_ewma.hpp`); `tests/risk_covariance_integration_test.cpp`; `bench/risk_covariance_bench.cpp`; user ref `sprint-8.md`.
- **Acceptance:** the integration test runs data → robust regression → EWMA+NW cov → eigenfactor clean → VRA → specific-risk blend → assemble `V` → optimize, asserting determinism (byte-identical replay incl. the seeded eigenfactor step), no look-ahead (truncation-invariance at the fit boundary), and SPD `V`; bench reports build time + condition-number improvement vs the P4 baseline on a large synthetic universe.

---

## Exit criteria

- Robust regression (√-cap + Huber, fixed-iteration) is determinism-stable and outlier-resistant vs plain WLS.
- Factor covariance `F` uses split vol/correlation EWMA half-lives + Newey-West and is better-conditioned than the P4 single-window MLE at `T≈K`.
- Eigenfactor adjustment corrects the small-eigenfactor under-forecast, is **PSD-preserving**, and is **seed-deterministic** (the seed is in the artifact; default amplify `a=1.0`, not `1.4`).
- Specific risk blends EWMA+NW time-series with a structural estimate; thin-history names inherit the structural value (no spurious near-zero variance).
- VRA rescales the forecast in injected vol regimes and emits a bias-stat diagnostic; no-op on stationary panels.
- APCA statistical factors recover planted latent structure and apply through the existing Woodbury path (the `n_stat_factors` `NotImplemented` is retired).
- Shrinkage/PSD toolkit: constant-correlation + nonlinear LW + MP clip + Higham repair, all PD-guaranteed; usable as a model-free optimizer bootstrap.
- The deepened `V` flows through `PortfolioOptimizer` / `MultiPeriodOptimizer` **unchanged**; the integration test passes deterministically, look-ahead-free, SPD.
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; one test file per unit; full engine suite stays green per unit.

## Invariants this sprint must prove

All seven carried-forward invariants (ROADMAP "Carried-forward invariants"), with two explicit stress points:
1. **Determinism** — the Monte-Carlo eigenfactor adjustment (S8.3) is the **only new RNG**. It must be seeded, the seed recorded in the artifact, the simulation loop fixed-count, and the digest byte-identical run-to-run. This is the determinism invariant's exact crux for S8.
2. **No look-ahead** — every estimated object (robust factor returns, EWMA/NW covariances, eigenfactor bias, VRA multiplier, structural specific-risk coefficients, APCA factors) is fit on the trailing window and applied OOS only. **Truncation-invariance** is the test, extending P4's fit/apply firewall.
3. **Differential correctness** — each new kernel (EWMA, Newey-West, eigenfactor sim, APCA, shrinkage, PSD repair) ships with an obviously-correct reference and a bit-/ULP-bounded differential test.

## Dependencies

- **Upstream:** P4 (`FactorModel` + `FactorModelBuilder` + `PortfolioOptimizer` — the factored apply path S8 keeps verbatim), S6 (calibrated cost for the optimizer turnover term, only relevant at the integration step).
- **atx-core (already available — reuse, no edge):** L7 `symmetric_eig` / `svd` / `pca` ([`decompose.hpp`](../../../atx-core/include/atx/core/linalg/decompose.hpp), [`pca.hpp`](../../../atx-core/include/atx/core/linalg/pca.hpp)), `solve_spd` ([`spd.hpp`](../../../atx-core/include/atx/core/linalg/spd.hpp)), seeded `Xoshiro256pp` ([`random.hpp`](../../../atx-core/include/atx/core/random.hpp)), `ols`/`ridge`/`wls` ([`regression.hpp`](../../../atx-core/include/atx/core/linalg/regression.hpp)).
- **atx-core (Pattern B — new edges raised by this sprint):**

| S8 unit | Needs from `atx-core` | Engine-side fallback (shippable now) |
|---|---|---|
| S8.1 | **L7 `huber_irls`** — robust/Huber IRLS least-squares | fixed-iteration IRLS loop wrapping `wls` |
| S8.6 | **L7 `apca`** — asymptotic PCA on the `T×T` Gram (`N≫T`) | `symmetric_eig` on the explicitly-formed `T×T` Gram |
| S8.7 | **L7 `nonlinear_shrinkage` (QIS) + `mp_clip`** + optional **`nearest_corr`** | constant-corr LW + MP clip engine-side; Higham via `symmetric_eig` alternating projections |

## Explicitly NOT in this sprint

- **No new general-purpose primitives in the engine** (anti-roadmap #6): solvers, robust regression, APCA, nonlinear shrinkage → atx-core requests (Pattern B above), with engine-side fallbacks only until the L7 kernels land.
- **No DCC / multivariate-GARCH** — the EWMA+NW+VRA stack is the vendor-verified path; DCC (Engle) is a documented alternative (research §"EWMA dynamics") but not the production recipe at this N.
- **No Marchenko-Pastur as the *factor-model* cleaner** — eigenfactor Monte-Carlo (S8.3) is the verified vendor method on the small `K×K` `F`; MP clip (S8.7) is for the model-free `N×N` path only.
- **No live/vendor-data factor exposures** — exposures stay panel-derived (OHLCV) + optional cap/sector spans, as P4 built; ingesting a commercial factor library is a later module.
- **No issuer-specific-covariance data sourcing** — S8.4 ships the ISC *interface*; populating it needs a multi-share-class universe (deferred).

## Baton → next

S8 hands S7 a **risk-shop-grade covariance**: the multi-period optimizer trades a cleaned, regime-aware, eigenfactor-de-biased `V`, and S7.3's dead-alpha→risk-factor extraction reuses S8.6's statistical-factor append path. With S8 + S7 complete, the engine's risk/optimization layer is at Barra/Axioma parity — the last structural gap between "a believable backtest, industrialized" and "a believable backtest, industrialized, with institutional-grade risk."

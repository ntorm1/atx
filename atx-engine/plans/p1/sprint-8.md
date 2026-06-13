# Sprint S8 — Risk Covariance Construction (user reference)

**What S8 proves.** atx-engine now builds the factor covariance `V = X F Xᵀ + D` the way a
Barra/Axioma-grade risk shop actually trades it — not the single shrunk-MLE placeholder P4 shipped.
S8 deepens *only the estimation* of `F` and `D`; the `FactorModel` **apply interface is unchanged**
(`risk` / `apply_inverse` / `neutralize` are byte-for-byte the P4 code), so the optimizer and everything
downstream consume the deepened `V` with no edit.

Every new behavior is **opt-in via `FactorModelConfig.cov` (`CovarianceConfig`)**, and **every default
reproduces the P4 path bit-for-bit** — the entire pre-S8 risk suite stays green untouched.

---

## The S8 arc — one paragraph per unit

- **S8.1 — Robust regression.** The per-date cross-sectional factor-return fit gains a √-market-cap prior
  weight (down-weights mega-caps without letting a few names dominate) composed with a fixed-iteration
  Huber IRLS (reusing the S6 cost kernel — no second IRLS loop). A single fat-tailed return date no longer
  tilts the factor returns. Opt-in: `cov.robust_regression` (+ `huber_c`, `robust_iters`, `cap_weight`,
  `industry_sum_to_zero`).

- **S8.2 — EWMA split-half-life + Newey-West factor covariance.** Replaces the single-window MLE with the
  vendor recipe: EWMA **variances** at a fast half-life, EWMA **correlations** at a slow half-life,
  recombined `F_ij = ρ_ij·σ_i·σ_j`, plus a Newey-West Bartlett serial-correlation add and an SPD eigenvalue
  floor. Better-conditioned than the single MLE at `T≈K`. Opt-in: `cov.factor_cov_method = EwmaNeweyWest`
  (+ `vol_halflife`, `corr_halflife`, `nw_lags`).

- **S8.3 — Monte-Carlo eigenfactor de-biasing (MWO).** A sample factor covariance systematically
  under-forecasts the variance of its smallest eigenfactors; S8.3 measures that bias by simulation and
  rescales each eigenvariance to correct it (Menchero-Wang-Orr). PSD-preserving (`γ²>0`). **This is the one
  and only new RNG site in S8** — a single seeded `Xoshiro256pp`, a fixed-count loop, byte-identical on
  replay. Default amplify `a = 1.0` (not the paper's empirical 1.4). Opt-in: `cov.eigen_adjust_sims > 0`
  (+ `eigen_adjust_amplify`, `eigen_adjust_seed` — the seed IS the reproducibility record).

- **S8.4 — Specific-risk model: EWMA + Newey-West + structural blend.** Idiosyncratic variance `D` becomes
  an EWMA time-series specific-vol estimate, inflated by a scalar Newey-West autocorrelation ratio, blended
  toward a ln-vol-on-exposures **structural** model by history depth — thin-history names inherit a sane
  structural level instead of a noisy near-zero. (The issuer-specific off-diagonal carrier `isc` is defined
  interface-only; `D` stays diagonal this sprint.) Opt-in: `cov.specific_method = EwmaNeweyWestStructural`
  (+ `spec_halflife`, `spec_nw_lags`, `structural_blend`).

- **S8.5 — Volatility Regime Adjustment (VRA).** A market-wide cross-sectional bias statistic
  `B_t = √((1/K)·Σ_j (f_{t,j}/σ_j)²)` is EWMA-averaged into a regime multiplier `λ²`; the forecast is
  rescaled `F ← λ²·F`, `D ← λ²·D`. `λ²≈1` on a stationary panel, `λ²>1` in a high-vol regime the fixed
  half-life under-forecast. Opt-in: `cov.vra_halflife > 0`.

- **S8.6 — APCA statistical factor model.** Retires the `n_stat_factors > 0` `NotImplemented`: a
  Connor-Korajczyk 2-pass asymptotic-PCA path extracts latent factors from a complete-case `N×T` return
  panel via the **T×T Gram** (the `N≫T` trick), recovers exposures + specific variances, optionally reweights
  by `1/√sₙ` (GLS pass), and assembles a `FactorModel` that flows through the same Woodbury apply path.
  Activates when `FactorModelConfig.n_stat_factors > 0` (+ `cov.apca_gls_reweight`). (`n_dead_factors > 0`
  remains `NotImplemented` — an S7.3 residual.)

- **S8.7 — Model-free shrinkage + RMT + PSD repair (standalone toolkit).** A `risk::cov` utility surface,
  *not* auto-wired into `build`: constant-correlation Ledoit-Wolf shrinkage (PD-guaranteed at `N>T` where the
  raw sample cov is singular), Marchenko-Pastur eigenvalue clipping (trace-preserving noise-bulk removal),
  Higham nearest-correlation (bounded alternating projections), and a cheap eigenvalue-clip. Usable as a
  model-free covariance / optimizer bootstrap.

- **S8.8 — Short/long-horizon blend + integration + bench + close.** Vendors ship two horizons (USE4S/L,
  AXUS4 SH/MH); S8.8 reconciles them with a convex blend `F = w·F_short + (1−w)·F_long` (and the analogous
  `D` blend), each horizon built by the SAME estimator at a different half-life set. PSD-preserving (convex
  combo of SPD is SPD — no extra repair). The all-features-on integration test proves the whole pipeline
  end-to-end; a bench reports build wall-time + the condition-number profile. Opt-in: `cov.horizon_blend`
  (+ `horizon_blend_weight`, `vol_halflife_long`, `corr_halflife_long`, `spec_halflife_long`). The blend
  requires `factor_cov_method = EwmaNeweyWest` (the long half-life set applies only there; under
  `LedoitWolfSingle` it is a no-op).

---

## Invariants held across all of S8

- **Apply interface unchanged.** `FactorModel::risk` / `apply_inverse` (Woodbury) / `neutralize` are the
  verbatim P4 code. The optimizer (`PortfolioOptimizer::solve`) consumes the deepened `V` unchanged.
- **Determinism — exactly one seeded RNG.** S8.3 `eigen_adjust` is the only RNG in the model path: one
  `Xoshiro256pp` from a recorded seed, a fixed-count loop, all reductions order-fixed. The full
  all-features-on build + solve is **byte-identical on replay** (proven by `std::memcmp` on the optimizer
  output). No clock, no map iteration, no parallelism in the build path.
- **No look-ahead.** Every estimate is fit on the trailing window only; appending older rows beyond the
  window+lookback does not change the model (truncation-invariance, proven per unit and end-to-end).
- **PSD by construction.** Each constructed `F` is SPD (EWMA eigenvalue-floor; `γ²>0` eigen-adjust; convex
  blend of SPD; LW shrink toward `m·I`); `FactorModel::create` re-checks via Cholesky and floors `D > 0`.
- **Backward-compatible.** `CovarianceConfig{}` defaults + `n_stat_factors == 0` reproduce the P4 path
  bit-for-bit; the pre-S8 risk suite is the regression gate.

---

## How to enable each feature (via `FactorModelConfig.cov`)

| Feature | Flag(s) | Default (no-op) |
|---|---|---|
| Robust √-cap + Huber regression (S8.1) | `robust_regression = true` (+ `huber_c`, `robust_iters`, `cap_weight`, `industry_sum_to_zero`) | `false` ⇒ plain inverse-d0 WLS |
| EWMA split-HL + Newey-West factor cov (S8.2) | `factor_cov_method = EwmaNeweyWest` (+ `vol_halflife`, `corr_halflife`, `nw_lags`) | `LedoitWolfSingle` (single-MLE shrink) |
| Monte-Carlo eigenfactor de-biasing (S8.3) | `eigen_adjust_sims > 0` (+ `eigen_adjust_amplify`, `eigen_adjust_seed`) | `0` ⇒ no adjustment |
| Specific risk: EWMA+NW+structural (S8.4) | `specific_method = EwmaNeweyWestStructural` (+ `spec_halflife`, `spec_nw_lags`, `structural_blend`) | `PopVariance` |
| Volatility Regime Adjustment (S8.5) | `vra_halflife > 0` | `0` ⇒ `λ²≡1` |
| APCA statistical factors (S8.6) | `FactorModelConfig.n_stat_factors > 0` (+ `cov.apca_gls_reweight`) | `0` ⇒ fundamental path |
| Short/long-horizon blend (S8.8) | `horizon_blend = true` (+ `horizon_blend_weight`, `vol_halflife_long`, `corr_halflife_long`, `spec_halflife_long`) | `false` ⇒ single-horizon |

S8.7's shrinkage/PSD toolkit (`risk/shrinkage.hpp`, `risk/psd_repair.hpp`) takes no `CovarianceConfig` flag —
it is a standalone model-free utility, not part of the `build` pipeline.

Recommended half-life sets (research §11; document, not enforced): USE4S vol 84 / corr 504 / vra 42;
USE4L vol 252 / corr 504 / vra 168; AXUS4 SH 60/125, MH 125/250. No universal blend weight exists — vendors
ship two models; `w = 0.5` is the neutral default.

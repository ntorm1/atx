# atx-vol `analytics.hpp` — Structured Vol-Surface Analytics

**Date:** 2026-07-15
**Branch/worktree:** `feat/atx-vol-analytics` @ `C:/atx-wt/atx-vol-analytics`
**Status:** Design — approved for sub-agent implementation

## 1. Purpose

A new module `atx/vol/analytics.hpp` (+ `src/analytics.cpp`) that ingests a **fitted, served volatility surface** and emits a **structured, serializable analytic bundle** oriented at:

1. **Equity single-name vol analytics** — term structure, skew, RND/implied probabilities, earnings.
2. **Volatility dispersion trading** — the per-name building blocks (ATM variance, var-swap strike, skew) plus a pure implied-correlation helper.
3. **Relative-value / cross-sectional vol trading** — fixed-tenor ATM, skew, term-structure, and **two-surface change** metrics (sticky-strike vs sticky-delta).

The module is a **pure analytics layer**: no fitting, no market data ingestion, no I/O beyond deterministic CSV writers. It reads a `PricedSurface` (the atx-vol served type) and folds it into value structs.

## 2. Inputs & entry points

The **core** consumes `const PricedSurface&` (the served, snapshot-ready fitted surface: `iv(K,T)`, `total_variance(K,T)`, `forward_at/q_eff_at/rate_at(T)`, `context()` per-expiry ascending T, `delta/greeks` for delta→strike and RND).

Convenience overloads adapt the other "surface / pricer-fitter" forms named in the goal:

- `compute_surface_analytics(const PricedSurface&, const AnalyticsConfig&, const EventContext* = nullptr)` — primary.
- `compute_surface_analytics(const VolaSession&, const AnalyticsConfig&)` — calls `session.to_priced_surface()`; auto-extracts earnings (`SessionInputs::events`) and solved `SessionDiagnostics::implied_emove`.
- `compute_surface_analytics(const FittedSurface&, const AnalyticsConfig&)` — via `fitted.session()`.
- `compute_surface_analytics(const PricerFitter&, const AnalyticsConfig&)` — via `fitter.surface()->session()`.

`EventContext { const EventSchedule* schedule; double implied_emove; }` — supplied explicitly on the bare-`PricedSurface` path (a `PricedSurface` does **not** retain an `EventSchedule`; `now_ts_ns` comes from `ps.pricing().now_ts_ns`). When absent, earnings-dependent fields are `NaN` and `n_earnings = 0`.

All aggregating entry points return `Result<...>`; per-metric primitives return `double`/`Result<double>` and yield `NaN` outside the surface's no-extrapolation domain, matching `PricedSurface::iv`.

## 3. Conventions (load-bearing — match the library)

- **Time** `T` = year-fraction (`double`), Calendar365 = ACT/365.25. Tenor grid expressed in years.
- **Moneyness** `k = ln(K / F(T))` (log-forward-moneyness). ATM ≡ ATMF ≡ `K = forward_at(T)` (`k = 0`).
- **Interpolation of constant-maturity quantities is done in the surface's own coordinates** — `ps.iv(K,T)` / `ps.total_variance(K,T)` already interpolate across expiries in total-variance space; we never re-interpolate vol linearly in T ourselves. Constant-maturity ATM at tenor `T*` is `ps.iv(ps.forward_at(T*), T*)`.
- **Rates** continuous; `df(T) = exp(-rate_at(T)·T)`.
- **Delta strikes** use the existing `resolve_strike_by_delta(ps, T, side, |Δ|)` (American |delta|, bisection on log-moneyness). Δ inputs are **absolute** (0.25, 0.10); put/call chosen by `Side`.
- **Skew sign**: risk reversal `RR(Δ) = σ(Δ-put) − σ(Δ-call)` (equity convention → positive = downside rich). Skew slope `∂σ/∂k` (per unit log-moneyness); reported raw and normalized (`·√T`, `/σ_atm`).
- **Vols** annualized lognormal; European-equivalent IV as returned by the surface.
- House style: `namespace atx::vol`, `#pragma once`, C++20, LLVM/2-space/col-100; declarations in header, definitions in `src/analytics.cpp`; snake_case struct fields with brace-init defaults; `Result<T>`/`Status` error channel; **no JSON** — plain structs + `snprintf`-based CSV/TSV writers (`%.17g` series, `%.10g` headline), mirroring `fit_metrics.hpp` / `run_report.hpp`. Every entry point is a pure function of its args → concurrent-safe.

## 4. Feature catalog

### 4.1 Term structure / ATM (per fixed tenor)
- ATMF vol at each **fixed constant-maturity tenor** (default grid: 1w, 2w, 1m, 2m, 3m, 6m, 9m, 1y, 18m, 2y) and at each fitted pillar.
- Forward `F(T)`, `df(T)`.
- **ATMF vol excluding earnings** (censored diffusive vol) at each tenor + `n_earnings` between now and T: `w_cen = w_total − n·eMove²`, `σ_cen = sqrt(w_cen/T)` (reuse `censored_total_variance`).
- Term-structure slope metrics: `σ_3m − σ_1m`, `σ_1y − σ_3m`; ratio `σ_1m/σ_3m`; contango/backwardation flag.
- **Forward vol** between consecutive tenors: `fwd_vol(T1,T2) = sqrt((w2 − w1)/(T2 − T1))` on ATMF total variance.

### 4.2 Strike / delta vols & skew (per fixed tenor)
- Fixed-delta vols: 25Δ / 10Δ put & call (configurable delta set).
- **Risk reversal** `RR(Δ)`, **butterfly** `BF(Δ) = (σ_Δput + σ_Δcall)/2 − σ_atm`.
- **Skew slope** `∂σ/∂k` at ATM and **curvature** `∂²σ/∂k²` — 3-pivot quadratic through `k ∈ {−k_ref, 0, +k_ref}` (the `pnl_attribution` recipe: `a1 = (σ₊−σ₋)/(2k_ref)`, `a2 = (σ₊+σ₋−2σ₀)/(2k_ref²)`), `k_ref` configurable (default 0.10).
- Fixed-moneyness vols (90/95/100/105/110% of F) and moneyness skew `σ(90%) − σ(110%)`.
- Normalized skew (`·√T`, `/σ_atm`).

### 4.3 Risk-neutral density & implied probabilities (per selected tenor)
- **RND** via Breeden–Litzenberger: `q(K) = e^{rT} ∂²C/∂K²`, `C(K)` = Black-76 call price on `ps.iv(K,T)` over a strike grid (central 2nd difference). Clamp negatives to 0, renormalize to unit mass.
- **Implied CDF** `P(S_T ≤ K)`; **cumulative implied probability** curve.
- **Implied quantiles** (inverse CDF): 5/25/50/75/95% (configurable).
- `prob_below_forward`, `prob_itm(K)`, expected-move (1σ) proxy.
- **RND moments**: mean (≈F sanity check), variance, **skewness**, **kurtosis** — computed two ways: (a) direct integration of the normalized RND, and (b) **BKM (Bakshi–Kapadia–Madan) model-free moments** via the power-payoff strips `P1=E[R]`, `P2=E[R²]`, `P3=E[R³]`, `P4=E[R⁴]` replicated from OTM prices — `skew = (P3−3P1P2+2P1³)/(P2−P1²)^{3/2}`, `kurt = (P4−4P1P3+6P1²P2−3P1⁴)/(P2−P1²)²`. BKM is the CBOE-SKEW machinery and is the **preferred** third/fourth-moment estimate (FD-of-RND is a cross-check); the analytics also emits a **CBOE-SKEW-style single-name statistic** `100 − 10·skew_BKM`.
- **Model-free implied variance / VIX-style** log-strip `K_var(T) = (2/T)∫ OTM(K)/(df·K²) dK` on the served surface (own PricedSurface strip — `derivatives.hpp` is templated on raw eSSVI/SVI, not the polymorphic served type); **convexity premium** = `sqrt(K_var) − σ_atmf`.

### 4.4 Earnings / events
- **Earnings implied move** (per-event `eMove`) via `implied_emove(w1,T1,n1, w2,T2,n2)` across the two expiries bracketing the next earnings date.
- **Straddle-implied move** for an expiry = ATM straddle price / spot (`≈ σ_atmf·√(T·2/π)` cross-check).
- **Event-variance decomposition**: `w_total = σ_diffusive²·T + n·eMove²`; report event-variance share per tenor.
- De-earnings-ed (censored) ATM term structure (from 4.1).

### 4.5 Two-surface diff (same underlying, t1 → t2)
- Spot move `ΔS`, `Δforward(T)`.
- **ATMF vol change** per tenor.
- **Fixed-strike vol change (sticky-strike)**: `σ₂(K₀,T) − σ₁(K₀,T)` at t1's ATM strike `K₀` (and at t1's delta strikes).
- **Fixed-moneyness / fixed-delta vol change (sticky-delta)**.
- **Fixed-strike skew change**; `ΔRR`, `ΔBF`.
- Term-structure change.
- **Sticky-regime diagnostic**: decompose realized ATMF change into the spot-move component predicted by sticky-strike vs sticky-delta, plus residual "true" vol move.

### 4.6 Dispersion helper (pure, standalone)
- `implied_correlation` from index variance + constituent variances + weights:
  - clean: `ρ = (σ²_idx − Σ wᵢ²σᵢ²) / (Σ_{i≠j} wᵢwⱼσᵢσⱼ)`
  - dirty: `ρ = σ²_idx / (Σ wᵢσᵢ)²`
  operating on **already-computed** per-name vols. Multi-name orchestration stays in `dispersion.hpp`; analytics.hpp only supplies the pure formula + the per-name inputs.

## 5. Public API (structs + functions)

```cpp
namespace atx::vol {

// ---- Config ----
struct TenorGrid {                       // constant-maturity targets (years)
  std::vector<double> tenors_years;
  std::vector<std::string> labels;       // "1w","1m",...
  static TenorGrid standard();           // 1w,2w,1m,2m,3m,6m,9m,1y,18m,2y
};

struct RndConfig {
  double k_min = -0.60, k_max = 0.60;    // log-moneyness span of the strike grid
  int    n_grid = 201;                   // odd; Simpson/central-diff friendly
  std::vector<double> quantiles = {0.05, 0.25, 0.50, 0.75, 0.95};
  bool   compute_moments = true;
};

struct AnalyticsConfig {
  TenorGrid tenors = TenorGrid::standard();
  std::vector<double> delta_points = {0.25, 0.10};   // abs deltas for RR/BF/wing vols
  std::vector<double> moneyness_points = {0.90,0.95,1.00,1.05,1.10};
  double skew_k_ref = 0.10;              // 3-pivot skew/curvature pivot (log-moneyness)
  bool   ex_earnings = true;
  bool   compute_rnd = true;
  RndConfig rnd{};
  std::vector<double> rnd_tenors_years = {30.0/365.25, 90.0/365.25};  // RND is heavier; select
  bool   compute_varswap = true;
};

struct EventContext { const EventSchedule* schedule = nullptr; double implied_emove = 0.0; };

// ---- Fine-grained density output ----
struct RiskNeutralDensity {
  double T = 0.0, forward = 0.0, df = 0.0;
  std::vector<double> strikes, pdf, cdf;         // aligned; pdf normalized to unit mass
  double mean = 0.0, variance = 0.0, skewness = 0.0, kurtosis = 0.0;      // from RND integ.
  double bkm_variance = 0.0, bkm_skew = 0.0, bkm_kurt = 0.0;              // BKM strips
  double skew_index = 0.0;                        // 100 - 10*bkm_skew (CBOE-SKEW style)
  double mass_before_norm = 1.0;                  // ragged-smile diagnostic
  std::vector<double> quantile_p, quantile_k;    // inverse CDF at RndConfig::quantiles
  double prob_below_forward = 0.0;
  bool valid = false;
};

struct SkewCurvature { double atm = 0.0, skew_slope = 0.0, curvature = 0.0; bool valid = false; };

// ---- Per-tenor bundle ----
struct TenorAnalytics {
  double tenor_years = 0.0;
  std::string label;
  double forward = 0.0, df = 0.0;
  double atm_vol = 0.0;                   // ATMF, k=0
  double atm_vol_ex_earn = 0.0;           // censored (NaN if no EventContext)
  int    n_earnings = 0;
  // delta vols & skew (indexed by AnalyticsConfig::delta_points)
  std::vector<double> put_delta_vol, call_delta_vol, risk_reversal, butterfly;
  double skew_slope = 0.0, curvature = 0.0;
  std::vector<double> moneyness_vol;      // aligned to moneyness_points
  double skew_90_110 = 0.0;
  // variance-based
  double var_swap_vol = 0.0, convexity_premium = 0.0;   // NaN if !compute_varswap
  // density moments (populated only for tenors in rnd_tenors_years)
  double rnd_skewness = 0.0, rnd_kurtosis = 0.0, prob_below_forward = 0.0;
  bool   valid = false;
};

struct SurfaceAnalytics {
  std::uint32_t uid = 0;
  std::int64_t  as_of_ts_ns = 0;
  double spot = 0.0, implied_emove = 0.0;
  std::vector<TenorAnalytics> tenors;
  std::vector<RiskNeutralDensity> densities;   // one per rnd_tenors_years (if compute_rnd)
  // term-structure summary
  double ts_slope_1m_3m = 0.0, ts_slope_3m_1y = 0.0, ts_ratio_1m_3m = 0.0;
  bool   backwardation = false;
  bool   valid = false;
};

// ---- Two-surface diff ----
struct TenorDiff {
  double tenor_years = 0.0; std::string label;
  double d_forward = 0.0;
  double d_atm_vol = 0.0;                 // ATMF change
  double d_vol_fixed_strike = 0.0;        // sticky-strike at t1 ATM strike
  double d_vol_fixed_delta = 0.0;         // sticky-delta
  double d_skew_slope = 0.0, d_risk_reversal_25 = 0.0, d_butterfly_25 = 0.0;
  bool valid = false;
};
struct SurfaceDiff {
  std::int64_t ts1_ns = 0, ts2_ns = 0;
  double spot1 = 0.0, spot2 = 0.0, d_spot = 0.0;
  std::vector<TenorDiff> tenors;
  double sticky_strike_atm_pred = 0.0, sticky_delta_atm_pred = 0.0, residual_atm_move = 0.0;
  bool valid = false;
};

// ---- Primitives (public: unit-testable) ----
double            atmf_vol(const PricedSurface&, double T) noexcept;
double            atmf_forward(const PricedSurface&, double T) noexcept;
Result<double>    vol_at_delta(const PricedSurface&, double T, Side, double abs_delta);
double            vol_at_moneyness(const PricedSurface&, double T, double moneyness) noexcept; // K=F·m
Result<double>    risk_reversal(const PricedSurface&, double T, double abs_delta);
Result<double>    butterfly(const PricedSurface&, double T, double abs_delta);
SkewCurvature     skew_curvature(const PricedSurface&, double T, double k_ref) noexcept;
double            forward_vol(const PricedSurface&, double T1, double T2) noexcept;
Result<double>    var_swap_vol(const PricedSurface&, double T, const RndConfig& = {});   // log-strip
Result<RiskNeutralDensity> risk_neutral_density(const PricedSurface&, double T, const RndConfig& = {});
double            implied_cdf(const PricedSurface&, double T, double K, const RndConfig& = {}) noexcept;
double            atmf_vol_ex_earnings(const PricedSurface&, double T, const EventContext&) noexcept;
double            implied_correlation_clean(double idx_var, std::span<const double> w, std::span<const double> var);
double            implied_correlation_dirty(double idx_var, std::span<const double> w, std::span<const double> vol);

// ---- Aggregators ----
Result<SurfaceAnalytics> compute_surface_analytics(const PricedSurface&, const AnalyticsConfig& = {},
                                                   const EventContext* = nullptr);
Result<SurfaceAnalytics> compute_surface_analytics(const VolaSession&,  const AnalyticsConfig& = {});
Result<SurfaceAnalytics> compute_surface_analytics(const FittedSurface&, const AnalyticsConfig& = {});
Result<SurfaceDiff>      compute_surface_diff(const PricedSurface& a, const PricedSurface& b,
                                              const AnalyticsConfig& = {});

// ---- Serializers (house CSV style) ----
Status write_surface_analytics_csv(const SurfaceAnalytics&, std::string_view path);
Status write_surface_diff_csv(const SurfaceDiff&, std::string_view path);
Status write_rnd_csv(const RiskNeutralDensity&, std::string_view path);

}  // namespace atx::vol
```

## 6. Numerical methods & edge cases

- **Skew/curvature**: central FD on `iv(F·e^{±k_ref}, T)`; return `valid=false` if any pivot is NaN (outside domain).
- **RND**: Black-76 call `C(K)` on `iv(K,T)` over strikes `F·e^{k}`, `k` on `[k_min,k_max]`, `n_grid` (odd). `q(Kᵢ) = df⁻¹·(C₊ − 2C₀ + C₋)/ΔK²`? — note BL is `q(K)=e^{rT}∂²C/∂K²` on **undiscounted** convention; implement consistently with black76 `df` and validate against a lognormal-flat surface (RND = lognormal with σ√T). Clamp `q<0`→0, renormalize (trapezoid/Simpson) to unit mass; CDF = cumulative integral. Report a `mass_before_norm` diagnostic to flag ragged smiles.
- **Delta strikes**: propagate solver `InvalidArgument` (unreachable delta) as `NaN` in the aggregate (a wing may be beyond the no-extrapolation domain); the primitive returns the `Result`.
- **Earnings**: `implied_emove` requires two expiries with **different** event counts straddling the earnings date; when the session already solved `implied_emove` (`SessionDiagnostics`), prefer it and only recompute on the bare-surface path. Guard the non-identification case (`n1·T2 == n2·T1`).
- **Forward vol**: `NaN` if `w2 ≤ w1` (calendar arb / numerical) or `T2 ≤ T1`.
- **Diff**: both surfaces must be the same `uid`; sticky-strike uses t1's ATM strike `K₀ = forward_at₁(T)` evaluated on both surfaces.

## 7. Testing (`tests/analytics_test.cpp`, GoogleTest, `fast`)

Golden/property tests on synthetic + `spy_fit_fixture` surfaces:
1. **Flat lognormal surface** → skew_slope≈0, curvature≈0, RR≈0, RND ≈ lognormal(σ√T) (moments: skewness≈0 in log, mean≈F), CDF monotone in [0,1], mass≈1.
2. **Known SVI/eSSVI smile** → skew sign matches parameter sign; RR sign correct.
3. **RND** integrates to ~1 (±1e-3), CDF monotone non-decreasing, quantiles bracket forward, `implied_cdf(F)` ≈ prob_below_forward.
4. **Forward vol** on flat term structure recovers the flat vol; on upward term structure > front vol.
5. **Constant-maturity ATM** equals `iv(forward_at(T*),T*)` at pillar T* exactly.
6. **Earnings**: synthetic event surface (build w_total = σ_C²T + n·eMove²) → `earnings_implied_move` recovers eMove; `atmf_vol_ex_earn` recovers σ_C.
7. **Diff**: identical surfaces → all deltas 0; pure parallel vol shift → `d_atm_vol` = shift, sticky-strike/delta consistent; pure spot move on sticky-delta surface → residual ≈ 0.
8. **implied_correlation**: single-name (n=1,w=1) → ρ=1; dirty ρ∈[0,1] on a constructed index.
9. CSV writers: round-trip parse of headline metrics; deterministic byte output.

## 8. Build wiring

- `include/atx/vol/analytics.hpp` (new).
- `src/analytics.cpp` (new) → add to `add_library(atx-vol ...)` in `atx-vol/CMakeLists.txt`.
- `tests/analytics_test.cpp` (new) → add to `atx-vol-tests` source list in `atx-vol/tests/CMakeLists.txt`, `fast` label.
- No new third-party deps.

## 9. Scope / YAGNI

**In:** everything in §4. **Out (this pass):** live multi-name dispersion orchestration (stays in `dispersion.hpp`); historical z-scores / percentile ranks (need a history store — separate module); prob-of-touch / barrier (needs path model); listed-contract snapping. These are noted as follow-ons, not built.

## 10. Implementation plan (sub-agent waves)

- **Wave A** — header `analytics.hpp` (all structs + signatures + doc comments) and CMake wiring; compile-only stub `src/analytics.cpp` returning `NotImplemented`.
- **Wave B** — primitives: `atmf_vol`, `atmf_forward`, `vol_at_delta`, `vol_at_moneyness`, `risk_reversal`, `butterfly`, `skew_curvature`, `forward_vol`, `atmf_vol_ex_earnings`, `implied_correlation_*` + their tests.
- **Wave C** — RND stack: `risk_neutral_density`, `implied_cdf`, `var_swap_vol` + tests (flat-lognormal golden).
- **Wave D** — aggregators: `compute_surface_analytics` (+ session/fitted overloads), `compute_surface_diff`, earnings integration + tests.
- **Wave E** — CSV serializers + tests; full build + `atx-vol-tests` green; format/tidy pass.

Waves B/C are parallelizable (disjoint functions); D depends on B+C; E depends on D.

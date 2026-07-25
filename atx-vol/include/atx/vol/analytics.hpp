#pragma once

// Structured vol-surface analytics — a pure analytics layer over a fitted,
// served `PricedSurface` (and, via convenience overloads, a `VolaSession` /
// `FittedSurface` / `PricerFitter`). It folds a surface into a value-typed
// analytic bundle oriented at equity single-name vol analytics, volatility
// dispersion trading, and relative-value / cross-sectional vol trading.
//
// ## What this module computes
//
//   - Fixed constant-maturity ATMF term structure (interpolated in the
//     surface's own total-variance coordinates), forward-vol between tenors,
//     and an earnings-stripped ("de-earnings-ed") ATM term structure.
//   - Fixed-delta wing vols (25Δ/10Δ put & call), risk reversal, butterfly,
//     ATM skew slope ∂σ/∂k and curvature ∂²σ/∂k², fixed-moneyness vols.
//   - Risk-neutral density (Breeden–Litzenberger), implied CDF / quantiles /
//     probabilities, risk-neutral moments (RND-integrated AND Bakshi–Kapadia–
//     Madan model-free), a CBOE-SKEW-style single-name statistic, model-free
//     implied variance (VIX-style log-strip) and the convexity premium.
//   - Earnings implied move (per-event eMove) and the expected move at a chosen
//     straddle multiplier.
//   - Two-surface change analytics (same underlying, t1 → t2): ATMF vol change,
//     fixed-strike (sticky-strike) and fixed-delta (sticky-delta) vol change,
//     fixed-strike skew change, and a sticky-regime decomposition of the ATM
//     move into a spot-driven component plus a residual "pure vol" move.
//   - A pure implied-correlation helper (dispersion) over already-computed
//     per-name vols/variances.
//
// ## Conventions (match the library)
//
//   - Time `T` is a Calendar365 (ACT/365.25) year-fraction; the tenor grid is
//     expressed in years. Log-forward-moneyness `k = ln(K / F(T))`; ATM ≡ ATMF
//     (`K = forward_at(T)`, `k = 0`). Rates are continuous, `df = exp(-rT)`.
//   - Constant-maturity quantities are read straight off the surface's own
//     interpolation (`iv(K,T)` / `total_variance(K,T)`); this module never
//     re-interpolates vol linearly in T.
//   - Delta strikes are resolved under an EXPLICIT `DeltaConvention`
//     (projection.hpp), defaulting to `American` — `resolve_strike_by_delta`,
//     i.e. dP/dS on the American mark, absolute target. See the
//     "Delta conventions" section below: this used to be an undocumented
//     module-local choice, and it does NOT agree with the European B76 forward
//     delta that `projection.cpp` solves or with `contract_projection.cpp`'s
//     carry-seeded American solve (AN-P2-6). Risk reversal uses the equity sign
//     `RR = σ(Δ-put) − σ(Δ-call)` (positive = downside rich). Vols are
//     annualized lognormal.
//
// ## Delta conventions (AN-P2-6) — read before comparing a Δ-strike to anyone
//
// THREE different "25-delta strikes" exist in this library, and they are three
// different strikes:
//
//   1. `DeltaConvention::American` — dP/dS on the American mark
//      (`resolve_strike_by_delta`, strategy.hpp). What THIS module has always
//      used and still defaults to, so no existing number moves.
//   2. `DeltaConvention::Forward` — European Black-76 FORWARD delta, N(d1) for a
//      call and N(d1)−1 for a put. What `projection.cpp`'s coordinate solves use,
//      and the vendor-standard quote convention. Select it here for
//      vendor-comparable wings/RR/BF.
//   3. `contract_projection.cpp` solves American delta seeded from a
//      carry-discounted spot-delta inversion — a third route, not selectable
//      here, and documented in that module.
//
// (1) and (2) diverge with carry and with early-exercise premium; on a
// high-carry or deep-ITM name the gap is economically material. Anything that
// compares an atx RR/BF against a vendor's must pin the convention explicitly.
//
// A `PricedSurface` does NOT retain an `EventSchedule`; earnings-dependent
// fields require an `EventContext` on the bare-surface path (the session
// overloads extract it automatically). `now_ts_ns` comes from the surface's
// pricing context.
//
// ## Error / NaN semantics
//
// Aggregating entry points return `Result<...>`; per-metric primitives return
// `double` / `Result<double>` and yield `NaN` when the surface itself returns
// NaN (a non-finite/non-positive K/T). NOTE: the served `CurveSurface`
// FLAT-EXTRAPOLATES a parametric smile outside the fitted pillar range rather
// than returning NaN, so a primitive like `atmf_vol` is finite even for a tenor
// beyond the fitted expiries — that value is an extrapolation, not a fitted
// number. `compute_surface_analytics` therefore gates each tenor to the fitted
// pillar range `[context().front().T, context().back().T]`: an out-of-range
// tenor is marked `extrapolated = true` and excluded from `valid` and the
// term-structure summary. Every entry point is a pure function of its
// arguments — safe to call concurrently.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/event_vol.hpp" // EventSchedule, EmoveSolution (earnings_implied_move_ex)
// FIX-E M-9: `DeltaConvention` lives in types.hpp now. This header used to pull
// the whole projection spine (vol_surface / correction / curve / universe) to
// name one enum.
#include "atx/vol/types.hpp" // Side, Result, Status, ErrorCode, DeltaConvention

namespace atx::vol {

// Forward declarations — the heavy surface/session types are used only by
// const-reference in signatures, so their full definitions are not needed here.
class PricedSurface;
class VolaSession;
class FittedSurface;
class PricerFitter;
class EventSchedule;

// ── Configuration ───────────────────────────────────────────────────────────

// Constant-maturity tenor targets (year-fractions) with human labels. `standard`
// is 1w, 2w, 1m, 2m, 3m, 6m, 9m, 1y, 18m, 2y on the ACT/365.25 basis.
struct TenorGrid {
  std::vector<double> tenors_years;
  std::vector<std::string> labels;

  [[nodiscard]] static TenorGrid standard();
};

// Risk-neutral density / model-free integration grid. The strike grid is
// UNIFORM IN LOG-FORWARD-MONEYNESS `k = ln(K/F)` with `n_grid` points (forced
// odd, ≥ 11, for the composite-Simpson quadrature). The half-width is
// `max(|k_min|, k_max, width_sigmas · σ_atm · √T)`: `width_sigmas > 0` scales
// the grid with the tenor's own vol so long-dated / high-vol tenors keep enough
// tail coverage (a fixed ±0.60 truncates the strip and biases var-swap / BKM
// kurtosis low); `[k_min, k_max]` remains a floor. Set `width_sigmas = 0` to pin
// the fixed `[k_min, k_max]` span.
struct RndConfig {
  double k_min = -0.60;
  double k_max = 0.60;
  double width_sigmas = 6.0; // adaptive half-width in σ√T units (0 = fixed span)
  int n_grid = 201;
  std::vector<double> quantiles = {0.05, 0.25, 0.50, 0.75, 0.95};
  bool compute_moments = true;
};

// Top-level knobs for `compute_surface_analytics`.
struct AnalyticsConfig {
  TenorGrid tenors = TenorGrid::standard();
  std::vector<double> delta_points = {0.25, 0.10}; // abs deltas for wings/RR/BF
  // E5 / AN-P2-6. Convention the wing / RR / BF strike solves use. `American`
  // (dP/dS on the American mark) is the shipped behaviour and the default —
  // selecting `Forward` gives vendor-comparable Black-76 forward-delta strikes.
  // See the "Delta conventions" section in this header's module comment.
  DeltaConvention delta_convention = DeltaConvention::American;
  std::vector<double> moneyness_points = {0.90, 0.95, 1.00, 1.05, 1.10}; // K = F·m
  double skew_k_ref = 0.10; // 3-pivot skew/curvature pivot
  bool ex_earnings = true;  // censored (de-earnings-ed) ATM
  bool compute_rnd = true;
  RndConfig rnd{};
  // RND is heavier; compute it only at these tenors. Values that match a
  // `tenors` grid year-fraction (within ~1 day) get their BKM moments copied
  // back onto that TenorAnalytics. Defaults are grid-aligned (1m = 30d, 3m = 91d).
  std::vector<double> rnd_tenors_years = {30.0 / 365.25, 91.0 / 365.25};
  bool compute_varswap = true;
  // Straddle→move multiplier: 0.79788 = expected |move| (MAD, Bachelier);
  // 1.25331 = 1-sigma (68%) move; ~0.85 = empirical haircut. Documented, not
  // hard-coded — callers pick and record their convention.
  double straddle_move_multiplier = 0.79788;
};

// Earnings context for the bare-`PricedSurface` path. `schedule` is the sorted
// earnings instants; `implied_emove` is the per-event move vol (eMove). When
// `schedule` is null the earnings-dependent fields are left NaN / zero.
//
// `censor_space` selects how `atmf_vol_ex_earnings` strips earnings across a
// tenor that STRADDLES an earnings date (its two bracketing pillars carry
// different event counts):
//   - true  (default): CENSORED-space interpolation (SpiderRock FLEX). Censor
//            each bracket pillar's ATM total variance separately, interpolate
//            the censored variance linearly in T (via `event_aware_w`), and
//            return the event-free vol — the same censored path the live
//            projection surface uses.
//   - false: the legacy PLAIN-space path — censor a single plain cross-pillar
//            interpolated total variance once. Kept reachable for A/B. The two
//            agree at/away from an earnings straddle and differ only across one.
// (Task 9's `EarningsReproConfig::censor_space` maps onto this field.)
struct EventContext {
  const EventSchedule *schedule = nullptr;
  double implied_emove = 0.0;
  bool censor_space = true;
};

// ── Fine-grained outputs ────────────────────────────────────────────────────

// ATM level plus the first two moneyness derivatives of the smile at k = 0.
struct SkewCurvature {
  double atm = 0.0;        // σ at k = 0
  double skew_slope = 0.0; // ∂σ/∂k |_{k=0}  (per unit log-moneyness)
  double curvature = 0.0;  // ∂²σ/∂k² |_{k=0}
  bool valid = false;
};

// Risk-neutral density and implied-distribution summary at one expiry. `strikes`,
// `pdf`, `cdf` are aligned; `pdf` is normalized to unit mass over the grid.
struct RiskNeutralDensity {
  double T = 0.0;
  double forward = 0.0;
  double df = 0.0;
  std::vector<double> strikes; // absolute strikes, ascending
  std::vector<double> pdf;     // normalized risk-neutral density q(K)
  std::vector<double> cdf;     // P(S_T <= K), monotone in [0, 1]
  // Moments from direct integration of the normalized RND.
  double mean = 0.0;
  double variance = 0.0;
  double skewness = 0.0;
  double kurtosis = 0.0;
  // Bakshi–Kapadia–Madan model-free moments (log-return strips). Preferred for
  // the third/fourth moment; the RND-integrated values are a cross-check.
  double bkm_variance = 0.0;
  double bkm_skew = 0.0;
  double bkm_kurt = 0.0;
  double skew_index = 0.0;        // 100 − 10·bkm_skew  (CBOE-SKEW style)
  double var_swap_vol = 0.0;      // sqrt(K_var): model-free implied vol off the same strip
  double mass_before_norm = 1.0;  // raw grid mass pre-normalization (ragged-smile flag)
  bool extrapolated = false;      // any wing IV was flat-filled from the fitted range
  std::vector<double> quantile_p; // requested probabilities (== RndConfig::quantiles)
  std::vector<double> quantile_k; // inverse-CDF strikes at quantile_p
  double prob_below_forward = 0.0;
  bool valid = false;
};

// ── Per-tenor and surface-level bundles ─────────────────────────────────────

// All single-surface analytics at one constant-maturity tenor. Vectors indexed
// by the config point lists (`delta_points`, `moneyness_points`) they mirror.
struct TenorAnalytics {
  double tenor_years = 0.0;
  std::string label;
  double forward = 0.0;
  double df = 0.0;
  double atm_vol = 0.0;         // ATMF, k = 0
  double atm_vol_ex_earn = 0.0; // earnings-stripped ATMF (NaN without EventContext)
  int n_earnings = 0;           // scheduled events in (now, T]
  // Per delta point (aligned to AnalyticsConfig::delta_points).
  std::vector<double> put_delta_vol;
  std::vector<double> call_delta_vol;
  std::vector<double> risk_reversal; // σ_put − σ_call
  std::vector<double> butterfly;     // ½(σ_put + σ_call) − σ_atm
  // Local smile shape.
  double skew_slope = 0.0;           // ∂σ/∂k |_{k=0}  (raw, per unit log-moneyness)
  double skew_slope_sqrt_t = 0.0;    // skew_slope · √T   (tenor-normalized, cross-T comparable)
  double skew_slope_norm = 0.0;      // skew_slope / atm_vol   (level-normalized)
  double curvature = 0.0;            // ∂²σ/∂k² |_{k=0}
  std::vector<double> moneyness_vol; // aligned to AnalyticsConfig::moneyness_points
  double skew_90_110 = 0.0;          // σ(90%) − σ(110%)
  // Variance / density derived.
  double var_swap_vol = 0.0;      // sqrt(K_var), model-free (NaN if disabled)
  double convexity_premium = 0.0; // var_swap_vol − atm_vol
  double expected_move = 0.0;     // straddle × AnalyticsConfig::straddle_move_multiplier
  double event_var_share = 0.0;   // n·eMove² / w_atm  (fraction of ATM variance from events)
  // Populated only for tenors that match an AnalyticsConfig::rnd_tenors_years
  // value (within ~1 day). rnd_skewness/rnd_kurtosis carry the BKM (log-return)
  // moments; leave 0 when no RND was computed at this tenor.
  double rnd_skewness = 0.0;
  double rnd_kurtosis = 0.0;
  double prob_below_forward = 0.0;
  // True when this tenor lies outside the fitted pillar range and the surface
  // FLAT-EXTRAPOLATED the vol (the served CurveSurface does not NaN there). The
  // metrics are still finite but are an extrapolation, not a fitted value.
  bool extrapolated = false;
  bool valid = false;
};

// The full single-surface analytic bundle.
struct SurfaceAnalytics {
  std::uint32_t uid = 0;
  std::int64_t as_of_ts_ns = 0;
  double spot = 0.0;
  double implied_emove = 0.0; // per-event earnings move used (0 if none)
  std::vector<TenorAnalytics> tenors;
  std::vector<RiskNeutralDensity> densities; // one per rnd_tenors_years (if compute_rnd)
  // Forward (calendar) vol between consecutive IN-RANGE tenors, aligned so
  // forward_vol_segments[i] spans (in_range_tenors[i], in_range_tenors[i+1]).
  std::vector<double> forward_vol_segments;
  // Term-structure summary (constant-maturity ATMF; NaN if a leg is out of range).
  double ts_slope_1m_3m = 0.0; // σ_3m − σ_1m
  double ts_slope_3m_1y = 0.0; // σ_1y − σ_3m
  double ts_ratio_1m_3m = 0.0; // σ_1m / σ_3m
  bool backwardation = false;  // front in-range ATM > back in-range ATM
  bool valid = false;          // at least one in-range (non-extrapolated) tenor
  // FIX-E M-5 / AN-P2-6. WHICH delta convention resolved this bundle's wings
  // (`put_delta_vol` / `call_delta_vol` / `risk_reversal` / `butterfly`) —
  // copied from the `AnalyticsConfig` that produced it. Without it the emitted
  // bundle is a set of wing numbers with no statement of what "25 delta" meant,
  // which is exactly where AN-P2-6's fragmentation actually bites: the knob is
  // enforced at the API, but the ARTIFACT was unlabelled.
  // `write_surface_analytics_csv` emits it in the meta header.
  DeltaConvention delta_convention = DeltaConvention::American;
};

// ── Two-surface change analytics ────────────────────────────────────────────

// Per-tenor change between two surfaces of the same underlying (t1 → t2).
struct TenorDiff {
  double tenor_years = 0.0;
  std::string label;
  double d_forward = 0.0;
  double d_atm_vol = 0.0;          // ATMF change
  double d_vol_fixed_strike = 0.0; // sticky-strike: at t1's ATM strike K0
  double d_vol_fixed_delta = 0.0;  // sticky-delta: at a fixed delta
  double d_skew_slope = 0.0;
  double d_risk_reversal_25 = 0.0;
  double d_butterfly_25 = 0.0;
  bool valid = false;
};

// The two-surface bundle plus the ATM-move sticky decomposition. With
// R = ln(S2/S1) and 𝒮 = t1 ATM skew slope: the sticky-strike prediction is
// 𝒮·R, the sticky-delta prediction is 0, and the residual is the observed ATM
// change minus the sticky-strike prediction (the regime diagnostic).
struct SurfaceDiff {
  std::int64_t ts1_ns = 0;
  std::int64_t ts2_ns = 0;
  double spot1 = 0.0;
  double spot2 = 0.0;
  double d_spot = 0.0;
  double log_return = 0.0; // ln(S2 / S1)
  std::vector<TenorDiff> tenors;
  double sticky_strike_atm_pred = 0.0; // 𝒮·R (front-tenor reference)
  double sticky_delta_atm_pred = 0.0;  // 0
  double residual_atm_move = 0.0;      // observed Δσ_atm − sticky_strike_atm_pred
  bool valid = false;
  // FIX-E M-5 / I-1. WHICH delta convention resolved `d_vol_fixed_delta`,
  // `d_risk_reversal_25` and `d_butterfly_25` — copied from the config, and now
  // actually HONOURED by `compute_surface_diff` (it used to be ignored here).
  DeltaConvention delta_convention = DeltaConvention::American;
};

// ── Primitives (public: composable and unit-testable) ───────────────────────

// ATMF vol σ(F(T), T) and forward F(T). NaN for a non-finite/non-positive T.
// For a T beyond the fitted pillar range the served surface flat-extrapolates,
// so the value is finite but extrapolated (see the module note above); use
// `compute_surface_analytics`'s `extrapolated` gate to distinguish in-range.
[[nodiscard]] double atmf_vol(const PricedSurface &ps, double T) noexcept;
[[nodiscard]] double atmf_forward(const PricedSurface &ps, double T) noexcept;

// E5 / AN-P2-6. The strike whose |delta| equals `abs_delta` on `side`, under an
// EXPLICIT convention. `American` delegates to `resolve_strike_by_delta`
// (strategy.hpp) — bit-identical to the pre-E5 behaviour. `Forward` solves the
// European Black-76 forward-delta strike:
//
//     N(d1) = Δ_call  /  N(d1) − 1 = −Δ_put   =>   K = F·exp(−z·v + v²/2)
//
// with z = N⁻¹(Δ) for a call, N⁻¹(1−Δ) for a put, and v = σ(K,T)·√T. Because σ
// depends on K through the smile this is a fixed point, seeded at K = F and
// iterated to a tight log-strike tolerance; on a flat smile it converges in one
// step to the closed form.
//
// InvalidArgument if `abs_delta` ∉ (0,1), the tenor has no usable forward/vol,
// or the target is unreachable.
[[nodiscard]] Result<double> strike_at_delta(const PricedSurface &ps, double T, Side side,
                                             double abs_delta,
                                             DeltaConvention convention =
                                                 DeltaConvention::American);

// Smile vol at the strike whose |delta| equals `abs_delta` on `side`. The
// convention defaults to `American`, which is what this module has always used.
// InvalidArgument if `abs_delta` ∉ (0,1) or the target is unreachable.
[[nodiscard]] Result<double> vol_at_delta(const PricedSurface &ps, double T, Side side,
                                          double abs_delta,
                                          DeltaConvention convention = DeltaConvention::American);

// Smile vol at K = F(T)·moneyness (moneyness = 1.0 is ATMF). NaN outside domain.
[[nodiscard]] double vol_at_moneyness(const PricedSurface &ps, double T, double moneyness) noexcept;

// Risk reversal σ(Δ-put) − σ(Δ-call) and butterfly ½(σ_put+σ_call) − σ_atm at
// the given absolute delta, under the given convention (default `American`).
// InvalidArgument if either wing strike is unreachable.
[[nodiscard]] Result<double> risk_reversal(const PricedSurface &ps, double T, double abs_delta,
                                           DeltaConvention convention =
                                               DeltaConvention::American);
[[nodiscard]] Result<double> butterfly(const PricedSurface &ps, double T, double abs_delta,
                                       DeltaConvention convention = DeltaConvention::American);

// ATM level, skew slope ∂σ/∂k, curvature ∂²σ/∂k² via a 3-pivot quadratic through
// k ∈ {−k_ref, 0, +k_ref}. `valid` is false if any pivot is NaN.
[[nodiscard]] SkewCurvature skew_curvature(const PricedSurface &ps, double T,
                                           double k_ref) noexcept;

// Forward (calendar) vol between two tenors: sqrt((w2 − w1)/(T2 − T1)) on ATMF
// total variance w = σ²·T. NaN if T2 ≤ T1 or w2 ≤ w1 (calendar arb / numerical).
[[nodiscard]] double forward_vol(const PricedSurface &ps, double T1, double T2) noexcept;

// Model-free implied vol sqrt(K_var(T)) via the OTM log-strip on the served
// surface. InvalidArgument for a non-finite/non-positive T or unresolved carry.
[[nodiscard]] Result<double> var_swap_vol(const PricedSurface &ps, double T,
                                          const RndConfig &cfg = {});

// Risk-neutral density and implied-distribution summary at one expiry
// (Breeden–Litzenberger + BKM). InvalidArgument for a non-finite/non-positive T
// or unresolved carry.
[[nodiscard]] Result<RiskNeutralDensity> risk_neutral_density(const PricedSurface &ps, double T,
                                                              const RndConfig &cfg = {});

// Implied CDF P(S_T <= K) at one strike via a central difference of Black-76
// call prices (P(S_T ≤ K) = 1 + e^{rT}·∂C/∂K). NaN for a non-finite/non-positive
// T or K. `cfg` is accepted for call-site symmetry with the other density
// primitives but is not used (the estimator is a local finite difference, not a
// grid).
[[nodiscard]] double implied_cdf(const PricedSurface &ps, double T, double K,
                                 const RndConfig &cfg = {}) noexcept;

// Earnings-stripped ("censored") ATMF vol: sqrt((w_atm − n·eMove²)/T) over the
// events in (now, T]. NaN if `ctx.schedule` is null, eMove ≤ 0, or if
// n·eMove² ≥ w_atm.
[[nodiscard]] double atmf_vol_ex_earnings(const PricedSurface &ps, double T,
                                          const EventContext &ctx) noexcept;

// E3a / AN-P1-3. Implied per-event earnings move, solved over ALL fitted
// expiries via `implied_emove_joint` (event_vol.hpp): the identified joint
// {eMove, st, lt, decay} fit when the pillar set supports it, falling back to
// the pre-E3a two-pillar bracket when it does not. This `_ex` overload reports
// WHICH solve produced the answer (`EmoveSolution::method`) along with the joint
// fit's outcome code, residual and expiry count.
[[nodiscard]] Result<EmoveSolution> earnings_implied_move_ex(const PricedSurface &ps,
                                                             const EventContext &ctx);

// Back-compatible scalar projection of `earnings_implied_move_ex`.
// InvalidArgument if the schedule is null or fewer than two expiries are usable;
// NotFound if no adjacent pair brackets an event and the joint fit was not
// available either.
[[nodiscard]] Result<double> earnings_implied_move(const PricedSurface &ps,
                                                   const EventContext &ctx);

// Implied correlation for a dispersion basket from already-computed per-name
// variances/vols and index weights. `clean` uses the full cross term; `dirty`
// uses the weighted-average-vol approximation. InvalidArgument on a size
// mismatch, empty inputs, or a non-positive denominator.
//   clean: ρ = (idx_var − Σ wᵢ²·varᵢ) / (Σ_{i≠j} wᵢwⱼ·√(varᵢ·varⱼ))
//   dirty: ρ = idx_var / (Σ wᵢ·volᵢ)²
[[nodiscard]] Result<double> implied_correlation_clean(double idx_var, std::span<const double> w,
                                                       std::span<const double> var);
[[nodiscard]] Result<double> implied_correlation_dirty(double idx_var, std::span<const double> w,
                                                       std::span<const double> vol);

// ── Aggregators ─────────────────────────────────────────────────────────────

// Full single-surface bundle. The session/fitted/fitter overloads snapshot to a
// PricedSurface and auto-extract the earnings schedule + solved eMove.
[[nodiscard]] Result<SurfaceAnalytics> compute_surface_analytics(const PricedSurface &ps,
                                                                 const AnalyticsConfig &cfg = {},
                                                                 const EventContext *ctx = nullptr);
[[nodiscard]] Result<SurfaceAnalytics> compute_surface_analytics(const VolaSession &session,
                                                                 const AnalyticsConfig &cfg = {});
[[nodiscard]] Result<SurfaceAnalytics> compute_surface_analytics(const FittedSurface &fitted,
                                                                 const AnalyticsConfig &cfg = {});
[[nodiscard]] Result<SurfaceAnalytics> compute_surface_analytics(const PricerFitter &fitter,
                                                                 const AnalyticsConfig &cfg = {});

// Two-surface change bundle. Both surfaces must be the same underlying (uid);
// sticky-strike uses t1's ATM strike per tenor. InvalidArgument on a uid mismatch.
[[nodiscard]] Result<SurfaceDiff> compute_surface_diff(const PricedSurface &a,
                                                       const PricedSurface &b,
                                                       const AnalyticsConfig &cfg = {});

// ── Serializers (house CSV style: `# key=value` meta header + rows) ─────────

[[nodiscard]] Status write_surface_analytics_csv(const SurfaceAnalytics &a, std::string_view path);
[[nodiscard]] Status write_surface_diff_csv(const SurfaceDiff &d, std::string_view path);
[[nodiscard]] Status write_rnd_csv(const RiskNeutralDensity &r, std::string_view path);

} // namespace atx::vol

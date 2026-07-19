// Single-surface analytics primitives — ATMF term structure, delta/moneyness
// wing vols, risk reversal / butterfly, skew & curvature, forward vol,
// earnings-stripped ATM, and the dispersion implied-correlation helpers.
//
// Pure functions over a PricedSurface (see analytics.hpp for the contract and
// conventions). This translation unit is intentionally independent of the
// density and aggregation TUs so it can be developed and tested in isolation.

#include "atx/vol/analytics.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "atx/core/error.hpp"
#include "atx/vol/event_vol.hpp" // count_events_at, censored_total_variance
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/strategy.hpp" // resolve_strike_by_delta

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

TenorGrid TenorGrid::standard() {
  // 1w, 2w, 1m, 2m, 3m, 6m, 9m, 1y, 18m, 2y on the ACT/365.25 basis.
  constexpr double kYear = 365.25;
  return TenorGrid{
      {7.0 / kYear, 14.0 / kYear, 30.0 / kYear, 60.0 / kYear, 91.0 / kYear, 182.0 / kYear,
       273.0 / kYear, 365.0 / kYear, 548.0 / kYear, 730.0 / kYear},
      {"1w", "2w", "1m", "2m", "3m", "6m", "9m", "1y", "18m", "2y"},
  };
}

double atmf_forward(const PricedSurface &ps, double T) noexcept {
  if (!(T > 0.0 && std::isfinite(T))) {
    return kNaN;
  }
  return ps.forward_at(T);
}

double atmf_vol(const PricedSurface &ps, double T) noexcept {
  const double F = ps.forward_at(T);
  // ps.iv NaNs outside its domain; the F>0 guard also rejects the degenerate
  // carry that `forward_at` returns (0) for a non-finite / non-positive T.
  if (!(T > 0.0) || !(F > 0.0)) {
    return kNaN;
  }
  return ps.iv(F, T);
}

Result<double> vol_at_delta(const PricedSurface &ps, double T, Side side, double abs_delta) {
  if (!(abs_delta > 0.0 && abs_delta < 1.0)) {
    return Err(ErrorCode::InvalidArgument, "vol_at_delta: abs_delta must be in (0,1)");
  }
  ATX_TRY(auto K, resolve_strike_by_delta(ps, T, side, abs_delta));
  return Ok(ps.iv(K, T));
}

double vol_at_moneyness(const PricedSurface &ps, double T, double moneyness) noexcept {
  if (!(T > 0.0 && moneyness > 0.0)) {
    return kNaN;
  }
  return ps.iv(ps.forward_at(T) * moneyness, T);
}

Result<double> risk_reversal(const PricedSurface &ps, double T, double abs_delta) {
  // Equity sign: RR = σ(Δ-put) − σ(Δ-call)  (positive = downside rich).
  ATX_TRY(auto p, vol_at_delta(ps, T, Side::Put, abs_delta));
  ATX_TRY(auto c, vol_at_delta(ps, T, Side::Call, abs_delta));
  return Ok(p - c);
}

Result<double> butterfly(const PricedSurface &ps, double T, double abs_delta) {
  // BF = ½(σ_put + σ_call) − σ_atm at the given absolute delta.
  ATX_TRY(auto p, vol_at_delta(ps, T, Side::Put, abs_delta));
  ATX_TRY(auto c, vol_at_delta(ps, T, Side::Call, abs_delta));
  const double a = atmf_vol(ps, T);
  return Ok(0.5 * (p + c) - a);
}

SkewCurvature skew_curvature(const PricedSurface &ps, double T, double k_ref) noexcept {
  if (!(T > 0.0 && k_ref > 0.0)) {
    return SkewCurvature{};
  }
  const double F = ps.forward_at(T);
  const double s0 = ps.iv(F, T);
  const double sp = ps.iv(F * std::exp(k_ref), T);
  const double sm = ps.iv(F * std::exp(-k_ref), T);
  if (!std::isfinite(s0) || !std::isfinite(sp) || !std::isfinite(sm)) {
    return SkewCurvature{};
  }
  return SkewCurvature{/*atm=*/s0,
                       /*skew_slope=*/(sp - sm) / (2.0 * k_ref),
                       /*curvature=*/(sp + sm - 2.0 * s0) / (k_ref * k_ref),
                       /*valid=*/true};
}

double forward_vol(const PricedSurface &ps, double T1, double T2) noexcept {
  if (!(0.0 < T1 && T1 < T2)) {
    return kNaN;
  }
  const double w1 = ps.total_variance(ps.forward_at(T1), T1);
  const double w2 = ps.total_variance(ps.forward_at(T2), T2);
  if (!std::isfinite(w1) || !std::isfinite(w2) || w2 <= w1) {
    return kNaN;
  }
  return std::sqrt((w2 - w1) / (T2 - T1));
}

double atmf_vol_ex_earnings(const PricedSurface &ps, double T, const EventContext &ctx) noexcept {
  if (ctx.schedule == nullptr || !(ctx.implied_emove > 0.0) || !(T > 0.0)) {
    return kNaN;
  }
  const std::int64_t now_ns = ps.pricing().now_ts_ns;
  const double emove = ctx.implied_emove;

  // CENSORED-space (SpiderRock FLEX) default: for a tenor that straddles an
  // earnings date its two bracketing pillars carry DIFFERENT event counts, so
  // censoring must happen pillar-by-pillar BEFORE the cross-pillar interpolation
  // — not once on a single plain cross-pillar interpolated variance. Locate the
  // bracket pillars around T (the served surface interpolates total variance
  // linearly in T, so this mirrors the live projection ATM-anchor step), read
  // each pillar's forward-ATM total variance, count events at each, and reuse
  // `event_aware_w`: it censors both pillars separately, interpolates the
  // censored variance in T, and re-adds n_query·eMove². Because this entry point
  // returns the EX-earnings (event-free) vol, n_query = 0 here — the query lump
  // is NOT re-added (the only difference from the live serve path, which re-adds
  // it). This changes ONLY the interpolation space, not the ex-earnings contract.
  if (ctx.censor_space) {
    const auto slices = ps.context();
    // First pillar with T_pillar >= T; the interior bracket is (hi-1, hi). No
    // interior bracket (T at/below the front pillar, at/above the back pillar,
    // or a single-slice surface) ⇒ fall through to the legacy single-pillar
    // censor below, which is exact there (the interpolation weight collapses).
    std::size_t hi = slices.size();
    for (std::size_t i = 0; i < slices.size(); ++i) {
      if (slices[i].T >= T) {
        hi = i;
        break;
      }
    }
    if (hi > 0 && hi < slices.size()) {
      const double T_lo = slices[hi - 1].T;
      const double T_hi = slices[hi].T;
      const double w_lo = ps.total_variance(ps.forward_at(T_lo), T_lo);
      const double w_hi = ps.total_variance(ps.forward_at(T_hi), T_hi);
      if (std::isfinite(w_lo) && std::isfinite(w_hi)) {
        const std::size_t n_lo = count_events_at(*ctx.schedule, now_ns, T_lo);
        const std::size_t n_hi = count_events_at(*ctx.schedule, now_ns, T_hi);
        // n_query = 0: keep the EX-earnings contract (interpolated censored
        // variance, no query lump re-added). `event_aware_w` floors each
        // pillar's censored variance at kWCenFloor, so an eMove that overshoots a
        // pillar's smile yields a floored (near-zero), not NaN, variance; the
        // finite/positive guard below rejects only a non-finite/non-positive blend.
        const double wc =
            event_aware_w(w_lo, T_lo, n_lo, w_hi, T_hi, n_hi, T, /*n_query=*/0, emove);
        if (!(std::isfinite(wc) && wc > 0.0)) {
          return kNaN;
        }
        return std::sqrt(wc / T);
      }
    }
    // No usable bracket: degrade to the legacy single-pillar censor below.
  }

  // Legacy PLAIN-space path (ctx.censor_space == false, or the degraded
  // no-bracket case above): censor a SINGLE plain cross-pillar interpolated
  // total variance once with the query expiry's own event count.
  const std::size_t n = count_events_at(*ctx.schedule, now_ns, T);
  const double w = ps.total_variance(ps.forward_at(T), T);
  if (!std::isfinite(w)) {
    return kNaN;
  }
  // Earnings variance ≥ total ATM variance: the censor would floor the censored
  // variance to ~0 and return a spurious near-zero vol from an ill-conditioned
  // (eMove-overshoots-the-smile) input. Report NaN rather than a fabricated
  // number. `!(lump < w)` is NaN-safe (w is finite here), so this fires only on a
  // genuine overshoot, never on a comparison with NaN.
  const double lump = static_cast<double>(n) * emove * emove;
  if (!(lump < w)) {
    return kNaN;
  }
  const double wc = censored_total_variance(w, n, emove);
  return std::sqrt(wc / T);
}

Result<double> implied_correlation_clean(double idx_var, std::span<const double> w,
                                         std::span<const double> var) {
  if (w.size() != var.size() || w.empty()) {
    return Err(ErrorCode::InvalidArgument, "clean corr: size mismatch or empty inputs");
  }
  double s1 = 0.0; // Σ wᵢ·√varᵢ
  double s2 = 0.0; // Σ wᵢ²·varᵢ
  for (std::size_t i = 0; i < w.size(); ++i) {
    s1 += w[i] * std::sqrt(var[i]);
    s2 += w[i] * w[i] * var[i];
  }
  // denom = S1² − S2 = Σ_{i≠j} wᵢwⱼ·√(varᵢ·varⱼ)  (the off-diagonal cross term).
  // A single name (or fully-concentrated basket) has an exactly-zero cross term;
  // guard with a scale-relative tolerance so floating-point residual (~1e-17 of
  // S1²) does not slip through the bare `> 0` test as a spurious huge ρ.
  const double denom = s1 * s1 - s2;
  if (!(denom > 1e-12 * s1 * s1) || !std::isfinite(idx_var)) {
    return Err(ErrorCode::InvalidArgument, "clean corr: non-positive cross term");
  }
  return Ok((idx_var - s2) / denom); // ρ intentionally NOT clamped.
}

Result<double> implied_correlation_dirty(double idx_var, std::span<const double> w,
                                         std::span<const double> vol) {
  if (w.size() != vol.size() || w.empty()) {
    return Err(ErrorCode::InvalidArgument, "dirty corr: size mismatch or empty inputs");
  }
  double s1 = 0.0; // Σ wᵢ·volᵢ
  for (std::size_t i = 0; i < w.size(); ++i) {
    s1 += w[i] * vol[i];
  }
  const double denom = s1 * s1;
  if (!(denom > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "dirty corr: non-positive weighted-vol denominator");
  }
  return Ok(idx_var / denom);
}

} // namespace atx::vol

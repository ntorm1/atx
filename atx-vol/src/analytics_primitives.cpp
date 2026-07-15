// Single-surface analytics primitives — ATMF term structure, delta/moneyness
// wing vols, risk reversal / butterfly, skew & curvature, forward vol,
// earnings-stripped ATM, and the dispersion implied-correlation helpers.
//
// Pure functions over a PricedSurface (see analytics.hpp for the contract and
// conventions). This translation unit is intentionally independent of the
// density and aggregation TUs so it can be developed and tested in isolation.

#include "atx/vol/analytics.hpp"

#include <limits>

namespace atx::vol {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

TenorGrid TenorGrid::standard() {
  // 1w, 2w, 1m, 2m, 3m, 6m, 9m, 1y, 18m, 2y on the ACT/365.25 basis.
  constexpr double kYear = 365.25;
  return TenorGrid{
      {7.0 / kYear, 14.0 / kYear, 30.0 / kYear, 60.0 / kYear, 91.0 / kYear, 182.0 / kYear,
       273.0 / kYear, 365.0 / kYear, 548.0 / kYear, 730.0 / kYear},
      {"1w", "2w", "1m", "2m", "3m", "6m", "9m", "1y", "18m", "2y"},
  };
}

double atmf_vol(const PricedSurface&, double) noexcept { return kNaN; }
double atmf_forward(const PricedSurface&, double) noexcept { return kNaN; }

Result<double> vol_at_delta(const PricedSurface&, double, Side, double) {
  return Err(ErrorCode::NotImplemented, "vol_at_delta");
}

double vol_at_moneyness(const PricedSurface&, double, double) noexcept { return kNaN; }

Result<double> risk_reversal(const PricedSurface&, double, double) {
  return Err(ErrorCode::NotImplemented, "risk_reversal");
}

Result<double> butterfly(const PricedSurface&, double, double) {
  return Err(ErrorCode::NotImplemented, "butterfly");
}

SkewCurvature skew_curvature(const PricedSurface&, double, double) noexcept {
  return SkewCurvature{};
}

double forward_vol(const PricedSurface&, double, double) noexcept { return kNaN; }

double atmf_vol_ex_earnings(const PricedSurface&, double, const EventContext&) noexcept {
  return kNaN;
}

Result<double> implied_correlation_clean(double, std::span<const double>, std::span<const double>) {
  return Err(ErrorCode::NotImplemented, "implied_correlation_clean");
}

Result<double> implied_correlation_dirty(double, std::span<const double>, std::span<const double>) {
  return Err(ErrorCode::NotImplemented, "implied_correlation_dirty");
}

}  // namespace atx::vol

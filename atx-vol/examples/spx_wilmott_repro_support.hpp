#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "atx/vol/api/fitting/calib.hpp"
#include "atx/vol/api/fitting/dense_slice.hpp"
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/marketdata/universe.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"

namespace atx::vol::spx_wilmott {

struct ForwardEstimate {
  double forward{0.0};
  double mad{0.0};
  std::size_t n_pairs{0};
};

// Robust European put-call-parity estimate. Quotes are ranked by |C-P| so the
// selected strip is centered on the forward without requiring an external spot.
// The reported forward is the median of the selected per-strike PCP forwards;
// mad is the median absolute deviation around that estimate.
[[nodiscard]] Result<ForwardEstimate>
estimate_european_forward(const Chain &chain, double T, double r, std::size_t n_atm_pairs = 13);

// Market ATM volatility obtained by linear interpolation in log-moneyness.
// Falls back to the nearest finite observation when the strip does not bracket
// k=0. Returns NaN for an empty/non-finite strip or non-positive T.
[[nodiscard]] double interpolate_atm_sigma(std::span<const FitObs> observations, double T) noexcept;

inline constexpr double kFigureMarketZMin = -9.7;
inline constexpr double kFigureMarketZMax = 2.2;
inline constexpr double kFigureCurveZMin = -10.0;
inline constexpr double kFigureCurveZMax = 2.86;
inline constexpr double kZeroBidWeightScale = 0.01;

struct ReproductionObsSet {
  std::vector<FitObs> obs{};
  std::size_t n_zero_bid{0};
  std::size_t n_rejected{0};
  std::size_t n_outside_domain{0};
  double z_min{0.0};
  double z_max{0.0};
};

// Research-only observation builder for the published Figure 1 visual domain.
// Unlike production build_observations, it retains an OTM quote with bid==0 and
// ask>0 as a downweighted midpoint/error-band mark. It never changes the
// production filtering API. Crossed/malformed/non-finite quotes are rejected,
// and only k/(sigma0*sqrt(T)) in the digitized market-mark domain
// [kFigureMarketZMin,kFigureMarketZMax] is retained.
[[nodiscard]] Result<ReproductionObsSet>
build_figure_reproduction_observations(const Chain &chain, double F, double T, double df,
                                       double sigma0, double max_weight = 1.0e3);

struct PriceConeScore {
  std::size_t price_bound_violations{0};
  std::size_t monotonicity_violations{0};
  std::size_t convexity_violations{0};
  double max_slope_decrease{0.0};

  [[nodiscard]] bool clean() const noexcept {
    return price_bound_violations == 0u && monotonicity_violations == 0u &&
           convexity_violations == 0u;
  }
};

// Exact node-space no-arbitrage check for ConvexDense's piecewise-linear call
// representation. This complements (but never replaces) arb_check_butterfly's
// total-variance finite-difference diagnostic, which is ill-conditioned at the
// atomic-density knots of a piecewise-linear convex price curve.
[[nodiscard]] PriceConeScore check_convex_price_cone(const ConvexSliceFit &fit,
                                                     double tolerance = 2.0e-8) noexcept;

struct FitScore {
  double rmse_iv{0.0};
  double max_abs_iv_error{0.0};
  double in_band_percent{0.0};
  std::size_t n_scored{0};
  std::size_t n_band_scored{0};
  std::size_t n_in_band{0};
};

// Score one fitted European curve against the midpoint IVs and Black-76
// bid/ask-IV bands retained in FitObs. Invalid curve/band points are skipped.
[[nodiscard]] FitScore score_curve(std::span<const FitObs> observations, const IVolCurve &curve);

} // namespace atx::vol::spx_wilmott

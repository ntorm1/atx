// Risk-neutral density, implied CDF, and model-free implied variance.
//
// Breeden–Litzenberger density from Black-76 prices on the served surface's IV,
// implied CDF/quantiles, BKM model-free moments, and the OTM log-strip variance
// (see analytics.hpp). Independent of the primitives and aggregation TUs.

#include "atx/vol/analytics.hpp"

#include <limits>

namespace atx::vol {

Result<double> var_swap_vol(const PricedSurface&, double, const RndConfig&) {
  return Err(ErrorCode::NotImplemented, "var_swap_vol");
}

Result<RiskNeutralDensity> risk_neutral_density(const PricedSurface&, double, const RndConfig&) {
  return Err(ErrorCode::NotImplemented, "risk_neutral_density");
}

double implied_cdf(const PricedSurface&, double, double, const RndConfig&) noexcept {
  return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace atx::vol

#include "atx/engine/risk/vol_regime.hpp"

#include <limits>

namespace atx::engine::risk {
atx::core::Result<RegimeAdjustV2> vol_regime_multiplier_v2(
    const atx::core::linalg::MatX& realized, const PriorVarianceForecasts& prior,
    std::span<const atx::usize> ages, atx::usize half_life, atx::usize min_dates,
    atx::u64 budget) {
  namespace co = atx::core;
  const auto t = static_cast<atx::usize>(realized.rows());
  const auto k = static_cast<atx::usize>(realized.cols());
  const bool weighted = prior.weights.size() != 0;
  if (t == 0 || k == 0 || prior.variances.rows() != realized.rows() ||
      prior.variances.cols() != realized.cols() || prior.available_ages.size() != t ||
      (!ages.empty() && ages.size() != t) || min_dates == 0 ||
      (weighted && (prior.weights.rows() != realized.rows() || prior.weights.cols() != realized.cols())))
    return co::Err(co::ErrorCode::InvalidArgument, "VRA V2: forecast/return/clock shape mismatch");
  if (t > budget / 16 || k > std::numeric_limits<atx::usize>::max() / t)
    return co::Err(co::ErrorCode::OutOfRange, "VRA V2: history budget exceeded");
  for (atx::usize r = 1; r < ages.size(); ++r)
    if (ages[r] <= ages[r - 1])
      return co::Err(co::ErrorCode::InvalidArgument, "VRA V2: session ages must strictly increase");
  RegimeAdjustV2 out;
  out.bias_squared = atx::core::linalg::VecX::Constant(realized.rows(), std::numeric_limits<atx::f64>::quiet_NaN());
  atx::f64 total = 0.0, time_mass = 0.0;
  for (atx::usize r = 0; r < t; ++r) {
    const auto row_age = ages.empty() ? r : ages[r];
    const auto available = prior.available_ages[r];
    const bool known_clock = available != std::numeric_limits<atx::usize>::max();
    if (known_clock && available <= row_age)
      return co::Err(co::ErrorCode::InvalidArgument, "VRA V2: forecast was not available before realization");
    atx::f64 mass = 0.0, ss = 0.0;
    for (atx::usize i = 0; i < k; ++i) {
      const auto rr = static_cast<Eigen::Index>(r), ii = static_cast<Eigen::Index>(i);
      const auto value = realized(rr, ii), variance = prior.variances(rr, ii);
      const auto w = weighted ? prior.weights(rr, ii) : 1.0;
      if (std::isinf(value) || std::isinf(variance) || std::isinf(w) ||
          (std::isfinite(variance) && variance <= 0.0) || (std::isfinite(w) && w < 0.0))
        return co::Err(co::ErrorCode::InvalidArgument, "VRA V2: invalid return/prior variance/weight");
      if (!known_clock || !std::isfinite(value) || !std::isfinite(variance) || !std::isfinite(w) || w == 0.0) {
        ++out.pairs_unavailable;
        continue;
      }
      const auto standardized = value / std::sqrt(variance);
      ss += w * standardized * standardized; mass += w; ++out.pairs_used;
    }
    if (!std::isfinite(ss) || !std::isfinite(mass))
      return co::Err(co::ErrorCode::OutOfRange, "VRA V2: cross-sectional statistic overflow");
    if (mass == 0.0) continue;
    const auto square = ss / mass;
    out.bias_squared[static_cast<Eigen::Index>(r)] = square;
    const auto tw = half_life == 0 ? 1.0 : std::exp2(-static_cast<atx::f64>(row_age) / static_cast<atx::f64>(half_life));
    total += tw * square; time_mass += tw; ++out.dates_used;
  }
  if (!std::isfinite(total) || !std::isfinite(time_mass))
    return co::Err(co::ErrorCode::OutOfRange, "VRA V2: time aggregation overflow");
  if (out.dates_used >= min_dates && time_mass > 0.0) {
    const auto lambda = total / time_mass;
    // An all-zero observed regime cannot supply a positive covariance scale.
    // Keep the unadjusted forecast and report unavailable rather than flattening
    // the entire risk model to the numerical floor.
    if (std::isfinite(lambda) && lambda > 0.0) { out.lambda2 = lambda; out.available = true; }
  }
  return co::Ok(std::move(out));
}
} // namespace atx::engine::risk

#include "atx/engine/risk/eigen_adjust.hpp"

#include <limits>

namespace atx::engine::risk {
atx::core::Result<EigenAdjustmentV2> eigen_adjust_v2(
    const atx::core::linalg::MatX& f, atx::f64 effective, atx::usize simulations,
    atx::f64 amplification, atx::u64 seed, atx::u64 budget) {
  namespace co = atx::core;
  const auto k = static_cast<atx::usize>(f.rows());
  if (k == 0 || f.rows() != f.cols() || !f.allFinite() ||
      !std::isfinite(effective) || effective < 2.0 ||
      effective > static_cast<atx::f64>(std::numeric_limits<int>::max() / 4) ||
      !std::isfinite(amplification) || amplification < 0.0 || simulations > 10'000)
    return co::Err(co::ErrorCode::InvalidArgument, "eigen adjustment V2: invalid covariance/effective history/config");
  const auto t = static_cast<atx::usize>(std::floor(effective + 0.5));
  if (k > budget / 128 / k || t > budget / 64 / k)
    return co::Err(co::ErrorCode::OutOfRange, "eigen adjustment V2: workspace exceeds budget");
  if (!f.isApprox(f.transpose(), 1e-12))
    return co::Err(co::ErrorCode::InvalidArgument, "eigen adjustment V2: covariance is not symmetric");
  ATX_TRY(const auto eig, atx::core::linalg::symmetric_eig(f));
  for (Eigen::Index i = 0; i < eig.values.size(); ++i)
    if (!std::isfinite(eig.values[i]) || eig.values[i] <= 0.0)
      return co::Err(co::ErrorCode::InvalidArgument, "eigen adjustment V2: covariance is not positive definite");
  if (simulations == 0 || amplification == 0.0)
    return co::Ok(EigenAdjustmentV2{f, effective, 0});
  if (t <= k)
    return co::Err(co::ErrorCode::InvalidArgument, "eigen adjustment V2: effective sample cannot identify K eigenfactors");
  atx::core::Xoshiro256pp rng{seed};
  atx::usize invalid_samples = 0;
  const auto bias = detail::accumulate_vol_bias(eig.vectors, eig.values, simulations, t, rng, &invalid_samples);
  if (invalid_samples != 0)
    return co::Err(co::ErrorCode::InvalidArgument, "eigen adjustment V2: invalid simulated covariance");
  auto adjusted = eig.values;
  for (Eigen::Index i = 0; i < adjusted.size(); ++i) {
    const auto gamma = 1.0 + amplification * (bias[i] / static_cast<atx::f64>(simulations) - 1.0);
    adjusted[i] *= gamma * gamma;
    if (!std::isfinite(gamma) || gamma <= 0.0 || !std::isfinite(adjusted[i]) || adjusted[i] <= 0.0)
      return co::Err(co::ErrorCode::OutOfRange, "eigen adjustment V2: invalid simulated adjustment");
  atx::core::linalg::MatX out = eig.vectors * adjusted.asDiagonal() * eig.vectors.transpose();
  out = 0.5 * (out + out.transpose().eval());
  if (!out.allFinite())
    return co::Err(co::ErrorCode::OutOfRange, "eigen adjustment V2: reconstruction overflow");
  return co::Ok(EigenAdjustmentV2{std::move(out), effective, t});
}
} // namespace atx::engine::risk

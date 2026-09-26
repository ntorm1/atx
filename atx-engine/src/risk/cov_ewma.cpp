#include "atx/engine/risk/cov_ewma.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <sstream>
#include <locale>

#include "atx/engine/risk/estimator_policy.hpp"

namespace atx::engine::risk {
namespace {
namespace co = atx::core;
using atx::f64;
using atx::usize;

usize age(std::span<const usize> ages, usize row) { return ages.empty() ? row : ages[row]; }
f64 weight(usize row_age, usize half_life) {
  return half_life == 0 ? 1.0 : std::exp2(-static_cast<f64>(row_age) / static_cast<f64>(half_life));
}
co::Status validate_clock(usize n, std::span<const usize> ages, usize lags) {
  if (n == 0 || (!ages.empty() && ages.size() != n) || lags > 10'000)
    return co::Err(co::ErrorCode::InvalidArgument, "EWMA V2: invalid history/clock/lag shape");
  for (usize r = 1; r < ages.size(); ++r)
    if (ages[r] <= ages[r - 1])
      return co::Err(co::ErrorCode::InvalidArgument, "EWMA V2: session ages must strictly increase");
  return co::Ok();
}
struct Mean {
  f64 value{}, sw{}, sw2{};
  usize observations{};
};
co::Result<Mean> mean(std::span<const f64> values, std::span<const usize> ages, usize half_life) {
  Mean out;
  // Shifted summation avoids cancellation in a nearly constant return series.
  f64 anchor = 0.0, sum = 0.0;
  bool anchored = false;
  for (usize r = 0; r < values.size(); ++r) {
    const auto value = values[r];
    if (std::isnan(value)) continue;
    if (!std::isfinite(value))
      return co::Err(co::ErrorCode::InvalidArgument, "EWMA V2: infinite return");
    if (!anchored) { anchor = value; anchored = true; }
    const auto w = weight(age(ages, r), half_life);
    sum += w * (value - anchor);
    out.sw += w; out.sw2 += w * w; ++out.observations;
  }
  out.value = out.sw > 0.0 ? anchor + sum / out.sw : 0.0;
  if (!std::isfinite(sum) || !std::isfinite(out.value))
    return co::Err(co::ErrorCode::OutOfRange, "EWMA V2: mean overflow");
  return co::Ok(out);
}

// Same column means/half-life for every lag in a HAC estimate. The lag uses
// exact session distance, so an unobserved session is never a one-day neighbor.
co::Result<f64> covariance(std::span<const f64> a, std::span<const f64> b,
    std::span<const usize> ages, usize half_life, usize lags) {
  ATX_TRY(const auto ma, mean(a, ages, half_life));
  ATX_TRY(const auto mb, mean(b, ages, half_life));
  f64 sum = 0.0, mass = 0.0;
  for (usize r = 0; r < a.size(); ++r) {
    if (!std::isfinite(a[r]) || !std::isfinite(b[r])) continue;
    const auto w = weight(age(ages, r), half_life);
    sum += w * (a[r] - ma.value) * (b[r] - mb.value); mass += w;
  }
  f64 out = mass > 0.0 ? sum / mass : 0.0;
  for (usize lag = 1; lag <= lags; ++lag) {
    f64 ab = 0.0, ba = 0.0;
    usize newer = 0;
    for (usize older = 0; older < a.size(); ++older) {
      const auto old_age = age(ages, older);
      if (old_age < lag) continue;
      while (newer < older && age(ages, newer) < old_age - lag) ++newer;
      if (newer >= older || age(ages, newer) != old_age - lag) continue;
      const auto w = weight(old_age, half_life);
      if (std::isfinite(a[older]) && std::isfinite(b[newer]))
        ab += w * (a[older] - ma.value) * (b[newer] - mb.value);
      if (std::isfinite(b[older]) && std::isfinite(a[newer]))
        ba += w * (b[older] - mb.value) * (a[newer] - ma.value);
    }
    // Normalize by zero-lag mass, as in the finite-sample Bartlett estimator;
    // missing lag pairs contribute no observation and no compacted adjacency.
    if (mass > 0.0)
      out += (1.0 - static_cast<f64>(lag) / static_cast<f64>(lags + 1)) * (ab + ba) / mass;
  }
  if (!std::isfinite(out))
    return co::Err(co::ErrorCode::OutOfRange, "EWMA V2: covariance overflow");
  return co::Ok(out);
}
} // namespace

std::string risk_estimator_recipe(const RiskEstimatorPolicy& p) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "risk-estimator/" << static_cast<unsigned>(p.rule) << ':'
      << p.vol_halflife << ':' << p.correlation_halflife << ':' << p.factor_nw_lags << ':'
      << p.specific_halflife << ':' << p.specific_nw_lags << ':' << p.nw_halflife << ':'
      << p.vra_halflife << ':' << p.eigen_simulations << ':' << std::bit_cast<atx::u64>(p.eigen_amplification) << ':'
      << p.eigen_seed << ':' << p.structural_min_observations << ':' << std::bit_cast<atx::u64>(p.bayesian_q) << ':'
      << std::bit_cast<atx::u64>(p.variance_floor) << ':' << static_cast<unsigned>(p.missing_prior) << ':'
      << p.max_working_bytes;
  return out.str();
}

atx::core::Result<EwmaVarianceV2> ewma_variance_v2(std::span<const atx::f64> values,
    std::span<const atx::usize> ages, atx::usize vol_hl, atx::usize lags, atx::usize nw_hl) {
  ATX_TRY_VOID(validate_clock(values.size(), ages, lags));
  ATX_TRY(const auto m, mean(values, ages, vol_hl));
  ATX_TRY(const auto fast, covariance(values, values, ages, vol_hl, 0));
  ATX_TRY(const auto base, covariance(values, values, ages, nw_hl, 0));
  ATX_TRY(const auto hac, covariance(values, values, ages, nw_hl, lags));
  const auto ratio = lags == 0 || base <= 0.0 ? 1.0 : std::max(0.0, hac / base);
  const auto value = fast * ratio;
  if (!std::isfinite(value) || value < 0.0)
    return co::Err(co::ErrorCode::OutOfRange, "EWMA V2: serial-adjusted variance overflow");
  return co::Ok(EwmaVarianceV2{value, m.sw2 > 0.0 ? m.sw * m.sw / m.sw2 : 0.0, m.observations});
}

atx::core::Result<EwmaCovarianceV2> ewma_factor_covariance_v2(
    const atx::core::linalg::MatX& values, std::span<const atx::usize> ages,
    atx::usize vol_hl, atx::usize corr_hl, atx::usize lags, atx::usize nw_hl,
    atx::u64 budget) {
  const auto t = static_cast<usize>(values.rows()), k = static_cast<usize>(values.cols());
  ATX_TRY_VOID(validate_clock(t, ages, lags));
  if (k == 0 || k > 4096 || k > budget / 128 / k)
    return co::Err(co::ErrorCode::OutOfRange, "EWMA V2: covariance workspace exceeds budget");
  EwmaCovarianceV2 out;
  out.covariance = atx::core::linalg::MatX::Zero(values.cols(), values.cols());
  out.effective_observations.resize(k); out.observations.resize(k);
  std::vector<f64> vol(k), correlation_variance(k);
  for (usize i = 0; i < k; ++i) {
    const std::span<const f64> column{values.col(static_cast<Eigen::Index>(i)).data(), t};
    ATX_TRY(const auto estimate, ewma_variance_v2(column, ages, vol_hl, lags, nw_hl));
    if (estimate.observations < 2 || estimate.effective_observations <= 1.0)
      return co::Err(co::ErrorCode::InvalidArgument, "EWMA V2: factor has insufficient observed history");
    out.effective_observations[i] = estimate.effective_observations;
    out.observations[i] = estimate.observations;
    vol[i] = estimate.variance;
    ATX_TRY(const auto hc, covariance(column, column, ages, corr_hl, lags));
    correlation_variance[i] = hc;
    out.covariance(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = vol[i];
  }
  for (usize i = 0; i < k; ++i) for (usize j = i + 1; j < k; ++j) {
    const std::span<const f64> a{values.col(static_cast<Eigen::Index>(i)).data(), t};
    const std::span<const f64> b{values.col(static_cast<Eigen::Index>(j)).data(), t};
    ATX_TRY(const auto hc, covariance(a, b, ages, corr_hl, lags));
    const auto denominator = std::sqrt(std::max(0.0, correlation_variance[i])) *
        std::sqrt(std::max(0.0, correlation_variance[j]));
    const auto rho = denominator > 0.0 ? std::clamp(hc / denominator, -1.0, 1.0) : 0.0;
    const auto value = rho * std::sqrt(vol[i]) * std::sqrt(vol[j]);
    if (!std::isfinite(value))
      return co::Err(co::ErrorCode::OutOfRange, "EWMA V2: recombined covariance overflow");
    out.covariance(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = value;
    out.covariance(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i)) = value;
  }
  ATX_TRY(const auto eig, atx::core::linalg::symmetric_eig(out.covariance));
  auto eigenvalues = eig.values;
  const auto trace = out.covariance.trace();
  if (!std::isfinite(trace))
    return co::Err(co::ErrorCode::OutOfRange, "EWMA V2: trace overflow");
  const auto floor = std::max(1e-12, kEwmaSpdFloorRel * trace / static_cast<f64>(k));
  bool floored = false;
  for (Eigen::Index i = 0; i < eigenvalues.size(); ++i) {
    if (eigenvalues[i] < floor) { eigenvalues[i] = floor; floored = true; }
  }
  if (floored) {
    out.covariance = eig.vectors * eigenvalues.asDiagonal() * eig.vectors.transpose();
    out.covariance = 0.5 * (out.covariance + out.covariance.transpose().eval());
  }
  if (!out.covariance.allFinite())
    return co::Err(co::ErrorCode::OutOfRange, "EWMA V2: covariance floor overflow");
  return co::Ok(std::move(out));
}
} // namespace atx::engine::risk

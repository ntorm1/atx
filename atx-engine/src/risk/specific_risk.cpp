#include "atx/engine/risk/specific_risk.hpp"

#include <numeric>

#include "atx/engine/risk/cov_ewma.hpp"

namespace atx::engine::risk {
atx::core::Result<SpecificRiskV2> specific_risk_v2(
    const atx::core::linalg::MatX& residuals, const atx::core::linalg::MatX& exposures,
    std::span<const atx::f64> caps, std::span<const atx::usize> ages,
    const SpecificRiskConfigV2& cfg) {
  namespace co = atx::core;
  using atx::f64;
  using atx::usize;
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;
  const auto m = static_cast<usize>(residuals.cols()), t = static_cast<usize>(residuals.rows());
  const auto p = static_cast<usize>(exposures.cols());
  if (m == 0 || t == 0 || static_cast<usize>(exposures.rows()) != m ||
      (!caps.empty() && caps.size() != m) || (cfg.bayesian_q > 0.0 && caps.size() != m) ||
      cfg.min_observations < 2 || !std::isfinite(cfg.bayesian_q) || cfg.bayesian_q < 0.0 ||
      !std::isfinite(cfg.variance_floor) || cfg.variance_floor <= 0.0)
    return co::Err(co::ErrorCode::InvalidArgument, "specific risk V2: invalid inputs/configuration");
  if (p > 4096 || m > cfg.max_working_bytes / 128 / (p + 1))
    return co::Err(co::ErrorCode::OutOfRange, "specific risk V2: workspace exceeds budget");
  f64 max_cap = 1.0;
  if (!caps.empty()) {
    max_cap = 0.0;
    for (const auto cap : caps) {
      if (!std::isfinite(cap) || cap <= 0.0)
        return co::Err(co::ErrorCode::InvalidArgument, "specific risk V2: observed positive market caps required");
      max_cap = std::max(max_cap, cap);
    }
  }
  const auto cw = [&](usize i) { return caps.empty() ? 1.0 : caps[i] / max_cap; };
  SpecificRiskV2 out;
  out.variances.resize(static_cast<Eigen::Index>(m));
  out.time_series_sigma.resize(static_cast<Eigen::Index>(m));
  out.structural_sigma.resize(static_cast<Eigen::Index>(m));
  out.shrinkage_weight = VecX::Zero(static_cast<Eigen::Index>(m));
  out.observations.resize(m); out.structural_fallback.resize(m); out.size_decile.resize(m);
  std::vector<f64> effective(m);
  std::vector<usize> reliable;
  f64 prior_sum = 0.0, prior_mass = 0.0;
  for (usize i = 0; i < m; ++i) {
    const std::span<const f64> column{residuals.col(static_cast<Eigen::Index>(i)).data(), t};
    ATX_TRY(const auto estimate, ewma_variance_v2(column, ages, cfg.half_life, cfg.nw_lags, cfg.nw_half_life));
    out.observations[i] = estimate.observations; effective[i] = estimate.effective_observations;
    const auto sigma = std::sqrt(std::max(cfg.variance_floor, estimate.variance));
    out.time_series_sigma[static_cast<Eigen::Index>(i)] = sigma;
    if (estimate.observations >= cfg.min_observations && effective[i] >= static_cast<f64>(cfg.min_observations)) {
      prior_sum += cw(i) * sigma; prior_mass += cw(i);
      if (exposures.row(static_cast<Eigen::Index>(i)).allFinite()) reliable.push_back(i);
    }
  }
  if (!std::isfinite(prior_sum) || !std::isfinite(prior_mass) || prior_mass <= 0.0)
    return co::Err(co::ErrorCode::InvalidArgument, "specific risk V2: no reliable population risk prior");
  const auto population = prior_sum / prior_mass;
  out.structural_sigma.setConstant(population);
  // Add a genuine intercept, excluding constant exposure columns (including an
  // existing Market intercept). Remaining collinearity is reported as a
  // population fallback, never solved with an unreported arbitrary ridge.
  std::vector<usize> columns;
  if (!reliable.empty()) for (usize c = 0; c < p; ++c) {
    const auto first = exposures(static_cast<Eigen::Index>(reliable.front()), static_cast<Eigen::Index>(c));
    bool varying = false;
    for (const auto i : reliable)
      varying = varying || exposures(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(c)) != first;
    if (varying) columns.push_back(c);
  }
  if (reliable.size() > columns.size() + 1) {
    MatX design(static_cast<Eigen::Index>(reliable.size()), static_cast<Eigen::Index>(columns.size() + 1));
    VecX response(static_cast<Eigen::Index>(reliable.size()));
    for (usize r = 0; r < reliable.size(); ++r) {
      const auto i = reliable[r];
      design(static_cast<Eigen::Index>(r), 0) = 1.0;
      for (usize c = 0; c < columns.size(); ++c)
        design(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c + 1)) =
            exposures(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(columns[c]));
      response[static_cast<Eigen::Index>(r)] = std::log(out.time_series_sigma[static_cast<Eigen::Index>(i)]);
    }
    const auto fit = atx::core::linalg::ols(design, response);
    if (fit) {
      const VecX residual = response - design * fit->beta;
      f64 correction = 0.0;
      for (Eigen::Index i = 0; i < residual.size(); ++i) correction += std::exp(residual[i]);
      correction /= static_cast<f64>(reliable.size());
      if (!std::isfinite(correction) || correction <= 0.0)
        return co::Err(co::ErrorCode::OutOfRange, "specific risk V2: exponentiation correction overflow");
      out.exponentiation_correction = correction;
      out.exposure_model_fitted = true;
      for (usize i = 0; i < m; ++i) {
        if (!exposures.row(static_cast<Eigen::Index>(i)).allFinite()) continue;
        auto value = fit->beta[0];
        for (usize c = 0; c < columns.size(); ++c)
          value += exposures(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(columns[c])) *
              fit->beta[static_cast<Eigen::Index>(c + 1)];
        const auto sigma = correction * std::exp(value);
        if (!std::isfinite(sigma) || sigma <= 0.0)
          return co::Err(co::ErrorCode::OutOfRange, "specific risk V2: structural extrapolation overflow");
        out.structural_sigma[static_cast<Eigen::Index>(i)] = sigma;
      }
    }
  }
  VecX blended(static_cast<Eigen::Index>(m));
  for (usize i = 0; i < m; ++i) {
    const auto gamma = std::clamp(effective[i] / static_cast<f64>(cfg.min_observations), 0.0, 1.0);
    out.structural_fallback[i] = gamma < 1.0 ? 1 : 0;
    blended[static_cast<Eigen::Index>(i)] = gamma * out.time_series_sigma[static_cast<Eigen::Index>(i)] +
        (1.0 - gamma) * out.structural_sigma[static_cast<Eigen::Index>(i)];
  }
  std::vector<usize> order(m);
  std::iota(order.begin(), order.end(), usize{0});
  if (!caps.empty()) std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return caps[a] < caps[b]; });
  for (usize rank = 0; rank < m; ++rank) out.size_decile[order[rank]] = static_cast<atx::u8>(rank * 10 / m);
  for (usize group = 0; group < 10; ++group) {
    f64 sum = 0.0, mass = 0.0; usize count = 0;
    for (usize i = 0; i < m; ++i) if (out.size_decile[i] == group) {
      sum += cw(i) * blended[static_cast<Eigen::Index>(i)]; mass += cw(i); ++count;
    }
    if (count == 0) continue;
    if (!std::isfinite(sum) || !std::isfinite(mass) || mass <= 0.0)
      return co::Err(co::ErrorCode::OutOfRange, "specific risk V2: size-decile mean overflow");
    const auto target = sum / mass;
    f64 ss = 0.0;
    for (usize i = 0; i < m; ++i) if (out.size_decile[i] == group) {
      const auto delta = blended[static_cast<Eigen::Index>(i)] - target;
      ss += delta * delta;
    }
    const auto dispersion = std::sqrt(ss / static_cast<f64>(count));
    if (!std::isfinite(dispersion))
      return co::Err(co::ErrorCode::OutOfRange, "specific risk V2: size-decile dispersion overflow");
    for (usize i = 0; i < m; ++i) if (out.size_decile[i] == group) {
      const auto sigma = blended[static_cast<Eigen::Index>(i)];
      const auto distance = cfg.bayesian_q * std::abs(sigma - target);
      const auto intensity = distance > 0.0 ? distance / (dispersion + distance) : 0.0;
      const auto shrunk = intensity * target + (1.0 - intensity) * sigma;
      const auto variance = shrunk * shrunk;
      if (!std::isfinite(intensity) || !std::isfinite(variance))
        return co::Err(co::ErrorCode::OutOfRange, "specific risk V2: shrinkage overflow");
      out.shrinkage_weight[static_cast<Eigen::Index>(i)] = intensity;
      out.variances[static_cast<Eigen::Index>(i)] = std::max(cfg.variance_floor, variance);
    }
  }
  return co::Ok(std::move(out));
}
} // namespace atx::engine::risk

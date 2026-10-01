// atx::engine::book -- ex-ante risk target (platform v8 R-8, risk-target-v1). Contracts live in
// book/risk_target.hpp.

#include "atx/engine/book/risk_target.hpp"

#include <cmath>   // std::abs, std::isfinite, std::sqrt
#include <span>    // std::span
#include <string>  // std::string
#include <utility> // std::move (ATX_TRY)
#include <vector>  // std::vector

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

using atx::f64;
using atx::usize;

namespace {

// An InvalidArgument error, convertible to every Result<T>.
[[nodiscard]] auto invalid(const char *what) {
  return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                        std::string("risk-target-v1: ") + what);
}

[[nodiscard]] bool finite_positive(f64 x) noexcept { return std::isfinite(x) && x > 0.0; }

// The model's shape and entries against n = specific.size() names and w.
[[nodiscard]] atx::core::Status check_model(const FactorRiskView &m, std::span<const f64> w) {
  const usize n = m.specific.size();
  const usize k = m.factors();
  if (w.size() != n || m.group.size() != n || m.exposures.size() != n * m.styles ||
      m.covariance.size() != k * k) {
    return invalid("weights, groups, exposures, covariance and specific variances differ in size");
  }
  for (usize i = 0U; i < n; ++i) {
    if (m.group[i] > m.groups) {
      return invalid("a group column above the model's groups");
    }
    if (!std::isfinite(w[i]) || !std::isfinite(m.specific[i]) || m.specific[i] < 0.0) {
      return invalid("a non-finite weight or a non-finite or negative specific variance");
    }
  }
  for (const f64 x : m.exposures) {
    if (!std::isfinite(x)) {
      return invalid("a non-finite style exposure");
    }
  }
  for (const f64 x : m.covariance) {
    if (!std::isfinite(x)) {
      return invalid("a non-finite factor covariance entry");
    }
  }
  return atx::core::Ok();
}

} // namespace

atx::core::Status validate_risk_target(const RiskTargetParams &p) {
  if (!finite_positive(p.sigma_star) || p.sigma_star > 1.0) {
    return invalid("sigma_star (--risk-target) must be finite in (0, 1]");
  }
  if (!finite_positive(p.bias) || p.bias > 10.0) {
    return invalid("the bias b (--risk-target-bias) must be finite in (0, 10]");
  }
  if (p.cadence < 1U || p.cadence > 10000U) {
    return invalid("the cadence (--risk-target-cadence) must be in [1, 10000] sessions");
  }
  return atx::core::Ok();
}

atx::core::Result<f64> factor_variance(const FactorRiskView &m, std::span<const f64> w) {
  ATX_TRY_VOID(check_model(m, w));
  const usize k = m.factors();
  const usize style_column = 1U + m.groups;
  std::vector<f64> e(k, 0.0); // B'w
  f64 specific = 0.0;
  for (usize i = 0U; i < w.size(); ++i) {
    const f64 wi = w[i];
    if (wi == 0.0) {
      continue;
    }
    e[0] += wi;
    if (m.group[i] != 0U) {
      e[m.group[i]] += wi;
    }
    const f64 *row = m.exposures.data() + i * m.styles;
    for (usize c = 0U; c < m.styles; ++c) {
      e[style_column + c] += row[c] * wi;
    }
    specific += m.specific[i] * wi * wi;
  }
  f64 factor = 0.0;
  for (usize a = 0U; a < k; ++a) {
    f64 row = 0.0;
    for (usize b = 0U; b < k; ++b) {
      row += m.covariance[a * k + b] * e[b];
    }
    factor += e[a] * row;
  }
  return atx::core::Ok(factor + specific);
}

atx::core::Result<f64> gross_one_vol(const FactorRiskView &m, std::span<const f64> w, f64 gross,
                                     f64 periods_per_year) {
  if (!finite_positive(periods_per_year) || !std::isfinite(gross) || gross < 0.0) {
    return invalid("gross and the periods per year must be finite, gross >= 0 and P > 0");
  }
  if (gross == 0.0) {
    return atx::core::Err(atx::core::ErrorCode::Unavailable,
                          "risk-target-v1: a flat book (gross 0) has no ex-ante volatility");
  }
  f64 priced = 0.0;
  for (const f64 x : w) {
    priced += std::abs(x);
  }
  if (!(priced <= gross * (1.0 + 1e-12))) { // NaN weights fall here too
    return invalid("gross is below the priced names' sum of |w|");
  }
  std::vector<f64> unit(w.size());
  for (usize i = 0U; i < w.size(); ++i) {
    unit[i] = w[i] / gross;
  }
  ATX_TRY(const f64 variance, factor_variance(m, unit));
  if (!(variance > 0.0)) {
    return atx::core::Err(atx::core::ErrorCode::Unavailable,
                          "risk-target-v1: the gross-1 book has no positive ex-ante variance");
  }
  return atx::core::Ok(std::sqrt(periods_per_year * variance));
}

RiskTargetLeverage risk_target_leverage(const RiskTargetParams &p, f64 sigma_hat,
                                        f64 base) noexcept {
  RiskTargetLeverage out;
  out.raw = p.sigma_star / (p.bias * sigma_hat);
  const f64 lo = risk_target_clip_lo * base;
  const f64 hi = risk_target_clip_hi * base;
  if (out.raw < lo) {
    out.leverage = lo;
    out.clip = RiskTargetClip::Low;
  } else if (out.raw > hi) {
    out.leverage = hi;
    out.clip = RiskTargetClip::High;
  } else {
    out.leverage = out.raw;
    out.clip = RiskTargetClip::None;
  }
  return out;
}

bool risk_target_due(const RiskTargetState &s, usize session, usize cadence) noexcept {
  return !s.estimated || (session >= s.last && session - s.last >= cadence);
}

f64 risk_target_in_force(const RiskTargetState &s, f64 base) noexcept {
  return s.estimated ? s.at.leverage : base;
}

atx::core::Result<bool> risk_target_update(const RiskTargetParams &p, f64 base, usize session,
                                           const FactorRiskView &m, std::span<const f64> w,
                                           f64 gross, RiskTargetState &s) {
  ATX_TRY_VOID(validate_risk_target(p));
  if (!finite_positive(base)) {
    return invalid("the base leverage L must be finite and > 0");
  }
  if (!risk_target_due(s, session, p.cadence)) {
    return atx::core::Ok(false);
  }
  auto sigma = gross_one_vol(m, w, gross);
  if (!sigma) {
    if (sigma.error().code() == atx::core::ErrorCode::Unavailable) {
      return atx::core::Ok(false); // a flat book or no positive variance: nothing to scale
    }
    return atx::core::Err(std::move(sigma).error());
  }
  s.estimated = true;
  s.last = session;
  s.sigma_hat = *sigma;
  s.at = risk_target_leverage(p, *sigma, base);
  return atx::core::Ok(true);
}

} // namespace atx::engine::book

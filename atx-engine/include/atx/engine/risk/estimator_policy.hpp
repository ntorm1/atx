#pragma once

#include <string>
#include <string_view>

#include "atx/core/types.hpp"

namespace atx::engine::risk {

enum class RiskEstimatorRule : atx::u8 { LegacyV1 = 1, EffectiveHistoryV2 = 2 };
enum class MissingPriorForecastRule : atx::u8 {
  RequireObservedV1 = 1,
  LeaveUnadjustedUnverifiedV2 = 2
};

// Explicit V2 recipe, in trading-session units. Older overloads/configurations
// retain their original defaults. These are model assumptions, not estimates of
// empirical calibration quality. No field authorizes a future observation.
struct RiskEstimatorPolicy {
  RiskEstimatorRule rule{RiskEstimatorRule::LegacyV1};
  atx::usize vol_halflife{84};
  atx::usize correlation_halflife{504};
  atx::usize factor_nw_lags{2};
  atx::usize specific_halflife{84};
  atx::usize specific_nw_lags{5};
  atx::usize nw_halflife{252};
  atx::usize vra_halflife{42};
  atx::usize eigen_simulations{64};
  atx::f64 eigen_amplification{1.4};
  atx::u64 eigen_seed{7};
  atx::usize structural_min_observations{21};
  atx::f64 bayesian_q{0.1};
  atx::f64 variance_floor{1e-12};
  MissingPriorForecastRule missing_prior{MissingPriorForecastRule::LeaveUnadjustedUnverifiedV2};
  atx::u64 max_working_bytes{268'435'456};
};

[[nodiscard]] inline RiskEstimatorPolicy effective_history_v2(bool long_horizon = false) {
  RiskEstimatorPolicy out;
  out.rule = RiskEstimatorRule::EffectiveHistoryV2;
  if (long_horizon) {
    out.vol_halflife = 252;
    out.specific_halflife = 252;
    out.vra_halflife = 168;
  }
  return out;
}

// Canonical algorithm/config identity. Model outputs bind this together with
// source/axis identities; it is not a claim about observed data provenance.
[[nodiscard]] std::string risk_estimator_recipe(const RiskEstimatorPolicy& policy);

} // namespace atx::engine::risk

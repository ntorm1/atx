#pragma once

#include <limits>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::cost {

enum class FimRule : atx::u8 { DisabledV1 = 1, Table7Global5AnchoredV2 = 2,
                              Table7Us10AnchoredV2 = 3 };
struct FimRecipe {
  FimRule rule{FimRule::DisabledV1};
  atx::f64 reference_market_cap_usd{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 reference_idio_annual_fraction{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 participation_anchor{std::numeric_limits<atx::f64>::quiet_NaN()};
};
struct FimPredictors {
  bool available{false};
  atx::i64 available_at_ns{};
  atx::f64 market_cap_usd{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 idio_annual_fraction{std::numeric_limits<atx::f64>::quiet_NaN()};
};
struct FimAdjustment {
  bool available{false};
  atx::f64 impact_multiplier{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 differential_bps{std::numeric_limits<atx::f64>::quiet_NaN()};
  bool floored_at_zero{false};
  atx::i64 available_at_ns{};
};
// FIM Aug-2018 Table VII: ln(1+ME in billion USD), idio vol in annualized percent.
// Apply only the DIFFERENCE from explicit reference characteristics to Y*sigma
// at the caller's anchor; retain sqrt participation away from that anchor. This
// nonnegative convex adaptation is NOT the complete empirical FIM regression.
[[nodiscard]] atx::core::Result<FimAdjustment> fim_adjustment(const FimRecipe&,
    const FimPredictors&, atx::i64 decision_time_ns, atx::f64 impact_y, atx::f64 daily_vol);
// Reference algebra only. other_controls_bps MUST explicitly supply the omitted
// intercept, fixed effects, trend, VIX and contemporaneous-return controls. No
// default, no pretrade use of contemporaneous returns, no inferred sample mean.
[[nodiscard]] atx::core::Result<atx::f64> fim_reference_impact_bps(FimRule,
    atx::f64 participation_fraction, atx::f64 market_cap_usd,
    atx::f64 idio_annual_fraction, atx::f64 other_controls_bps);

} // namespace atx::engine::cost

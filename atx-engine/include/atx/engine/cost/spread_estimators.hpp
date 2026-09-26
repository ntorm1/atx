#pragma once

#include <limits>
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::cost {

enum class SpreadRule : atx::u8 { BidaskUnsignedCompleteV1 = 1 };
enum class SpreadState : atx::u8 { Available = 1, Unavailable = 2 };
struct SpreadRecipe {
  SpreadRule rule{SpreadRule::BidaskUnsignedCompleteV1};
  atx::f64 scale{1.0}; // explicit bias/sensitivity knob; no empirical calibration implied
  atx::usize min_observations{3U};
  atx::usize max_observations{4096U};
};
struct SpreadOhlc {
  atx::i64 session{}; // consecutive exchange-session ordinals, NOT calendar-day numbers
  atx::i64 available_at_ns{}; // includes the close; strictly before decision
  SpreadState state{SpreadState::Unavailable};
  atx::f64 open{}, high{}, low{}, close{}; // same raw price basis within each row
};
struct SpreadEstimate {
  SpreadState state{SpreadState::Unavailable};
  atx::f64 corwin_schultz{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 abdi_ranaldo{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 edge{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::f64 full_spread{std::numeric_limits<atx::f64>::quiet_NaN()};
  atx::i64 available_at_ns{};
};
// Full spread fractions, e.g. .01 == 1%; CostSurface charges one half per trade.
// bidask 1caba55d: abs(mean(CS)), sqrt(abs(mean(AR squared spread))), unsigned EDGE.
// Every adjacent row is retained. A missing row/gap/not-yet-public input makes the
// composite unavailable; no omission, imputation, or partial-estimator ensemble.
// Complete windows match bidask's unsigned CS/AR/edge recipes, not CS2/AR2.
// O(window) time, O(1) scratch; immutable input, no allocation on success.
[[nodiscard]] atx::core::Result<SpreadEstimate> estimate_spread(
    std::span<const SpreadOhlc> history, atx::i64 decision_time_ns,
    const SpreadRecipe& recipe = {});

} // namespace atx::engine::cost

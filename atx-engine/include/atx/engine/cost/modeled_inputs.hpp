#pragma once

#include <span>

#include "atx/engine/cost/borrow_tiers.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include "atx/engine/cost/fim_adjustment.hpp"
#include "atx/engine/cost/spread_estimators.hpp"

namespace atx::engine::cost {

struct ModeledCostRecipe {
  CostSurfaceRecipe surface{CostSurfaceRule::ModeledInputsV2};
  SpreadRecipe spread;
  FimRecipe fim;
  BorrowTierRecipe borrow;
};
struct ModeledCostInput {
  atx::u64 instrument_id{};
  CostInputState liquidity_state{CostInputState::Unavailable};
  atx::i64 liquidity_available_at_ns{};
  atx::f64 adv_dollars{}; // caller's explicit trailing raw-dollar volume recipe
  atx::f64 daily_vol{}; // caller's explicit trailing return recipe
  std::span<const SpreadOhlc> history; // borrowed only until make_modeled_cost_surface returns
  FimPredictors fim;
  BorrowPredictors borrow;
};
// Explicit opt-in. surface.rule MUST be ModeledInputsV2 and surface.spread_scale
// MUST equal one: spread.scale is the only composite scaling knob. Automatically
// hash all active model knobs and the upstream version into the surface identity.
// source_sha256 binds caller-supplied vintage prices/public predictors; this API
// does not acquire them or verify an assertion of source authenticity. Missing
// trade inputs block nonzero trade quotes; missing borrow blocks only borrow.
[[nodiscard]] atx::core::Result<CostSurface> make_modeled_cost_surface(
    const ModeledCostRecipe&, const CostSurfaceIdentity&, std::span<const ModeledCostInput>,
    atx::u64 max_working_bytes = atx::u64{64} * 1024U * 1024U);

} // namespace atx::engine::cost

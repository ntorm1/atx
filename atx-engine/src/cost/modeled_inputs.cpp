#include "atx/engine/cost/modeled_inputs.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/sha256.hpp"

namespace atx::engine::cost {
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
void word(std::string& out, atx::u64 x) {
  for (atx::usize i = 0U; i < 8U; ++i) {
    out.push_back(static_cast<char>(x & 0xffU)); x >>= 8U;
  }
}
void number(std::string& out, atx::f64 x) {
  word(out, std::bit_cast<atx::u64>(x == 0.0 ? 0.0 : x));
}
Result<std::string> model_hash(const ModeledCostRecipe& r) {
  std::string body = "cost-inputs-v2/bidask-1caba55d63ebab6c855536be51c43bdfc48d2dec";
  word(body, static_cast<atx::u64>(r.spread.rule)); number(body, r.spread.scale);
  word(body, static_cast<atx::u64>(r.spread.min_observations));
  word(body, static_cast<atx::u64>(r.spread.max_observations));
  word(body, static_cast<atx::u64>(r.fim.rule));
  if (r.fim.rule != FimRule::DisabledV1) {
    number(body, r.fim.reference_market_cap_usd);
    number(body, r.fim.reference_idio_annual_fraction); number(body, r.fim.participation_anchor);
  }
  word(body, static_cast<atx::u64>(r.borrow.rule));
  number(body, r.borrow.small_cap_usd); number(body, r.borrow.low_raw_price_usd);
  number(body, r.borrow.high_short_interest_fraction);
  number(body, r.borrow.young_ipo_calendar_days);
  number(body, r.borrow.gc_annual_fraction); number(body, r.borrow.warm_annual_fraction);
  number(body, r.borrow.special_annual_fraction);
  return atx::core::sha256_hex(body);
}
Result<CostSurfaceRow> make_row(const ModeledCostInput& p, atx::i64 decision,
                               const ModeledCostRecipe& r) {
  CostSurfaceRow row;
  row.instrument_id = p.instrument_id;
  ATX_TRY(auto borrow, estimate_borrow_tier(p.borrow, decision, r.borrow));
  if (borrow.tier != BorrowTier::Unavailable) {
    row.borrow_state = CostInputState::Available;
    row.borrow_available_at_ns = borrow.available_at_ns;
    row.borrow_annual_fraction = borrow.annual_fraction;
  }
  if (p.liquidity_state == CostInputState::Unavailable) return Ok(row);
  if (p.liquidity_state != CostInputState::Available)
    return Err(ErrorCode::InvalidArgument, "modeled cost: invalid liquidity state");
  if (p.liquidity_available_at_ns <= 0 || p.liquidity_available_at_ns >= decision) return Ok(row);
  if (!std::isfinite(p.adv_dollars) || p.adv_dollars <= 0.0 ||
      !std::isfinite(p.daily_vol) || p.daily_vol < 0.0)
    return Err(ErrorCode::InvalidArgument, "modeled cost: malformed available liquidity");
  ATX_TRY(auto spread, estimate_spread(p.history, decision, r.spread));
  ATX_TRY(auto fim, fim_adjustment(r.fim, p.fim, decision, r.surface.impact_y, p.daily_vol));
  if (spread.state != SpreadState::Available || !fim.available) return Ok(row);
  row.state = CostInputState::Available;
  row.available_at_ns = std::max({p.liquidity_available_at_ns,
      spread.available_at_ns, fim.available_at_ns});
  row.adv_dollars = p.adv_dollars; row.daily_vol = p.daily_vol;
  row.full_spread = spread.full_spread; row.impact_multiplier = fim.impact_multiplier;
  return Ok(row);
}
} // namespace

Result<CostSurface> make_modeled_cost_surface(const ModeledCostRecipe& r,
    const CostSurfaceIdentity& identity, std::span<const ModeledCostInput> input, atx::u64 budget) {
  if (r.surface.rule != CostSurfaceRule::ModeledInputsV2 || r.surface.spread_scale != 1.0)
    return Err(ErrorCode::InvalidArgument,
               "modeled cost: explicit V2 and one spread scale required");
  if (identity.source_sha256.size() != 64U || identity.liquidity_recipe.empty() ||
      identity.calibration_identity.empty() || identity.liquidity_recipe.size() > 3900U ||
      identity.calibration_identity.size() > 3900U)
    return Err(ErrorCode::InvalidArgument, "modeled cost: missing or oversized source identity");
  // Temporary output rows overlap CostSurface's own row/hash/ID allocations.
  constexpr atx::u64 fixed = 65536U;
  constexpr atx::u64 per_name = 2U * sizeof(CostSurfaceRow) + sizeof(atx::u64) + 160U;
  if (input.empty() || budget < fixed || input.size() > (budget - fixed) / per_name ||
      input.size() > (std::numeric_limits<atx::usize>::max() - fixed) / per_name)
    return Err(ErrorCode::InvalidArgument,
               "modeled cost: empty geometry or working budget exceeded");
  // Validate active recipes even if all input rows are explicitly missing.
  ATX_TRY(auto spread_check, estimate_spread({}, identity.decision_time_ns, r.spread));
  ATX_TRY(auto borrow_check, estimate_borrow_tier({}, identity.decision_time_ns, r.borrow));
  ATX_TRY(auto fim_check, fim_adjustment(r.fim, {}, identity.decision_time_ns,
                                        r.surface.impact_y, 0.0));
  (void)spread_check; (void)borrow_check; (void)fim_check;
  ATX_TRY(auto hash, model_hash(r));
  auto bound = identity;
  bound.liquidity_recipe += "/modeled-inputs-v2/" + hash;
  bound.calibration_identity += "/FIM-differential-anchor/borrow-public-prior/" + hash;
  std::vector<CostSurfaceRow> rows(input.size());
  for (atx::usize i = 0U; i < input.size(); ++i) {
    ATX_TRY(rows[i], make_row(input[i], identity.decision_time_ns, r));
  }
  return CostSurface::create(r.surface, bound, rows,
      budget - static_cast<atx::u64>(rows.size()) * sizeof(CostSurfaceRow) - 32768U);
}
} // namespace atx::engine::cost

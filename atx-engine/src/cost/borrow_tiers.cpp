#include "atx/engine/cost/borrow_tiers.hpp"

#include <cmath>

namespace atx::engine::cost {
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
bool nonnegative(atx::f64 x) noexcept { return std::isfinite(x) && x >= 0.0; }
bool positive(atx::f64 x) noexcept { return std::isfinite(x) && x > 0.0; }
}

atx::core::Result<BorrowTierEstimate> estimate_borrow_tier(const BorrowPredictors& p,
    atx::i64 decision, const BorrowTierRecipe& r) {
  if (decision <= 0 || r.rule != BorrowTierRule::PublicPredictorPriorV1 ||
      !positive(r.small_cap_usd) || !positive(r.low_raw_price_usd) ||
      !positive(r.high_short_interest_fraction) || !positive(r.young_ipo_calendar_days) ||
      !nonnegative(r.gc_annual_fraction) || !nonnegative(r.warm_annual_fraction) ||
      !nonnegative(r.special_annual_fraction) || r.gc_annual_fraction > r.warm_annual_fraction ||
      r.warm_annual_fraction > r.special_annual_fraction)
    return Err(ErrorCode::InvalidArgument, "borrow tiers: invalid scenario recipe or decision");
  BorrowTierEstimate out;
  if (!p.available || p.available_at_ns <= 0 || p.available_at_ns >= decision) return Ok(out);
  if (!positive(p.market_cap_usd) || !positive(p.raw_price_usd) ||
      !nonnegative(p.short_interest_to_float) || !nonnegative(p.ipo_age_calendar_days))
    return Err(ErrorCode::InvalidArgument, "borrow tiers: malformed available public predictors");
  const unsigned int flags = static_cast<unsigned int>(p.market_cap_usd < r.small_cap_usd) +
      static_cast<unsigned int>(p.raw_price_usd < r.low_raw_price_usd) +
      static_cast<unsigned int>(p.short_interest_to_float >= r.high_short_interest_fraction) +
      static_cast<unsigned int>(p.ipo_age_calendar_days < r.young_ipo_calendar_days);
  out.risk_flags = static_cast<atx::u8>(flags);
  out.tier = flags == 0U ? BorrowTier::GeneralCollateral :
      (flags == 1U ? BorrowTier::Warm : BorrowTier::Special);
  out.annual_fraction = flags == 0U ? r.gc_annual_fraction :
      (flags == 1U ? r.warm_annual_fraction : r.special_annual_fraction);
  out.available_at_ns = p.available_at_ns;
  return Ok(out);
}

atx::core::Result<atx::f64> annual_fraction_to_bps(atx::f64 x) {
  if (!nonnegative(x) || !std::isfinite(x * 1e4))
    return Err(ErrorCode::InvalidArgument, "borrow units: invalid annual fraction");
  return Ok(x * 1e4);
}
atx::core::Result<atx::f64> annual_bps_to_fraction(atx::f64 x) {
  if (!nonnegative(x)) return Err(ErrorCode::InvalidArgument, "borrow units: invalid annual bps");
  const auto rate = x * 1e-4;
  if (x > 0.0 && rate == 0.0)
    return Err(ErrorCode::InvalidArgument, "borrow units: positive annual rate underflow");
  return Ok(rate);
}
atx::core::Result<BorrowModel> as_borrow_model(const BorrowTierEstimate& e, DayCount day_count) {
  if (e.tier != BorrowTier::GeneralCollateral && e.tier != BorrowTier::Warm &&
      e.tier != BorrowTier::Special)
    return Err(ErrorCode::Unavailable, "borrow tiers: no modeled annual rate");
  if (!nonnegative(e.annual_fraction) ||
      (day_count != DayCount::D360 && day_count != DayCount::D365 && day_count != DayCount::D252))
    return Err(ErrorCode::InvalidArgument, "borrow tiers: invalid rate or day-count rule");
  return Ok(BorrowModel{e.annual_fraction, day_count});
}
} // namespace atx::engine::cost

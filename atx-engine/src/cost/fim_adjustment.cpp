#include "atx/engine/cost/fim_adjustment.hpp"

#include <algorithm>
#include <cmath>

namespace atx::engine::cost {
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
bool nonnegative(atx::f64 x) noexcept { return std::isfinite(x) && x >= 0.0; }
bool positive(atx::f64 x) noexcept { return std::isfinite(x) && x > 0.0; }
bool active(FimRule rule) noexcept {
  return rule == FimRule::Table7Global5AnchoredV2 || rule == FimRule::Table7Us10AnchoredV2;
}
struct Coefficients { atx::f64 size, idio, participation, root_participation; };
Coefficients coefficients(FimRule rule) noexcept {
  return rule == FimRule::Table7Global5AnchoredV2 ?
      Coefficients{-0.62, 0.28, -0.13, 8.89} : Coefficients{-0.14, 0.31, -0.53, 11.21};
}
} // namespace

atx::core::Result<FimAdjustment> fim_adjustment(const FimRecipe& r, const FimPredictors& p,
    atx::i64 decision, atx::f64 y, atx::f64 daily_vol) {
  if (decision <= 0 || !nonnegative(y) || !nonnegative(daily_vol))
    return Err(ErrorCode::InvalidArgument, "FIM: invalid decision or square-root prior");
  if (r.rule == FimRule::DisabledV1) return Ok(FimAdjustment{true, 1.0, 0.0, false, 0});
  if (!active(r.rule) || !positive(r.reference_market_cap_usd) ||
      !nonnegative(r.reference_idio_annual_fraction) || !positive(r.participation_anchor) ||
      r.participation_anchor > 1.0)
    return Err(ErrorCode::InvalidArgument, "FIM: invalid explicit reference scenario");
  if (!p.available || p.available_at_ns <= 0 || p.available_at_ns >= decision)
    return Ok(FimAdjustment{});
  if (!positive(p.market_cap_usd) || !nonnegative(p.idio_annual_fraction))
    return Err(ErrorCode::InvalidArgument, "FIM: malformed available predictors");
  const auto c = coefficients(r.rule);
  const auto difference = c.size * (std::log1p(p.market_cap_usd / 1e9) -
      std::log1p(r.reference_market_cap_usd / 1e9)) +
      c.idio * ((p.idio_annual_fraction - r.reference_idio_annual_fraction) * 100.0);
  const auto base = ((y * daily_vol) * std::sqrt(r.participation_anchor)) * 1e4;
  if (!std::isfinite(difference) || !nonnegative(base))
    return Err(ErrorCode::InvalidArgument, "FIM: nonfinite anchor calculation");
  if (base == 0.0 && difference != 0.0) return Ok(FimAdjustment{});
  const auto raw = base == 0.0 ? 1.0 : 1.0 + difference / base;
  if (!std::isfinite(raw)) return Err(ErrorCode::InvalidArgument, "FIM: multiplier overflow");
  return Ok(FimAdjustment{true, std::max(0.0, raw), difference, raw < 0.0, p.available_at_ns});
}

atx::core::Result<atx::f64> fim_reference_impact_bps(FimRule rule, atx::f64 participation,
    atx::f64 market_cap, atx::f64 idio, atx::f64 other_controls) {
  if (!active(rule) || !nonnegative(participation) || participation > 1.0 ||
      !positive(market_cap) || !nonnegative(idio) || !std::isfinite(other_controls))
    return Err(ErrorCode::InvalidArgument,
               "FIM reference: invalid units, controls or table column");
  const auto c = coefficients(rule);
  const auto percent = participation * 100.0;
  const auto bps = other_controls + c.size * std::log1p(market_cap / 1e9) +
      c.idio * (idio * 100.0) + c.participation * percent +
      c.root_participation * std::sqrt(percent);
  if (!std::isfinite(bps)) return Err(ErrorCode::InvalidArgument, "FIM reference: overflow");
  return Ok(bps); // signed regression prediction; not a nonnegative trade-cost quote
}
} // namespace atx::engine::cost

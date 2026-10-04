// atx::engine::book -- per-name replay trade cost models.
#include "atx/engine/book/replay_cost.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace atx::engine::book {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

constexpr atx::f64 kBpsToFraction = 1.0e-4;
constexpr atx::f64 kMaxImpactExponent = 2.0;

bool finite_nonnegative(atx::f64 value) noexcept { return std::isfinite(value) && value >= 0.0; }

// participation^delta. The registered square-root law (delta == .5) takes std::sqrt, which IEEE
// 754 rounds correctly in every build and C runtime, so the modeled cost is the same bits in a
// Debug and a Release build (P9 C1, DEC-11: std::pow is not correctly rounded and the Debug and
// Release C runtimes may differ in its last bit). Any other exponent keeps std::pow.
atx::f64 participation_power(atx::f64 participation, atx::f64 delta) noexcept {
  return delta == 0.5 ? std::sqrt(participation) : std::pow(participation, delta);
}

} // namespace

SurfaceReplayCost::SurfaceReplayCost(atx::engine::cost::CostSurface surface,
    atx::usize execution_period, atx::i64 execution_time_ns) noexcept
    : surface_{std::move(surface)}, period_{execution_period}, execution_time_ns_{execution_time_ns} {}

Result<SurfaceReplayCost> SurfaceReplayCost::create(atx::engine::cost::CostSurface surface,
    atx::usize execution_period, atx::i64 execution_time_ns) {
  if (surface.instruments() == 0U || execution_time_ns < surface.decision_time_ns())
    return Err(ErrorCode::InvalidArgument, "surface replay: invalid surface or execution precedes decision");
  return Ok(SurfaceReplayCost{std::move(surface), execution_period, execution_time_ns});
}

TradeCost SurfaceReplayCost::cost(atx::usize instrument, atx::usize period,
    atx::f64 trade_dollars, const LiquidityRow& /*liquidity*/) const noexcept {
  if (period != period_) return {};
  const auto quote = surface_.quote_dollars(instrument, surface_.decision_time_ns(), trade_dollars,
                                           atx::engine::cost::CostFillRule::ParticipationCapped);
  return quote.priced() ? TradeCost{quote.filled_dollars, quote.total_dollars} : TradeCost{};
}

atx::f64 SurfaceReplayCost::unrationed_cost(atx::usize instrument, atx::usize period,
    atx::f64 trade_dollars, const LiquidityRow& /*liquidity*/) const noexcept {
  if (period != period_) return std::numeric_limits<atx::f64>::quiet_NaN();
  const auto quote = surface_.quote_dollars(instrument, surface_.decision_time_ns(), trade_dollars,
                                           atx::engine::cost::CostFillRule::FullRequest);
  return quote.priced() ? quote.total_dollars : std::numeric_limits<atx::f64>::quiet_NaN();
}

FlatBpsCost::FlatBpsCost(atx::f64 bps) noexcept : bps_{bps}, rate_{bps * kBpsToFraction} {}

Result<FlatBpsCost> FlatBpsCost::create(atx::f64 bps) {
  if (!finite_nonnegative(bps)) {
    return Err(ErrorCode::InvalidArgument, "replay cost: flat bps must be finite and >= 0");
  }
  return Ok(FlatBpsCost{bps});
}

TradeCost FlatBpsCost::cost(atx::usize /*instrument*/, atx::usize /*period*/,
                            atx::f64 trade_dollars,
                            const LiquidityRow & /*liquidity*/) const noexcept {
  return TradeCost{trade_dollars, std::abs(trade_dollars) * rate_};
}

SqrtImpactCost::SqrtImpactCost(ReplayImpactCfg cfg, atx::f64 max_participation,
                               atx::f64 commission_bps) noexcept
    : cfg_{cfg}, max_participation_{max_participation}, commission_bps_{commission_bps} {}

Result<SqrtImpactCost> SqrtImpactCost::create(ReplayImpactCfg cfg, atx::f64 max_participation,
                                              atx::f64 commission_bps) {
  if (!finite_nonnegative(cfg.y) || !std::isfinite(cfg.delta) || cfg.delta <= 0.0 ||
      cfg.delta > kMaxImpactExponent || !finite_nonnegative(commission_bps)) {
    return Err(ErrorCode::InvalidArgument,
               "replay cost: sqrt impact needs y >= 0, 0 < delta <= 2, commission >= 0");
  }
  if (std::isnan(max_participation) || max_participation <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "replay cost: max participation must be in (0, inf]");
  }
  return Ok(SqrtImpactCost{cfg, max_participation, commission_bps});
}

atx::f64 SqrtImpactCost::cost_fraction(atx::f64 abs_dollars,
                                       const LiquidityRow &liquidity) const noexcept {
  const auto adv = liquidity.adv_dollars;
  if (!std::isfinite(adv) || adv <= 0.0 || !finite_nonnegative(liquidity.daily_vol) ||
      !finite_nonnegative(liquidity.half_spread_bps) || !finite_nonnegative(abs_dollars)) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  const auto participation = abs_dollars / adv;
  const auto impact =
      cfg_.y * liquidity.daily_vol * participation_power(participation, cfg_.delta);
  return (liquidity.half_spread_bps + commission_bps_) * kBpsToFraction + impact;
}

TradeCost SqrtImpactCost::cost(atx::usize /*instrument*/, atx::usize /*period*/,
                               atx::f64 trade_dollars,
                               const LiquidityRow &liquidity) const noexcept {
  const auto requested = std::abs(trade_dollars);
  if (!std::isfinite(requested) || !std::isfinite(cost_fraction(0.0, liquidity))) {
    return TradeCost{0.0, 0.0}; // No usable liquidity estimate: no fill, no charge.
  }
  auto fill = requested;
  if (std::isfinite(max_participation_)) {
    fill = std::min(fill, max_participation_ * liquidity.adv_dollars);
  }
  const auto fraction = cost_fraction(fill, liquidity);
  if (!std::isfinite(fraction)) return TradeCost{0.0, 0.0};
  const auto signed_fill = fill == requested ? trade_dollars : std::copysign(fill, trade_dollars);
  return TradeCost{signed_fill, fill * fraction};
}

atx::f64 SqrtImpactCost::unrationed_cost(atx::usize /*instrument*/, atx::usize /*period*/,
                                         atx::f64 trade_dollars,
                                         const LiquidityRow &liquidity) const noexcept {
  const auto requested = std::abs(trade_dollars);
  if (requested == 0.0) return 0.0;
  return requested * cost_fraction(requested, liquidity); // NaN propagates.
}

} // namespace atx::engine::book

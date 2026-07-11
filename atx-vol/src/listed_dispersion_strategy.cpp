#include "atx/vol/listed_dispersion_strategy.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/portfolio_pricer.hpp" // kNsPerYear
#include "atx/vol/priced_surface.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

Result<std::vector<Lot>> materialize_listed_dispersion_roll(const ListedScheduleRoll &roll,
                                                            std::int64_t valuation_ts_ns,
                                                            std::uint64_t first_lot_id) {
  ListedDispersionSchedule one;
  one.rolls.push_back(roll);
  ATX_TRY_VOID(validate_listed_dispersion_schedule(one));
  if (valuation_ts_ns != roll.valuation_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "materialize_listed_dispersion_roll: valuation timestamp mismatch");
  }

  const double T = static_cast<double>(roll.expiry_ts_ns - valuation_ts_ns) / kNsPerYear;
  if (!std::isfinite(T) || T <= 0.0) {
    return Err(ErrorCode::InvalidArgument, "materialize_listed_dispersion_roll: roll has expired");
  }

  std::vector<Lot> lots;
  lots.reserve(roll.legs.size());
  for (std::size_t i = 0; i < roll.legs.size(); ++i) {
    const ListedScheduleLeg &leg = roll.legs[i];
    Lot lot;
    lot.id = first_lot_id + static_cast<std::uint64_t>(i);
    lot.contract = OptionContract{leg.uid, leg.strike, T, leg.side};
    lot.qty = leg.quantity;
    lot.multiplier = leg.multiplier;
    lot.expiry_ts_ns = leg.expiry_ts_ns;
    lot.cohort = roll.cohort;
    lot.entry_price = leg.model_mark;
    lots.push_back(lot);
  }
  return Ok(std::move(lots));
}

Result<ListedDispersionStrategy> ListedDispersionStrategy::create(ListedDispersionSchedule schedule,
                                                                  double delta_band) {
  if (!std::isfinite(delta_band) || delta_band < 0.0) {
    return Err(ErrorCode::InvalidArgument, "ListedDispersionStrategy::create: invalid delta band");
  }
  ATX_TRY_VOID(validate_listed_dispersion_schedule(schedule));
  HedgeSpec hedge;
  hedge.kind = HedgeSpec::Kind::DeltaToZero;
  hedge.cadence = HedgeSpec::Cadence::Daily;
  hedge.band = delta_band;
  return Ok(ListedDispersionStrategy{std::move(schedule), hedge});
}

Status ListedDispersionStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                         PortfolioState &book, std::uint64_t &next_lot_id) {
  (void)step_index;
  if (next_roll_ == schedule_.rolls.size()) {
    return Ok();
  }
  const ListedScheduleRoll &roll = schedule_.rolls[next_roll_];
  if (base.ts_ns() < roll.valuation_ts_ns) {
    return Ok();
  }
  if (base.ts_ns() > roll.valuation_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "ListedDispersionStrategy: scheduled roll date was not observed");
  }

  // Validate the complete replacement against the loaded archive before
  // mutating the book, id counter, or schedule cursor.
  for (const ListedScheduleLeg &leg : roll.legs) {
    const PricedSurface *surface = base.find(leg.uid);
    if (surface == nullptr) {
      return Err(ErrorCode::NotFound, "ListedDispersionStrategy: scheduled surface is unavailable");
    }
    const double T = static_cast<double>(leg.expiry_ts_ns - base.ts_ns()) / kNsPerYear;
    ATX_TRY(double mark, surface->fair_value(leg.strike, T, leg.side));
    if (mark != leg.model_mark) {
      return Err(ErrorCode::Unavailable,
                 "ListedDispersionStrategy: archive mark differs from schedule");
    }
  }
  ATX_TRY(auto replacement, materialize_listed_dispersion_roll(roll, base.ts_ns(), next_lot_id));

  book.lots = std::move(replacement);
  next_lot_id += static_cast<std::uint64_t>(roll.legs.size());
  ++next_roll_;
  return Ok();
}

} // namespace atx::vol

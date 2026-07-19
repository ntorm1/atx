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

namespace {

[[nodiscard]] Result<double> residual_T(std::int64_t expiry_ts_ns, std::int64_t valuation_ts_ns) {
  if (expiry_ts_ns <= valuation_ts_ns) {
    return Err(ErrorCode::InvalidArgument, "materialize_listed_dispersion_roll: roll has expired");
  }
  const std::uint64_t delta_ns =
      static_cast<std::uint64_t>(expiry_ts_ns) - static_cast<std::uint64_t>(valuation_ts_ns);
  const double T = static_cast<double>(delta_ns) / kNsPerYear;
  if (!std::isfinite(T) || T <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "materialize_listed_dispersion_roll: invalid residual tenor");
  }
  return Ok(T);
}

} // namespace

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

  ATX_TRY(const double T, residual_T(roll.expiry_ts_ns, valuation_ts_ns));

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
  return on_step(base, step_index, book, next_lot_id, PriceOptions{});
}

Status ListedDispersionStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                         PortfolioState &book, std::uint64_t &next_lot_id,
                                         const PriceOptions &price_options) {
  (void)step_index;
  last_entry_seeds_.clear();
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
  std::vector<FullGreekSeed> seeds;
  seeds.reserve(roll.legs.size());
  for (const ListedScheduleLeg &leg : roll.legs) {
    const PricedSurface *surface = base.find(leg.uid);
    if (surface == nullptr) {
      return Err(ErrorCode::NotFound, "ListedDispersionStrategy: scheduled surface is unavailable");
    }
    ATX_TRY(const double T, residual_T(leg.expiry_ts_ns, base.ts_ns()));
    ATX_TRY(FullGreekSeed seed,
            surface->full_greek_seed(leg.strike, T, leg.side, price_options.analytic_greeks,
                                     price_options.query_execution));
    // Same cross-check, same tolerance, same scale as the reconciliation guard
    // (listed_dispersion_reconciliation.cpp). This was a bit-exact `!=`, which
    // fails CLOSED — Unavailable on the happy path — as soon as the seed route
    // and the build route disagree by even one ULP. NaN still hard-rejects.
    if (!listed_entry_mark_agrees(seed.greeks().price, leg.model_mark, entry_mark_tolerance_)) {
      return Err(ErrorCode::Unavailable,
                 "ListedDispersionStrategy: archive mark differs from schedule");
    }
    seeds.push_back(std::move(seed));
  }
  ATX_TRY(auto replacement, materialize_listed_dispersion_roll(roll, base.ts_ns(), next_lot_id));

  book.lots = std::move(replacement);
  next_lot_id += static_cast<std::uint64_t>(roll.legs.size());
  ++next_roll_;
  last_entry_seeds_ = std::move(seeds);
  return Ok();
}

} // namespace atx::vol

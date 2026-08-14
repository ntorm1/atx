#include "atx/vol/api/backtest/listed_dispersion_strategy.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/backtest/backtest.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // kNsPerYear
#include "atx/vol/api/backtest/priced_surface.hpp"

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

[[nodiscard]] bool valid_fill_policy(ScheduleFillPolicy p) noexcept {
  switch (p) {
  case ScheduleFillPolicy::ModelMark:
  case ScheduleFillPolicy::QuoteMid:
  case ScheduleFillPolicy::CrossSpread:
    return true;
  }
  return false;
}

} // namespace

Result<double> listed_leg_fill_price(const ListedScheduleLeg &leg, ScheduleFillPolicy policy) {
  if (policy == ScheduleFillPolicy::ModelMark) {
    return Ok(leg.model_mark);
  }
  if (!valid_fill_policy(policy)) {
    return Err(ErrorCode::InvalidArgument, "listed_leg_fill_price: invalid fill policy");
  }
  // Quote-side fills need a real, usable two-sided market. A zero bid makes the
  // mid a fiction (ask/2) and there is nobody to sell to at all — fail closed
  // rather than fabricate a fill (the same judgement F6 applies at selection).
  if (!std::isfinite(leg.raw_bid) || !std::isfinite(leg.raw_ask) || leg.raw_bid <= 0.0 ||
      leg.raw_ask < leg.raw_bid) {
    return Err(ErrorCode::NotFound,
               "listed_leg_fill_price: leg uid=" + std::to_string(leg.uid) +
                   " has no usable two-sided quote for a quote-side fill (bid=" +
                   std::to_string(leg.raw_bid) + ", ask=" + std::to_string(leg.raw_ask) + ")");
  }
  if (policy == ScheduleFillPolicy::QuoteMid) {
    return Ok(0.5 * (leg.raw_bid + leg.raw_ask));
  }
  // CrossSpread: a buy lifts the offer, a sell hits the bid. `quantity` carries
  // the direction (positive long, negative short); a zero-quantity leg trades
  // nothing, so either side is economically the same and the ask is arbitrary.
  return Ok(leg.quantity >= 0.0 ? leg.raw_ask : leg.raw_bid);
}

Result<std::vector<Lot>> materialize_listed_dispersion_roll(const ListedScheduleRoll &roll,
                                                            std::int64_t valuation_ts_ns,
                                                            std::uint64_t first_lot_id,
                                                            ScheduleFillPolicy fill) {
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
    ATX_TRY(const double fill_price, listed_leg_fill_price(leg, fill));
    lot.entry_price = fill_price;
    lots.push_back(lot);
  }
  return Ok(std::move(lots));
}

Result<ListedDispersionStrategy> ListedDispersionStrategy::create(ListedDispersionSchedule schedule,
                                                                  double delta_band,
                                                                  ScheduleMarkPolicy policy,
                                                                  ScheduleFillPolicy fill) {
  if (!std::isfinite(delta_band) || delta_band < 0.0) {
    return Err(ErrorCode::InvalidArgument, "ListedDispersionStrategy::create: invalid delta band");
  }
  if (!valid_fill_policy(fill)) {
    return Err(ErrorCode::InvalidArgument, "ListedDispersionStrategy::create: invalid fill policy");
  }
  ATX_TRY_VOID(validate_listed_dispersion_schedule(schedule));
  // F2: a quote-side policy must be satisfiable on EVERY leg of EVERY roll before
  // the run starts. Discovering an unquotable leg 40 dates in is a wasted replay.
  if (fill != ScheduleFillPolicy::ModelMark) {
    for (const ListedScheduleRoll &roll : schedule.rolls) {
      for (const ListedScheduleLeg &leg : roll.legs) {
        ATX_TRY_VOID(listed_leg_fill_price(leg, fill));
      }
    }
  }
  // F5 (BT-T2): the schedule is the COMPLETE set of names this strategy can ever
  // touch, so the engine can subset-deserialize. Sorted + deduped for a
  // deterministic set that does not depend on roll/leg order in the artifact.
  std::vector<std::uint32_t> referenced_uids;
  for (const ListedScheduleRoll &roll : schedule.rolls) {
    for (const ListedScheduleLeg &leg : roll.legs) {
      referenced_uids.push_back(leg.uid);
    }
  }
  std::sort(referenced_uids.begin(), referenced_uids.end());
  referenced_uids.erase(std::unique(referenced_uids.begin(), referenced_uids.end()),
                        referenced_uids.end());

  HedgeSpec hedge;
  hedge.kind = HedgeSpec::Kind::DeltaToZero;
  hedge.cadence = HedgeSpec::Cadence::Daily;
  hedge.band = delta_band;
  return Ok(ListedDispersionStrategy{std::move(schedule), hedge, policy, fill,
                                     std::move(referenced_uids)});
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
  last_mark_divergences_.clear();
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
    const SurfaceRef surface = base.find(leg.uid);
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
      if (policy_ == ScheduleMarkPolicy::ExactArchive) {
        return Err(ErrorCode::Unavailable,
                   "ListedDispersionStrategy: archive mark differs from schedule");
      }
      // Record: accept the live mark, log the divergence instead of failing.
      last_mark_divergences_.push_back(MarkDivergence{leg.uid, leg.strike, leg.expiry_ts_ns,
                                                      leg.side, leg.model_mark,
                                                      seed.greeks().price});
    }
    seeds.push_back(std::move(seed));
  }
  ATX_TRY(auto replacement,
          materialize_listed_dispersion_roll(roll, base.ts_ns(), next_lot_id, fill_));

  // ExactArchive keeps the bit-identical entry_price = leg.model_mark set by
  // materialize. Record reprices under its own route, so pin each entry to the
  // live seed price (schedule order is 1:1 with the seeds and the lots) to keep
  // the replay self-consistent.
  //
  // F2: Record's re-pin is a MARK correction, so it applies only when the fill
  // IS the mark. Under a quote-side policy the fill came from the recorded NBBO
  // and must survive the re-pin untouched.
  if (policy_ == ScheduleMarkPolicy::Record && fill_ == ScheduleFillPolicy::ModelMark) {
    for (std::size_t i = 0; i < replacement.size(); ++i) {
      replacement[i].entry_price = seeds[i].greeks().price;
    }
  }

  book.lots = std::move(replacement);
  next_lot_id += static_cast<std::uint64_t>(roll.legs.size());
  ++next_roll_;
  last_entry_seeds_ = std::move(seeds);
  return Ok();
}

} // namespace atx::vol

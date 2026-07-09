// atx-vol DispersionStrategy (Phase B1) — an IStrategy adapter over the existing
// `build_dispersion_book` (P4-1). It re-expresses the vega-neutral straddle
// dispersion trade as a backtest strategy WITHOUT reimplementing its sizing:
// on entry it calls `build_dispersion_book` (authoritative) and converts the
// emitted `Position`s into engine `Lot`s, and it surfaces the implied-correlation
// diagnostic through the `signals` hook so a backtest records it per row.

#include "atx/vol/strategy.hpp"  // DispersionStrategy, IStrategy, lifecycle_decide

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/backtest.hpp"          // MarketSnapshot, Lot, PortfolioState
#include "atx/vol/dispersion.hpp"        // build_dispersion_book, dispersion_signal
#include "atx/vol/portfolio_pricer.hpp"  // OptionContract, kNsPerYear, Position
#include "atx/vol/priced_surface.hpp"    // PricedSurface
#include "atx/vol/types.hpp"             // Result, Status

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

Status DispersionStrategy::on_step(const MarketSnapshot& base, std::size_t step_index,
                                   PortfolioState& book, std::uint64_t& next_lot_id) {
  const LifecycleDecision d = lifecycle_decide(lifecycle_, step_index, book.lots.empty(),
                                               base.ts_ns(), front_expiry_, have_front_);
  if (!d.open) {
    return Ok();
  }
  if (d.clear) {
    book.lots.clear();
    have_front_ = false;
  }

  // Bind the (possibly symbol-only) universe to THIS snapshot's uid scheme before
  // sizing, so one authored universe works across every date of a corpus.
  Result<DispersionUniverse> ru =
      resolve_universe_uids(universe_, [&](std::string_view s) { return base.uid_of(s); });
  if (!ru) {
    return Err(ru.error());
  }

  Result<DispersionBook> built = build_dispersion_book(*ru, base.set(), cfg_);
  if (!built) {
    return Err(built.error());
  }
  const std::uint32_t cohort = cohort_counter_++;
  const std::int64_t expiry = base.ts_ns() + std::llround(cfg_.target_T * kNsPerYear);
  for (const Position& p : built->positions) {
    const PricedSurface* surf = base.find(p.contract.uid);
    if (surf == nullptr) {
      return Err(ErrorCode::NotFound, "DispersionStrategy: no surface for position");
    }
    const Result<double> mark = surf->fair_value(p.contract.K, p.contract.T, p.contract.side);
    if (!mark) {
      return Err(mark.error());
    }
    Lot lot;
    lot.id = next_lot_id++;
    lot.contract = p.contract;
    lot.qty = p.qty;
    lot.multiplier = p.multiplier;
    lot.expiry_ts_ns = expiry;
    lot.cohort = cohort;
    lot.entry_price = *mark;  // fill at mid
    book.lots.push_back(lot);
  }
  front_expiry_ = expiry;
  have_front_ = true;
  return Ok();
}

std::vector<std::pair<std::string, double>> DispersionStrategy::signals(
    const MarketSnapshot& base) const {
  const Result<DispersionUniverse> ru =
      resolve_universe_uids(universe_, [&](std::string_view s) { return base.uid_of(s); });
  if (!ru) {
    return {};  // no signal on this snapshot — same behaviour as a failed signal
  }
  const Result<DispersionSignal> sig = dispersion_signal(*ru, base.set(), cfg_.target_T);
  if (!sig) {
    return {};
  }
  return {{"implied_corr", sig->implied_corr}};
}

Result<DispersionBook> DispersionStrategy::build_book(const MarketSnapshot& base) const {
  const Result<DispersionUniverse> ru =
      resolve_universe_uids(universe_, [&](std::string_view s) { return base.uid_of(s); });
  if (!ru) {
    return Err(ru.error());
  }
  return build_dispersion_book(*ru, base.set(), cfg_);
}

}  // namespace atx::vol

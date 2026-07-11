// atx-vol DispersionStrategy (Phase B1) — an IStrategy adapter over the existing
// `build_dispersion_book` (P4-1). It re-expresses the vega-neutral straddle
// dispersion trade as a backtest strategy WITHOUT reimplementing its sizing:
// on entry it calls `build_dispersion_book` (authoritative) and converts the
// emitted `Position`s into engine `Lot`s, and it surfaces the implied-correlation
// diagnostic through the `signals` hook so a backtest records it per row.

#include "atx/vol/strategy.hpp" // DispersionStrategy, IStrategy, lifecycle_decide

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/backtest.hpp"         // MarketSnapshot, Lot, PortfolioState
#include "atx/vol/dispersion.hpp"       // build_dispersion_book, dispersion_signal
#include "atx/vol/phase_profile.hpp"
#include "atx/vol/portfolio_pricer.hpp" // OptionContract, kNsPerYear, Position
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/types.hpp"            // Result, Status

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

Status DispersionStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                   PortfolioState &book, std::uint64_t &next_lot_id) {
  const LifecycleDecision d = lifecycle_decide(lifecycle_, step_index, book.lots.empty(),
                                               base.ts_ns(), front_expiry_, have_front_);
  if (!d.open) {
    return Ok();
  }

  // Decide the WHOLE no-trade question BEFORE mutating any state. Resolve + size
  // first; only once `build_dispersion_book` has returned Ok is this a real trading
  // step where the roll (`d.clear`) and the fresh lots may be applied. On any
  // no-trade / abort path above the mutation below, `book.lots`, `have_front_`,
  // `front_expiry_` and `cohort_counter_` are left EXACTLY as found — the documented
  // no-trade contract. (Pre-fix this cleared the book at the top of the roll branch,
  // so a no-trade roll step force-closed the held basket; see
  // NoTradeOnRollDateLeavesBookIntact.)
  //
  // Bind the (possibly symbol-only) universe to THIS snapshot's uid scheme before
  // sizing, so one authored universe works across every date of a corpus. Under
  // DropRenormalize a name absent from the snapshot is dropped here (not fatal);
  // an unresolved INDEX or an authoring bug is still a hard error.
  Result<ResolvedUniverse> ru = resolve_universe_uids(
      universe_, [&](std::string_view s) { return base.uid_of(s); }, cfg_.missing);
  if (!ru) {
    return Err(ru.error());
  }

  Result<DispersionBook> built = [&]() {
    ATX_VOL_PROFILE_SCOPE(StrategyBuildBook);
    return build_dispersion_book(ru->universe, base.set(), cfg_);
  }();
  if (!built) {
    // NO-TRADE CONTRACT: under DropRenormalize an Unavailable book means too few
    // names survived today => a flat / no-trade step. Open no lots, leave the
    // existing book untouched, and continue. Any other code stays fatal.
    if (cfg_.missing.policy == MissingNamePolicy::DropRenormalize &&
        built.error().code() == ErrorCode::Unavailable) {
      return Ok();
    }
    return Err(built.error());
  }

  // The build succeeded: this is a real trading step. NOW apply the roll (erase the
  // prior cohort) and open the fresh lots. Applying `d.clear` here — after the build,
  // not before — is what keeps a no-trade roll step from force-closing the held book.
  if (d.clear) {
    book.lots.clear();
    have_front_ = false;
  }
  const std::uint32_t cohort = cohort_counter_++;
  const std::int64_t projected_expiry = built->index_leg.call_definition.expiry_ts_ns;
  const std::int64_t expiry = projected_expiry != 0
                                  ? projected_expiry
                                  : base.ts_ns() + std::llround(cfg_.target_T * kNsPerYear);
  {
    ATX_VOL_PROFILE_SCOPE(StrategyEntryMarks);
    for (const Position &p : built->positions) {
      const PricedSurface *surf = base.find(p.contract.uid);
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
      lot.entry_price = *mark; // fill at mid
      book.lots.push_back(lot);
    }
  }
  front_expiry_ = expiry;
  have_front_ = true;
  return Ok();
}

std::vector<std::pair<std::string, double>>
DispersionStrategy::signals(const MarketSnapshot &base) const {
  const Result<ResolvedUniverse> ru = resolve_universe_uids(
      universe_, [&](std::string_view s) { return base.uid_of(s); }, cfg_.missing);
  if (!ru) {
    return {}; // universe can't bind (index missing / authoring bug): no signal, as pre-S1-3
  }
  const double n_resolve_dropped = static_cast<double>(ru->dropped.size());
  const Result<DispersionSignal> sig =
      dispersion_signal(ru->universe, base.set(), cfg_.target_T, cfg_.missing);
  if (!sig) {
    // No tradeable signal this snapshot (e.g. too few survivors => Unavailable):
    // emit implied_corr as NaN but still surface the drops we know about, so the
    // series stay full-length and the drop shows up in the run diagnostics.
    return {{"implied_corr", std::numeric_limits<double>::quiet_NaN()},
            {"n_names_dropped", n_resolve_dropped}};
  }
  return {{"implied_corr", sig->implied_corr},
          {"n_names_dropped", n_resolve_dropped + static_cast<double>(sig->dropped.size())}};
}

Result<DispersionBook> DispersionStrategy::build_book(const MarketSnapshot &base) const {
  const Result<ResolvedUniverse> ru = resolve_universe_uids(
      universe_, [&](std::string_view s) { return base.uid_of(s); }, cfg_.missing);
  if (!ru) {
    return Err(ru.error());
  }
  return build_dispersion_book(ru->universe, base.set(), cfg_);
}

std::vector<DroppedName> DispersionStrategy::dropped_on(const MarketSnapshot &base) const {
  std::vector<DroppedName> out;
  const Result<ResolvedUniverse> ru = resolve_universe_uids(
      universe_, [&](std::string_view s) { return base.uid_of(s); }, cfg_.missing);
  if (!ru) {
    return out; // universe can't bind: no per-name drop list
  }
  out = ru->dropped; // resolve-stage drops (symbol absent from the snapshot)
  const Result<DispersionSignal> sig =
      dispersion_signal(ru->universe, base.set(), cfg_.target_T, cfg_.missing);
  if (sig) {
    for (const DroppedName &d : sig->dropped) { // signal-stage drops (surface / unusable)
      out.push_back(d);
    }
  }
  return out;
}

} // namespace atx::vol

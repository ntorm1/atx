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
#include "atx/vol/backtest.hpp"   // MarketSnapshot, Lot, PortfolioState
#include "atx/vol/dispersion.hpp" // build_dispersion_book, dispersion_signal
#include "atx/vol/phase_profile.hpp"
#include "atx/vol/portfolio_pricer.hpp" // OptionContract, kNsPerYear, Position
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/types.hpp"            // Result, Status

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

struct CanonicalTenor {
  std::int64_t expiry_ts_ns{0};
  double T{0.0};
};

// M5 SYNTHETIC-EXPIRY / SETTLEMENT INVARIANT. When `cfg.projected_maturity` is
// unset the entry expiry is SYNTHETIC — `valuation_ts_ns + round(target_T *
// kNsPerYear)` — an instant that need NOT coincide with any later snapshot's
// timestamp. The engine settles an expiring lot ONLY at a snapshot whose ts_ns
// matches the lot's expiry EXACTLY (run_backtest hard-errors "no exact expiry
// observation" otherwise — the guard is engine-side and never silent). Under the
// canonical RollAtHorizon lifecycle a cohort is roll-CLOSED at marks once its
// residual T falls below `roll_at_T` (= roll_dte), so it is retired BEFORE its
// synthetic expiry and settlement is never reached. That safety holds only while
// consecutive snapshots are spaced closer than `roll_at_T`: a clock GAP wider
// than roll_dte can step a lot from residual-T > roll_at_T straight past its
// synthetic expiry, skipping the roll window and tripping the engine's exact-ts
// settlement guard. Keep roll_dte comfortably above the largest expected
// inter-snapshot gap, or pin an exact projected expiry via `projected_maturity`
// (which lands on a real snapshot instant) to remove the synthetic-expiry risk.

[[nodiscard]] Result<CanonicalTenor> canonical_tenor(std::int64_t valuation_ts_ns,
                                                     double requested_T) {
  if (!std::isfinite(requested_T) || requested_T <= 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "DispersionStrategy: target_T must be finite and positive");
  }
  const long double offset =
      static_cast<long double>(requested_T) * static_cast<long double>(kNsPerYear);
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  if (!(offset >= 0.5L && offset < static_cast<long double>(kMax) - 0.5L)) {
    return Err(ErrorCode::InvalidArgument,
               "DispersionStrategy: target_T cannot be represented as an expiry");
  }
  const std::int64_t tenor_ns = static_cast<std::int64_t>(std::llround(offset));
  if (tenor_ns <= 0 || valuation_ts_ns > kMax - tenor_ns) {
    return Err(ErrorCode::InvalidArgument,
               "DispersionStrategy: target expiry overflows timestamp range");
  }
  return Ok(CanonicalTenor{valuation_ts_ns + tenor_ns, static_cast<double>(tenor_ns) / kNsPerYear});
}

} // namespace

Status DispersionStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                   PortfolioState &book, std::uint64_t &next_lot_id) {
  return on_step_impl(base, step_index, book, next_lot_id, nullptr);
}

Status DispersionStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                   PortfolioState &book, std::uint64_t &next_lot_id,
                                   const PriceOptions &price_options) {
  return on_step_impl(base, step_index, book, next_lot_id, &price_options);
}

Status DispersionStrategy::on_step_impl(const MarketSnapshot &base, std::size_t step_index,
                                        PortfolioState &book, std::uint64_t &next_lot_id,
                                        const PriceOptions *price_options) {
  last_entry_seeds_.clear();
  // C1 POINT-IN-TIME UNIVERSE. Re-resolve the basket for THIS step's date before
  // any sizing so a mid-backtest reconstitution is honored (the next roll rebuilds
  // vega-flat legs from the fresh membership; a dropped name actually exits — this
  // is also where C3's removals take effect end-to-end). A resolve failure (e.g. a
  // date before the first effective block) keeps the last-known-good universe. With
  // no resolver installed (the frozen ctor) this is a no-op and the run is
  // bit-identical to pre-C1 — the reproducibility golden is unaffected.
  if (pit_resolver_) {
    Result<DispersionUniverse> pit = pit_resolver_(base.ts_ns());
    if (pit) {
      universe_ = std::move(*pit);
    }
  }
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

  DispersionConfig effective_cfg = cfg_;
  std::int64_t expiry = 0;
  if (!effective_cfg.projected_maturity.has_value()) {
    ATX_TRY(const CanonicalTenor canonical, canonical_tenor(base.ts_ns(), effective_cfg.target_T));
    if (price_options != nullptr) {
      effective_cfg.target_T = canonical.T;
    }
    expiry = canonical.expiry_ts_ns;
  }

  Result<DispersionBook> built = [&]() {
    ATX_VOL_PROFILE_SCOPE(StrategyBuildBook);
    return price_options == nullptr
               ? build_dispersion_book(ru->universe, base.set(), effective_cfg)
               : build_dispersion_book(ru->universe, base.set(), effective_cfg, *price_options);
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
  if (built->entry_marks.size() != built->positions.size()) {
    return Err(ErrorCode::Internal, "DispersionStrategy: entry mark count mismatch");
  }
  if (price_options != nullptr && built->entry_risk_seeds.size() != built->positions.size()) {
    return Err(ErrorCode::Internal, "DispersionStrategy: entry seed count mismatch");
  }
  if (price_options == nullptr && !built->entry_risk_seeds.empty()) {
    return Err(ErrorCode::Internal, "DispersionStrategy: legacy builder produced entry seeds");
  }
  if (built->positions.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max() - next_lot_id)) {
    return Err(ErrorCode::OutOfRange, "DispersionStrategy: lot id range overflow");
  }
  const std::int64_t projected_expiry = built->index_leg.call_definition.expiry_ts_ns;
  if (projected_expiry != 0) {
    expiry = projected_expiry;
  }
  if (expiry <= base.ts_ns()) {
    return Err(ErrorCode::Internal, "DispersionStrategy: nonpositive entry expiry");
  }

  const std::uint32_t cohort = cohort_counter_;
  std::vector<Lot> replacement;
  replacement.reserve(built->positions.size());
  std::uint64_t staged_next_lot_id = next_lot_id;
  {
    ATX_VOL_PROFILE_SCOPE(StrategyEntryMarks);
    for (std::size_t i = 0; i < built->positions.size(); ++i) {
      const Position &p = built->positions[i];
      Lot lot;
      lot.id = staged_next_lot_id++;
      lot.contract = p.contract;
      lot.qty = p.qty;
      lot.multiplier = p.multiplier;
      lot.expiry_ts_ns = expiry;
      lot.cohort = cohort;
      lot.entry_price = built->entry_marks[i];
      replacement.push_back(lot);
    }
  }

  if (d.clear) {
    book.lots = std::move(replacement);
  } else {
    book.lots.reserve(book.lots.size() + replacement.size());
    for (Lot &lot : replacement) {
      book.lots.push_back(std::move(lot));
    }
  }
  next_lot_id = staged_next_lot_id;
  ++cohort_counter_;
  front_expiry_ = expiry;
  have_front_ = true;
  last_entry_seeds_ = std::move(built->entry_risk_seeds);
  return Ok();
}

std::vector<std::pair<std::string, double>>
DispersionStrategy::signals(const MarketSnapshot &base) const {
  if (!cfg_.record_diagnostics) {
    return {};
  }
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

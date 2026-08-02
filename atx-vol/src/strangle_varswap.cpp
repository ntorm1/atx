// atx-vol XOM strangle-vs-varswap comparison backtest — the options leg.
// See strangle_varswap.hpp for the cycle/restrike contract.

#include "atx/vol/strangle_varswap.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "atx/core/error.hpp"           // Ok, Err, ErrorCode, ATX_TRY_VOID
#include "atx/vol/portfolio_pricer.hpp" // OptionContract, SurfaceRef, kNsPerYear

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// The upper bound is exclusive because double(INT64_MAX) rounds to 2^63;
// checking against INT64_MAX after conversion would itself risk UB.
constexpr double kInt64ExclusiveUpper = 0x1p63;

} // namespace

StrangleVsVarswapStrategy::StrangleVsVarswapStrategy(StrangleVarswapConfig cfg)
    : cfg_{std::move(cfg)} {}

HedgeSpec StrangleVsVarswapStrategy::hedge_spec() const {
  return HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, /*band=*/0.0};
}

Status StrangleVsVarswapStrategy::validate_config() {
  if (cfg_.symbol.empty()) {
    return Err(ErrorCode::InvalidArgument, "StrangleVsVarswapStrategy: symbol must be non-empty");
  }
  if (!(std::isfinite(cfg_.target_abs_delta) && cfg_.target_abs_delta > 0.0 &&
        cfg_.target_abs_delta < 1.0)) {
    return Err(ErrorCode::InvalidArgument,
               "StrangleVsVarswapStrategy: target_abs_delta must lie in (0,1)");
  }
  if (!(std::isfinite(cfg_.contracts) && cfg_.contracts != 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "StrangleVsVarswapStrategy: contracts must be finite and non-zero");
  }
  if (!(std::isfinite(cfg_.tenor_years) && cfg_.tenor_years > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "StrangleVsVarswapStrategy: tenor_years must be finite and positive");
  }
  const double tenor_ns = std::round(cfg_.tenor_years * kNsPerYear);
  if (!(tenor_ns >= 1.0 && tenor_ns < kInt64ExclusiveUpper)) {
    return Err(ErrorCode::InvalidArgument,
               "StrangleVsVarswapStrategy: tenor_years is out of range");
  }
  tenor_ns_ = static_cast<std::int64_t>(tenor_ns);
  if (cfg_.session_ts.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "StrangleVsVarswapStrategy: session_ts is required to fix a cycle expiry");
  }
  // A binary search over an unsorted grid still RETURNS an anchor — a wrong
  // expiry reported as success — so an out-of-order calendar fails closed here,
  // exactly as `resolve_spec` rejects one for a snap_to_sessions leg.
  if (!std::is_sorted(cfg_.session_ts.begin(), cfg_.session_ts.end())) {
    return Err(ErrorCode::InvalidArgument,
               "StrangleVsVarswapStrategy: session_ts must be sorted ascending");
  }
  return Ok();
}

std::int64_t StrangleVsVarswapStrategy::select_cycle_expiry(std::int64_t base_ts) const noexcept {
  const std::vector<std::int64_t> &sessions = cfg_.session_ts;
  const std::int64_t anchor = (base_ts > std::numeric_limits<std::int64_t>::max() - tenor_ns_)
                                  ? std::numeric_limits<std::int64_t>::max()
                                  : base_ts + tenor_ns_;
  const auto at_or_after = std::lower_bound(sessions.begin(), sessions.end(), anchor);
  // Past the end of the grid there is no session to hold the full tenor. Take the
  // LAST one instead: a short final cycle that the run still observes settling
  // beats an expiry the corpus never reaches.
  const std::int64_t expiry = (at_or_after != sessions.end()) ? *at_or_after : sessions.back();
  return expiry > base_ts ? expiry : 0;
}

std::optional<StrangleVsVarswapStrategy::ResolvedStrangle>
StrangleVsVarswapStrategy::resolve_wings(const MarketSnapshot &base,
                                         const PriceOptions &price_options) const {
  const std::optional<std::uint32_t> uid = base.uid_of(cfg_.symbol);
  if (!uid.has_value()) {
    return std::nullopt; // the name has no board on this session
  }
  const SurfaceRef surface = base.find(*uid);
  if (surface == nullptr) {
    return std::nullopt;
  }
  // Both timestamps are inside the corpus and the cycle expiry is strictly after
  // the base, so this difference is positive and far from overflow.
  const double T = static_cast<double>(cycle_expiry_ts_ns_ - base.ts_ns()) / kNsPerYear;
  if (!(std::isfinite(T) && T > 0.0)) {
    return std::nullopt;
  }

  ResolvedStrangle out;
  out.uid = *uid;
  out.T = T;
  out.seeds.reserve(kWings.size());
  for (std::size_t w = 0; w < kWings.size(); ++w) {
    const Result<double> K = resolve_strike_by_delta(surface, T, kWings[w], cfg_.target_abs_delta);
    if (!K || !(std::isfinite(*K) && *K > 0.0)) {
      return std::nullopt;
    }
    // The seed carries the mark this entry fills at AND the full-risk bundle the
    // engine reuses for the entry frame, so the wing is priced exactly once.
    Result<FullGreekSeed> seed = surface->full_greek_seed(
        *K, T, kWings[w], price_options.analytic_greeks, price_options.query_execution);
    if (!seed) {
      return std::nullopt;
    }
    const double mark = seed->greeks().price;
    if (!(std::isfinite(mark) && mark >= 0.0)) {
      return std::nullopt; // a lot the engine boundary would reject; never a 0.0 fill
    }
    out.K[w] = *K;
    out.mark[w] = mark;
    out.seeds.push_back(std::move(*seed));
  }
  return out;
}

Status StrangleVsVarswapStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                          PortfolioState &book, std::uint64_t &next_lot_id) {
  return on_step(base, step_index, book, next_lot_id, PriceOptions{});
}

Status StrangleVsVarswapStrategy::on_step(const MarketSnapshot &base, std::size_t /*step_index*/,
                                          PortfolioState &book, std::uint64_t &next_lot_id,
                                          const PriceOptions &price_options) {
  last_entry_seeds_.clear();
  if (!validated_) {
    ATX_TRY_VOID(validate_config());
    validated_ = true;
  }

  const std::int64_t base_ts = base.ts_ns();
  if (cycle_expiry_ts_ns_ <= base_ts) {
    // Inception, or the engine settled the cycle before calling us (it erases
    // expired lots strictly before `on_step`). Fix the next cycle's expiry.
    cycle_expiry_ts_ns_ = select_cycle_expiry(base_ts);
    if (cycle_expiry_ts_ns_ <= base_ts) {
      return Ok(); // the session grid is exhausted: no cycle left to open
    }
    ++cycle_index_; // `Lot::cohort` counts CYCLES, not the daily clips inside one
  }

  const std::optional<ResolvedStrangle> wings = resolve_wings(base, price_options);
  if (!wings.has_value()) {
    // KEEP THE LIVE STRIKES. The alternative — closing without reopening, or
    // reopening at a fabricated strike — either flattens real exposure for a
    // data gap or books a position the surface never priced.
    ++unresolved_strike_steps_;
    return Ok();
  }

  // Close-and-reopen: the engine diffs `before_lots` against the post-step book
  // and books the departed pair as a roll-close at today's marks (the expiry
  // settle path ran earlier in the loop, so nothing here can reach it).
  book.lots.clear();
  book.lots.reserve(kWings.size());
  for (std::size_t w = 0; w < kWings.size(); ++w) {
    Lot lot;
    lot.id = next_lot_id++;
    lot.contract = OptionContract{wings->uid, wings->K[w], wings->T, kWings[w]};
    lot.qty = cfg_.contracts;
    // `Lot::multiplier` keeps its 100-share listed-equity-option default.
    lot.expiry_ts_ns = cycle_expiry_ts_ns_;
    lot.cohort = cycle_index_;
    lot.entry_price = wings->mark[w]; // fill at the model mid
    book.lots.push_back(lot);
  }
  last_entry_seeds_ = std::move(wings->seeds);
  return Ok();
}

} // namespace atx::vol

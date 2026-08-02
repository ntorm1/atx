// atx-vol XOM strangle-vs-varswap comparison backtest — the options leg.
// See strangle_varswap.hpp for the cycle/restrike contract.

#include "atx/vol/strangle_varswap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/error.hpp"                  // Ok, Err, ErrorCode, ATX_TRY_VOID
#include "atx/vol/detail/deriv_ref_bridge.hpp" // deriv_price_on_ref, deriv_greeks_on_ref
#include "atx/vol/portfolio_pricer.hpp"        // OptionContract, SurfaceRef, kNsPerYear

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// The upper bound is exclusive because double(INT64_MAX) rounds to 2^63;
// checking against INT64_MAX after conversion would itself risk UB.
constexpr double kInt64ExclusiveUpper = 0x1p63;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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

double
StrangleVsVarswapStrategy::strangle_dollar_vega(const ResolvedStrangle &wings) const noexcept {
  // Dropping the multiplier would size the swap 100x light and report a dollar
  // vega 100x light beside it.
  double total = 0.0;
  for (const FullGreekSeed &seed : wings.seeds) {
    total += seed.greeks().vega * cfg_.contracts * kMultiplier;
  }
  return total;
}

void StrangleVsVarswapStrategy::open_cycle_swap(const MarketSnapshot &base,
                                                const ResolvedStrangle &wings, double strangle_vega,
                                                PortfolioState &book, std::uint64_t &next_lot_id) {
  const SurfaceRef surface = base.find(wings.uid);
  if (surface == nullptr) {
    ++skipped_swap_cycles_; // defence in depth: resolve_wings already found it
    return;
  }

  // The solve owns the fixing-schedule count, the fair strike and the
  // equal-vega qty (swap_leg.hpp); every refusal is one skipped cycle here.
  CycleSwapRequest req;
  req.uid = wings.uid;
  req.kind = DerivKind::VarSwap;
  req.cap_dec = 0.0; // UNCAPPED: `cap_dec` must be 0 on an uncapped kind
  req.notional = kSwapNotional;
  req.annualization = kSwapAnnualization;
  req.open_ts_ns = base.ts_ns();
  req.expiry_ts_ns = cycle_expiry_ts_ns_;
  req.session_ts = cfg_.session_ts;
  req.deriv_cfg = cfg_.deriv_cfg;

  Result<SwapLot> lot = solve_cycle_swap(surface, req, strangle_vega);
  if (!lot) {
    ++skipped_swap_cycles_;
    return;
  }
  lot->id = next_lot_id++; // the watermark moves only once the lot is real
  book.swap_lots.push_back(*lot);
}

Status StrangleVsVarswapStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                          PortfolioState &book, std::uint64_t &next_lot_id) {
  return on_step(base, step_index, book, next_lot_id, PriceOptions{});
}

Status StrangleVsVarswapStrategy::on_step(const MarketSnapshot &base, std::size_t /*step_index*/,
                                          PortfolioState &book, std::uint64_t &next_lot_id,
                                          const PriceOptions &price_options) {
  // Captured BEFORE the strategy touches the book: a lot in `book.swap_lots`
  // afterwards but not in the capture is one this step OPENED, and a lot in
  // both is one the engine carried in — from the previous step, or from a
  // checkpoint. The probe owns the distinction (swap_leg.hpp).
  probe_.capture_pre_step(book);
  ATX_TRY_VOID(step(base, book, next_lot_id, price_options));
  // Only on success: an errored step aborts the whole run, and refreshing off a
  // half-built book would be state nobody can use for state nobody will read.
  probe_.refresh(base, book);
  last_step_ts_ns_ = base.ts_ns();
  return Ok();
}

Status StrangleVsVarswapStrategy::step(const MarketSnapshot &base, PortfolioState &book,
                                       std::uint64_t &next_lot_id,
                                       const PriceOptions &price_options) {
  last_entry_seeds_.clear();
  // Nothing measured YET this step. Every path that resolves a pair overwrites
  // this; every path that does not leaves the step reporting "not measured"
  // rather than the previous step's vega.
  last_strangle_vega_ = kNaN;
  if (!validated_) {
    ATX_TRY_VOID(validate_config());
    validated_ = true;
  }

  const std::int64_t base_ts = base.ts_ns();
  bool cycle_opened = false;
  if (cycle_expiry_ts_ns_ <= base_ts) {
    // Inception, or the engine settled the cycle before calling us (it erases
    // expired lots strictly before `on_step`). Fix the next cycle's expiry.
    cycle_expiry_ts_ns_ = select_cycle_expiry(base_ts);
    if (cycle_expiry_ts_ns_ <= base_ts) {
      return Ok(); // the session grid is exhausted: no cycle left to open
    }
    ++cycle_index_; // `Lot::cohort` counts CYCLES, not the daily clips inside one
    cycle_opened = true;
  }

  const std::optional<ResolvedStrangle> wings = resolve_wings(base, price_options);
  if (!wings.has_value()) {
    // KEEP THE LIVE STRIKES. The alternative — closing without reopening, or
    // reopening at a fabricated strike — either flattens real exposure for a
    // data gap or books a position the surface never priced.
    //
    // Which of the two counters this is depends on whether there is anything TO
    // keep. `book.lots` holds the survivors the engine did not settle, so an
    // empty book here means inception or a cycle roll: no strikes were held, and
    // saying otherwise would put a fictitious hold on the record.
    if (book.lots.empty()) {
      ++unopened_strangle_steps_;
    } else {
      ++skipped_restrikes_;
    }
    if (cycle_opened && cfg_.enable_swap_leg) {
      // The cycle is fixed regardless (its expiry comes off the calendar, not
      // the surface) but there is no entry vega to size a swap against, and the
      // leg is a per-CYCLE instrument — a later session getting its board back
      // does not retro-open it. This cycle runs one-legged, and says so.
      ++skipped_swap_cycles_;
    }
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
  // The book the engine will price this row IS this pair, so its dollar vega is
  // both the comparison's `strangle_vega` signal and — on a cycle-open step —
  // the quantity the swap is sized against.
  last_strangle_vega_ = strangle_dollar_vega(*wings);
  // The swap opens ONCE, on the step that FIXES the cycle — never on a restrike,
  // because the swap-lot lane is append-only and held to expiry (there is no
  // unwind price for an OTC swap here), so a per-step reopen would pile up one
  // stale leg per session instead of expressing today's view.
  if (cycle_opened && cfg_.enable_swap_leg) {
    open_cycle_swap(base, *wings, last_strangle_vega_, book, next_lot_id);
  }
  last_entry_seeds_ = std::move(wings->seeds);
  return Ok();
}

std::vector<std::pair<std::string, double>>
StrangleVsVarswapStrategy::signals(const MarketSnapshot &base) const {
  // The five swap greek columns come off the probe, which enforces the as-of
  // discipline itself (swap_leg.hpp). `strangle_vega` is guarded by the same
  // condition: the cached vega is as-of ONE snapshot, and handed any other this
  // row reports "not measured" rather than the previous step's number.
  const bool as_of = probe_.stepped() && base.ts_ns() == last_step_ts_ns_;

  std::vector<std::pair<std::string, double>> out;
  out.reserve(8);
  probe_.append_swap_greek_signals(base, out);
  out.emplace_back("strangle_vega", as_of ? last_strangle_vega_ : kNaN);
  out.emplace_back("skipped_restrikes", static_cast<double>(skipped_restrikes_));
  out.emplace_back("skipped_swaps", static_cast<double>(skipped_swap_cycles_));
  return out;
}

} // namespace atx::vol

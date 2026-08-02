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

// `SwapLot::n_obs_total`'s ceiling, named so the fixing-count narrowing reads as
// a range check rather than a cast.
constexpr std::ptrdiff_t kUint32Max =
    static_cast<std::ptrdiff_t>(std::numeric_limits<std::uint32_t>::max());

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The engine's swap MARK config (`swap_price_cfg`, backtest.cpp), transcribed.
// The signals differentiate the marks the engine actually took, so they price
// through the engine's config rather than `cfg_.deriv_cfg` — which is documented
// as the ENTRY SOLVE's alone. At the default the two are the same object and the
// distinction is moot; set a non-default entry config and the signals still
// describe the position the run is being paid on.
const DerivConfig kEngineSwapMarkCfg{};

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

DerivContract StrangleVsVarswapStrategy::swap_contract(const SwapLot &lot, std::int64_t base_ts,
                                                       const RealizedVarianceSpec &rv) noexcept {
  DerivContract contract;
  contract.kind = lot.kind;
  // The engine's `residual_T` verbatim, unsigned-differenced so the subtraction
  // is defined for any pair of timestamps rather than only for the live-lot case
  // this strategy calls it with.
  const bool live = lot.expiry_ts_ns >= base_ts;
  const std::uint64_t hi = static_cast<std::uint64_t>(live ? lot.expiry_ts_ns : base_ts);
  const std::uint64_t lo = static_cast<std::uint64_t>(live ? base_ts : lot.expiry_ts_ns);
  const double years = static_cast<double>(hi - lo) / kNsPerYear;
  contract.maturity_t = live ? years : -years;
  contract.strike_dec = lot.strike_dec;
  contract.cap_dec = lot.cap_dec;
  contract.notional = lot.notional;
  contract.rv_spec = rv;
  return contract;
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
  if (!(std::isfinite(strangle_vega) && strangle_vega != 0.0)) {
    ++skipped_swap_cycles_;
    return;
  }

  // The fixing schedule the ENGINE will actually observe. The swap pass runs on
  // the SHIFTED snapshot, so the first session after this one merely SEEDS the
  // series and accrues nothing; every session after that through expiry adds one
  // return. `n_obs_total` is therefore one FEWER than the count of sessions in
  // (open, expiry] — set it to the raw count and the lot reaches its expiry an
  // observation short, with every intermediate mark over-weighting the future
  // leg against the residual maturity it is priced at.
  const std::vector<std::int64_t> &sessions = cfg_.session_ts;
  const auto after_open = std::upper_bound(sessions.begin(), sessions.end(), base.ts_ns());
  const auto after_expiry = std::upper_bound(sessions.begin(), sessions.end(), cycle_expiry_ts_ns_);
  const std::ptrdiff_t in_window = after_expiry - after_open;
  if (in_window < 2 || in_window - 1 > kUint32Max) {
    // A one-session cycle observes NO return at all: `n_obs_total` would be 0,
    // which the engine boundary rejects outright, and the lot would reach expiry
    // with an empty estimator — a NotFound that aborts the whole run. A cycle
    // too short to carry a variance swap runs options-only instead. The upper
    // bound is unreachable on any real calendar and exists so the narrowing
    // below can never wrap a schedule into a smaller, wrong one.
    ++skipped_swap_cycles_;
    return;
  }

  const SurfaceRef surface = base.find(wings.uid);
  if (surface == nullptr) {
    ++skipped_swap_cycles_; // defence in depth: resolve_wings already found it
    return;
  }

  SwapLot lot;
  lot.uid = wings.uid;
  lot.kind = DerivKind::VarSwap;
  lot.strike_dec = 0.0; // solved below; 0 also makes the probe quote's PV harmless
  lot.cap_dec = 0.0;    // UNCAPPED: `cap_dec` must be 0 on an uncapped kind
  lot.notional = kSwapNotional;
  lot.qty = 0.0; // solved below
  lot.start_ts_ns = base.ts_ns();
  lot.expiry_ts_ns = cycle_expiry_ts_ns_;
  lot.n_obs_total = static_cast<std::uint32_t>(in_window - 1);
  lot.annualization = kSwapAnnualization;

  // Nothing is realized at entry — the engine seeds this lot's series one step
  // from now — so the solve prices a purely forward-looking contract.
  RealizedVarianceSpec rv{};
  rv.annualization = kSwapAnnualization;
  rv.n_obs_total = lot.n_obs_total;

  // The fair strike is read off a quote rather than from the PricedSurface-native
  // `var_swap_fair_strike`, DELIBERATELY: `DerivQuote::fair_strike_dec` IS the
  // strike that prices this contract to PV = 0, and taking it from the same
  // `deriv_price_on_ref` bridge the engine's mark lane prices through is the only
  // way the two agree. The native entry derives its carry from the fitted pillar
  // list while the bridge derives it from the handle; striking against one and
  // marking against the other would open the swap at a non-zero PV, and the whole
  // of that artifact would land in the first step's `swap_pnl` (a swap opens at
  // zero cost, so its first mark carries the entire entry difference).
  const Result<DerivQuote> fair =
      detail::deriv_price_on_ref(surface, swap_contract(lot, lot.start_ts_ns, rv), cfg_.deriv_cfg);
  if (!fair || !(std::isfinite(fair->fair_strike_dec) && fair->fair_strike_dec > 0.0)) {
    ++skipped_swap_cycles_;
    return;
  }
  lot.strike_dec = fair->fair_strike_dec;

  const Result<DerivGreeks> greeks = detail::deriv_greeks_on_ref(
      surface, swap_contract(lot, lot.start_ts_ns, rv), cfg_.deriv_cfg, DerivGreekBumps{});
  if (!greeks || !(std::isfinite(greeks->vega) && greeks->vega != 0.0)) {
    ++skipped_swap_cycles_; // never a garbage qty: no vega, no leg
    return;
  }

  // EQUAL VEGA. Both quantities are dollars per 1.00 of vol on their own leg, so
  // the ratio is a pure number and the sign carries: a long-vega strangle sizes a
  // long (variance-receiving) swap.
  const double qty = strangle_vega / greeks->vega;
  if (!std::isfinite(qty)) {
    ++skipped_swap_cycles_;
    return;
  }
  lot.qty = qty;
  lot.id = next_lot_id++; // the watermark moves only once the lot is real
  book.swap_lots.push_back(lot);
}

Status StrangleVsVarswapStrategy::on_step(const MarketSnapshot &base, std::size_t step_index,
                                          PortfolioState &book, std::uint64_t &next_lot_id) {
  return on_step(base, step_index, book, next_lot_id, PriceOptions{});
}

Status StrangleVsVarswapStrategy::on_step(const MarketSnapshot &base, std::size_t /*step_index*/,
                                          PortfolioState &book, std::uint64_t &next_lot_id,
                                          const PriceOptions &price_options) {
  // Captured BEFORE the strategy touches the book: a lot in `book.swap_lots`
  // afterwards but not here is one this step OPENED, and a lot in both is one
  // the engine carried in — from the previous step, or from a checkpoint.
  swap_ids_before_step_.clear();
  swap_ids_before_step_.reserve(book.swap_lots.size());
  for (const SwapLot &lot : book.swap_lots) {
    swap_ids_before_step_.push_back(lot.id);
  }
  ATX_TRY_VOID(step(base, book, next_lot_id, price_options));
  // Only on success: an errored step aborts the whole run, and refreshing off a
  // half-built book would be state nobody can use for state nobody will read.
  refresh_signal_state(base, book, swap_ids_before_step_);
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

const StrangleVsVarswapStrategy::SwapMirror *
StrangleVsVarswapStrategy::find_mirror(std::uint64_t lot_id) const noexcept {
  for (const SwapMirror &mirror : swap_mirrors_) {
    if (mirror.lot_id == lot_id) {
      return &mirror;
    }
  }
  return nullptr;
}

void StrangleVsVarswapStrategy::refresh_signal_state(
    const MarketSnapshot &base, const PortfolioState &book,
    std::span<const std::uint64_t> swap_ids_before_step) {
  // Lots the engine settled this step took their terminal fixing and left the
  // book before `on_step`; their accrual is finished business.
  std::erase_if(swap_mirrors_, [&book](const SwapMirror &mirror) {
    return std::none_of(book.swap_lots.begin(), book.swap_lots.end(),
                        [&mirror](const SwapLot &lot) { return lot.id == mirror.lot_id; });
  });
  for (const SwapLot &lot : book.swap_lots) {
    const auto found = std::find_if(swap_mirrors_.begin(), swap_mirrors_.end(),
                                    [&lot](const SwapMirror &m) { return m.lot_id == lot.id; });
    if (found == swap_mirrors_.end()) {
      // FIRST SIGHT, and deliberately no fixing: the engine's swap pass will not
      // see this lot until the next step, and that step is the one that seeds
      // its series. Taking a fixing here would age every later mark by a session.
      SwapMirror fresh;
      fresh.lot_id = lot.id;
      fresh.rv.annualization = lot.annualization;
      fresh.rv.n_obs_total = lot.n_obs_total;
      // ... UNLESS the lot was already in the book when this step began. Then it
      // is not new at all: the engine restored it, and its realized variance
      // with it, into a strategy that has never seen a fixing for it. A clean
      // accrual would describe a swap that had realized nothing — see
      // `SwapMirror::desynced`.
      fresh.desynced = std::find(swap_ids_before_step.begin(), swap_ids_before_step.end(),
                                 lot.id) != swap_ids_before_step.end();
      swap_mirrors_.push_back(fresh);
      continue;
    }
    SwapMirror &mirror = *found;
    const SurfaceRef surface = base.find(lot.uid);
    if (surface == nullptr) {
      mirror.desynced = true; // the engine has already failed the run on this
      continue;
    }
    const double spot = surface->pricing().S;
    if (!(spot > 0.0)) {
      mirror.desynced = true;
      continue;
    }
    if (!mirror.have_prev) {
      mirror.prev_spot = spot; // the seed observation accrues nothing
      mirror.have_prev = true;
      continue;
    }
    if (mirror.rv.n_obs_done >= mirror.rv.n_obs_total) {
      continue; // fully observed: the series is closed and `prev_spot` freezes
    }
    const double r = std::log(spot / mirror.prev_spot);
    mirror.rv.sum_sq_log_returns_done += r * r;
    mirror.rv.n_obs_done += 1u;
    mirror.prev_spot = spot;
    mirror.rv.rv_done_dec = mirror.rv.annualization * mirror.rv.sum_sq_log_returns_done /
                            static_cast<double>(mirror.rv.n_obs_done);
  }
  live_swaps_.assign(book.swap_lots.begin(), book.swap_lots.end());
  signal_ts_ns_ = base.ts_ns();
  stepped_ = true;
}

std::vector<std::pair<std::string, double>>
StrangleVsVarswapStrategy::signals(const MarketSnapshot &base) const {
  // The cached accrual is as-of ONE snapshot. Handed any other, the swap greeks
  // would value this row's fixings against someone else's market — report
  // nothing instead. The engine never trips this: `record_signals(*base)` runs
  // on the same snapshot `on_step` was just given.
  const bool as_of = stepped_ && base.ts_ns() == signal_ts_ns_;

  double delta = kNaN;
  double gamma = kNaN;
  double vega = kNaN;
  double theta = kNaN;
  double rho = kNaN;
  if (as_of && !live_swaps_.empty()) {
    double sum_delta = 0.0;
    double sum_gamma = 0.0;
    double sum_vega = 0.0;
    double sum_theta = 0.0;
    double sum_rho = 0.0;
    bool priced = true;
    for (const SwapLot &lot : live_swaps_) {
      const SwapMirror *mirror = find_mirror(lot.id);
      const SurfaceRef surface = base.find(lot.uid);
      if (mirror == nullptr || mirror->desynced || surface == nullptr) {
        priced = false;
        break;
      }
      // The contract the engine's mark lane priced this row, rebuilt through the
      // one helper that also built the entry solve's — residual tenor plus the
      // fixings observed so far.
      const Result<DerivGreeks> greeks =
          detail::deriv_greeks_on_ref(surface, swap_contract(lot, base.ts_ns(), mirror->rv),
                                      kEngineSwapMarkCfg, DerivGreekBumps{});
      if (!greeks) {
        priced = false; // never a PARTIAL total: one unpriced lot voids the set
        break;
      }
      // Qty-scaled, exactly as the engine scales this lane's marks
      // (`pv_scaled = lot.qty * quote.pv`), so these are position dollars.
      sum_delta += lot.qty * greeks->delta;
      sum_gamma += lot.qty * greeks->gamma;
      sum_vega += lot.qty * greeks->vega;
      sum_theta += lot.qty * greeks->theta;
      sum_rho += lot.qty * greeks->rho;
    }
    if (priced) {
      delta = sum_delta;
      gamma = sum_gamma;
      vega = sum_vega;
      theta = sum_theta;
      rho = sum_rho;
    }
  }

  std::vector<std::pair<std::string, double>> out;
  out.reserve(8);
  out.emplace_back("swap_delta", delta);
  out.emplace_back("swap_gamma", gamma);
  out.emplace_back("swap_vega", vega);
  out.emplace_back("swap_theta", theta);
  out.emplace_back("swap_rho", rho);
  out.emplace_back("strangle_vega", as_of ? last_strangle_vega_ : kNaN);
  out.emplace_back("skipped_restrikes", static_cast<double>(skipped_restrikes_));
  out.emplace_back("skipped_swaps", static_cast<double>(skipped_swap_cycles_));
  return out;
}

} // namespace atx::vol

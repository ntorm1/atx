// atx-vol swap-leg toolkit — see swap_leg.hpp for the contracts. The bodies
// here were MOVED from the strangle-vs-varswap strategy (strangle_varswap.cpp)
// when the swap lane was generalized; the semantics are pinned by
// tests/swap_leg_test.cpp and, transitively, by the strangle-varswap suite.

#include "atx/vol/swap_leg.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

#include "atx/core/error.hpp"                  // Ok, Err, ErrorCode
#include "atx/vol/detail/deriv_ref_bridge.hpp" // deriv_price_on_ref, deriv_greeks_on_ref
#include "atx/vol/portfolio_pricer.hpp"        // SurfaceRef, kNsPerYear

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// `SwapLot::n_obs_total`'s ceiling, named so the fixing-count narrowing reads
// as a range check rather than a cast.
constexpr std::ptrdiff_t kUint32Max =
    static_cast<std::ptrdiff_t>(std::numeric_limits<std::uint32_t>::max());

// The engine's swap MARK config (`swap_price_cfg`, backtest.cpp), transcribed.
// The signals differentiate the marks the engine actually took, so they price
// through the engine's config rather than any strategy's entry config — which
// is documented as the ENTRY SOLVE's alone. At the default the two are the
// same object and the distinction is moot; set a non-default entry config and
// the signals still describe the position the run is being paid on.
const DerivConfig kEngineSwapMarkCfg{};

} // namespace

DerivContract swap_contract_for_lot(const SwapLot &lot, std::int64_t base_ts,
                                    const RealizedVarianceSpec &rv) noexcept {
  DerivContract contract;
  contract.kind = lot.kind;
  // The engine's `residual_T` verbatim, unsigned-differenced so the subtraction
  // is defined for any pair of timestamps rather than only for the live-lot
  // case strategies call it with.
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

Result<SwapLot> solve_cycle_swap(const SurfaceRef &surface, const CycleSwapRequest &req,
                                 double target_vega) {
  if (!(std::isfinite(target_vega) && target_vega != 0.0)) {
    return Err(ErrorCode::Unavailable,
               "solve_cycle_swap: target vega is non-finite or zero; no leg to size against");
  }
  if (surface == nullptr) {
    return Err(ErrorCode::Unavailable, "solve_cycle_swap: no surface for the underlier");
  }

  // The fixing schedule the ENGINE will actually observe. The swap pass runs on
  // the SHIFTED snapshot, so the first session after the open merely SEEDS the
  // series and accrues nothing; every session after that through expiry adds
  // one return. `n_obs_total` is therefore one FEWER than the count of sessions
  // in (open, expiry] — set it to the raw count and the lot reaches its expiry
  // an observation short, with every intermediate mark over-weighting the
  // future leg against the residual maturity it is priced at.
  const auto after_open =
      std::upper_bound(req.session_ts.begin(), req.session_ts.end(), req.open_ts_ns);
  const auto after_expiry =
      std::upper_bound(req.session_ts.begin(), req.session_ts.end(), req.expiry_ts_ns);
  const std::ptrdiff_t in_window = after_expiry - after_open;
  if (in_window < 2 || in_window - 1 > kUint32Max) {
    // A one-session cycle observes NO return at all: `n_obs_total` would be 0,
    // which the engine boundary rejects outright, and the lot would reach
    // expiry with an empty estimator — a NotFound that aborts the whole run. A
    // cycle too short to carry a variance swap runs options-only instead. The
    // upper bound is unreachable on any real calendar and exists so the
    // narrowing below can never wrap a schedule into a smaller, wrong one.
    return Err(ErrorCode::Unavailable,
               "solve_cycle_swap: cycle fixing window holds " + std::to_string(in_window) +
                   " session(s); at least one accruable return is required");
  }

  SwapLot lot;
  lot.uid = req.uid;
  lot.kind = req.kind;
  lot.strike_dec = 0.0; // solved below; 0 also makes the probe quote's PV harmless
  lot.cap_dec = req.cap_dec;
  lot.notional = req.notional;
  lot.qty = 0.0; // solved below
  lot.start_ts_ns = req.open_ts_ns;
  lot.expiry_ts_ns = req.expiry_ts_ns;
  lot.n_obs_total = static_cast<std::uint32_t>(in_window - 1);
  lot.annualization = req.annualization;

  // Nothing is realized at entry — the engine seeds this lot's series one step
  // from now — so the solve prices a purely forward-looking contract.
  RealizedVarianceSpec rv{};
  rv.annualization = req.annualization;
  rv.n_obs_total = lot.n_obs_total;

  // The fair strike is read off a quote rather than from the PricedSurface-
  // native `var_swap_fair_strike`, DELIBERATELY: `DerivQuote::fair_strike_dec`
  // IS the strike that prices this contract to PV = 0, and taking it from the
  // same `deriv_price_on_ref` bridge the engine's mark lane prices through is
  // the only way the two agree. The native entry derives its carry from the
  // fitted pillar list while the bridge derives it from the handle; striking
  // against one and marking against the other would open the swap at a
  // non-zero PV, and the whole of that artifact would land in the first step's
  // `swap_pnl` (a swap opens at zero cost, so its first mark carries the
  // entire entry difference).
  const Result<DerivQuote> fair =
      detail::deriv_price_on_ref(surface, swap_contract_for_lot(lot, lot.start_ts_ns, rv),
                                 req.deriv_cfg);
  if (!fair || !(std::isfinite(fair->fair_strike_dec) && fair->fair_strike_dec > 0.0)) {
    return Err(ErrorCode::Unavailable,
               "solve_cycle_swap: fair-strike solve failed or returned a non-positive strike");
  }
  lot.strike_dec = fair->fair_strike_dec;

  const Result<DerivGreeks> greeks = detail::deriv_greeks_on_ref(
      surface, swap_contract_for_lot(lot, lot.start_ts_ns, rv), req.deriv_cfg, DerivGreekBumps{});
  if (!greeks || !(std::isfinite(greeks->vega) && greeks->vega != 0.0)) {
    // Never a garbage qty: no vega, no leg.
    return Err(ErrorCode::Unavailable,
               "solve_cycle_swap: entry greeks failed or the swap vega is non-finite/zero");
  }

  // EQUAL VEGA. Both quantities are dollars per 1.00 of vol on their own leg,
  // so the ratio is a pure number and the sign carries: a long-vega target
  // sizes a long (variance-receiving) swap.
  const double qty = target_vega / greeks->vega;
  if (!std::isfinite(qty)) {
    return Err(ErrorCode::Unavailable, "solve_cycle_swap: sized quantity is non-finite");
  }
  lot.qty = qty;
  return Ok(lot);
}

void SwapSignalProbe::capture_pre_step(const PortfolioState &book) {
  ids_before_step_.clear();
  ids_before_step_.reserve(book.swap_lots.size());
  for (const SwapLot &lot : book.swap_lots) {
    ids_before_step_.push_back(lot.id);
  }
}

void SwapSignalProbe::refresh(const MarketSnapshot &base, const PortfolioState &book) {
  // Lots the engine settled this step took their terminal fixing and left the
  // book before the strategy stepped; their accrual is finished business.
  std::erase_if(mirrors_, [&book](const Mirror &mirror) {
    return std::none_of(book.swap_lots.begin(), book.swap_lots.end(),
                        [&mirror](const SwapLot &lot) { return lot.id == mirror.lot_id; });
  });
  for (const SwapLot &lot : book.swap_lots) {
    const auto found = std::find_if(mirrors_.begin(), mirrors_.end(),
                                    [&lot](const Mirror &m) { return m.lot_id == lot.id; });
    if (found == mirrors_.end()) {
      // FIRST SIGHT, and deliberately no fixing: the engine's swap pass will
      // not see this lot until the next step, and that step is the one that
      // seeds its series. Taking a fixing here would age every later mark by a
      // session.
      Mirror fresh;
      fresh.lot_id = lot.id;
      fresh.rv.annualization = lot.annualization;
      fresh.rv.n_obs_total = lot.n_obs_total;
      // ... UNLESS the lot was already in the book when this step began. Then
      // it is not new at all: the engine restored it, and its realized
      // variance with it, into a strategy that has never seen a fixing for it.
      // A clean accrual would describe a swap that had realized nothing — see
      // the desync contract in swap_leg.hpp.
      fresh.desynced = std::find(ids_before_step_.begin(), ids_before_step_.end(), lot.id) !=
                       ids_before_step_.end();
      mirrors_.push_back(fresh);
      continue;
    }
    Mirror &mirror = *found;
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

const SwapSignalProbe::Mirror *SwapSignalProbe::find_mirror(std::uint64_t lot_id) const noexcept {
  for (const Mirror &mirror : mirrors_) {
    if (mirror.lot_id == lot_id) {
      return &mirror;
    }
  }
  return nullptr;
}

void SwapSignalProbe::append_swap_greek_signals(
    const MarketSnapshot &base, std::vector<std::pair<std::string, double>> &out) const {
  // The cached accrual is as-of ONE snapshot. Handed any other, the greeks
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
      const Mirror *mirror = find_mirror(lot.id);
      const SurfaceRef surface = base.find(lot.uid);
      if (mirror == nullptr || mirror->desynced || surface == nullptr) {
        priced = false;
        break;
      }
      // The contract the engine's mark lane priced this row, rebuilt through
      // the one helper that also builds entry solves' — residual tenor plus
      // the fixings observed so far.
      const Result<DerivGreeks> greeks =
          detail::deriv_greeks_on_ref(surface, swap_contract_for_lot(lot, base.ts_ns(), mirror->rv),
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

  out.emplace_back("swap_delta", delta);
  out.emplace_back("swap_gamma", gamma);
  out.emplace_back("swap_vega", vega);
  out.emplace_back("swap_theta", theta);
  out.emplace_back("swap_rho", rho);
}

} // namespace atx::vol

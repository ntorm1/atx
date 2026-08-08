#include "atx/vol/deriv_book.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/detail/deriv_ref_bridge.hpp" // deriv_price_on_ref / deriv_greeks_on_ref

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

constexpr double kNaN = kPriceColumnNaN;

// A DerivGreeks in which NOTHING is claimed to have been computed. The struct
// default-initializes its sensitivities to 0.0 (Task 7's convention, where a
// fully-aged contract genuinely HAS zero market greeks), which at the portfolio
// layer would be indistinguishable from a measured zero. Every numeric field --
// the nine sensitivities AND the embedded centre quote's -- is therefore
// overwritten with the frame's "not computed" sentinel. `strip_nodes_used == 0`
// and `flags == None` are already exactly "no strip ran".
[[nodiscard]] DerivGreeks nan_greeks() noexcept {
  DerivGreeks g{};
  g.pv = kNaN;
  g.delta = kNaN;
  g.gamma = kNaN;
  g.vega = kNaN;
  g.volga = kNaN;
  g.vanna = kNaN;
  g.theta = kNaN;
  g.rho = kNaN;
  g.charm = kNaN;
  g.quote.fair_strike_dec = kNaN;
  g.quote.fair_strike_points = kNaN;
  g.quote.pv = kNaN;
  g.quote.undiscounted_expectation_dec = kNaN;
  g.quote.uncapped_var_dec = kNaN;
  g.quote.accrued_component_dec = kNaN;
  g.quote.future_component_dec = kNaN;
  g.quote.convexity_adjustment_dec = kNaN;
  g.quote.integration_error_est = kNaN;
  g.quote.vol_of_vol_used = kNaN;
  g.quote.cap_option_value_dec = kNaN;
  g.quote.strip_k_lo_used = kNaN;
  g.quote.strip_k_hi_used = kNaN;
  return g;
}

// Position-scale a computed greek block. The nine sensitivities and the pv are
// cash amounts and scale; `quote` is the per-contract centre diagnostic and is
// carried through VERBATIM (see deriv_book.hpp "Scaling").
[[nodiscard]] DerivGreeks scaled_greeks(const DerivGreeks &g, double qty) noexcept {
  DerivGreeks out = g;
  out.pv = qty * g.pv;
  out.delta = qty * g.delta;
  out.gamma = qty * g.gamma;
  out.vega = qty * g.vega;
  out.volga = qty * g.volga;
  out.vanna = qty * g.vanna;
  out.theta = qty * g.theta;
  out.rho = qty * g.rho;
  out.charm = qty * g.charm;
  return out;
}

// Pricer error -> lane status. The pricers report every malformed-contract
// rejection (T <= 0 on a live leg, a cap on an uncapped kind, a negative
// vol-of-vol, a non-positive bump) as InvalidArgument; everything else --
// OutOfRange carry, NotImplemented reserved engines, numeric blow-ups -- is a
// model/numeric failure on a well-formed contract.
[[nodiscard]] PriceStatus status_for(const Error &e) noexcept {
  return e.code() == ErrorCode::InvalidArgument ? PriceStatus::InvalidContract
                                                : PriceStatus::NumericError;
}

// Price ONE position. Never fails: every rejection is encoded in `status` with
// a NaN-filled row, which is what keeps a bad lane from failing the call.
[[nodiscard]] DerivPriceRow price_one(const SurfaceSet &surfaces, const DerivPosition &p,
                                      const DerivConfig &cfg, bool greeks,
                                      const DerivGreekBumps &bumps,
                                      const WingBandResolver &wing_band_of) {
  DerivPriceRow row{};
  row.id = p.id;
  row.uid = p.uid;
  row.pv = kNaN;
  row.fair_strike_dec = kNaN;
  row.greeks = nan_greeks();

  const SurfaceRef ref = surfaces.find(p.uid);
  if (ref == nullptr) {
    row.status = PriceStatus::ModelUnavailable;
    return row;
  }
  // Boundary validation of the one input the pricers never see. A non-finite
  // qty would scale a perfectly good mark into a NaN on an "Ok" lane and poison
  // the totals; qty == 0 is a legitimate flat position and is priced normally.
  if (!std::isfinite(p.qty)) {
    row.status = PriceStatus::InvalidContract;
    return row;
  }

  // FIT-C7 / Task C-6: this row's own certified wing band, when the caller
  // supplied a resolver; std::nullopt (an unset resolver) resolves the
  // mode-blind default, unchanged prior behaviour.
  const std::optional<double> wing_band = wing_band_of ? wing_band_of(p.uid) : std::nullopt;

  if (greeks) {
    const Result<DerivGreeks> g =
        detail::deriv_greeks_on_ref(ref, p.contract, cfg, bumps, wing_band);
    if (!g.has_value()) {
      row.status = status_for(g.error());
      return row;
    }
    row.pv = p.qty * g->pv;
    row.fair_strike_dec = g->quote.fair_strike_dec;
    row.greeks = scaled_greeks(*g, p.qty);
    row.status = PriceStatus::Ok;
    return row;
  }

  const Result<DerivQuote> q = detail::deriv_price_on_ref(ref, p.contract, cfg, wing_band);
  if (!q.has_value()) {
    row.status = status_for(q.error());
    return row;
  }
  row.pv = p.qty * q->pv;
  row.fair_strike_dec = q->fair_strike_dec;
  // The sensitivities stay NaN (not requested), but the centre quote is real
  // and carries the strip diagnostics a caller needs to audit this mark.
  row.greeks.quote = *q;
  row.status = PriceStatus::Ok;
  return row;
}

// Open the totals accumulator for the requested mode. Which columns are NaN is
// driven by the MODE, not by the row count: an empty greek-bearing book has a
// genuine zero delta, whereas a marks-only book has no delta at all.
[[nodiscard]] PriceTotals open_totals(bool greeks) noexcept {
  PriceTotals t{};
  if (greeks) {
    // `abs_vega` defaults to NaN ("not computed"); this reduction does compute
    // it, so open the accumulator explicitly (mirrors the option pricer's
    // reduce_price_totals).
    t.abs_vega = 0.0;
  } else {
    t.delta = t.gamma = t.vega = t.theta = t.rho = kNaN;
    t.vanna = t.volga = t.charm = kNaN; // abs_vega is already NaN by default
  }
  // `dP_dq` stays NaN unconditionally: the carry/borrow axis is defined on the
  // option pipeline's per-contract IV lane, and a swap has no such lane.
  return t;
}

// Accumulate one row. Non-Ok rows are EXCLUDED, not zeroed. A greek a lane
// itself reported as "not computed" (theta/charm on a contract too short to
// roll, vanna/charm under second_order=false) propagates its NaN into that
// column of the total -- the total is genuinely unknown, and a partial sum
// presented as complete is the failure mode being avoided. That NaN-poisoning
// is unchanged; GK-C9b adds the count of WHICH Ok rows caused it, per column,
// so a NaN total names its own cause instead of going silent.
void accumulate(DerivPriceFrame &frame, const DerivPriceRow &row, bool greeks) noexcept {
  if (row.status != PriceStatus::Ok) {
    return;
  }
  PriceTotals &t = frame.totals;
  t.pv += row.pv;
  if (greeks) {
    const DerivGreeks &g = row.greeks;
    t.delta += g.delta;
    t.gamma += g.gamma;
    const double leg_vega = g.vega;
    t.vega += leg_vega;
    t.abs_vega += std::fabs(leg_vega);
    t.theta += g.theta;
    frame.n_theta_excluded += std::isfinite(g.theta) ? 0u : 1u;
    t.rho += g.rho;
    t.vanna += g.vanna;
    frame.n_vanna_excluded += std::isfinite(g.vanna) ? 0u : 1u;
    t.volga += g.volga;
    t.charm += g.charm;
    frame.n_charm_excluded += std::isfinite(g.charm) ? 0u : 1u;
  }
  ++t.n_ok;
}

} // namespace

std::size_t DerivPriceFrame::n_ok() const noexcept {
  const auto n = std::count_if(rows.begin(), rows.end(), [](const DerivPriceRow &r) noexcept {
    return r.status == PriceStatus::Ok;
  });
  return static_cast<std::size_t>(n);
}

Result<DerivPriceFrame> price_deriv_book(const SurfaceSet &surfaces,
                                         std::span<const DerivPosition> book,
                                         const DerivConfig &cfg, bool greeks,
                                         const DerivGreekBumps &bumps,
                                         const WingBandResolver &wing_band_of) {
  // The only STRUCTURAL rejection: `PriceTotals::n_ok` is a uint32 counter, so a
  // book that cannot be counted in one would silently wrap. Refused before any
  // allocation (mirrors Portfolio::create's index-representability gate).
  if (book.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
    return Err(ErrorCode::InvalidArgument, "deriv book: position count exceeds UINT32_MAX");
  }

  DerivPriceFrame frame;
  frame.rows.reserve(book.size());
  frame.totals = open_totals(greeks);
  // Serial, fixed input order: the float-add association is the book's own
  // order, so the totals are bit-reproducible for a given input.
  for (const DerivPosition &p : book) {
    frame.rows.push_back(price_one(surfaces, p, cfg, greeks, bumps, wing_band_of));
    accumulate(frame, frame.rows.back(), greeks);
  }
  return Ok(std::move(frame));
}

PriceTotals combine_totals(const PriceTotals &a, const PriceTotals &b) noexcept {
  PriceTotals out{};
  out.pv = a.pv + b.pv;
  out.delta = a.delta + b.delta;
  out.gamma = a.gamma + b.gamma;
  out.vega = a.vega + b.vega;
  out.abs_vega = a.abs_vega + b.abs_vega;
  out.theta = a.theta + b.theta;
  out.rho = a.rho + b.rho;
  out.vanna = a.vanna + b.vanna;
  out.volga = a.volga + b.volga;
  out.charm = a.charm + b.charm;
  out.dP_dq = a.dP_dq + b.dP_dq;
  // Saturating rather than wrapping: an overflowed lane count would understate
  // the priced population, which is worse than pinning it at the maximum.
  constexpr std::uint64_t kMaxOk =
      static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
  const std::uint64_t n = static_cast<std::uint64_t>(a.n_ok) + static_cast<std::uint64_t>(b.n_ok);
  out.n_ok = static_cast<std::uint32_t>(n < kMaxOk ? n : kMaxOk);
  return out;
}

} // namespace atx::vol

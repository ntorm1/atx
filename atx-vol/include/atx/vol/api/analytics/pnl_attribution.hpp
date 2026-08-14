#pragma once

// pnl_attribution — additive P&L attribution on the CANONICAL PortfolioPricer /
// PricedSurface stack.
//
// ## What it does
//
// Given a book of `Position`s, a BASE and a SHIFTED `SurfaceSet` (one surface per
// underlying), `pnl_attribution` decomposes each position's base->shifted P&L into
// the Vola attribution vocabulary — one additive row per position, position-scaled
// (qty * multiplier * per-share) dollars:
//
//   spot        = pnl_delta + pnl_gamma          (first+second order in S)
//   vol_atf     = vega * a0                       (ATF / level smile move)
//   vol_skew    = vega * a1 * k                   (linear-in-k smile move)
//   vol_curv    = vega * a2 * k^2                 (quadratic-in-k smile move)
//   vol_resid   = vega * dvol - (atf+skew+curv)   (higher-order smile move; remainder)
//   vol_second  = pnl_volga + pnl_vanna           (2nd-order vol, kept whole)
//   rates       = pnl_rho
//   time        = pnl_theta + pnl_charm
//   unexplained = PnlFrame.pnl_unexplained        (the Taylor residual, verbatim)
//
// It carries the {level(ATF), skew, curvature, higher} vol vocabulary of the
// legacy European risk stack — the stack S4-T22 deleted, having established this
// module as its canonical replacement — and is built entirely on the PUBLIC
// `PortfolioPricer::pnl_explain` frame plus a cheap pivot-sampling pass — it touches
// no pricer / executor / risk internals.
//
// ## The vol split (per unique (uid, T) group, exact-identity discipline)
//
// `pnl_explain` already carries the per-position vega P&L as `pnl_vega = w * vega *
// dvol`, where `dvol = sigma_shift(K,T) - sigma_base(K,T)` is the per-contract smile
// move at the contract's own (K, T) evaluated at the COMMON base maturity (the
// column `PnlFrame::d_vol`). We split THAT dollar term into level / skew / curvature
// by fitting the smile MOVE to a quadratic through three pivots.
//
// For each (uid, T) present in the book we sample BOTH surfaces at three log-
// moneyness pivots k in {-k_ref, 0, +k_ref} (strikes K = F_base(T) * e^k, the SAME
// strikes on both surfaces) and form dsigma(k) = sigma_shift(K,T) - sigma_base(K,T).
// The exact quadratic dsigma(k) ~ a0 + a1*k + a2*k^2 through those three points is
//
//   a0 = dsigma(0)
//   a1 = (dsigma(+k_ref) - dsigma(-k_ref)) / (2 * k_ref)
//   a2 = (dsigma(+k_ref) + dsigma(-k_ref) - 2*dsigma(0)) / (2 * k_ref^2)
//
// (derived by evaluating the quadratic at -k_ref, 0, +k_ref and solving the 3x3 —
// see pnl_attribution.cpp). Per contract i on that group, with k_i = ln(K_i /
// F_base(T_i)), the vega P&L splits as fractions of the frame's own `pnl_vega`:
//
//   vol_atf_i  = pnl_vega_i * (a0        / dvol_i)
//   vol_skew_i = pnl_vega_i * (a1*k_i    / dvol_i)
//   vol_curv_i = pnl_vega_i * (a2*k_i^2  / dvol_i)
//   vol_resid_i = pnl_vega_i - vol_atf_i - vol_skew_i - vol_curv_i     (remainder)
//
// Because `pnl_vega_i = w*vega_i*dvol_i`, the fraction `(a0/dvol_i)` recovers the
// SAME `w*vega_i*a0` the direct formula would give, WITHOUT a second Greek solve
// (the frame already paid the base-greeks solve) and WITHOUT re-sampling vega — and
// it makes the four pieces sum to `pnl_vega_i` BIT-EXACTLY (vol_resid is the exact
// remainder). A contract whose `dvol_i == 0` (no smile move at its strike) has
// `pnl_vega_i == 0`; all four vega pieces are then 0. A group whose pivot IV is NaN
// (a domain-edge strike outside the surface) contributes a0=a1=a2=0, so its whole
// vega P&L falls to `vol_resid` (no NaN poisoning) and the group is counted once in
// `AttributionFrame::n_pivot_edge_fallback`.
//
// ## Identities (pinned by the tests)
//
//   * vol_atf + vol_skew + vol_curv + vol_resid == PnlFrame.pnl_vega  (bit-exact).
//   * spot / vol_second / rates / time / unexplained == their PnlFrame column
//     regroupings, bit-exact.
//   * sum of all attribution axes ~= pnl_total (differs from bit-exact only by the
//     IEEE reassociation between the regrouped axes and PnlFrame's own internal
//     `explained` accumulation — the same tolerance the pnl_explain column tests
//     use; see pnl_attribution_test.cpp).
//
// ## Determinism / thread-safety
//
// The only parallel section is the internal `pnl_explain` solve, which is bit-
// identical across `n_threads` (PortfolioPricer's guarantee). The pivot sampling and
// the per-position split are serial, deterministic pure reads; the totals are a
// serial fixed-input-order reduction. So the whole frame is bit-identical at any
// `n_threads`.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // Position, SurfaceSet, PriceStatus
#include "atx/vol/api/core/types.hpp"            // Result

namespace atx::vol {

// Attribution knobs. `k_ref` is the pivot half-width in log-moneyness (the smile is
// fit on k in {-k_ref, 0, +k_ref}); `n_threads` / `analytic_greeks` are threaded
// straight through to the internal `pnl_explain` (0 threads => hardware concurrency;
// analytic routes the base greeks through the Andersen-Lake analytic path).
struct AttributionOptions {
  double k_ref{0.10};
  unsigned n_threads{0};
  bool analytic_greeks{false};
};

// One attribution row per position (input order), position-scaled dollars. The nine
// axes (spot, vol_atf, vol_skew, vol_curv, vol_resid, vol_second, rates, time,
// unexplained) sum to `pnl_total` (up to IEEE reassociation — see header).
struct AttributionRow {
  std::uint64_t id{};
  std::uint32_t uid{};
  double pnl_total{};   // PnlFrame.pnl_total (the realized base->shifted reprice P&L)
  double spot{};        // pnl_delta + pnl_gamma
  double vol_atf{};     // vega * a0        (ATF / level)
  double vol_skew{};    // vega * a1*k
  double vol_curv{};    // vega * a2*k^2
  double vol_resid{};   // vega * (dvol - quadratic(k))  (remainder; preserves identity)
  double vol_second{};  // pnl_volga + pnl_vanna
  double rates{};       // pnl_rho
  double time{};        // pnl_theta + pnl_charm
  double unexplained{}; // PnlFrame.pnl_unexplained (verbatim)
  PriceStatus status{};
};

// Portfolio-level sums over the Ok rows (fixed input-order reduction), mirroring
// PnlTotals' shape with the attribution axes.
struct AttributionTotals {
  double pnl_total{};
  double spot{};
  double vol_atf{};
  double vol_skew{};
  double vol_curv{};
  double vol_resid{};
  double vol_second{};
  double rates{};
  double time{};
  double unexplained{};
  std::uint32_t n_ok{};
};

struct AttributionFrame {
  std::vector<AttributionRow> rows;
  AttributionTotals total{};
  // (uid, T) groups whose pivot sampling hit a NaN IV (domain edge) and therefore
  // attributed their whole vega P&L to `vol_resid` (a0=a1=a2=0). Counted once per
  // such group referenced by the book.
  std::size_t n_pivot_edge_fallback{0};

  [[nodiscard]] std::size_t size() const noexcept { return rows.size(); }
};

// Decompose the book's base->shifted P&L into the attribution axes. Dedups the book
// and runs `PortfolioPricer::pnl_explain(base, shifted)` ONCE, then the pivot split.
//
// @return InvalidArgument on a non-positive `k_ref`, or the propagated
//         dedup / pnl_explain error. An empty book yields an empty frame.
[[nodiscard]] Result<AttributionFrame> pnl_attribution(const std::vector<Position> &book,
                                                       const SurfaceSet &base,
                                                       const SurfaceSet &shifted,
                                                       const AttributionOptions &opts = {});

} // namespace atx::vol

#pragma once

// atx::engine::cost — optimizer_cost_terms: ONE cost calibration for replay and optimizer
// (Lane 6, plan §9 item 4). Maps the Lane 8 replay cost models (book/replay_cost.hpp) and
// the S6-5 borrow model (cost/borrow.hpp) onto the per-name trade-cost coefficients the
// optimizer prices (risk::TradeCostTerms, risk/cost_terms.hpp), so the book the optimizer
// chooses is charged, in replay, by the SAME law it was optimized against.
//
// ===========================================================================
//  Units — objective units are fractions of the book (return units, like −α)
// ===========================================================================
//  A trade Δ_i in weight units is x_i = Δ_i·B dollars on a book of B dollars. The replay
//  charges (SqrtImpactCost, per dollar):
//      cost_i(x) = |x|·(h_i + c_bps)·1e-4 + |x|·Y·σ_i·(|x| / adv_i)^δ
//  so, as a fraction of the book,
//      cost_i / B = κ_i |Δ_i| + c_i |Δ_i|^{1+δ},
//      κ_i = (h_i + c_bps)·1e-4,     c_i = Y·σ_i·(B / adv_i)^δ.
//  TradeCostTerms carries the 3/2-power law exactly (two rotated cones), i.e. δ = 1/2 — the
//  square-root law and the replay's default. Another δ is rejected (a silent refit would
//  break the one-calibration contract). FlatBpsCost maps to κ_i = bps·1e-4, c_i = 0.
//
//  A name whose liquidity row is unusable (ADV not finite and > 0, or a negative / NaN
//  σ / spread) is one the replay REFUSES to trade (it fills nothing). Its coefficients are
//  left 0 and it is flagged in `untradeable`: the caller must pin it at w_prev (e.g. a
//  discretize pin or an equality row), or the optimizer would plan a trade that never
//  fills. A finite max_participation caps each fill at max_participation·adv_i dollars;
//  `max_trade` carries it in weight units (+inf when uncapped) for the caller's box.
//
//  Borrow (cost/borrow.hpp): the per-period fee on the short leg is
//      fee_i = annual_rate_i · period_days / day_count_denom(day_count)
//  (the same accrual daily_borrow charges), and a locate of L_i dollars caps the short at
//  L_i / B in weight units (+inf ⇒ no locate limit).

#include <cmath>   // std::isfinite, std::sqrt
#include <limits>  // std::numeric_limits (+inf caps)
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/core/error.hpp" // Result, Status, Ok, Err
#include "atx/core/types.hpp" // f64, u8, usize

#include "atx/engine/book/replay_cost.hpp" // FlatBpsCost, SqrtImpactCost, LiquidityRow
#include "atx/engine/cost/borrow.hpp"      // DayCount, day_count_denom
#include "atx/engine/risk/cost_terms.hpp"  // risk::TradeCostTerms

namespace atx::engine::cost {

// Owning storage behind a risk::TradeCostTerms view. Every vector is empty (term off) or
// length M.
struct OptimizerCostTerms {
  std::vector<atx::f64> kappa_lin;      // κ_i (book fraction per unit |Δw|)
  std::vector<atx::f64> c_three_halves; // c_i (book fraction per unit |Δw|^{3/2})
  std::vector<atx::f64> borrow_fee;     // per-period fee on max(0, −w_i)
  std::vector<atx::f64> locate_cap;     // max(0, −w_i) ≤ cap_i (+inf ⇒ none)
  std::vector<atx::f64> w_prev;         // pre-trade book (length M)
  std::vector<atx::u8> untradeable;     // 1 ⇒ the replay will not fill this name: pin it
  std::vector<atx::f64> max_trade;      // |Δw_i| the replay can fill (+inf ⇒ uncapped)

  // Non-owning view for risk::solve_with_costs. Valid while *this is alive and unmodified.
  [[nodiscard]] risk::TradeCostTerms view() const noexcept {
    return risk::TradeCostTerms{kappa_lin, c_three_halves, borrow_fee, locate_cap, w_prev};
  }
};

namespace detail {

[[nodiscard]] inline atx::core::Status check_book(std::span<const atx::f64> w_prev,
                                                  atx::usize m) {
  namespace co = atx::core;
  if (!w_prev.empty() && w_prev.size() != m) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "optimizer cost terms: w_prev must be empty or length M");
  }
  for (const atx::f64 w : w_prev) {
    if (!std::isfinite(w)) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "optimizer cost terms: w_prev must be finite");
    }
  }
  return co::Ok();
}

[[nodiscard]] inline std::vector<atx::f64> book_or_zero(std::span<const atx::f64> w_prev,
                                                        atx::usize m) {
  return w_prev.empty() ? std::vector<atx::f64>(m, 0.0)
                        : std::vector<atx::f64>(w_prev.begin(), w_prev.end());
}

} // namespace detail

// FlatBpsCost ⇒ κ_i = bps·1e-4 on every name (no impact, no cap, every name tradeable).
[[nodiscard]] inline atx::core::Result<OptimizerCostTerms>
cost_terms_from_flat(const book::FlatBpsCost &model, atx::usize m,
                     std::span<const atx::f64> w_prev = {}) {
  namespace co = atx::core;
  ATX_TRY_VOID(detail::check_book(w_prev, m));
  OptimizerCostTerms out;
  out.kappa_lin.assign(m, model.proportional_rate().value_or(0.0));
  out.w_prev = detail::book_or_zero(w_prev, m);
  out.untradeable.assign(m, 0U);
  out.max_trade.assign(m, std::numeric_limits<atx::f64>::infinity());
  return co::Ok(std::move(out));
}

// SqrtImpactCost (δ must be 1/2) on the execution period's liquidity rows (length M) for a
// book of `book_dollars` > 0. See the header block for the mapping.
[[nodiscard]] inline atx::core::Result<OptimizerCostTerms>
cost_terms_from_sqrt_impact(const book::SqrtImpactCost &model,
                            std::span<const book::LiquidityRow> liquidity,
                            atx::f64 book_dollars, std::span<const atx::f64> w_prev = {}) {
  namespace co = atx::core;
  const atx::usize m = liquidity.size();
  ATX_TRY_VOID(detail::check_book(w_prev, m));
  if (!(book_dollars > 0.0) || !std::isfinite(book_dollars)) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "cost_terms_from_sqrt_impact: book_dollars must be finite and > 0");
  }
  if (model.impact().delta != 0.5) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "cost_terms_from_sqrt_impact: TradeCostTerms carries the 3/2-power law "
                   "only (impact delta must be 0.5)");
  }
  const atx::f64 y = model.impact().y;
  const atx::f64 comm = model.commission_bps();
  const atx::f64 cap = model.max_participation();
  OptimizerCostTerms out;
  out.kappa_lin.assign(m, 0.0);
  out.c_three_halves.assign(m, 0.0);
  out.w_prev = detail::book_or_zero(w_prev, m);
  out.untradeable.assign(m, 0U);
  out.max_trade.assign(m, std::numeric_limits<atx::f64>::infinity());
  for (atx::usize i = 0; i < m; ++i) {
    const book::LiquidityRow &row = liquidity[i];
    // The replay's own usability test: cost_fraction is NaN exactly when it refuses to fill.
    if (!std::isfinite(model.cost_fraction(0.0, row))) {
      out.untradeable[i] = 1U;
      out.max_trade[i] = 0.0;
      continue;
    }
    out.kappa_lin[i] = (row.half_spread_bps + comm) * 1e-4;
    out.c_three_halves[i] = y * row.daily_vol * std::sqrt(book_dollars / row.adv_dollars);
    if (std::isfinite(cap)) {
      out.max_trade[i] = cap * row.adv_dollars / book_dollars;
    }
  }
  return co::Ok(std::move(out));
}

// Add the S6-5 borrow leg: fee_i = annual_rate_i·period_days/denom(day_count) and, when
// `locate_dollars` is non-empty, cap_i = locate_dollars_i / book_dollars (+inf allowed).
// annual_rate must be length M = terms.w_prev.size(); entries finite and ≥ 0.
[[nodiscard]] inline atx::core::Status
add_borrow_terms(OptimizerCostTerms &terms, std::span<const atx::f64> annual_rate,
                 atx::f64 period_days, DayCount day_count,
                 std::span<const atx::f64> locate_dollars = {}, atx::f64 book_dollars = 1.0) {
  namespace co = atx::core;
  const atx::usize m = terms.w_prev.size();
  if (annual_rate.size() != m || (!locate_dollars.empty() && locate_dollars.size() != m)) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "add_borrow_terms: annual_rate / locate_dollars must be length M");
  }
  if (!(period_days >= 0.0) || !std::isfinite(period_days) || !(book_dollars > 0.0) ||
      !std::isfinite(book_dollars)) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "add_borrow_terms: period_days >= 0 and book_dollars > 0, both finite");
  }
  const atx::f64 scale = period_days / day_count_denom(day_count);
  std::vector<atx::f64> fee(m, 0.0);
  std::vector<atx::f64> cap;
  for (atx::usize i = 0; i < m; ++i) {
    if (!(annual_rate[i] >= 0.0) || !std::isfinite(annual_rate[i])) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "add_borrow_terms: annual_rate must be finite and >= 0");
    }
    fee[i] = annual_rate[i] * scale;
  }
  if (!locate_dollars.empty()) {
    cap.assign(m, 0.0);
    for (atx::usize i = 0; i < m; ++i) {
      if (!(locate_dollars[i] >= 0.0)) {
        return co::Err(co::ErrorCode::InvalidArgument,
                       "add_borrow_terms: locate_dollars must be >= 0 (+inf allowed)");
      }
      cap[i] = locate_dollars[i] / book_dollars;
    }
  }
  terms.borrow_fee = std::move(fee);
  terms.locate_cap = std::move(cap);
  return co::Ok();
}

} // namespace atx::engine::cost

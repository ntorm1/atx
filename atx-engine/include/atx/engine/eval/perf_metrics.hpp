#pragma once

// atx::engine::eval — ReturnMetrics + compute_return_metrics (Sprint S1-1).
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  ReturnMetrics is the evaluation-spine performance summary of one PnL stream.
//  It is a strict SUPERSET of combine::AlphaMetrics' realized-performance subset
//  plus the eval-specific risk/skill measures (Sortino, information ratio,
//  appraisal ratio, hit rate, and a monthly return grid). compute_return_metrics
//  is the pure free function that populates it.
//
// ===========================================================================
//  ONE Sharpe convention — the shared subset is DELEGATED, never re-derived
// ===========================================================================
//  sharpe, max_dd, calmar's drawdown/return, turnover, fitness, and holding are
//  taken DIRECTLY from combine::compute_metrics over the same inputs. This is the
//  load-bearing correctness requirement: there is exactly ONE Sharpe definition
//  in the engine (sqrt(252)*mean/std_pop over r[1..T)), so ReturnMetrics.sharpe
//  is BIT-EQUAL to combine's. The eval layer does NOT fork a second annualization
//  or std convention for the shared fields.
//
//  STRUCTURAL INDEX-0 EXCLUSION: like combine, the moment-based metrics here run
//  over r = pnl[1..T) — pnl[0] is the structural zero (no prior weight/close) and
//  is excluded from Sortino/IR/appraisal/hit-rate/monthly too, so every metric
//  shares the same observation window as the delegated Sharpe.
//
// ===========================================================================
//  Annualization scope (cfg.periods_per_year)
// ===========================================================================
//  cfg.periods_per_year (default 252) governs ONLY the eval-ADDED annualized
//  metrics: Sortino, IR, and appraisal. The delegated combine subset (sharpe,
//  calmar's annualized return) is FIXED at 252 inside combine — which is exactly
//  the default, so the two agree out of the box. Passing a non-252 pp rescales
//  only the eval-added ratios; it deliberately does NOT retro-fork combine's
//  Sharpe (one convention).
//
// ===========================================================================
//  NaN / degeneracy policy (documented)
// ===========================================================================
//  * No benchmark supplied (cfg.benchmark empty) -> ir = NaN.
//  * No residual series supplied (cfg.residual empty) -> appraisal = NaN.
//  * No positions supplied (the {} cfg path here passes an empty position span /
//    n_instruments = 0) -> combine's turnover-derived fields degenerate exactly
//    as combine documents; sharpe/max_dd/calmar remain valid.
//  * Sortino downside-deviation == 0 (no negative returns, or 0/1 observations)
//    -> sortino = 0.
//  * No dates supplied -> monthly grid is empty.
//  * Empty / single-element pnl -> r is empty; combine returns NaN sharpe and the
//    eval-added ratios degenerate (Sortino 0, hit_rate NaN from 0/0); the call is
//    total and never traps.

#include <cmath>   // std::sqrt, std::isnan, NaN
#include <limits>  // std::numeric_limits
#include <span>    // std::span
#include <vector>  // std::vector (monthly grid output)

#include "atx/core/datetime.hpp"             // atx::core::time::Timestamp
#include "atx/core/types.hpp"                // atx::f64, atx::usize, atx::i32, atx::u8
#include "atx/engine/combine/metrics.hpp"    // combine::compute_metrics (the delegated subset)
#include "atx/engine/eval/stats_ext.hpp"     // eval::mean_std_pop (the eval-added moments)

namespace atx::engine::eval {

// ===========================================================================
//  MonthlyCell — one (year, month) compounded return bucket of the monthly grid.
//  ret = ∏(1 + rᵢ) − 1 over the periods whose date falls in that calendar month.
// ===========================================================================
struct MonthlyCell {
  atx::i32 year;  // calendar year (UTC)
  atx::u8 month;  // calendar month 1..12 (UTC)
  atx::f64 ret;   // compounded simple return of the month
};

// ===========================================================================
//  ReturnMetricsCfg — inputs governing the eval-added metrics.
//
//  A default-constructed cfg ({}) is a VALID empty configuration: periods_per_year
//  = 252, no benchmark / residual / dates. All spans are non-owning and must
//  outlive the compute call. `dates`, when supplied, are aligned to r (i.e. one
//  Timestamp per observation in pnl[1..T) — caller passes the dates for periods
//  1..T-1, NOT period 0).
// ===========================================================================
struct ReturnMetricsCfg {
  atx::f64 periods_per_year = 252.0;
  std::span<const atx::f64> benchmark{};               // active-return baseline (IR)
  std::span<const atx::f64> residual{};                // residual series (appraisal)
  std::span<const atx::core::time::Timestamp> dates{}; // per-observation dates (monthly grid)
};

// ===========================================================================
//  ReturnMetrics — full evaluation-spine performance summary of one PnL stream.
//
//  sharpe/max_dd/calmar are the delegated combine subset (one convention);
//  sortino/ir/appraisal/hit_rate/monthly are eval-added. NaN where undefined per
//  the policy above. Rule of Zero: an owning aggregate (monthly is a small vector).
// ===========================================================================
struct ReturnMetrics {
  atx::f64 sharpe;                 // == combine::compute_metrics(...).sharpe (bit-equal)
  atx::f64 sortino;                // sqrt(pp)*mean(r)/downside_dev; 0 if no downside
  atx::f64 max_dd;                 // == combine(...).drawdown (peak-to-trough fraction)
  atx::f64 calmar;                 // ann_return / max(max_dd, eps)
  atx::f64 ir;                     // information ratio vs benchmark; NaN if none
  atx::f64 appraisal;              // appraisal ratio vs residual; NaN if none
  atx::f64 hit_rate;               // fraction of r > 0; NaN for 0 observations
  std::vector<MonthlyCell> monthly; // (year,month)->compounded return; empty if no dates
};

namespace detail {

// Div-by-zero floor for calmar's drawdown denominator (mirrors combine::kEps).
inline constexpr atx::f64 kCalmarEps = 1e-9;

// Sortino downside deviation over r vs target 0: sqrt( mean( min(rᵢ,0)² ) ),
// ascending-index. Empty r -> 0. Pure.
[[nodiscard]] inline atx::f64 downside_deviation(std::span<const atx::f64> r) noexcept {
  const atx::usize n = r.size();
  if (n == 0U) {
    return 0.0;
  }
  atx::f64 acc = 0.0;
  for (const atx::f64 v : r) {
    const atx::f64 d = (v < 0.0) ? v : 0.0; // only downside relative to target 0
    acc += d * d;
  }
  return std::sqrt(acc / static_cast<atx::f64>(n));
}

// Annualized ratio sqrt(pp)*mean(active)/std_pop(active) over `active`, ascending
// index. std_pop == 0 (flat) -> 0; empty -> 0. The shared form behind IR/appraisal.
[[nodiscard]] inline atx::f64 annualized_ratio(std::span<const atx::f64> active,
                                               atx::f64 periods_per_year) noexcept {
  const MeanStd ms = mean_std_pop(active);
  if (ms.std == 0.0) {
    return 0.0;
  }
  return std::sqrt(periods_per_year) * ms.mean / ms.std;
}

} // namespace detail

// ===========================================================================
//  compute_return_metrics — populate ReturnMetrics from a PnL stream + cfg.
//
//  PURE (a function of pnl + cfg). pnl is the realized-return stream; pnl[0] is
//  the structural zero excluded from every moment (r = pnl[1..T)). The shared
//  subset (sharpe, drawdown, turnover/fitness/holding) is delegated to
//  combine::compute_metrics with an empty position span and n_instruments = 0,
//  book_size 1.0 — matching the {}-cfg test path; sharpe/drawdown are therefore
//  bit-equal to combine. See header for the NaN policy and pp scope.
// ===========================================================================
[[nodiscard]] inline ReturnMetrics compute_return_metrics(std::span<const atx::f64> pnl,
                                                          const ReturnMetricsCfg &cfg) {
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();

  // --- Delegated subset: the ONE Sharpe / drawdown / return convention. ---
  // No positions are threaded through this entry point, so turnover-derived
  // fields degenerate exactly as combine documents; sharpe/drawdown/returns are
  // valid regardless. book_size = 1.0 (weights treated as gross-normalized).
  const combine::AlphaMetrics cm =
      combine::compute_metrics(pnl, std::span<const atx::f64>{}, /*n_instruments=*/0U,
                               /*book_size=*/1.0);
  const atx::f64 sharpe = cm.sharpe;
  const atx::f64 max_dd = cm.drawdown;
  // calmar = annualized return (252*mean, from combine) / floored drawdown.
  const atx::f64 dd_denom = (max_dd > detail::kCalmarEps) ? max_dd : detail::kCalmarEps;
  const atx::f64 calmar = cm.returns / dd_denom;

  // --- r = pnl[1..T): the structural-zero-excluded observation window. ---
  const std::span<const atx::f64> r =
      (pnl.size() >= 2U) ? pnl.subspan(1U) : std::span<const atx::f64>{};
  const atx::usize n_obs = r.size();

  // Sortino: sqrt(pp)*mean(r)/downside_dev; downside_dev == 0 -> 0.
  const MeanStd r_ms = mean_std_pop(r);
  const atx::f64 dd = detail::downside_deviation(r);
  const atx::f64 sortino = (dd == 0.0) ? 0.0 : std::sqrt(cfg.periods_per_year) * r_ms.mean / dd;

  // IR: active = r - benchmark (benchmark aligned to r). Absent -> NaN.
  atx::f64 ir = nan;
  if (!cfg.benchmark.empty() && cfg.benchmark.size() == n_obs && n_obs > 0U) {
    std::vector<atx::f64> active(n_obs);
    for (atx::usize i = 0U; i < n_obs; ++i) {
      active[i] = r[i] - cfg.benchmark[i];
    }
    ir = detail::annualized_ratio(active, cfg.periods_per_year);
  }

  // Appraisal: ratio over the supplied residual series. Absent -> NaN.
  const atx::f64 appraisal =
      cfg.residual.empty() ? nan : detail::annualized_ratio(cfg.residual, cfg.periods_per_year);

  // Hit rate: count(rᵢ > 0) / n_obs, ascending index. 0 observations -> NaN.
  atx::f64 hit_rate = nan;
  if (n_obs > 0U) {
    atx::usize wins = 0U;
    for (const atx::f64 v : r) {
      if (v > 0.0) {
        ++wins;
      }
    }
    hit_rate = static_cast<atx::f64>(wins) / static_cast<atx::f64>(n_obs);
  }

  // Monthly grid: compound r by (year, month). Empty when no aligned dates.
  std::vector<MonthlyCell> monthly;
  if (!cfg.dates.empty() && cfg.dates.size() == n_obs) {
    for (atx::usize i = 0U; i < n_obs; ++i) {
      const atx::core::time::CivilTime ct = atx::core::time::to_civil_utc(cfg.dates[i]);
      const atx::i32 y = ct.date.year;
      const atx::u8 m = static_cast<atx::u8>(ct.date.month);
      // Append to the current month-cell if it matches the last; else open a new
      // cell. dates are assumed ascending (chronological), so equal (y,m) runs are
      // contiguous — one linear pass, no map, deterministic order.
      if (!monthly.empty() && monthly.back().year == y && monthly.back().month == m) {
        monthly.back().ret = (1.0 + monthly.back().ret) * (1.0 + r[i]) - 1.0;
      } else {
        monthly.push_back(MonthlyCell{y, m, r[i]});
      }
    }
  }

  return ReturnMetrics{sharpe, sortino, max_dd, calmar, ir, appraisal, hit_rate, std::move(monthly)};
}

} // namespace atx::engine::eval

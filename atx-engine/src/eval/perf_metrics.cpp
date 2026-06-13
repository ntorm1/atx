#include "atx/engine/eval/perf_metrics.hpp"

#include <limits>  // std::numeric_limits
#include <utility> // std::move (monthly grid into the result aggregate)

#include "atx/engine/combine/metrics.hpp"    // combine::compute_metrics (the delegated subset)

namespace atx::engine::eval {

namespace detail {

[[nodiscard]] std::vector<MonthlyCell>
build_monthly_grid(std::span<const atx::f64> r,
                   std::span<const atx::core::time::Timestamp> dates) {
  std::vector<MonthlyCell> monthly;
  if (dates.empty() || dates.size() != r.size()) {
    return monthly;
  }
  for (atx::usize i = 0U; i < r.size(); ++i) {
    const atx::core::time::CivilTime ct = atx::core::time::to_civil_utc(dates[i]);
    const atx::i32 y = ct.date.year;
    // SAFETY: ct.date.month is the civil month, guaranteed ∈ [1, 12] by
    //         to_civil_utc (civil_from_days yields m ∈ [1, 12]); the explicit
    //         narrowing cast to u8 cannot lose information and silences
    //         -Wconversion on non-MSVC toolchains.
    const atx::u8 m = static_cast<atx::u8>(ct.date.month);
    if (!monthly.empty() && monthly.back().year == y && monthly.back().month == m) {
      monthly.back().ret = (1.0 + monthly.back().ret) * (1.0 + r[i]) - 1.0;
    } else {
      monthly.push_back(MonthlyCell{y, m, r[i]});
    }
  }
  return monthly;
}

} // namespace detail

[[nodiscard]] ReturnMetrics compute_return_metrics(std::span<const atx::f64> pnl,
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
  std::vector<MonthlyCell> monthly = detail::build_monthly_grid(r, cfg.dates);

  return ReturnMetrics{sharpe, sortino, max_dd, calmar, ir, appraisal, hit_rate, std::move(monthly)};
}

} // namespace atx::engine::eval

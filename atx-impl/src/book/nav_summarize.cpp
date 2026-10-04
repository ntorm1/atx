// NAV replay: summaries (months, moments, summarize_nav).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace {
struct Moments {
  usize n{}; f64 mean{}, m2{};
  void add(f64 x) {
    ++n; const f64 d = x - mean; mean += d / static_cast<f64>(n); m2 += d * (x - mean);
  }
  [[nodiscard]] f64 sd() const { return n > 1 ? std::sqrt(m2 / static_cast<f64>(n - 1)) : nan; }
  [[nodiscard]] f64 sharpe() const {
    return n > 1 && m2 > 0 ? mean / sd() * std::sqrt(252.0) : nan;
  }
};
void accumulate_day(const NavReplayDay& d, NavSummary& s) {
  s.trade_cost_dollars += d.trade_cost_dollars; s.linear_cost_dollars += d.linear_cost_dollars;
  s.impact_cost_dollars += d.impact_cost_dollars;
  s.unrationed_cost_dollars += d.unrationed_cost_dollars;
  s.borrow_dollars += d.borrow_dollars; s.writeoff_dollars += d.writeoff_dollars;
  s.fills += d.fills; s.capped_fills += d.capped_fills; s.blocked_absent += d.blocked_absent;
  s.blocked_liquidity += d.blocked_liquidity; s.fallback_vol_fills += d.fallback_vol_fills;
  s.unrationed_unpriced += d.unrationed_unpriced; s.unfilled_dollars += d.unfilled_dollars;
  s.max_gross_leverage = std::max(s.max_gross_leverage, d.gross_leverage);
  s.max_abs_net_leverage = std::max(s.max_abs_net_leverage, std::abs(d.net_leverage));
  s.max_stale_names = std::max(s.max_stale_names, d.stale_names);
  s.max_stale_gross_fraction = std::max(s.max_stale_gross_fraction,
      (d.stale_long_dollars + d.stale_short_dollars) / d.posttrade_nav);
  s.min_cash_ratio = std::min(s.min_cash_ratio, d.cash_ratio);
  s.long_financing_dollars += d.long_financing_dollars;
  for (usize k = 0; k < s.short_financing_by_tier.size(); ++k) {
    s.short_financing_by_tier[k] += d.short_financing_by_tier[k];
    s.member_tier_days[k] += d.member_tiers[k];
  }
  s.missing_predictor_short_name_days += d.missing_predictor_shorts;
  s.missing_predictor_member_days += d.member_missing_predictors;
  s.blocked_short_name_decisions += d.blocked_short_names;
  s.blocked_short_dollars += d.blocked_short_dollars;
}
// Each tier's share of pre-mark short dollars on a return row with shorts.
struct TierShares {
  std::array<std::vector<f64>, 3> share;
  void add(const NavReplayDay& d) {
    const f64 total = d.short_dollars_by_tier[0] + d.short_dollars_by_tier[1] +
                      d.short_dollars_by_tier[2];
    if (!(total > 0)) return;
    for (usize k = 0; k < share.size(); ++k) share[k].push_back(d.short_dollars_by_tier[k] / total);
  }
  void summarize(NavSummary& s) {
    s.short_share_sessions = share[0].size();
    for (usize k = 0; k < share.size(); ++k) {
      auto& v = share[k];
      f64 sum = 0;
      for (const f64 x : v) sum += x;
      std::sort(v.begin(), v.end());
      s.short_share_mean[k] = v.empty() ? nan : sum / static_cast<f64>(v.size());
      s.short_share_p95[k] = detail::sorted_quantile(v, .95);
    }
  }
};
bool within_target(f64 turnover) { return turnover <= nav_monthly_turnover_target; }
void summarize_months(const std::map<u32, NavMonth>& months, u32 deployment_month,
                      NavSummary& s) {
  f64 all = 0, ex = 0; usize count = 0, ex_count = 0;
  for (auto [id, m] : months) {
    m.is_deployment_month = s.deployed && id == deployment_month;
    s.months.push_back(m);
    if (!m.execution_sessions) continue;
    ++count; all += m.one_way_turnover;
    s.max_monthly_turnover = std::max(s.max_monthly_turnover, m.one_way_turnover);
    s.months_within_target += within_target(m.one_way_turnover) ? 1U : 0U;
    if (m.is_deployment_month) continue;
    ++ex_count; ex += m.one_way_turnover;
    s.max_monthly_turnover_ex_deployment =
        std::max(s.max_monthly_turnover_ex_deployment, m.one_way_turnover);
    s.months_within_target_ex_deployment += within_target(m.one_way_turnover) ? 1U : 0U;
  }
  s.execution_months = count;
  s.mean_monthly_turnover = count ? all / static_cast<f64>(count) : nan;
  s.mean_monthly_turnover_ex_deployment = ex_count ? ex / static_cast<f64>(ex_count) : nan;
  if (!ex_count) s.max_monthly_turnover_ex_deployment = nan;
  // NaN compares false: an empty set never meets a target.
  s.meets_turnover_target_mean = within_target(s.mean_monthly_turnover);
  s.meets_turnover_target_mean_ex_deployment =
      within_target(s.mean_monthly_turnover_ex_deployment);
  s.meets_turnover_target_all_months = count && within_target(s.max_monthly_turnover);
  s.meets_turnover_target_all_months_ex_deployment =
      ex_count && within_target(s.max_monthly_turnover_ex_deployment);
}
} // namespace
co::Result<NavSummary> summarize_nav(const NavReplayResult& result,
                                     const NavTurnoverLimits& limits) {
  const auto& days = result.days;
  if (days.size() < 3)
    return co::Err(co::ErrorCode::InvalidArgument, "nav summary: fewer than three sessions");
  if (!std::isfinite(limits.daily_mean_max) || !(limits.daily_mean_max > 0) ||
      !std::isfinite(limits.daily_p95_max) || !(limits.daily_p95_max > 0))
    return co::Err(co::ErrorCode::InvalidArgument, "nav summary: daily turnover ceilings");
  try {
    NavSummary s; s.min_cash_ratio = inf; s.daily_limits = limits;
    TierShares shares; f64 net_leverage = 0, abs_net_leverage = 0;
    Moments net, gross; std::vector<f64> series; series.reserve(days.size());
    std::vector<f64> daily_gmv; daily_gmv.reserve(days.size());
    s.daily_turnover_max_session = 0;
    std::map<u32, NavMonth> months;
    struct YearNav { usize observations{}; f64 first{}, last{}; };
    std::map<i32, YearNav> years;
    f64 peak = days.front().pretrade_nav, leverage = 0, held = 0, stale = 0;
    for (usize k = 0; k < days.size(); ++k) {
      const auto& d = days[k];
      if (d.executed || d.decision) {
        auto& m = months[d.calendar_month]; m.month = d.calendar_month;
        if (d.executed) {
          ++m.execution_sessions; m.traded_sessions += d.traded_dollars > 0 ? 1U : 0U;
          m.one_way_turnover += d.one_way_turnover; m.traded_dollars += d.traded_dollars;
          s.total_actual_turnover += d.one_way_turnover;
        }
        if (d.decision) {
          ++m.decision_sessions; m.planned_turnover += d.planned_turnover;
          s.total_planned_turnover += d.planned_turnover;
        }
      }
      if (d.executed && d.session_index != result.deployment_index) {
        if (d.pretrade_gross_dollars > 0) {
          daily_gmv.push_back(d.one_way_turnover_gmv);
          if (daily_gmv.size() == 1 || d.one_way_turnover_gmv > s.daily_turnover_max) {
            s.daily_turnover_max = d.one_way_turnover_gmv;
            s.daily_turnover_max_session = d.session;
          }
        } else {
          ++s.daily_turnover_zero_gmv_sessions;
        }
      }
      accumulate_day(d, s);
      peak = std::max(peak, d.pretrade_nav);
      s.max_drawdown = std::max(s.max_drawdown, 1.0 - d.pretrade_nav / peak);
      if (!d.return_observation || k == 0) continue;
      net.add(d.net_return); gross.add(d.gross_return); series.push_back(d.net_return);
      s.summed_trade_cost_return += d.trade_cost_return;
      s.summed_borrow_return += d.borrow_return; s.summed_writeoff_return += d.writeoff_return;
      s.summed_long_financing_return += d.long_financing_return;
      shares.add(d);
      // Exposure that earned this row's return: the previous session's closing book.
      leverage += days[k - 1].gross_leverage; held += static_cast<f64>(days[k - 1].held_names);
      net_leverage += days[k - 1].net_leverage;
      abs_net_leverage += std::abs(days[k - 1].net_leverage);
      stale += static_cast<f64>(d.stale_names);
      auto& y = years[static_cast<i32>(detail::calendar_month(d.session) / 100U)];
      if (y.observations++ == 0) y.first = days[k - 1].pretrade_nav;
      y.last = d.pretrade_nav;
    }
    if (net.n < 2) return co::Err(co::ErrorCode::Unavailable, "nav summary: too few return rows");
    const f64 n = static_cast<f64>(net.n);
    s.observations = net.n; s.net_sharpe = net.sharpe(); s.gross_sharpe = gross.sharpe();
    s.mean_daily_net = net.mean; s.ann_mean = net.mean * 252;
    s.ann_vol = net.sd() * std::sqrt(252.0);
    s.final_nav = days.back().pretrade_nav;
    s.total_net_return = s.final_nav / days.front().pretrade_nav - 1;
    s.cagr = std::pow(s.final_nav / days.front().pretrade_nav, 252.0 / n) - 1;
    const auto inference = hac::mean_inference(std::span<const f64>{series},
                                                hac::Kernel::BartlettV1, 5, true, true);
    s.hac_defined = inference.defined != 0; s.hac_t = s.hac_defined ? inference.t : nan;
    s.hac_lag = inference.lag;
    s.mean_gross_leverage = leverage / n; s.mean_held_names = held / n;
    s.mean_stale_names = stale / n;
    s.mean_net_leverage = net_leverage / n; s.mean_abs_net_leverage = abs_net_leverage / n;
    shares.summarize(s);
    for (const auto& [year, v] : years)
      s.years.push_back({year, v.observations, v.last / v.first - 1});
    u32 deployment_month = 0;
    const usize first = days.front().session_index;
    if (result.deployment_index >= first && result.deployment_index - first < days.size()) {
      const auto& d = days[result.deployment_index - first];
      s.deployed = true; s.deployment_session = d.session; deployment_month = d.calendar_month;
      s.deployment_turnover = d.one_way_turnover; s.deployment_dollars = d.traded_dollars;
    }
    summarize_months(months, deployment_month, s);
    s.meets_sharpe_target = s.net_sharpe >= nav_sharpe_target;
    // Daily GMV turnover (executed fills only), deployment session excluded.
    f64 gmv_sum = 0;
    for (const f64 v : daily_gmv) gmv_sum += v;
    std::sort(daily_gmv.begin(), daily_gmv.end());
    s.daily_turnover_sessions = daily_gmv.size();
    s.daily_turnover_mean = daily_gmv.empty() ? nan : gmv_sum / static_cast<f64>(daily_gmv.size());
    s.daily_turnover_median = detail::sorted_quantile(daily_gmv, .5);
    s.daily_turnover_p95 = detail::sorted_quantile(daily_gmv, .95);
    if (daily_gmv.empty()) s.daily_turnover_max = nan;
    // NaN compares false: an empty or undefined statistic never meets a ceiling.
    s.meets_daily_turnover_mean = s.daily_turnover_mean <= limits.daily_mean_max;
    s.meets_daily_turnover_p95 = s.daily_turnover_p95 <= limits.daily_p95_max;
    return co::Ok(std::move(s));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav summary: allocation failed");
  }
}
} // namespace atx::impl::strategy

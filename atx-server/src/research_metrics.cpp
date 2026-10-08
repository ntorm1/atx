#include "research_detail.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace atx::server::research_detail {
namespace {
Json finite(double value) { return std::isfinite(value) ? Json(value) : Json(nullptr); }

Json reconcile(const Evidence& evidence, std::stop_token stop) {
  // The canonical ATX NAV writer publishes 17 significant digits. This admits decimal
  // serialization roundoff while rejecting materially inconsistent return evidence.
  constexpr double tolerance = 1e-10;
  const auto column = evidence.provenance.value("nav_column", std::string{});
  if (column != "pretrade_nav" && column != "engine interval.nav")
    return {{"status", "unavailable"},
            {"reason", "posttrade valuations have a different fee clock"}};
  std::size_t checked = 0, mismatches = 0;
  double maximum_error = 0;
  for (std::size_t i = 1; i < evidence.days.size(); ++i) {
    if (i % 256 == 0) checkpoint(stop, {}, 0, {});
    if (!evidence.days[i].return_observation) continue;
    ++checked;
    const auto expected = evidence.days[i].nav / evidence.days[i - 1].nav - 1.0;
    const auto error = std::abs(expected - evidence.days[i].net_return);
    maximum_error = std::max(maximum_error, error);
    if (error > tolerance) ++mismatches;
  }
  if (mismatches)
    throw std::invalid_argument("NAV returns do not reconcile: " + std::to_string(mismatches) +
                                " qualified rows exceed absolute tolerance 1e-10");
  return {{"status", checked ? "passed" : "unavailable"}, {"checked_rows", checked},
      {"mismatched_rows", 0}, {"max_absolute_error", maximum_error},
      {"absolute_tolerance", tolerance}};
}

struct Statistics {
  std::size_t count{};
  std::size_t wins{};
  double mean{};
  double m2{};
  double downside_squared{};
  double maximum_drawdown{};
  std::vector<double> returns;
  std::vector<double> drawdown;
  std::map<std::string, double> months;
};

Statistics measure(const Evidence& evidence, std::stop_token stop) {
  Statistics result;
  auto peak = evidence.days.front().nav;
  result.drawdown.reserve(evidence.days.size());
  for (std::size_t i = 0; i < evidence.days.size(); ++i) {
    if (i % 256 == 0) checkpoint(stop, {}, 0, {});
    const auto& day = evidence.days[i];
    peak = std::max(peak, day.nav);
    const auto drawdown = day.nav / peak - 1.0;
    result.drawdown.push_back(drawdown);
    result.maximum_drawdown = std::max(result.maximum_drawdown, -drawdown);
    if (!day.return_observation || i == 0) continue;
    ++result.count;
    const auto delta = day.net_return - result.mean;
    result.mean += delta / static_cast<double>(result.count);
    result.m2 += delta * (day.net_return - result.mean);
    if (day.net_return > 0) ++result.wins;
    if (day.net_return < 0) result.downside_squared += day.net_return * day.net_return;
    result.returns.push_back(day.net_return);
    const auto month = day.date.substr(0, 7);
    auto [found, inserted] = result.months.try_emplace(month, 1.0);
    (void)inserted;
    found->second *= 1.0 + day.net_return;
  }
  return result;
}

Json metrics(const Evidence& evidence, const Statistics& stats) {
  const auto first = evidence.days.front().nav;
  const auto last = evidence.days.back().nav;
  const auto count = static_cast<double>(stats.count);
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  const auto cagr = stats.count ? std::expm1(std::log(last / first) * 252.0 / count) : nan;
  const auto sd = stats.count >= 2 ? std::sqrt(std::max(0.0, stats.m2 / (count - 1.0))) : nan;
  const auto downside = stats.count ? std::sqrt(stats.downside_squared / count) : nan;
  return {{"initial_nav", finite(first)}, {"final_nav", finite(last)},
      {"total_return", finite(last / first - 1.0)}, {"cagr", finite(cagr)},
      {"annualized_return", stats.count ? finite(stats.mean * 252.0) : Json(nullptr)},
      {"annualized_volatility", finite(sd * std::sqrt(252.0))},
      {"sharpe_ratio", stats.count >= 2 && sd > 0
          ? finite(stats.mean / sd * std::sqrt(252.0)) : Json(nullptr)},
      {"sortino_ratio", stats.count >= 2 && downside > 0
          ? finite(stats.mean / downside * std::sqrt(252.0)) : Json(nullptr)},
      {"max_drawdown", finite(stats.maximum_drawdown)},
      {"calmar_ratio", stats.maximum_drawdown > 0
          ? finite(cagr / stats.maximum_drawdown) : Json(nullptr)},
      {"win_rate", stats.count ? finite(static_cast<double>(stats.wins) / count) : Json(nullptr)},
      {"observations", stats.count}, {"sessions", evidence.days.size()}};
}

Json histogram(const Statistics& stats, std::size_t max_points) {
  if (stats.returns.empty()) return Json::array();
  const auto [minimum, maximum] = std::minmax_element(stats.returns.begin(), stats.returns.end());
  const auto bins = std::min(std::size_t{20}, max_points);
  const auto lower = *minimum == *maximum ? *minimum - 0.000001 : *minimum;
  const auto upper = *minimum == *maximum ? *maximum + 0.000001 : *maximum;
  const auto range = upper - lower;
  if (!(range > 0) || !std::isfinite(range)) {
    return Json::array({{{"lower", *minimum}, {"upper", *maximum},
                         {"count", stats.returns.size()}}});
  }
  std::vector<std::size_t> counts(bins, 0);
  for (const auto value : stats.returns) {
    // Normalize before multiplying: dividing the range into bins can underflow to zero.
    const auto fraction = (value - lower) / range;
    if (!std::isfinite(fraction))
      throw std::runtime_error("cannot normalize histogram return");
    const auto position = std::clamp(fraction * static_cast<double>(bins),
                                     0.0, static_cast<double>(bins - 1));
    ++counts[static_cast<std::size_t>(position)];
  }
  Json result = Json::array();
  for (std::size_t i = 0; i < bins; ++i)
    result.push_back({{"lower", lower + range *
        (static_cast<double>(i) / static_cast<double>(bins))},
        {"upper", lower + range * (static_cast<double>(i + 1) / static_cast<double>(bins))},
        {"count", counts[i]}});
  return result;
}
} // namespace

Json summarize(const Evidence& evidence, const ResearchOptions& options,
               std::stop_token stop, const Progress& progress) {
  checkpoint(stop, progress, 0.9, "Summarizing complete results and bounding chart payloads");
  if (evidence.days.empty()) throw std::invalid_argument("no research observations");
  const auto reconciliation = reconcile(evidence, stop);
  const auto stats = measure(evidence, stop);
  Json curve = Json::array(), drawdown = Json::array(), months = Json::array();
  Json daily_returns = Json::array();
  for (std::size_t i = 1; i < evidence.days.size(); ++i) {
    if (i % 256 == 0) checkpoint(stop, {}, 0, {});
    if (evidence.days[i].return_observation)
      daily_returns.push_back({{"date", evidence.days[i].date},
          {"period_start_date", evidence.days[i - 1].date}, {"value", evidence.days[i].net_return}});
  }
  const auto displayed = std::min(evidence.days.size(), options.max_result_points);
  for (std::size_t i = 0; i < displayed; ++i) {
    const auto index = displayed <= 1 ? std::size_t{0} :
        static_cast<std::size_t>(static_cast<double>(i) *
            static_cast<double>(evidence.days.size() - 1) / static_cast<double>(displayed - 1));
    curve.push_back({{"date", evidence.days[index].date}, {"value", evidence.days[index].nav}});
    drawdown.push_back({{"date", evidence.days[index].date},
                        {"value", stats.drawdown[index]}});
  }
  for (const auto& [month, growth] : stats.months) {
    if (months.size() >= options.max_result_points) break;
    months.push_back({{"month", month}, {"value", finite(growth - 1.0)}});
  }
  auto provenance = evidence.provenance;
  provenance["nav_return_reconciliation"] = reconciliation;
  provenance["series_downsampled"] = displayed < evidence.days.size();
  provenance["series_points"] = displayed;
  provenance["series_method"] =
      "uniform indices including first and last; metrics use complete rows";
  provenance["monthly_returns_truncated"] = stats.months.size() > months.size();
  provenance["risk_free_rate"] = 0.0;
  provenance["daily_returns_complete"] = true;
  provenance["daily_returns_schema"] = "atx.daily-return/v1";
  provenance["volatility_ddof"] = 1;
  provenance["sortino_convention"] = "zero target; downside squared averaged over all return rows";
  Json result{{"schema_version", "atx.research-result/v1"}, {"metrics", metrics(evidence, stats)},
      {"equity_curve", std::move(curve)}, {"drawdown", std::move(drawdown)},
      {"returns_histogram", histogram(stats, options.max_result_points)},
      {"monthly_returns", std::move(months)}, {"trades", evidence.trades},
      {"daily_returns", std::move(daily_returns)},
      {"positions", evidence.positions}, {"factor_exposures", Json::array()},
      {"provenance", std::move(provenance)}};
  if (evidence.provenance.contains("portfolio_geometry"))
    result["portfolio_allocations"] = evidence.portfolio_allocations;
  return result;
}
} // namespace atx::server::research_detail

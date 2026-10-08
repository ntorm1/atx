#include "research_dataset.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>

#include "atx/engine/book/replay.hpp"

namespace atx::server::research_detail {
namespace {
namespace alpha = atx::engine::alpha;
namespace book = atx::engine::book;

std::string session_date(std::int64_t key) {
  return date_text(std::chrono::floor<std::chrono::days>(
      std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{key}}));
}

alpha::Panel score_panel(const DatasetPanel& data) {
  const auto count = data.panel.dates() - data.score_begin;
  const auto names = data.panel.instruments();
  std::vector<std::span<const double>> columns;
  for (const auto& field : dataset_fields())
    columns.push_back(data.panel.field_all(data.panel.field_id(field).value()).subspan(
        data.score_begin * names));
  std::vector<std::uint8_t> universe(count * names, 0);
  for (std::size_t day = 0; day < count; ++day)
    for (std::size_t name = 0; name < names; ++name)
      universe[day * names + name] = static_cast<std::uint8_t>(
          data.panel.in_universe(day + data.score_begin, name));
  auto panel = alpha::Panel::create_borrowed(count, names, dataset_fields(),
      std::move(columns), std::move(universe));
  if (!panel) throw std::runtime_error(panel.error().message());
  return std::move(*panel);
}

void neutral_weights(const DatasetPanel& data, const alpha::SignalSet& signal,
    std::size_t day, std::span<double> weights) {
  const auto values = signal.alpha_cross_section(0, day + data.score_begin);
  double scale = 0;
  std::size_t count = 0;
  for (std::size_t name = 0; name < weights.size(); ++name)
    if (data.panel.in_universe(day + data.score_begin, name) && std::isfinite(values[name])) {
      scale = std::max(scale, std::abs(values[name])); ++count;
    }
  if (!(scale > 0) || count < 2) return;
  double mean = 0;
  for (std::size_t name = 0; name < weights.size(); ++name)
    if (data.panel.in_universe(day + data.score_begin, name) && std::isfinite(values[name]))
      mean += values[name] / scale;
  mean /= static_cast<double>(count);
  double gross = 0;
  for (std::size_t name = 0; name < weights.size(); ++name) {
    if (data.panel.in_universe(day + data.score_begin, name) && std::isfinite(values[name]))
      weights[name] = values[name] / scale - mean;
    gross += std::abs(weights[name]);
  }
  if (gross > 0) for (auto& value : weights) value /= gross;
}

void append_accounting(Evidence& evidence, const book::ReplayResult& replay,
    const alpha::Panel& panel, std::span<const std::int64_t> sessions,
    const DatasetPanel& data, const ResearchOptions& options, std::stop_token stop) {
  evidence.days.push_back({sessions.front(), session_date(sessions.front()),
                           replay.initial_nav, 0, false});
  for (const auto& interval : replay.intervals) {
    checkpoint(stop, {}, 0, {});
    const auto key = sessions[interval.end_period];
    evidence.days.push_back({key, session_date(key), interval.nav, interval.net_return, true});
  }
  for (const auto& trade : replay.trades) {
    if (evidence.trades.size() >= options.max_result_points) break;
    evidence.trades.push_back({{"date", session_date(sessions[trade.period])},
        {"decision_date", session_date(sessions[trade.decision_period])},
        {"instrument", "SR:" + std::to_string(data.instruments.at(trade.instrument))},
        {"side", trade.dollar_delta > 0 ? "buy" : "sell"}, {"dollar_delta", trade.dollar_delta}});
  }
  const auto close = panel.field_all(panel.field_id("close").value());
  for (std::size_t name = 0; name < panel.instruments(); ++name) {
    const auto units = replay.final_tri_units.at(name);
    const auto dollars = units == 0 ? 0 : units * close[(panel.dates() - 1) *
        panel.instruments() + name];
    if (!std::isfinite(dollars)) throw std::runtime_error("nonfinite final held-position mark");
    evidence.positions.push_back({{"instrument", "SR:" + std::to_string(data.instruments[name])},
        {"tri_units", units}, {"marked_dollars", dollars}, {"weight", dollars / replay.final_nav}});
  }
}
} // namespace

alpha::Panel make_dataset_score_panel(const DatasetPanel& data) { return score_panel(data); }

void append_dataset_accounting(Evidence& evidence, const book::ReplayResult& replay,
    const alpha::Panel& panel, std::span<const std::int64_t> sessions,
    const DatasetPanel& data, const ResearchOptions& options, std::stop_token stop) {
  append_accounting(evidence, replay, panel, sessions, data, options, stop);
}

Evidence replay_dataset(const DatasetPanel& data, const DatasetRecipe& recipe,
    const alpha::SignalSet& signal, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress) {
  const auto dates = data.panel.dates() - data.score_begin;
  const auto names = data.panel.instruments();
  // Includes panel, signal, targets, trade rows, allocation snapshots and vector overhead.
  if (static_cast<std::uint64_t>(data.panel.cells()) * 256ULL > options.max_working_bytes)
    throw std::invalid_argument("delayed portfolio replay exceeds working memory budget");
  const auto panel = score_panel(data);
  const std::span<const std::int64_t> sessions(data.sessions.data() + data.score_begin, dates);
  std::vector<std::size_t> decisions;
  for (std::size_t day = 0; day + recipe.delay + 1 < dates; day += recipe.cadence)
    decisions.push_back(day);
  std::vector<double> weights(decisions.size() * names, 0);
  for (std::size_t i = 0; i < decisions.size(); ++i) {
    checkpoint(stop, {}, 0, {});
    neutral_weights(data, signal, decisions[i],
        std::span<double>{weights.data() + i * names, names});
  }
  book::ReplayConfig config;
  config.initial_nav = recipe.capital;
  config.execution_delay_periods = recipe.delay;
  config.trade_bps = recipe.trade_bps;
  config.annual_borrow_bps = recipe.borrow_bps;
  checkpoint(stop, progress, 0.6, "Replaying delayed dollar-neutral cash and holdings");
  const book::ReplayAllocationPolicy policy = [&](const book::ReplayAllocationState& state)
      -> atx::core::Result<std::vector<double>> {
    checkpoint(stop, progress, 0.6 + 0.2 * static_cast<double>(state.schedule_index + 1) /
        static_cast<double>(std::max(decisions.size(), std::size_t{1})),
        "Executing bounded delayed portfolio schedule");
    return std::vector<double>{state.preference_weights.begin(), state.preference_weights.end()};
  };
  const auto replay = book::replay_scheduled_targets(panel, sessions, decisions,
      weights, policy, config);
  if (!replay) throw std::runtime_error(replay.error().message());
  checkpoint(stop, progress, 0.85, "Collecting native market-data accounting diagnostics");
  Evidence evidence;
  append_accounting(evidence, replay->replay, panel, sessions, data, options, stop);
  evidence.provenance = data.provenance;
  auto& provenance = evidence.provenance;
  provenance["strategy_dsl"] = recipe.dsl;
  provenance["engine"] = "atx::engine::book::replay_scheduled_targets";
  provenance["alpha_engine"] = "atx::engine::alpha::evaluate_reference";
  provenance["execution_delay_sessions"] = recipe.delay;
  provenance["cadence"] = recipe.cadence;
  provenance["neutralization"] = "finite eligible signal demeaned; unit gross target weights";
  provenance["trade_bps"] = recipe.trade_bps;
  provenance["annual_borrow_bps"] = recipe.borrow_bps;
  provenance["annualization_sessions"] = 252;
  provenance["nav_column"] = "engine interval.nav";
  provenance["total_trade_count"] = replay->replay.trades.size();
  provenance["trades_truncated"] = replay->replay.trades.size() > evidence.trades.size();
  provenance["effective_rebalances"] = replay->replay.effective_rebalances;
  provenance["assumed_liquidations"] = replay->replay.assumed_liquidations;
  provenance["assumed_liquidation_pnl"] = replay->replay.assumed_liquidation_pnl;
  provenance["return_convention"] = "all scored native intervals, including initial cash, "
      "linear trading costs and calendar-day borrow charges; warmup excluded";
  provenance["warnings"] = Json::array({
      "Published TRAIN research only; historical vintage and common-stock identity unverified",
      "Modeled close/decision clocks are not historical data-delivery receipts",
      "Fixed formation cohort is a bounded research universe, not the full daily market",
      "No capacity, impact, locate, sector neutrality or production execution qualification",
      "Missing or guarded held marks use explicit native adverse liquidation assumptions"});
  return evidence;
}
} // namespace atx::server::research_detail

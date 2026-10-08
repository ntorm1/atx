#include "atx/server/research.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"
#include "build_provenance.hpp"
#include "research_detail.hpp"
#include "research_dataset.hpp"
#include "research_portfolio.hpp"
#include "research_dsl.hpp"

namespace atx::server::research_detail {
namespace {
namespace alpha = atx::engine::alpha;
namespace book = atx::engine::book;
constexpr std::int64_t day_ns = 86'400'000'000'000LL;
constexpr std::size_t instruments = 8;

double number(const Json& request, std::string_view key, double fallback,
              double lower, double upper) {
  const auto value = request.value(std::string(key), fallback);
  if (!std::isfinite(value) || value < lower || value > upper)
    throw std::invalid_argument(std::string(key) + " outside supported range");
  return value;
}

std::vector<double> targets(const alpha::SignalSet& signal, std::size_t sessions,
                            std::stop_token stop) {
  std::vector<double> weights(sessions * instruments, 0.0);
  for (std::size_t day = 0; day < sessions; ++day) {
    checkpoint(stop, {}, 0, {});
    const auto values = signal.alpha_cross_section(0, day);
    double mean = 0;
    std::size_t finite = 0;
    for (const auto value : values) if (std::isfinite(value)) { mean += value; ++finite; }
    if (finite < 2 || !std::isfinite(mean)) continue;
    mean /= static_cast<double>(finite);
    double gross = 0;
    for (std::size_t name = 0; name < instruments; ++name) {
      const auto centered = std::isfinite(values[name]) ? values[name] - mean : 0.0;
      weights[day * instruments + name] = centered;
      gross += std::abs(centered);
    }
    if (gross > 0 && std::isfinite(gross)) {
      for (std::size_t name = 0; name < instruments; ++name)
        weights[day * instruments + name] /= gross;
    } else {
      const auto begin = weights.begin() + static_cast<std::ptrdiff_t>(day * instruments);
      std::fill_n(begin, instruments, 0.0);
    }
  }
  return weights;
}

std::vector<std::int64_t> session_keys(std::size_t sessions) {
  std::vector<std::int64_t> result;
  result.reserve(sessions);
  auto date = std::chrono::sys_days{std::chrono::year{2020} / 1 / 1};
  for (std::size_t count = 0; count < 1100 && result.size() < sessions; ++count) {
    const std::chrono::weekday weekday{date};
    if (weekday != std::chrono::Saturday && weekday != std::chrono::Sunday)
      result.push_back(date.time_since_epoch().count() * day_ns);
    date += std::chrono::days{1};
  }
  return result;
}

alpha::Panel synthetic_panel(std::size_t sessions) {
  std::vector<double> close(sessions * instruments), open(close.size()),
                      volume(close.size()), returns(close.size());
  for (std::size_t day = 0; day < sessions; ++day) {
    for (std::size_t name = 0; name < instruments; ++name) {
      const auto index = day * instruments + name;
      const auto prior = day == 0 ? 100.0 + 10.0 * static_cast<double>(name)
                                  : close[index - instruments];
      const auto phase = static_cast<double>(day) * 0.09 + static_cast<double>(name) * 0.8;
      const auto change = day == 0 ? 0.0 : 0.0002 + 0.008 * std::sin(phase) +
                               0.003 * std::cos(static_cast<double>(day) * 0.37 + phase);
      open[index] = prior;
      close[index] = prior * (1.0 + change);
      returns[index] = change;
      volume[index] = 1e6 + 100'000.0 * static_cast<double>(name);
    }
  }
  auto panel = alpha::Panel::create(sessions, instruments, {"close", "open", "volume", "returns"},
      {std::move(close), std::move(open), std::move(volume), std::move(returns)}, {});
  if (!panel) throw std::runtime_error(panel.error().message());
  return std::move(*panel);
}
} // namespace

void checkpoint(std::stop_token stop, const Progress& progress, double fraction,
                std::string_view message) {
  if (stop.stop_requested()) throw std::runtime_error("research cancelled");
  if (progress) progress(fraction, message);
  if (stop.stop_requested()) throw std::runtime_error("research cancelled");
}

Json parse_json(std::string_view text) {
  std::array<std::set<std::string>, 34> keys;
  const auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
    if (depth < 0 || depth > 32) throw std::invalid_argument("JSON exceeds nesting limit");
    const auto level = static_cast<std::size_t>(depth);
    if (event == Json::parse_event_t::object_start) keys.at(level + 1).clear();
    if (event == Json::parse_event_t::key &&
        !keys.at(level).insert(parsed.get<std::string>()).second)
      throw std::invalid_argument("duplicate JSON key");
    return true;
  };
  try {
    return Json::parse(text, callback);
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("malformed JSON: ") + error.what());
  }
}

Evidence synthetic_demo(const Json& request, const ResearchOptions& options,
                        std::stop_token stop, const Progress& progress) {
  const auto count = number(request, "sessions", 504, 10, 756);
  if (std::floor(count) != count) throw std::invalid_argument("sessions must be integral");
  const auto sessions = static_cast<std::size_t>(count);
  if (sessions > options.max_csv_rows) throw std::invalid_argument("sessions exceeds row budget");
  const auto capital = number(request, "initial_capital", 1e6, 1, 1e12);
  const auto trade_bps = number(request, "trade_bps", 6, 0, 100);
  const auto borrow_bps = number(request, "annual_borrow_bps", 300, 0, 5000);
  const auto dsl = request.value("strategy_dsl", std::string("rank(delta(close,5))"));
  checkpoint(stop, progress, 0.1, "Generating explicitly synthetic research panel");
  const auto panel = synthetic_panel(sessions);
  const auto signal = evaluate_research_signal(dsl, panel, options.max_working_bytes, stop);
  const auto weights = targets(signal, sessions, stop);
  const auto keys = session_keys(sessions);
  std::vector<std::size_t> decisions;
  for (std::size_t day = 0; day + 2 < sessions; ++day) decisions.push_back(day);
  book::ReplayConfig config;
  config.initial_nav = capital;
  config.trade_bps = trade_bps;
  config.annual_borrow_bps = borrow_bps;
  const std::span<const double> scheduled(weights.data(), decisions.size() * instruments);
  checkpoint(stop, progress, 0.4, "Executing causal alpha and delayed cash/holdings replay");
  const auto replay = book::replay_scheduled_targets(panel, keys, decisions, scheduled, config);
  if (!replay) throw std::runtime_error(replay.error().message());
  checkpoint(stop, progress, 0.75, "Computing statistics from actual replay accounting");
  Evidence evidence;
  evidence.days.push_back({keys.front(), date_text(std::chrono::floor<std::chrono::days>(
      std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{keys.front()}})),
      capital, 0, false});
  for (const auto& interval : replay->intervals) {
    const auto key = keys.at(interval.end_period);
    evidence.days.push_back({key, date_text(std::chrono::floor<std::chrono::days>(
        std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{key}})),
        interval.nav, interval.net_return, true});
  }
  const auto close = panel.field_all(panel.field_id("close").value());
  for (const auto& trade : replay->trades) {
    if (evidence.trades.size() >= options.max_result_points) break;
    evidence.trades.push_back({{"date", evidence.days.at(trade.period).date},
        {"instrument", "SYN" + std::to_string(trade.instrument + 1)},
        {"side", trade.dollar_delta > 0 ? "buy" : "sell"}, {"dollar_delta", trade.dollar_delta}});
  }
  for (std::size_t name = 0; name < instruments; ++name) {
    const auto mark = close[(sessions - 1) * instruments + name];
    const auto dollars = replay->final_tri_units.at(name) * mark;
    evidence.positions.push_back({{"instrument", "SYN" + std::to_string(name + 1)},
        {"tri_units", replay->final_tri_units.at(name)}, {"marked_dollars", dollars},
        {"weight", dollars / replay->final_nav}});
  }
  evidence.provenance = {{"synthetic", true}, {"evaluation", "synthetic_test"},
      {"data_source", "deterministic mathematical price panel; no market observations"},
      {"engine", "atx::engine::book::replay_scheduled_targets"}, {"strategy_dsl", dsl},
      {"alpha_engine", "atx::engine::alpha::evaluate_reference"},
      {"execution_delay_sessions", 1}, {"trade_bps", trade_bps},
      {"annual_borrow_bps", borrow_bps}, {"annualization_sessions", 252},
      {"nav_column", "engine interval.nav"}, {"total_trade_count", replay->trades.size()},
      {"trades_truncated", replay->trades.size() > evidence.trades.size()},
      {"return_convention", "all engine intervals, including initial cash and execution costs"},
      {"warnings", Json::array({"Synthetic demonstration is not trading evidence",
           "No observed liquidity, corporate actions, universe history or borrow availability"})}};
  return evidence;
}
} // namespace atx::server::research_detail

namespace atx::server {
using research_detail::Json;

ResearchBackend::ResearchBackend(ResearchOptions options) : options_(std::move(options)) {
  if (!options_.runtime_build_sha256.empty() &&
      (options_.runtime_build_sha256.size() != 64 ||
       !std::all_of(options_.runtime_build_sha256.begin(), options_.runtime_build_sha256.end(),
           [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); })))
    throw std::invalid_argument("runtime build pin requires 64 lowercase hexadecimal characters");
  if (options_.max_input_bytes == 0 || options_.max_input_bytes > 256U * 1024U * 1024U ||
      options_.max_result_points < 2 || options_.max_result_points > 10'000 ||
      options_.max_csv_rows == 0 || options_.max_csv_rows > 1'000'000)
    throw std::invalid_argument("invalid research bounds");
  if (options_.repo_root.empty()) options_.repo_root = std::filesystem::current_path();
  options_.repo_root = std::filesystem::canonical(options_.repo_root);
  for (auto& root : options_.artifact_roots) {
    if (root.is_relative()) root = options_.repo_root / root;
    if (!std::filesystem::is_directory(root))
      throw std::invalid_argument("artifact root must be an existing directory");
    root = std::filesystem::canonical(root);
  }
  research_detail::validate_datasets(options_);
}

std::string ResearchBackend::strategies_json() const {
  auto catalog = Json{{"strategies", Json::array({
      {{"id", "synthetic-long-short-v1"}, {"name", "Causal long / short laboratory"},
       {"description", "Editable alpha DSL evaluated by the ATX engine on synthetic prices"},
       {"mode", "synthetic_demo"}, {"synthetic", true}, {"parameters",
        {{"sessions", 504}, {"initial_capital", 1e6}, {"trade_bps", 6},
         {"annual_borrow_bps", 300}, {"strategy_dsl", "rank(delta(close,5))"}}}},
      {{"id", "imported-nav"}, {"name", "ATX saved NAV evidence"},
       {"description", "Recompute metrics from allowlisted strategy NAV daily artifacts"},
       {"mode", "import_nav"}, {"synthetic", false}, {"parameters", Json::object()}},
      {{"id", "saved-blend-nav"}, {"name", "Pinned TRAIN strategy replay"},
       {"description", "Execute the self-financing ATX NAV pipeline on pinned saved inputs"},
       {"mode", "real_nav"}, {"synthetic", false}, {"enabled", !options_.output_root.empty()},
       {"parameters", {{"rule", "baseline-v1"}, {"cadence", 5}, {"trade_fraction", 0.25},
                       {"monthly_budget", 0.30}}}}
      })}, {"locked_holdout", {{"enabled", true}, {"blocked_years", {2023, 2024, 2025}},
         {"blocked_roles", {"validation", "VAL", "holdout"}}}}};
  catalog["strategies"].push_back({{"id", "real-data-long-short-v1"},
      {"name", "Pinned TRAIN market-data alpha"}, {"mode", "real_dsl"}, {"synthetic", false},
      {"enabled", !options_.datasets.empty()},
      {"description", "Causal DSL and delayed cash/holdings replay on published TRAIN panels"},
      {"parameters", {{"strategy_dsl", "-rank(delta(close,5)/delay(close,5))"},
        {"instrument_limit", 256}, {"initial_capital", 1e6}, {"trade_bps", 6},
        {"annual_borrow_bps", 300}, {"cadence", 1}, {"execution_delay_sessions", 1},
        {"neutralization", "dollar"}, {"start_date", "2020-01-01"}, {"end_date", "2022-12-31"}}}});
  catalog["datasets"] = research_detail::dataset_catalog(options_);
  catalog["strategies"].push_back({{"id", "real-data-portfolio-v1"},
      {"name", "Pinned TRAIN constrained expression portfolio"}, {"mode", "real_portfolio"},
      {"synthetic", false}, {"enabled", !options_.datasets.empty()},
      {"description", "Frozen native signal blend and delayed allocation against marked holdings"},
      {"parameters", {{"instrument_limit", 128}, {"cadence", 5}, {"gross_limit", 1},
        {"name_cap", 0.05}, {"max_turnover", 1}, {"signal_decay_sessions", 1},
        {"execution_delay_sessions", 1}, {"neutralization", "dollar"},
        {"initial_capital", 1e6}, {"trade_bps", 6}, {"annual_borrow_bps", 300},
        {"start_date", "2020-01-01"}, {"end_date", "2020-03-31"}}}});
  catalog["operators"] = research_detail::research_operator_catalog();
  return catalog.dump();
}

std::string ResearchBackend::execute(std::string_view request_json, std::stop_token stop,
    const std::function<void(double, std::string_view)>& progress) const {
  research_detail::checkpoint(stop, progress, 0.0, "Validating research request");
  if (request_json.size() > std::min(options_.max_input_bytes, std::size_t{65536}))
    throw std::invalid_argument("request exceeds input byte budget");
  const auto request = research_detail::parse_json(request_json);
  if (!request.is_object()) throw std::invalid_argument("request must be a JSON object");
  try {
    const auto mode = request.value("mode", std::string("synthetic_demo"));
    const std::set<std::string> demo_keys{"mode", "strategy_id", "sessions", "initial_capital",
        "trade_bps", "annual_borrow_bps", "strategy_dsl"};
    const std::set<std::string> import_keys{"mode", "strategy_id", "artifact_path", "summary_path",
                                          "start_date", "end_date"};
    const std::set<std::string> real_keys{"mode", "strategy_id", "combined_path", "combined_sha256",
        "role_path", "role_sha256", "rule", "cadence", "trade_fraction", "monthly_budget",
        "start_date", "end_date"};
    const std::set<std::string> dataset_keys{"mode", "strategy_id", "dataset_id", "dataset_sha256",
        "strategy_dsl", "start_date", "end_date", "instrument_limit", "initial_capital",
        "trade_bps", "annual_borrow_bps", "cadence", "neutralization", "execution_delay_sessions"};
    auto portfolio_keys = dataset_keys;
    portfolio_keys.erase("strategy_dsl");
    portfolio_keys.insert({"components", "gross_limit", "name_cap", "max_turnover",
                           "signal_decay_sessions"});
    if (mode != "synthetic_demo" && mode != "import_nav" && mode != "real_nav" &&
        mode != "real_dsl" && mode != "real_portfolio")
      throw std::invalid_argument("unsupported research mode");
    const auto& allowed = mode == "synthetic_demo" ? demo_keys
        : mode == "real_nav" ? real_keys : mode == "real_dsl" ? dataset_keys
        : mode == "real_portfolio" ? portfolio_keys : import_keys;
    for (const auto& item : request.items()) if (!allowed.contains(item.key()))
      throw std::invalid_argument("unknown research parameter: " + item.key());
    const std::string expected_id = mode == "synthetic_demo" ? "synthetic-long-short-v1"
        : mode == "real_nav" ? "saved-blend-nav"
        : mode == "real_dsl" ? "real-data-long-short-v1"
        : mode == "real_portfolio" ? "real-data-portfolio-v1" : "imported-nav";
    const auto id = request.value("strategy_id", expected_id);
    if (id != expected_id)
      throw std::invalid_argument("strategy_id does not match research mode");
    const auto evidence = mode == "synthetic_demo"
        ? research_detail::synthetic_demo(request, options_, stop, progress)
        : mode == "real_nav" ? research_detail::real_nav(request, options_, stop, progress)
        : mode == "real_dsl" ? research_detail::real_dsl(request, options_, stop, progress)
        : mode == "real_portfolio" ? research_detail::real_portfolio(request, options_, stop, progress)
        : research_detail::import_nav(request, options_, stop, progress);
    auto result = research_detail::summarize(evidence, options_, stop, progress);
    result["strategy_id"] = id;
    result["mode"] = mode;
    const auto submitted_digest = atx::core::sha256_hex(request_json);
    if (!submitted_digest) throw std::runtime_error(submitted_digest.error().message());
    result["provenance"]["submitted_request_sha256"] = *submitted_digest;
    result["provenance"]["engine_git_sha"] = std::string(atx::impl::build_engine_git_sha());
    if (!options_.runtime_build_sha256.empty())
      result["provenance"]["runtime_build_sha256"] = options_.runtime_build_sha256;
    research_detail::checkpoint(stop, progress, 1.0, "Research result complete");
    return result.dump();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid research parameter: ") + error.what());
  }
}
} // namespace atx::server

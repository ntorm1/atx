#include "research_portfolio.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "atx/core/sha256.hpp"
#include "research_dsl.hpp"

namespace atx::server::research_detail {
namespace {
constexpr std::uint64_t work_budget = 250'000'000;

double bounded_number(const Json& value, double minimum, double maximum,
                      std::string_view label) {
  if (!value.is_number()) throw std::invalid_argument(std::string(label) + " must be numeric");
  const auto number = value.get<double>();
  if (!std::isfinite(number) || number < minimum || number > maximum)
    throw std::invalid_argument(std::string(label) + " outside supported range");
  return number;
}

std::size_t decay_sessions(const Json& request) {
  if (!request.contains("signal_decay_sessions")) return 1;
  const auto& value = request.at("signal_decay_sessions");
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    throw std::invalid_argument("signal_decay_sessions must be an integer");
  const auto count = value.get<std::uint64_t>();
  if (count < 1 || count > 21)
    throw std::invalid_argument("signal_decay_sessions outside 1..21");
  return static_cast<std::size_t>(count);
}

std::vector<PortfolioComponent> components(const Json& request, std::size_t decay) {
  const auto& entries = request.at("components");
  if (!entries.is_array() || entries.size() < 2 || entries.size() > 12)
    throw std::invalid_argument("portfolio requires 2..12 frozen expression components");
  const std::set<std::string> allowed{"strategy_dsl", "expression_sha256", "weight"};
  std::vector<PortfolioComponent> result;
  double gross = 0;
  for (const auto& item : entries) {
    if (!item.is_object()) throw std::invalid_argument("portfolio component must be an object");
    for (const auto& field : item.items()) if (!allowed.contains(field.key()))
      throw std::invalid_argument("unknown portfolio component parameter: " + field.key());
    const auto expression = item.at("strategy_dsl").get<std::string>();
    const auto digest = atx::core::sha256_hex(expression);
    if (!digest || item.at("expression_sha256").get<std::string>() != *digest)
      throw std::invalid_argument("portfolio component expression SHA256 does not match");
    const auto weight = bounded_number(item.at("weight"), -1e6, 1e6, "component weight");
    gross += std::abs(weight);
    const auto effective = decay == 1 ? expression : "decay_linear((" + expression + ")," +
        std::to_string(decay) + ")";
    auto program = compile_research_dsl(effective, dataset_fields());
    const auto work = research_dsl_work_per_cell(effective, dataset_fields());
    result.push_back({expression, effective, *digest, weight, std::move(program), work});
  }
  if (!(gross > 1e-12)) throw std::invalid_argument("portfolio component gross weight is zero");
  for (auto& item : result) item.weight /= gross;
  return result;
}

PortfolioRecipe make_recipe(const Json& request) {
  PortfolioRecipe result;
  result.decay = decay_sessions(request);
  result.components = components(request, result.decay);
  auto common = request;
  common["strategy_dsl"] = result.components.front().effective_expression;
  // Portfolio solves are deliberately bounded more tightly than independent signal research.
  if (!common.contains("instrument_limit")) common["instrument_limit"] = 128;
  if (!common.contains("cadence")) common["cadence"] = 5;
  if (!common.contains("end_date")) common["end_date"] = "2020-03-31";
  result.data = make_dataset_recipe(common);
  for (const auto& item : result.components) {
    result.data.lookback = std::max(result.data.lookback,
        static_cast<std::size_t>(item.program.required_lookback));
    result.max_slots = std::max(result.max_slots, static_cast<std::size_t>(item.program.num_slots));
  }
  result.gross = bounded_number(request.value("gross_limit", Json(1)), 0, 2, "gross_limit");
  result.name_cap = bounded_number(request.value("name_cap", Json(0.05)), .001, 1, "name_cap");
  result.turnover = bounded_number(request.value("max_turnover", Json(1)), 0, 2, "max_turnover");
  return result;
}

Json admit(const DatasetPanel& data, const PortfolioRecipe& recipe, const ResearchOptions& options) {
  const auto cells = static_cast<std::uint64_t>(data.panel.cells());
  auto remaining = work_budget;
  for (const auto& item : recipe.components) {
    if (item.work_per_cell > remaining / cells)
      throw std::invalid_argument("portfolio aggregate signal work exceeds budget; "
                                  "use fewer names or a shorter date window");
    remaining -= item.work_per_cell * cells;
  }
  const auto names = static_cast<std::uint64_t>(data.panel.instruments());
  const auto dates = data.panel.dates() - data.score_begin;
  const auto decisions = dates > recipe.data.delay + 1
      ? (dates - recipe.data.delay - 2) / recipe.data.cadence + 1 : std::size_t{0};
  // Fixed ADMM steps plus at most three numeric factorizations after bounded
  // deterministic rho adaptation. This reservation covers their cubic work.
  const auto per_solve = names * names * (600ULL + 3ULL * names);
  if (decisions > remaining / per_solve)
    throw std::invalid_argument("portfolio aggregate solver work exceeds budget; "
                                "use fewer names, shorter dates or a larger cadence");
  remaining -= static_cast<std::uint64_t>(decisions) * per_solve;
  // Entire retained signal pool and the largest evaluator coexist before blending.
  // A power-of-two loop view may double date storage; include its five real OHLCV columns.
  const auto arrays = 6ULL + recipe.components.size() + recipe.max_slots + 4ULL + 10ULL;
  const auto evaluation_bytes = cells * (arrays * sizeof(double) + 2ULL);
  // Replay allocations/trades, dense linear constraints, sparse factorization and scratch.
  const auto replay_bytes = cells * 512ULL + names * names * 64ULL + 32ULL * 1024ULL * 1024ULL;
  if (std::max(evaluation_bytes, replay_bytes) > options.max_working_bytes)
    throw std::invalid_argument("portfolio aggregate working memory exceeds budget");
  return {{"aggregate_estimated_work_units", work_budget - remaining},
      {"aggregate_work_budget", work_budget},
      {"reserved_working_bytes", std::max(evaluation_bytes, replay_bytes)},
      {"working_memory_budget", options.max_working_bytes}};
}
} // namespace

PortfolioRecipe make_portfolio_recipe(const Json& request) { return make_recipe(request); }

Json admit_portfolio(const DatasetPanel& data, const PortfolioRecipe& recipe,
    const ResearchOptions& options) { return admit(data, recipe, options); }

Evidence real_portfolio(const Json& request, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress) {
  checkpoint(stop, progress, 0.05, "Admitting frozen portfolio expressions and TRAIN recipe");
  const auto recipe = make_recipe(request);
  const auto id = request.value("dataset_id", std::string{});
  const auto dataset = std::find_if(options.datasets.begin(), options.datasets.end(),
      [&](const auto& entry) { return entry.id == id; });
  if (dataset == options.datasets.end()) throw std::invalid_argument("unknown registered dataset ID");
  if (request.value("dataset_sha256", std::string{}) != dataset->manifest_sha256)
    throw std::invalid_argument("dataset revision SHA256 does not match registered pin");
  const auto data = load_dataset(*dataset, recipe.data, options, stop, progress);
  const auto admission = admit(data, recipe, options);
  const auto signal = blend_portfolio_signals(data, recipe, options, stop, progress);
  auto evidence = replay_portfolio(data, recipe, signal, options, stop, progress);
  evidence.provenance["resource_admission"] = admission;
  return evidence;
}
} // namespace atx::server::research_detail

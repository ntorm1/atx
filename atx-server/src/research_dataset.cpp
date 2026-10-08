#include "research_dataset.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "atx/engine/alpha/bytecode.hpp"
#include "research_dsl.hpp"

namespace atx::server::research_detail {
namespace {
bool valid_sha(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

double numeric(const Json& request, std::string_view name, double fallback,
               double minimum, double maximum) {
  const auto key = std::string(name);
  if (request.contains(key) && !request.at(key).is_number())
    throw std::invalid_argument(key + " must be numeric");
  const auto value = request.value(key, fallback);
  if (!std::isfinite(value) || value < minimum || value > maximum)
    throw std::invalid_argument(key + " outside supported range");
  return value;
}

std::size_t integer(const Json& request, std::string_view name, std::size_t fallback,
                    std::size_t minimum, std::size_t maximum) {
  const auto key = std::string(name);
  if (!request.contains(key)) return fallback;
  const auto& value = request.at(key);
  if (!value.is_number_integer() ||
      (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    throw std::invalid_argument(key + " must be a nonnegative integer");
  const auto count = value.get<std::uint64_t>();
  if (count < minimum || count > maximum)
    throw std::invalid_argument(key + " outside supported range");
  return static_cast<std::size_t>(count);
}

DatasetRecipe recipe(const Json& request) {
  DatasetRecipe result;
  result.start = parse_date(request.value("start_date", std::string("2020-01-01")));
  result.end = parse_date(request.value("end_date", std::string("2022-12-31")));
  if (result.start > result.end) throw std::invalid_argument("reversed research date window");
  if (result.start < parse_date("2018-01-01") || result.end > parse_date("2022-12-31"))
    throw std::invalid_argument("real_dsl supports only the 2018-2022 TRAIN window");
  if (request.value("neutralization", std::string("dollar")) != "dollar")
    throw std::invalid_argument("only dollar neutralization is supported for this dataset");
  result.dsl = request.value("strategy_dsl", std::string("-rank(delta(close,5)/delay(close,5))"));
  result.instrument_limit = integer(request, "instrument_limit", 256, 2, 512);
  result.cadence = integer(request, "cadence", 1, 1, 252);
  result.delay = integer(request, "execution_delay_sessions", 1, 1, 5);
  result.capital = numeric(request, "initial_capital", 1e6, 1, 1e12);
  result.trade_bps = numeric(request, "trade_bps", 6, 0, 100);
  result.borrow_bps = numeric(request, "annual_borrow_bps", 300, 0, 5000);
  const auto program = compile_research_dsl(result.dsl, dataset_fields());
  result.lookback = program.required_lookback;
  return result;
}
} // namespace

std::vector<std::string> dataset_fields() {
  return {"close", "raw_close", "volume", "returns", "dollar_volume", "adv63"};
}

DatasetRecipe make_dataset_recipe(const Json& request) { return recipe(request); }

void validate_datasets(ResearchOptions& options) {
  if (options.datasets.size() > 32) throw std::invalid_argument("too many research datasets");
  std::set<std::string> ids;
  for (auto& dataset : options.datasets) {
    if (dataset.id.empty() || dataset.id.size() > 64 ||
        !std::all_of(dataset.id.begin(), dataset.id.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
        }) || !ids.insert(dataset.id).second)
      throw std::invalid_argument("invalid or duplicate research dataset ID");
    if (!valid_sha(dataset.manifest_sha256))
      throw std::invalid_argument("research dataset needs an exact lowercase SHA256 pin");
    if (dataset.name.empty()) dataset.name = dataset.id;
    if (dataset.name.size() > 256) throw std::invalid_argument("dataset name exceeds limit");
    if (dataset.manifest_path.is_relative())
      dataset.manifest_path = options.repo_root / dataset.manifest_path;
    if (!std::filesystem::is_regular_file(dataset.manifest_path))
      throw std::invalid_argument("research dataset manifest must be an existing file");
    dataset.manifest_path = std::filesystem::canonical(dataset.manifest_path);
  }
}

Json dataset_catalog(const ResearchOptions& options) {
  Json result = Json::array();
  for (const auto& dataset : options.datasets) {
    result.push_back({{"id", dataset.id}, {"name", dataset.name},
        {"manifest_sha256", dataset.manifest_sha256}, {"evaluation", "TRAIN"},
        {"start_date", "2018-01-01"}, {"end_date", "2022-12-31"},
        {"fields", dataset_fields()}, {"max_instruments", 512},
        {"historical_vintage_verified", false}, {"common_stock_history_verified", false},
        {"field_contract", {{"close", "causal forward total-return index from observed ret"},
          {"raw_close", "observed unadjusted close; available at modeled session close"},
          {"returns", "observed total return; guarded observations excluded"},
          {"adv63", "published prior-session 63-session average dollar volume"}}}});
  }
  return result;
}

Evidence real_dsl(const Json& request, const ResearchOptions& options,
                  std::stop_token stop, const Progress& progress) {
  checkpoint(stop, progress, 0.05, "Validating pinned TRAIN dataset recipe");
  const auto settings = recipe(request);
  const auto id = request.value("dataset_id", std::string{});
  const auto found = std::find_if(options.datasets.begin(), options.datasets.end(),
      [&](const ResearchDataset& dataset) { return dataset.id == id; });
  if (found == options.datasets.end()) throw std::invalid_argument("unknown registered dataset ID");
  if (request.value("dataset_sha256", std::string{}) != found->manifest_sha256)
    throw std::invalid_argument("dataset revision SHA256 does not match registered pin");
  const auto data = load_dataset(*found, settings, options, stop, progress);
  const auto panel_bytes = static_cast<std::uint64_t>(data.panel.cells()) * 49ULL;
  if (panel_bytes >= options.max_working_bytes)
    throw std::invalid_argument("dataset panel exceeds research working memory budget");
  checkpoint(stop, progress, 0.5, "Evaluating causal DSL on pinned market observations");
  const auto signal = evaluate_research_signal(settings.dsl, data.panel,
      options.max_working_bytes - panel_bytes, stop);
  return replay_dataset(data, settings, signal, options, stop, progress);
}
} // namespace atx::server::research_detail

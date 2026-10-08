#include "research_paper.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "atx/core/sha256.hpp"
#include "build_provenance.hpp"

namespace atx::server::research_detail {
namespace {
std::string sha(std::string_view bytes) {
  const auto result = atx::core::sha256_hex(bytes);
  if (!result) throw std::runtime_error(result.error().message());
  return *result;
}

Json paper_request(const PaperReplayInput& input, const ResearchOptions& options) {
  if (input.approved_portfolio_request_json.size() >
      std::min(options.max_input_bytes, std::size_t{64 * 1024}))
    throw std::invalid_argument("paper frozen recipe exceeds input bound");
  if (input.through_session_count < 1 || input.through_session_count > 63)
    throw std::invalid_argument("through_session_count outside 1..63");
  const auto frozen = parse_json(input.approved_portfolio_request_json);
  if (!frozen.is_object() || frozen.value("mode", std::string{}) != "real_portfolio")
    throw std::invalid_argument("paper requires a frozen real_portfolio recipe");
  const std::set<std::string> allowed{"mode", "strategy_id", "dataset_id", "dataset_sha256",
      "start_date", "end_date", "instrument_limit", "initial_capital", "trade_bps",
      "annual_borrow_bps", "execution_delay_sessions", "cadence", "neutralization",
      "components", "signal_decay_sessions", "gross_limit", "name_cap", "max_turnover"};
  for (const auto& item : frozen.items()) if (!allowed.contains(item.key()))
    throw std::invalid_argument("unknown paper frozen recipe parameter: " + item.key());
  if (frozen.value("strategy_id", std::string("real-data-portfolio-v1")) !=
      "real-data-portfolio-v1") throw std::invalid_argument("unknown paper portfolio strategy");
  const auto original = make_portfolio_recipe(frozen);
  if (original.data.instrument_limit > 128)
    throw std::invalid_argument("paper supports at most 128 instrument_limit");
  const auto boundary = parse_date(input.approved_evidence_end_date);
  if (boundary < original.data.start || boundary > original.data.end)
    throw std::invalid_argument("approved evidence end must lie within frozen recipe dates");
  if (parse_date(input.paper_start_date) <= boundary)
    throw std::invalid_argument("paper window must start strictly after approved evidence end");
  auto request = frozen;
  request["start_date"] = input.paper_start_date;
  request["end_date"] = input.paper_end_date;
  return request;
}

DatasetPanel prefix_panel(const DatasetPanel& full, std::size_t through) {
  const auto dates = full.score_begin + through;
  const auto names = full.panel.instruments(), cells = dates * names;
  std::vector<std::span<const double>> columns;
  for (const auto& field : dataset_fields())
    columns.push_back(full.panel.field_all(full.panel.field_id(field).value()).first(cells));
  std::vector<std::uint8_t> mask(cells);
  for (std::size_t day = 0; day < dates; ++day)
    for (std::size_t name = 0; name < names; ++name)
      mask[day * names + name] = static_cast<std::uint8_t>(full.panel.in_universe(day, name));
  // The full materialization remains alive for the entire evaluation/replay call.
  // Evaluators cannot address fields or membership past the requested prefix.
  auto panel = atx::engine::alpha::Panel::create_borrowed(dates, names, dataset_fields(),
      std::move(columns), std::move(mask));
  if (!panel) throw std::runtime_error(panel.error().message());
  return {std::move(*panel), {full.sessions.begin(), full.sessions.begin() +
      static_cast<std::ptrdiff_t>(dates)}, full.instruments, full.score_begin, full.provenance};
}

Json paper_admission(const DatasetPanel& full, const PortfolioRecipe& recipe,
                     const ResearchOptions& options) {
  auto admission = admit_portfolio(full, recipe, options);
  // Complete positions, sizing, fills, native rows and typed/JSON diagnostics coexist.
  // The scored axes are bounded by 63*128; metadata/recipe storage gets a separate reserve.
  const auto cells = static_cast<std::uint64_t>(full.panel.dates() - full.score_begin) *
      static_cast<std::uint64_t>(full.instruments.size());
  const auto retained = cells * 2048ULL + 4ULL * 1024ULL * 1024ULL;
  const auto base = admission.at("reserved_working_bytes").get<std::uint64_t>();
  if (retained > options.max_working_bytes || base > options.max_working_bytes - retained)
    throw std::invalid_argument("paper full owned ledger exceeds working memory budget");
  admission["reserved_owned_ledger_bytes"] = retained;
  admission["reserved_working_bytes"] = base + retained;
  return admission;
}

Json receipt(const PaperReplayInput& input, const Json& request, const Json& admission,
             const PortfolioReplayOutput& owned, std::size_t scored,
             std::string_view runtime_pin) {
  auto out = owned.provenance;
  out["schema"] = "atx.paper-replay/v1";
  out["operation"] = "historical_simulated_paper";
  out["approval_authority"] = "caller resolved; pure adapter does not attest review approval";
  out["approved_evidence_end_date"] = input.approved_evidence_end_date;
  out["approved_recipe_sha256"] = sha(parse_json(input.approved_portfolio_request_json).dump());
  out["effective_recipe_sha256"] = sha(request.dump());
  out["effective_recipe"] = request;
  out["paper_start_date"] = input.paper_start_date;
  out["paper_end_date"] = input.paper_end_date;
  out["through_session_count"] = input.through_session_count;
  out["window_sessions"] = scored;
  out["clock_convention"] = "completed pre-execution valuation through T; "
      "execution at T and the interval starting T remain pending until the next advance";
  out["position_units"] = "synthetic TRI units, not exchange shares";
  out["trade_fee_basis"] = "authoritative native allocation trade_cost; per-fill modeled_fee "
      "is an informational attribution, not an additional cash debit";
  out["borrow_day_basis"] = 365;
  out["resource_admission"] = admission;
  out["engine_git_sha"] = atx::impl::build_engine_git_sha();
  out["runtime_build_sha256"] = runtime_pin.empty() ? Json(nullptr) : Json(runtime_pin);
  out["runtime_build_identity_available"] = !runtime_pin.empty();
  return out;
}

PaperReplayResult replay(const PaperReplayInput& input, const ResearchOptions& options,
                        std::stop_token stop, const Progress& progress) {
  checkpoint(stop, progress, .05, "Admitting fresh historical paper window and frozen policy");
  const auto request = paper_request(input, options);
  const auto recipe = make_portfolio_recipe(request);
  const auto id = request.value("dataset_id", std::string{});
  const auto dataset = std::find_if(options.datasets.begin(), options.datasets.end(),
      [&](const auto& entry) { return entry.id == id; });
  if (dataset == options.datasets.end()) throw std::invalid_argument("unknown registered dataset ID");
  if (request.value("dataset_sha256", std::string{}) != dataset->manifest_sha256)
    throw std::invalid_argument("dataset revision SHA256 does not match registered pin");
  const auto full = load_dataset(*dataset, recipe.data, options, stop, progress,
                                 DatasetAdmissionProfile::Paper);
  const auto scored = full.panel.dates() - full.score_begin;
  if (input.through_session_count > scored)
    throw std::invalid_argument("paper through count exceeds fixed window calendar");
  if ((input.expected_cohort_security_ids.empty() && input.through_session_count != 1) ||
      (!input.expected_cohort_security_ids.empty() &&
       input.expected_cohort_security_ids != full.instruments))
    throw std::invalid_argument("paper expected cohort differs from frozen formation cohort");
  const auto admission = paper_admission(full, recipe, options);
  const auto prefix = prefix_panel(full, input.through_session_count);
  const auto signal = blend_portfolio_signals(prefix, recipe, options, stop, progress);
  const auto owned = replay_portfolio_owned(prefix, recipe, signal, options, stop, progress, true);
  auto result = collect_paper_ledger(prefix, recipe, owned, stop);
  for (std::size_t day = full.score_begin; day < full.sessions.size(); ++day)
    result.window_calendar.push_back(date_text(std::chrono::floor<std::chrono::days>(
        std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{full.sessions[day]}})));
  result.receipt_json = receipt(input, request, admission, owned, scored,
                                options.runtime_build_sha256).dump();
  checkpoint(stop, progress, 1, "Complete owned historical paper ledger reconciled");
  checkpoint(stop, {}, 1, {});
  return result;
}
} // namespace
} // namespace atx::server::research_detail

namespace atx::server {
PaperReplayResult ResearchBackend::replay_paper(const PaperReplayInput& input,
    std::stop_token stop, const std::function<void(double, std::string_view)>& progress) const {
  try { return research_detail::replay(input, options_, stop, progress); }
  catch (const research_detail::Json::exception& error) {
    throw std::invalid_argument(std::string("invalid paper frozen recipe: ") + error.what());
  }
}
} // namespace atx::server

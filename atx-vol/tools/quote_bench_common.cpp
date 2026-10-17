#include "quote_bench_common.hpp"
#include "quote_bench_common_policy.hpp"
#include "quote_bench_policy.hpp"
#include "quote_bench_progress.hpp"

#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/american_markov_grid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace quote_bench {
namespace {
using namespace atx::vol;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Identity = std::array<std::string, 3>;

struct StageTimes {
  Json elapsed{{"market", 0.0}, {"policy", 0.0}, {"grid", 0.0},
               {"seed", 0.0}, {"fit", 0.0}, {"equivalent_iv", 0.0}};
  Json attempted{{"market", false}, {"policy", false}, {"grid", false},
                 {"seed", false}, {"fit", false}, {"equivalent_iv", false}};

  template <class Function> auto measure(const char *stage, Function function) {
    attempted[stage] = true;
    const auto start = Clock::now();
    auto value = function();
    elapsed[stage] = elapsed[stage].get<double>() +
        std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    return value;
  }
};

Identity snapshot_identity(const InputRow &row) {
  return {row.identity[0], row.identity[1], row.identity[3]};
}

void count(Json &object, const std::string &key, std::size_t increment = 1U) {
  if (object.is_null()) object = Json::object();
  object[key] = object.value(key, std::size_t{}) + increment;
}

Json summary_json(const AmericanMarkovMarketSummary &s) {
  return {{"median", s.median}, {"minimum", s.minimum}, {"maximum", s.maximum},
          {"median_absolute_deviation", s.median_absolute_deviation},
          {"maximum_absolute_deviation", s.maximum_absolute_deviation}};
}

Json market_json(const AmericanMarkovMarketPreparation &prepared) {
  const auto &d = prepared.diagnostics;
  Json result{{"status", to_string(prepared.status)}, {"quote_count", d.quote_count},
              {"spot", summary_json(d.spot)}, {"expiries", Json::array()},
              {"refused_row", d.refused_row ? Json(*d.refused_row) : Json(nullptr)},
              {"refused_expiry", d.refused_expiry ? Json(*d.refused_expiry) : Json(nullptr)}};
  for (const auto &e : d.expiries) {
    result["expiries"].push_back({{"chain_id", e.expiry_key}, {"quote_count", e.quote_count},
        {"calendar_years", summary_json(e.calendar_years)},
        {"variance_years", summary_json(e.variance_years)}, {"rate", summary_json(e.rate)},
        {"supplied_yield", summary_json(e.supplied_yield)},
        {"dividend_pv", summary_json(e.dividend_pv)},
        {"prepaid_over_spot", summary_json(e.prepaid_over_spot)},
        {"forward_over_spot", summary_json(e.forward_over_spot)},
        {"integrated_rate", e.integrated_rate},
        {"integrated_effective_yield", e.integrated_effective_yield},
        {"resolved_prepaid_over_spot", e.resolved_prepaid_over_spot},
        {"resolved_forward_over_spot", e.resolved_forward_over_spot},
        {"interval_rate", e.interval_rate}, {"interval_yield", e.interval_yield}});
  }
  return result;
}

const char *termination_name(AmericanMarkovFitTermination value) {
  switch (value) {
  case AmericanMarkovFitTermination::StepTolerance: return "step_tolerance";
  case AmericanMarkovFitTermination::EvaluationLimit: return "evaluation_limit";
  case AmericanMarkovFitTermination::SweepLimit: return "sweep_limit";
  case AmericanMarkovFitTermination::ObjectiveTolerance: return "objective_tolerance";
  case AmericanMarkovFitTermination::GradientTolerance: return "gradient_tolerance";
  case AmericanMarkovFitTermination::DampingLimit: return "damping_limit";
  case AmericanMarkovFitTermination::JacobianFailure: return "jacobian_failure";
  }
  return "unknown";
}

// Owned fitter time provenance: one row per original interval, including paused
// variance. Knot-mode basis diagnostics describe the represented interval
// log-sigma map only, not a price Jacobian or calibration identifiability.
void time_layout_json(const AmericanMarkovFit &fit, Json &result) {
  result["time_shape"] = common_time_shape_name(fit.time_shape());
  result["variance_time_knots"] = std::vector<double>(fit.variance_time_knots().begin(),
                                                      fit.variance_time_knots().end());
  Json weights = Json::array();
  for (const auto &row : fit.interval_time_weights()) {
    weights.push_back({{"left_group", row.left_group}, {"right_group", row.right_group},
        {"right_weight", row.right_weight}, {"variance_time", row.variance_time},
        {"variance_active", row.variance_active}});
  }
  result["interval_time_weights"] = std::move(weights);
  const auto &basis = fit.time_basis_diagnostics();
  if (!basis) {
    result["time_basis"] = nullptr;
    return;
  }
  result["time_basis"] = {{"quoted_variance_intervals", basis->quoted_variance_intervals},
      {"column_support_counts", basis->column_support_counts},
      {"column_l2_norms", basis->column_l2_norms},
      {"common_normalization", basis->common_normalization},
      {"qr_reciprocal_condition_inf", basis->qr_reciprocal_condition_inf},
      {"min_reciprocal_condition", kAmericanMarkovFitMinTimeBasisReciprocalCondition}};
  result["time_interpolation"] = "log-linear sigma at each original interval's variance "
      "midpoint, frozen for that whole interval; flat tails; not the exact integrated "
      "variance of a continuous sigma(t)";
}

Json fit_json(const AmericanMarkovFit &fit, std::span<const AmericanMarkovFitQuote> quotes) {
  const auto &d = fit.diagnostics();
  Json result{{"termination", termination_name(d.termination)},
      {"parameter_count", d.parameter_count}, {"time_groups", d.time_groups},
      {"quote_count", d.quote_count}, {"bidless_quotes", d.bidless_quotes},
      {"quotes_inside_interval", d.quotes_inside_interval}, {"evaluations", d.evaluations},
      {"best_evaluation", d.best_evaluation}, {"failed_evaluations", d.failed_evaluations},
      {"accepted_steps", d.accepted_steps}, {"completed_sweeps", d.completed_sweeps},
      {"initial_objective", d.initial_objective}, {"objective", d.objective},
      {"interval_loss", d.interval_loss}, {"midpoint_loss", d.midpoint_loss},
      {"regularization_loss", d.regularization_loss},
      {"max_interval_error", d.max_interval_error}, {"mean_interval_error", d.mean_interval_error},
      {"final_log_step", d.final_log_step},
      {"search", common_search_name(d.search)}, {"evaluation_budget", d.evaluation_budget},
      {"jacobian_evaluations", d.jacobian_evaluations}, {"rejected_steps", d.rejected_steps},
      {"evaluation_rollback_work", d.rollback_work}, {"priced_quotes", d.priced_quotes},
      {"reused_quote_prices", d.reused_quote_prices}, {"final_damping", d.final_damping},
      {"gradient_norm", d.gradient_norm},
      {"sigmas", std::vector<double>(fit.sigmas().begin(), fit.sigmas().end())},
      {"log_state_knots", std::vector<double>(fit.log_state_knots().begin(),
                                               fit.log_state_knots().end())}};
  time_layout_json(fit, result);
  const auto &m = fit.model().diagnostics();
  result["model"] = {{"diffusion_steps", m.diffusion_steps}, {"cash_steps", m.cash_steps},
      {"diffusion_method", fit.model().diffusion_method() == AmericanMarkovDiffusionMethod::ExplicitEuler
          ? "explicit-euler" : "variance-resolvent"},
      {"transition_work", m.transition_work}, {"resolvent_steps", m.resolvent_steps},
      {"physical_resolvent_factors", m.physical_resolvent_factors},
      {"resolvent_factor_blocks", m.resolvent_factor_blocks},
      {"physical_state_values", m.physical_state_values}, {"state_grid_blocks", m.state_grid_blocks},
      {"physical_transition_rows", m.physical_transition_rows},
      {"transition_row_blocks", m.transition_row_blocks},
      {"max_forward_mass_error", m.max_forward_mass_error},
      {"stored_rows", m.stored_rows}, {"max_states", m.max_states},
      {"stored_volatility_values", m.stored_volatility_values},
      {"max_probability_sum_error", m.max_probability_sum_error},
      {"max_relative_mean_error", m.max_relative_mean_error},
      {"terminal_zero_probability", m.terminal_zero_probability},
      {"terminal_upper_probability", m.terminal_upper_probability}};
  // Report the actual compiled clocks and quote-prefix work of the retained
  // best model. This is one objective evaluation's rollback work, not the sum
  // across all optimizer trials or compile-time factor construction.
  const auto &model = fit.model();
  const auto expiries = model.expiry_layers();
  std::vector<std::size_t> quote_counts(expiries.size(), 0);
  for (const auto &quote : quotes) ++quote_counts[quote.expiry_index];
  Json intervals = Json::array();
  std::size_t first = 0, prefix_work = 0, quote_work = 0;
  for (std::size_t i = 0; i < expiries.size(); ++i) {
    std::size_t diffusion_steps = 0, cash_steps = 0, interval_work = 0;
    double min_calendar = std::numeric_limits<double>::infinity(), max_calendar = 0.0;
    double min_variance = std::numeric_limits<double>::infinity(), max_variance = 0.0;
    for (std::size_t j = first; j < expiries[i]; ++j) {
      const auto &transition = model.transitions()[j];
      interval_work += transition.rollback_work();
      if (transition.kind == AmericanMarkovStepKind::CashDividend) {
        ++cash_steps;
      } else {
        ++diffusion_steps;
        min_calendar = std::min(min_calendar, transition.calendar_step);
        max_calendar = std::max(max_calendar, transition.calendar_step);
        min_variance = std::min(min_variance, transition.variance_step);
        max_variance = std::max(max_variance, transition.variance_step);
      }
    }
    prefix_work += interval_work;
    // Successful fits already admitted this sum under the bounded work cap.
    quote_work += quote_counts[i] * prefix_work;
    const auto &endpoint = model.layers()[expiries[i]];
    intervals.push_back({{"interval_index", i}, {"calendar_end", endpoint.calendar_time},
        {"variance_end", endpoint.variance_time}, {"diffusion_steps", diffusion_steps},
        {"cash_steps", cash_steps}, {"transition_work", interval_work},
        {"prefix_transition_work", prefix_work}, {"quote_count", quote_counts[i]},
        {"min_calendar_step", diffusion_steps ? Json(min_calendar) : Json(nullptr)},
        {"max_calendar_step", max_calendar},
        {"min_variance_step", diffusion_steps ? Json(min_variance) : Json(nullptr)},
        {"max_variance_step", max_variance}});
    first = expiries[i];
  }
  result["model"]["resolved_intervals"] = std::move(intervals);
  result["model"]["quote_prefix_rollback_work"] = quote_work;
  return result;
}

void equivalent_iv(Prediction &prediction, const AmericanMarkovFitQuote &quote,
                    const AmericanMarkovExpiryMarketDiagnostics &market, double spot,
                    const EquivalentIvConfig &config) {
  const double years = market.variance_years.median;
  const double intrinsic = std::max(quote.side == Side::Call ? spot - quote.strike
                                                           : quote.strike - spot, 0.0);
  const double numerical_floor = 64.0 * std::numeric_limits<double>::epsilon() *
                                 std::max({1.0, spot, quote.strike});
  if (years <= 0.0 || prediction.price - intrinsic <=
                          std::max(config.min_time_value, numerical_floor)) {
    prediction.vol_status = "unidentifiable";
    return;
  }
  // A reporting convention: flatten cumulative common coefficients to expiry.
  // This is not the local sigma and need not preserve time-varying exercise.
  const auto vol = american_implied_vol(prediction.price, spot, quote.strike, years,
      market.integrated_rate / years, market.integrated_effective_yield / years,
      quote.side, AmericanMethod::AndersenLake, config.tolerance, config.max_iterations);
  if (!vol || !std::isfinite(*vol) || *vol <= 0.0) {
    prediction.vol_status = "inversion_failure";
    return;
  }
  prediction.vol = *vol;
  prediction.vol_status = "ok";
}

bool ambiguous_expiries(const std::vector<InputRow> &rows, std::span<const std::size_t> indices,
                         Json &decision) {
  std::map<std::string, std::set<std::uint64_t>> by_expiry;
  std::map<double, std::set<std::uint64_t>> by_time;
  for (const auto index : indices) {
    const auto &row = rows[index];
    by_expiry[row.identity[2]].insert(row.chain);
    if (std::isfinite(row.market.calendar_years)) {
      by_time[row.market.calendar_years].insert(row.chain);
    }
  }
  std::set<std::uint64_t> ambiguous;
  const auto collect = [&](const auto &groups) {
    for (const auto &[key, chains] : groups) {
      static_cast<void>(key);
      if (chains.size() > 1U) ambiguous.insert(chains.begin(), chains.end());
    }
  };
  collect(by_expiry);
  collect(by_time);
  decision["ambiguous_chain_ids"] = std::vector<std::uint64_t>(ambiguous.begin(), ambiguous.end());
  return !ambiguous.empty();
}

std::vector<AmericanMarkovRawQuote> raw_quotes(const std::vector<InputRow> &rows,
                                              std::span<const std::size_t> indices) {
  std::vector<AmericanMarkovRawQuote> raw;
  raw.reserve(indices.size());
  for (const auto index : indices) {
    const auto &row = rows[index];
    const auto &m = row.market;
    raw.push_back({row.chain, row.quote.strike, row.quote.side, row.quote.bid, row.quote.ask,
        1.0, m.spot, m.calendar_years, m.variance_years, m.rate,
        m.continuous_yield, m.dividend_pv});
  }
  return raw;
}

// Raw bid/ask/strike observations with a single resolved spot and exact shared
// clock endpoints. Storage lives through catalog.resolve; the catalog owns its
// returned immutable configuration, never these observations.
struct LiquidityInputs {
  std::vector<std::vector<QuoteSliceQuote>> quotes;
  std::vector<QuoteFitExpirySnapshot> snapshots;
};

LiquidityInputs liquidity_inputs(const AmericanMarkovPreparedMarket &market,
                                  const AmericanMarkovMarketDiagnostics &diagnostics) {
  LiquidityInputs result;
  result.quotes.resize(market.intervals().size());
  for (const auto &quote : market.quotes()) {
    result.quotes[quote.expiry_index].push_back(
        {quote.strike, quote.side, quote.bid, quote.ask, {}});
  }
  result.snapshots.reserve(result.quotes.size());
  for (std::size_t i = 0; i < result.quotes.size(); ++i) {
    const auto &d = diagnostics.expiries[i];
    QuoteSliceMarket m{market.spot(), d.variance_years.median, d.calendar_years.median,
                       d.rate.median, d.supplied_yield.median, d.dividend_pv.median};
    result.snapshots.push_back({m, result.quotes[i]});
  }
  return result;
}

void fit_snapshot(const std::vector<InputRow> &rows, std::span<const std::size_t> indices,
                   const CommonPolicy &policy, std::vector<Prediction> &predictions,
                   Json &decision, StageTimes &timing) {
  const auto refuse = [&](const std::string &status, const std::string &stage) {
    decision["status"] = status;
    decision["failure_stage"] = stage;
    for (const auto index : indices) predictions[index].status = status;
  };
  if (ambiguous_expiries(rows, indices, decision)) {
    refuse("ambiguous_expiry", "identity");
    return;
  }
  const auto over_market_limit = timing.measure("market", [&] {
    return indices.size() > policy.market.max_quotes;
  });
  if (over_market_limit) {
    decision["market"] = {{"status", "resource_limit"}, {"quote_count", indices.size()}};
    refuse("market_resource_limit", "market");
    return;
  }
  auto prepared = timing.measure("market", [&] {
    const auto raw = raw_quotes(rows, indices);
    return prepare_american_markov_market(raw, policy.market);
  });
  decision["market"] = market_json(prepared);
  if (prepared.diagnostics.refused_row && *prepared.diagnostics.refused_row < indices.size()) {
    decision["market"]["refused_row_id"] = rows[indices[*prepared.diagnostics.refused_row]].id;
  }
  if (!prepared.market) {
    refuse(std::string{"market_"} + to_string(prepared.status), "market");
    return;
  }
  const auto &market = *prepared.market;
  const auto resolved = timing.measure("policy", [&] {
    const auto liquidity = liquidity_inputs(market, prepared.diagnostics);
    return policy.catalog.resolve(rows[indices.front()].identity[0],
                                  liquidity.snapshots, market.intervals());
  });
  if (!resolved) {
    decision["error"] = resolved.error().to_string();
    decision["error_code"] = atx::core::to_string(resolved.error().code());
    refuse("policy_resolution_failure", "policy");
    return;
  }
  const auto &profile = *resolved->profile;
  decision["resolved_profile"] = profile.name;
  decision["policy_source"] = to_string(resolved->source);
  decision["features"] = quote_bench_policy::features_json(resolved->features);
  decision["resolved_configuration"] = common_profile_json(profile);
  decision["time_groups"] = resolved->time_groups;
  decision["parameter_count"] = resolved->parameter_count;
  for (const auto index : indices) {
    auto &prediction = predictions[index];
    prediction.resolved_profile = profile.name;
    prediction.policy_source = to_string(resolved->source);
    prediction.configured_curve = "common-markov";
  }
  const auto grid = timing.measure("grid", [&] {
    return make_american_markov_sinh_grid(market.spot(), profile.grid_config);
  });
  if (!grid) {
    decision["error"] = grid.error().to_string();
    decision["error_code"] = atx::core::to_string(grid.error().code());
    refuse("grid_failure", "grid");
    return;
  }
  decision["grid"] = {{"states", grid->states.size()},
      {"initial_state_index", grid->initial_state_index},
      {"minimum_stock", grid->states.front()}, {"maximum_stock", grid->states.back()},
      {"lower_spot_gap_over_spot", grid->lower_spot_gap_over_spot},
      {"upper_spot_gap_over_spot", grid->upper_spot_gap_over_spot},
      {"min_gap_over_spot", grid->min_gap_over_spot},
      {"max_unit_volatility_exit_rate", grid->max_unit_volatility_exit_rate}};
  const auto over_fit_limit = timing.measure("fit", [&] {
    return market.quotes().size() > profile.fit_config.max_quotes;
  });
  if (over_fit_limit) {
    decision["error"] = "Snapshot quote count exceeds configured fitter max_quotes";
    refuse("fit_quote_limit", "resource");
    return;
  }
  const auto fit_market = market.fit_market(profile.model_config, grid->states);
  auto initialization = configured_common_fit_initialization(
      profile.fit_config, resolved->parameter_count, policy.initialization);
  auto effective_fit = profile.fit_config;
  if (policy.initialization.mode == CommonInitializationMode::QuoteSeed) {
    timing.measure("seed", [&] {
      apply_common_quote_seeds(fit_market, market.quotes(), profile.fit_config,
                              policy.initialization, initialization);
      return true;
    });
    effective_fit.initial_sigmas = initialization.initial_sigmas;
    effective_fit.prior_sigmas = initialization.prior_sigmas;
  }
  decision["initialization"] = std::move(initialization.diagnostics);
  const auto fit = timing.measure("fit", [&] {
    return fit_american_markov_intervals(fit_market, market.quotes(), effective_fit);
  });
  if (!fit) {
    decision["error"] = fit.error().to_string();
    decision["error_code"] = atx::core::to_string(fit.error().code());
    refuse("fit_failure", "fit");
    return;
  }
  decision["fit"] = fit_json(*fit, market.quotes());
  decision["status"] = "ok";
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const auto &quote = market.quotes()[i];
    auto &prediction = predictions[indices[i]];
    prediction.price = fit->quote_prices()[i];
    prediction.status = "ok";
    prediction.admitted = true;
    prediction.actual_curve = "common-markov";
    prediction.quote_status = quote.bid == 0.0 ? "bidless_upper_bound" : "two_sided_interval";
  }
  timing.measure("equivalent_iv", [&] {
    for (std::size_t i = 0; i < indices.size(); ++i) {
      const auto &quote = market.quotes()[i];
      equivalent_iv(predictions[indices[i]], quote,
                    prepared.diagnostics.expiries[quote.expiry_index],
                    market.spot(), policy.equivalent_iv);
    }
    return true;
  });
}
} // namespace

CommonRun run_common_markov(const std::vector<InputRow> &rows, const std::string &selection,
                             const std::string &dividends,
                             const std::filesystem::path &progress_path,
                             const Json &provenance) {
  const auto policy = load_common_policy(selection, dividends);
  if (!progress_path.empty() && selection != "auto" &&
      provenance.at("policy_sha256") != file_sha256(selection)) {
    throw std::runtime_error("Policy changed while reading progress-bound configuration");
  }
  const std::string configuration_hash = json_sha256(policy.definition);
  const Json run_binding{{"source", provenance}, {"configuration_sha256", configuration_hash},
                         {"dividends", dividends}, {"model", "common-markov"}};
  const std::string run_hash = json_sha256(run_binding);
  ProgressJournal progress(progress_path);
  CommonRun result{std::vector<Prediction>(rows.size()), Json::object()};
  std::map<Identity, std::vector<std::size_t>> groups;
  std::map<std::uint64_t, std::array<std::string, 4>> chain_identity;
  std::set<std::uint64_t> inconsistent;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    auto &prediction = result.predictions[i];
    prediction.model = "common-markov";
    if (rows[i].input_status != "ok") {
      prediction.status = rows[i].input_status == "unsupported_deliverable"
          ? "unsupported_deliverable" : "input_rejected";
      continue;
    }
    groups[snapshot_identity(rows[i])].push_back(i);
    const auto [entry, inserted] = chain_identity.emplace(rows[i].chain, rows[i].identity);
    if (!inserted && entry->second != rows[i].identity) inconsistent.insert(rows[i].chain);
  }
  Json decisions = Json::array();
  Json failures = Json::object();
  Json stages = Json::object();
  Json solver_codes = Json::object();
  Json market_failures = Json::object();
  Json resource_failures = Json::object();
  Json profiles = Json::object();
  Json initialization_states = Json::object();
  Json fit_terminations = Json::object();
  std::size_t fitted_snapshots = 0;
  std::size_t fitted_chains = 0;
  std::size_t ambiguous_chains = 0;
  std::size_t completed_snapshots = 0;
  std::size_t completed_rows = 0;
  std::size_t known_evaluations = 0;
  std::size_t unavailable_evaluation_counts = 0;
  std::size_t eligible_rows = 0;
  for (const auto &[identity, indices] : groups) {
    static_cast<void>(identity);
    eligible_rows += indices.size();
  }
  StageTimes totals;
  progress.append({{"record_type", "start"}, {"schema_version", 1}, {"sequence", 0},
      {"run_binding_sha256", run_hash}, {"run_binding", run_binding},
      {"configuration", policy.definition}, {"snapshots_expected", groups.size()},
      {"input_rows", rows.size()}, {"completed_snapshots", 0},
      {"rows_excluded_by_input_status", rows.size() - eligible_rows},
      {"state", "incomplete"}, {"record_max_bytes", 4U * 1024U * 1024U},
      {"completion_scope", "numerical snapshot processing only; final predictions/report "
          "and wrapper run receipt are separate outputs"},
      {"interruption_semantics", "no completion record means incomplete; only full "
          "newline-terminated records are valid; no resume or in-flight snapshot recovery"}});
  const auto start = Clock::now();
  for (const auto &[identity, indices] : groups) {
    const auto snapshot_start = Clock::now();
    StageTimes timing;
    std::set<std::uint64_t> chains;
    for (const auto index : indices) chains.insert(rows[index].chain);
    Json decision{{"underlier", identity[0]}, {"root", identity[1]}, {"snapshot_ns", identity[2]},
                  {"rows", indices.size()},
                  {"initialization", common_initialization_pending(policy.initialization)},
                  {"chain_ids", std::vector<std::uint64_t>(chains.begin(), chains.end())}};
    if (std::any_of(chains.begin(), chains.end(),
                    [&](auto id) { return inconsistent.contains(id); })) {
      decision["status"] = "inconsistent_chain";
      decision["failure_stage"] = "identity";
      for (const auto index : indices) result.predictions[index].status = "inconsistent_chain";
    } else {
      fit_snapshot(rows, indices, policy, result.predictions, decision, timing);
    }
    const auto status = decision.at("status").get<std::string>();
    auto &initialization = decision["initialization"];
    if (initialization["state"] == "not_attempted") initialization["reason"] = status;
    count(initialization_states, initialization.at("state").get<std::string>());
    if (status == "ok") {
      ++fitted_snapshots;
      fitted_chains += chains.size();
      count(fit_terminations, decision.value("resolved_profile", std::string{}) + ":" +
                                  decision["fit"]["termination"].get<std::string>());
    } else {
      count(failures, decision.value("error", status));
      count(stages, decision.at("failure_stage").get<std::string>());
      if (decision.contains("error_code")) {
        count(solver_codes, decision.at("error_code").get<std::string>());
      }
      if (decision.contains("market") && decision["market"]["status"] != "success") {
        count(market_failures, decision["market"]["status"].get<std::string>());
      }
      if (status == "market_resource_limit" || status == "fit_quote_limit") {
        count(resource_failures, status);
      }
    }
    if (decision.contains("ambiguous_chain_ids")) {
      ambiguous_chains += decision["ambiguous_chain_ids"].size();
    }
    if (decision.contains("resolved_profile")) {
      auto &p = profiles[decision.at("resolved_profile").get<std::string>()];
      count(p, "resolved_snapshots");
      count(p, "fitted_snapshots", status == "ok" ? 1U : 0U);
      count(p, "attempted_chains", chains.size());
      count(p, "fitted_chains", status == "ok" ? chains.size() : 0U);
      count(p["initialization_states"], initialization.at("state").get<std::string>());
    }
    decision["elapsed_microseconds"] =
        std::chrono::duration<double, std::micro>(Clock::now() - snapshot_start).count();
    decision["stage_elapsed_microseconds"] = timing.elapsed;
    decision["stage_attempted"] = timing.attempted;
    // The fitter currently returns no evaluation ledger on a failed Result.
    // Zero is known before fitting; a failed fit is explicitly unavailable.
    const Json evaluations = decision.contains("fit") ? decision["fit"]["evaluations"]
        : status == "fit_failure" ? Json(nullptr) : Json(0);
    decision["objective_evaluations"] = evaluations;
    if (evaluations.is_null()) ++unavailable_evaluation_counts;
    else known_evaluations += evaluations.get<std::size_t>();
    for (const auto &[stage, value] : timing.elapsed.items()) {
      totals.elapsed[stage] = totals.elapsed[stage].get<double>() + value.get<double>();
    }
    ++completed_snapshots;
    completed_rows += indices.size();
    Json row_counts = Json::object();
    Json vol_counts = Json::object();
    for (const auto index : indices) {
      count(row_counts, result.predictions[index].status);
      count(vol_counts, result.predictions[index].vol_status);
    }
    progress.append({{"record_type", "snapshot"}, {"schema_version", 1},
        {"sequence", completed_snapshots}, {"run_binding_sha256", run_hash},
        {"underlier", identity[0]}, {"root", identity[1]}, {"snapshot_ns", identity[2]},
        {"rows", indices.size()}, {"chains", chains.size()}, {"status", status},
        {"failure_stage", decision.value("failure_stage", "")},
        {"resolved_profile", decision.value("resolved_profile", "")},
        {"completed_snapshots", completed_snapshots}, {"completed_rows", completed_rows},
        {"row_status_counts", row_counts}, {"vol_status_counts", vol_counts},
        {"elapsed_microseconds", decision["elapsed_microseconds"]},
        {"stage_elapsed_microseconds", timing.elapsed}, {"stage_attempted", timing.attempted},
        {"objective_evaluations", evaluations},
        {"initialization", common_initialization_compact(initialization)},
        {"fit_termination", decision.contains("fit")
            ? decision["fit"]["termination"] : Json(nullptr)}});
    decisions.push_back(std::move(decision));
  }
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  std::size_t served = 0;
  std::size_t vol_served = 0;
  std::size_t unresolved_profile_rows = 0;
  Json row_status = Json::object();
  Json vol_status = Json::object();
  for (const auto &prediction : result.predictions) {
    const bool price_ok = prediction.status == "ok" && std::isfinite(prediction.price);
    const bool vol_ok = price_ok && std::isfinite(prediction.vol) && prediction.vol > 0.0;
    served += price_ok ? 1U : 0U;
    vol_served += vol_ok ? 1U : 0U;
    count(row_status, prediction.status);
    count(vol_status, prediction.vol_status);
    if (prediction.resolved_profile.empty()) {
      ++unresolved_profile_rows;
      continue;
    }
    auto &p = profiles[prediction.resolved_profile];
    count(p, "rows");
    count(p, "served_rows", price_ok ? 1U : 0U);
    count(p, "served_vol_rows", vol_ok ? 1U : 0U);
    count(p, "admitted_quotes", prediction.admitted ? 1U : 0U);
    count(p["row_status_counts"], prediction.status);
    count(p["vol_status_counts"], prediction.vol_status);
  }
  // Schema 4: each decision's fit record replaced interval_groups with
  // time_shape, variance_time_knots, interval_time_weights and time_basis
  // (plus time_interpolation for knot mode), and fit termination may be
  // jacobian_failure. Every other key is as in schema 3.
  result.report = {{"schema_version", 4}, {"model", "common-markov"}, {"rows", rows.size()},
      {"chains", chain_identity.size()}, {"snapshots", groups.size()},
      {"fitted_snapshots", fitted_snapshots}, {"fitted_chains", fitted_chains},
      {"served_rows", served}, {"served_vol_rows", vol_served}, {"admitted_quotes", served},
      {"ambiguous_expiry_chains", ambiguous_chains},
      {"unresolved_profile_rows", unresolved_profile_rows},
      {"policy_mode", selection == "auto" ? "auto" : "file"},
      {"policy_definition_format", "expanded-common-markov-runtime-configuration-v1"},
      {"policy_definition", policy.definition}, {"policy_decisions", decisions},
      {"per_profile_counts", profiles}, {"fit_failure_counts", failures},
      {"initialization_states", initialization_states},
      {"fit_termination_counts", fit_terminations},
      {"market_failure_counts", market_failures}, {"resource_refusal_counts", resource_failures},
      {"solver_failure_code_counts", solver_codes},
      {"resource_refusal_scope", "typed market/quote-count limits; numerical/compiler/fitter "
          "refusals retain full error code and message in fit_failure_counts"},
      {"snapshot_failure_stage_counts", stages}, {"row_status_counts", row_status},
      {"vol_status_counts", vol_status}, {"dividend_model", dividends},
      {"dividend_convention_assumption", dividends == "none"
          ? "continuous yield only; nonzero cash PV is refused"
          : "aggregate cash PV replaced by common continuous yield; exercise around "
            "unknown ex-dates is not represented"},
      {"chain_market_aggregation", "equal-weight coordinate medians with mandatory "
          "dispersion checks; no trimming"},
      {"contract_carry", "one common cumulative funding/yield schedule per exact stock snapshot"},
      {"source_provenance", "closed raw quote CSV only; wrapper receipt binds input, "
          "binary and policy hashes"},
      {"serving_representation", "one finite positive Markov process with common expiry prefixes"},
      {"predicted_vol_convention", "equivalent flat-to-expiry American IV: Andersen-Lake, "
          "variance years, r=integrated_common_rate/variance_years, "
          "q=integrated_common_effective_yield/variance_years; not fitted local sigma"},
      {"iv_failure_policy", "price remains usable; unidentifiable or failed IV is blank "
          "with vol_status"},
      {"fit_quality_scope", "bounded best iterate; interval loss and termination disclosed; "
          "no global optimizer certificate"},
      {"mandatory_admission", "common positive martingale transitions and Snell rollback, "
          "unchanged compiler/fitter budgets; no post-fit repairs"},
      {"accuracy_scope", "finite Bermudan model consistency; no continuous-American, "
          "tail or discretization error certificate"},
      {"fit_and_reprice_seconds", seconds},
      {"served_rows_per_second", seconds > 0 ? static_cast<double>(served) / seconds : 0},
      {"timing_scope", "serial common-market preparation, policy resolution, grid, optional seeding, fitting and "
          "equivalent IV; excludes CSV IO and policy parsing"},
      {"performance_claim", "diagnostic; requires quiet repeated release runs"}};
  result.report["stage_elapsed_microseconds"] = totals.elapsed;
  result.report["objective_evaluations_known"] = known_evaluations;
  result.report["objective_evaluation_counts_unavailable"] = unavailable_evaluation_counts;
  result.report["stage_timing_scope"] = "wall-clock stage calls, attempted flags distinguish "
      "unvisited stages; snapshot includes orchestration, cohort includes progress IO; "
      "fitter failure evaluation counts unavailable, never represented as zero";
  result.report["progress"] = {{"enabled", !progress_path.empty()},
      {"path", progress_path.empty() ? Json(nullptr)
          : Json(std::filesystem::weakly_canonical(progress_path).string())},
      {"run_binding_sha256", run_hash}, {"configuration_sha256", configuration_hash},
      {"completed_snapshots", completed_snapshots}, {"completed_rows", completed_rows}};
#ifdef __VERSION__
  result.report["compiler"] = __VERSION__;
#elif defined(_MSC_VER)
  result.report["compiler"] = "MSVC " + std::to_string(_MSC_VER);
#else
  result.report["compiler"] = "unknown";
#endif
#ifdef NDEBUG
  result.report["build_type"] = "release";
#else
  result.report["build_type"] = "debug";
#endif
  progress.append({{"record_type", "complete"}, {"schema_version", 1},
      {"sequence", completed_snapshots + 1U}, {"run_binding_sha256", run_hash},
      {"state", "numerical_processing_complete"}, {"completed_snapshots", completed_snapshots},
      {"completed_rows", completed_rows}, {"input_rows", rows.size()},
      {"rows_excluded_by_input_status", rows.size() - completed_rows},
      {"row_status_counts", row_status}, {"vol_status_counts", vol_status},
      {"initialization_states", initialization_states},
      {"stage_elapsed_microseconds", totals.elapsed},
      {"objective_evaluations_known", known_evaluations},
      {"objective_evaluation_counts_unavailable", unavailable_evaluation_counts}});
  return result;
}
} // namespace quote_bench

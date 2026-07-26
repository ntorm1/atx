#include "atx/vol/quant_pipeline.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace atx::vol {
namespace {

[[nodiscard]] bool identity_is_zero(const ArchiveContentIdentity &identity) noexcept {
  return identity.file_size == 0u && identity.created_ts_ns == 0 && identity.header_crc32c == 0u &&
         identity.metadata_crc32c == 0u;
}

[[nodiscard]] Result<const std::vector<double> *> find_signal(const BacktestResult &backtest,
                                                              std::string_view name) {
  const std::vector<double> *found = nullptr;
  for (const auto &[candidate_name, values] : backtest.signals) {
    if (candidate_name != name) {
      continue;
    }
    if (found != nullptr) {
      return Err(ErrorCode::InvalidArgument,
                 "BacktestResult contains duplicate signal name '" + std::string(name) + "'");
    }
    found = &values;
  }
  if (found == nullptr) {
    return Err(ErrorCode::NotFound,
               "BacktestResult has no signal named '" + std::string(name) + "'");
  }
  return found;
}

[[nodiscard]] Status validate_backtest_shape(const BacktestSeriesInfo &info,
                                             const BacktestSeriesData &series,
                                             std::string_view signal_name,
                                             const std::vector<double> &signal) {
  const BacktestResult &backtest = series.backtest;
  const std::size_t rows = backtest.size();
  if (info.uid == 0u || info.template_id.empty() || info.symbol.empty() ||
      info.partition_filename.empty() || identity_is_zero(info.partition_identity)) {
    return Err(ErrorCode::InvalidArgument,
               "BacktestSeriesInfo lacks an indexed immutable partition identity");
  }
  if (rows < 3u) {
    return Err(ErrorCode::InvalidArgument,
               "backtest signal research requires at least three recorded rows");
  }
  if (info.row_count != rows || backtest.ts_ns.size() != rows ||
      backtest.pnl_total.size() != rows || signal.size() != rows) {
    return Err(ErrorCode::InvalidArgument, "backtest series, signal '" + std::string(signal_name) +
                                               "', and catalog row counts are not parallel");
  }
  for (std::size_t i = 0; i < rows; ++i) {
    if (!std::isfinite(signal[i]) || !std::isfinite(backtest.pnl_total[i])) {
      return Err(ErrorCode::InvalidArgument,
                 "backtest signal research input contains a non-finite value");
    }
    if (i != 0u && backtest.ts_ns[i] <= backtest.ts_ns[i - 1u]) {
      return Err(ErrorCode::InvalidArgument, "backtest timestamps must be strictly increasing");
    }
  }
  return Status{};
}

[[nodiscard]] const ResearchCandidateEvaluation *
selected_evaluation(const ResearchMiningResult &mining) noexcept {
  const auto selected =
      std::find_if(mining.evaluations.begin(), mining.evaluations.end(),
                   [&mining](const ResearchCandidateEvaluation &evaluation) {
                     return evaluation.candidate_identity == mining.selected_candidate_identity &&
                            evaluation.candidate.id == mining.selected_candidate_id;
                   });
  return selected == mining.evaluations.end() ? nullptr : &*selected;
}

[[nodiscard]] bool same_stats(const ResearchReturnStats &lhs,
                              const ResearchReturnStats &rhs) noexcept {
  return lhs.n_observations == rhs.n_observations && lhs.attempted_trials == rhs.attempted_trials &&
         lhs.newey_west_lag == rhs.newey_west_lag && lhs.mean == rhs.mean &&
         lhs.sample_standard_deviation == rhs.sample_standard_deviation &&
         lhs.skewness == rhs.skewness && lhs.pearson_kurtosis == rhs.pearson_kurtosis &&
         lhs.sharpe == rhs.sharpe &&
         lhs.newey_west_long_run_variance == rhs.newey_west_long_run_variance &&
         lhs.hac_mean_standard_error == rhs.hac_mean_standard_error &&
         lhs.hac_t_statistic == rhs.hac_t_statistic &&
         lhs.one_sided_p_value == rhs.one_sided_p_value &&
         lhs.probabilistic_sharpe_probability == rhs.probabilistic_sharpe_probability &&
         lhs.deflated_sharpe_probability == rhs.deflated_sharpe_probability &&
         lhs.deflated_sharpe_threshold == rhs.deflated_sharpe_threshold &&
         lhs.max_drawdown == rhs.max_drawdown;
}

[[nodiscard]] bool same_evaluation(const ResearchCandidateEvaluation &lhs,
                                   const ResearchCandidateEvaluation &rhs) noexcept {
  return lhs.candidate == rhs.candidate && lhs.candidate_identity == rhs.candidate_identity &&
         lhs.in_sample_returns == rhs.in_sample_returns && lhs.oos_returns == rhs.oos_returns &&
         same_stats(lhs.in_sample_stats, rhs.in_sample_stats) &&
         same_stats(lhs.oos_stats, rhs.oos_stats);
}

[[nodiscard]] bool same_mining_result(const ResearchMiningResult &lhs,
                                      const ResearchMiningResult &rhs) noexcept {
  return lhs.selected_candidate_id == rhs.selected_candidate_id &&
         lhs.selected_candidate_identity == rhs.selected_candidate_identity &&
         lhs.evaluations.size() == rhs.evaluations.size() &&
         std::equal(lhs.evaluations.begin(), lhs.evaluations.end(), rhs.evaluations.begin(),
                    rhs.evaluations.end(), same_evaluation);
}

[[nodiscard]] bool same_position(const Position &lhs, const Position &rhs) noexcept {
  return lhs.id == rhs.id && lhs.contract == rhs.contract && lhs.qty == rhs.qty &&
         lhs.multiplier == rhs.multiplier;
}

[[nodiscard]] Status validate_risk_alignment(std::span<const NamedPosition> target,
                                             std::span<const PositionRiskInput> risk_inputs,
                                             std::span<const ScenarioRiskInput> scenario_inputs) {
  if (target.empty() || risk_inputs.size() != target.size() ||
      scenario_inputs.size() != target.size()) {
    return Err(ErrorCode::InvalidArgument,
               "implementation risk inputs must be one-for-one with a nonempty target");
  }
  std::vector<Position> target_positions;
  target_positions.reserve(target.size());
  for (const NamedPosition &named : target) {
    if (named.symbol.empty()) {
      return Err(ErrorCode::InvalidArgument, "implementation target symbol must not be empty");
    }
    target_positions.push_back(named.position);
  }
  std::sort(target_positions.begin(), target_positions.end(),
            [](const Position &lhs, const Position &rhs) { return lhs.id < rhs.id; });
  if (std::adjacent_find(target_positions.begin(), target_positions.end(),
                         [](const Position &lhs, const Position &rhs) {
                           return lhs.id == rhs.id;
                         }) != target_positions.end()) {
    return Err(ErrorCode::InvalidArgument, "implementation target position IDs must be unique");
  }

  std::vector<const PositionRiskInput *> ordered_risk;
  ordered_risk.reserve(risk_inputs.size());
  for (const PositionRiskInput &input : risk_inputs) {
    ordered_risk.push_back(&input);
  }
  std::sort(ordered_risk.begin(), ordered_risk.end(),
            [](const PositionRiskInput *lhs, const PositionRiskInput *rhs) {
              return lhs->position.id < rhs->position.id;
            });

  std::vector<const ScenarioRiskInput *> ordered_scenarios;
  ordered_scenarios.reserve(scenario_inputs.size());
  for (const ScenarioRiskInput &input : scenario_inputs) {
    ordered_scenarios.push_back(&input);
  }
  std::sort(ordered_scenarios.begin(), ordered_scenarios.end(),
            [](const ScenarioRiskInput *lhs, const ScenarioRiskInput *rhs) {
              return lhs->position.id < rhs->position.id;
            });

  for (std::size_t i = 0; i < target_positions.size(); ++i) {
    if (!same_position(target_positions[i], ordered_risk[i]->position)) {
      return Err(ErrorCode::InvalidArgument,
                 "implementation Greek inputs do not match the target positions");
    }
    if (!same_position(target_positions[i], ordered_scenarios[i]->position)) {
      return Err(ErrorCode::InvalidArgument,
                 "implementation scenario inputs do not match the target positions");
    }
    if (!(ordered_risk[i]->greeks_per_share == ordered_scenarios[i]->greeks_per_share)) {
      return Err(ErrorCode::InvalidArgument,
                 "implementation risk and scenario Greek snapshots do not match");
    }
  }
  return Status{};
}

} // namespace

Result<BacktestSignalResearchResult>
mine_backtest_signal_series(const BacktestSeriesInfo &info, const BacktestSeriesData &series,
                            const BacktestSignalResearchSpec &spec) {
  if (spec.signal_name.empty() || !std::isfinite(spec.lagged_capital) ||
      spec.lagged_capital <= 0.0 || spec.candidates.empty() || !spec.family.sealed ||
      spec.family.attempted_trials < spec.candidates.size()) {
    return Err(ErrorCode::InvalidArgument,
               "research spec requires a signal, positive lagged capital, candidates, "
               "and a sealed family counting every candidate");
  }

  ATX_TRY(const std::vector<double> *signal, find_signal(series.backtest, spec.signal_name));
  ATX_TRY_VOID(validate_backtest_shape(info, series, spec.signal_name, *signal));

  std::vector<ResearchObservation> observations;
  observations.reserve(series.backtest.size() - 2u);
  for (std::size_t i = 0; i + 2u < series.backtest.size(); ++i) {
    ResearchObservation observation;
    observation.uid = info.uid;
    observation.observed_ts_ns = series.backtest.ts_ns[i];
    observation.available_ts_ns = series.backtest.ts_ns[i];
    observation.decision_ts_ns = series.backtest.ts_ns[i];
    observation.execution_ts_ns = series.backtest.ts_ns[i + 1u];
    observation.label_end_ts_ns = series.backtest.ts_ns[i + 2u];
    observation.signal = (*signal)[i];
    // pnl_total[row] is the flow from the preceding stored row to this row.
    observation.forward_pnl = series.backtest.pnl_total[i + 2u];
    observation.lagged_capital = spec.lagged_capital;
    observation.source_identity = info.partition_identity;
    observations.push_back(observation);
  }

  ATX_TRY(auto canonical, canonicalize_research_observations(observations));
  ATX_TRY(auto validation, make_purged_walk_forward_plan(canonical, spec.validation));
  ATX_TRY(auto mining, mine_research_signal_candidates(canonical, validation, spec.candidates,
                                                       spec.newey_west_lag, spec.family));

  BacktestSignalResearchResult out;
  out.observations = std::move(canonical);
  out.validation = std::move(validation);
  out.mining = std::move(mining);
  return out;
}

Result<ResearchArtifactInfo> publish_backtest_signal_trial(
    ResearchDb &db, const BacktestSeriesInfo &info, const BacktestSignalResearchSpec &research_spec,
    const BacktestSignalResearchResult &result, const ResearchTrialPublishSpec &publish_spec) {
  if (publish_spec.logical_id.empty() || result.observations.empty() ||
      result.validation.folds.empty() || result.mining.evaluations.empty() ||
      identity_is_zero(info.partition_identity) || !research_spec.family.sealed ||
      research_spec.family.attempted_trials < research_spec.candidates.size() ||
      result.validation.spec != research_spec.validation || info.template_id.empty() ||
      info.symbol.empty() || info.uid == 0u || info.partition_filename.empty() ||
      info.row_count != result.observations.size() + 2u) {
    return Err(ErrorCode::InvalidArgument,
               "trial publication requires a logical id, exact validation spec, sealed family, "
               "and complete research evidence");
  }
  ATX_TRY(auto canonical, canonicalize_research_observations(result.observations));
  if (canonical != result.observations ||
      std::any_of(canonical.begin(), canonical.end(), [&info](const ResearchObservation &row) {
        return row.uid != info.uid || row.source_identity != info.partition_identity;
      })) {
    return Err(ErrorCode::InvalidArgument,
               "trial publication observations are noncanonical or have mismatched lineage");
  }
  ATX_TRY_VOID(validate_research_plan_no_leakage(canonical, result.validation));
  ATX_TRY(auto recomputed,
          mine_research_signal_candidates(canonical, result.validation, research_spec.candidates,
                                          research_spec.newey_west_lag, research_spec.family));
  if (!same_mining_result(recomputed, result.mining)) {
    return Err(ErrorCode::InvalidArgument,
               "trial publication evidence does not match deterministic recomputation");
  }
  const ResearchCandidateEvaluation *selected = selected_evaluation(result.mining);
  if (selected == nullptr || selected->oos_returns.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "selected research candidate is absent or has no OOS return path");
  }

  const std::vector<std::uint32_t> summary_candidates{
      static_cast<std::uint32_t>(result.mining.evaluations.size())};
  const std::vector<std::int64_t> summary_selected{
      std::bit_cast<std::int64_t>(result.mining.selected_candidate_identity)};
  const std::vector<std::int64_t> summary_run{std::bit_cast<std::int64_t>(info.run_identity_hash)};
  const std::vector<double> summary_mean{selected->oos_stats.mean};
  const std::vector<double> summary_hac_t{selected->oos_stats.hac_t_statistic};
  const std::vector<double> summary_dsr{selected->oos_stats.deflated_sharpe_probability};
  const std::vector<double> summary_drawdown{selected->oos_stats.max_drawdown};

  RaSectionData summary;
  summary.name = "trial_summary";
  summary.kind = RaSectionKind::SubTable;
  summary.n_rows = 1u;
  summary.columns.emplace_back(
      "n_candidates", RaColumnData::of_u32(std::span<const std::uint32_t>(summary_candidates)));
  summary.columns.emplace_back(
      "selected_id", RaColumnData::of_i64(std::span<const std::int64_t>(summary_selected)));
  summary.columns.emplace_back("run_identity",
                               RaColumnData::of_i64(std::span<const std::int64_t>(summary_run)));
  summary.columns.emplace_back("oos_mean",
                               RaColumnData::of_f64(std::span<const double>(summary_mean)));
  summary.columns.emplace_back("oos_hac_t",
                               RaColumnData::of_f64(std::span<const double>(summary_hac_t)));
  summary.columns.emplace_back("oos_dsr",
                               RaColumnData::of_f64(std::span<const double>(summary_dsr)));
  summary.columns.emplace_back("max_drawdown",
                               RaColumnData::of_f64(std::span<const double>(summary_drawdown)));

  std::vector<std::int64_t> oos_ts;
  std::vector<double> oos_pnl;
  std::vector<double> oos_capital;
  std::vector<double> oos_value;
  oos_ts.reserve(selected->oos_returns.size());
  oos_pnl.reserve(selected->oos_returns.size());
  oos_capital.reserve(selected->oos_returns.size());
  oos_value.reserve(selected->oos_returns.size());
  for (const ResearchReturnObservation &row : selected->oos_returns) {
    oos_ts.push_back(row.decision_ts_ns);
    oos_pnl.push_back(row.pnl);
    oos_capital.push_back(row.lagged_capital);
    oos_value.push_back(row.value);
  }

  RaSectionData oos;
  oos.name = "oos_returns";
  oos.kind = RaSectionKind::TimeSeries;
  oos.n_rows = oos_ts.size();
  oos.columns.emplace_back("decision_ts_ns",
                           RaColumnData::of_i64(std::span<const std::int64_t>(oos_ts)));
  oos.columns.emplace_back("pnl", RaColumnData::of_f64(std::span<const double>(oos_pnl)));
  oos.columns.emplace_back("lagged_capital",
                           RaColumnData::of_f64(std::span<const double>(oos_capital)));
  oos.columns.emplace_back("value", RaColumnData::of_f64(std::span<const double>(oos_value)));

  std::vector<std::uint32_t> fold_id;
  std::vector<std::int64_t> fold_train;
  std::vector<std::int64_t> fold_test;
  std::vector<std::int64_t> fold_purged;
  std::vector<std::int64_t> fold_embargoed;
  fold_id.reserve(result.validation.folds.size());
  fold_train.reserve(result.validation.folds.size());
  fold_test.reserve(result.validation.folds.size());
  fold_purged.reserve(result.validation.folds.size());
  fold_embargoed.reserve(result.validation.folds.size());
  for (const ResearchValidationFold &fold : result.validation.folds) {
    fold_id.push_back(fold.id);
    fold_train.push_back(static_cast<std::int64_t>(fold.train_indices.size()));
    fold_test.push_back(static_cast<std::int64_t>(fold.test_indices.size()));
    fold_purged.push_back(static_cast<std::int64_t>(fold.purged_indices.size()));
    fold_embargoed.push_back(static_cast<std::int64_t>(fold.embargoed_indices.size()));
  }

  RaSectionData folds;
  folds.name = "validation_folds";
  folds.kind = RaSectionKind::SubTable;
  folds.n_rows = fold_id.size();
  folds.columns.emplace_back("fold_id",
                             RaColumnData::of_u32(std::span<const std::uint32_t>(fold_id)));
  folds.columns.emplace_back("n_train",
                             RaColumnData::of_i64(std::span<const std::int64_t>(fold_train)));
  folds.columns.emplace_back("n_test",
                             RaColumnData::of_i64(std::span<const std::int64_t>(fold_test)));
  folds.columns.emplace_back("n_purged",
                             RaColumnData::of_i64(std::span<const std::int64_t>(fold_purged)));
  folds.columns.emplace_back("n_embargoed",
                             RaColumnData::of_i64(std::span<const std::int64_t>(fold_embargoed)));

  ResearchPublishRequest request;
  request.kind = ResearchArtifactKind::Trial;
  request.logical_id = publish_spec.logical_id;
  request.expected_head_id = publish_spec.expected_head_id;
  request.payload_schema_salt = kQuantPipelineTrialSchemaSalt;
  request.first_ts_ns = result.observations.front().decision_ts_ns;
  request.last_ts_ns = result.observations.back().label_end_ts_ns;
  request.row_count = selected->oos_returns.size();
  request.parameters = {
      ResearchParameter::text("series", "template_id", info.template_id),
      ResearchParameter::text("series", "symbol", info.symbol),
      ResearchParameter::text("signal", "name", research_spec.signal_name),
      ResearchParameter::text("selection", "candidate", result.mining.selected_candidate_id),
      ResearchParameter::i64(
          "selection", "candidate_identity",
          std::bit_cast<std::int64_t>(result.mining.selected_candidate_identity)),
      ResearchParameter::i64("selection", "attempted_trials",
                             static_cast<std::int64_t>(research_spec.family.attempted_trials)),
      ResearchParameter::i64("statistics", "newey_west_lag",
                             static_cast<std::int64_t>(research_spec.newey_west_lag)),
  };
  ResearchDependency dependency;
  dependency.role = 1u;
  dependency.kind = 1u;
  dependency.key = info.template_id + "/" + info.symbol;
  dependency.archive_identity = info.partition_identity;
  request.dependencies.push_back(std::move(dependency));
  request.sections = {std::move(summary), std::move(oos), std::move(folds)};
  return db.publish(request);
}

Result<std::vector<NamedPosition>> dispersion_named_positions(const DispersionBook &book) {
  if (book.index_leg.symbol.empty() || book.index_leg.uid == 0u ||
      book.positions.size() != 2u * (1u + book.name_legs.size())) {
    return Err(ErrorCode::InvalidArgument,
               "dispersion book has no complete index/name straddle position mapping");
  }
  std::vector<NamedPosition> output;
  output.reserve(book.positions.size());
  const auto append_pair = [&output, &book](const DispersionLeg &leg,
                                            std::size_t offset) -> Status {
    if (leg.symbol.empty() || leg.uid == 0u || offset + 1u >= book.positions.size() ||
        book.positions[offset].contract.uid != leg.uid ||
        book.positions[offset + 1u].contract.uid != leg.uid ||
        book.positions[offset].qty != leg.straddle_qty ||
        book.positions[offset + 1u].qty != leg.straddle_qty) {
      return Err(ErrorCode::InvalidArgument,
                 "dispersion leg does not match its emitted option positions");
    }
    output.push_back(NamedPosition{leg.symbol, book.positions[offset]});
    output.push_back(NamedPosition{leg.symbol, book.positions[offset + 1u]});
    return Status{};
  };
  ATX_TRY_VOID(append_pair(book.index_leg, 0u));
  for (std::size_t i = 0; i < book.name_legs.size(); ++i) {
    ATX_TRY_VOID(append_pair(book.name_legs[i], 2u * (i + 1u)));
  }
  return output;
}

Result<StrategyImplementationPlan> build_strategy_implementation_plan(
    std::span<const NamedPosition> target, std::span<const NamedPosition> current,
    std::span<const PositionRiskInput> risk_inputs,
    std::span<const ScenarioRiskInput> scenario_inputs, const StrategyImplementationSpec &spec) {
  if (spec.strategy_fingerprint == 0u || spec.decision_ts_ns <= 0 || spec.scenarios.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "implementation spec requires identity, decision time, and scenario evidence");
  }
  ATX_TRY_VOID(validate_risk_alignment(target, risk_inputs, scenario_inputs));
  ATX_TRY(auto aggregate, aggregate_strategy_risk(risk_inputs));

  std::vector<ConditionalComponentScenarioResult> scenario_results;
  std::vector<double> scenario_pnl;
  scenario_results.reserve(spec.scenarios.size());
  scenario_pnl.reserve(spec.scenarios.size());
  for (const ConditionalComponentScenario &scenario : spec.scenarios) {
    ATX_TRY(auto result, conditional_component_scenario_pnl(scenario_inputs, scenario));
    scenario_pnl.push_back(result.total_pnl);
    scenario_results.push_back(std::move(result));
  }
  ATX_TRY(auto overlay, apply_scalar_risk_overlay(aggregate, scenario_pnl, spec.risk_limits));

  std::vector<NamedPosition> adjusted(target.begin(), target.end());
  for (NamedPosition &position : adjusted) {
    position.position.qty *= overlay.scale;
  }
  std::vector<HedgeTarget> adjusted_hedges = spec.hedge_targets;
  for (HedgeTarget &hedge : adjusted_hedges) {
    hedge.target_shares =
        hedge.current_shares + overlay.scale * (hedge.target_shares - hedge.current_shares);
  }
  ATX_TRY(auto intent,
          make_basket_order_intent(spec.strategy_fingerprint, spec.decision_ts_ns, adjusted,
                                   current, adjusted_hedges, spec.algo, spec.disposition));

  StrategyImplementationPlan plan;
  plan.unconstrained_target.assign(target.begin(), target.end());
  plan.risk_adjusted_target = std::move(adjusted);
  plan.unconstrained_risk = aggregate;
  plan.scenario_results = std::move(scenario_results);
  plan.risk_overlay = std::move(overlay);
  plan.intent = std::move(intent);
  return plan;
}

} // namespace atx::vol

#include "research_portfolio.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>

#include "atx/engine/book/replay.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx::server::research_detail {
namespace {
namespace alpha = atx::engine::alpha;
namespace book = atx::engine::book;
namespace risk = atx::engine::risk;
constexpr double tolerance = 1e-6;
constexpr risk::AdmmSchedule allocation_schedule{
    {25, 75, 150}, true, 1e-6, 1e6, 1000, 5, 1.6, false, 25, 1e-9, 1e-9};

std::string session_date(std::int64_t key) {
  return date_text(std::chrono::floor<std::chrono::days>(
      std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{key}}));
}

void preferences(const DatasetPanel& data, const alpha::SignalSet& signal, std::size_t day,
                 double target_gross, std::span<double> output) {
  const auto row = signal.alpha_cross_section(0, day + data.score_begin);
  std::size_t count = 0;
  double scale = 0;
  for (std::size_t name = 0; name < output.size(); ++name)
    if (data.panel.in_universe(day + data.score_begin, name) && std::isfinite(row[name])) {
      scale = std::max(scale, std::abs(row[name])); ++count;
    }
  if (!(scale > 0) || count < 2) return;
  double mean = 0;
  for (std::size_t name = 0; name < output.size(); ++name)
    if (data.panel.in_universe(day + data.score_begin, name) && std::isfinite(row[name]))
      mean += row[name] / scale;
  mean /= static_cast<double>(count);
  double gross = 0;
  for (std::size_t name = 0; name < output.size(); ++name) {
    if (data.panel.in_universe(day + data.score_begin, name) && std::isfinite(row[name]))
      output[name] = row[name] / scale - mean;
    gross += std::abs(output[name]);
  }
  if (gross > 0) for (auto& weight : output) weight = weight / gross * target_gross;
}

risk::FactorModel identity_geometry(std::size_t names) {
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;
  const auto dimension = static_cast<Eigen::Index>(names);
  // X=0, F=1, D=1 gives identity geometry. The shape API needs a nonempty fit
  // interval, but this constant geometry estimates no market risk.
  auto model = risk::FactorModel::create(MatX::Zero(dimension, 1), MatX::Ones(1, 1),
      VecX::Ones(dimension), 0, 1);
  if (!model) throw std::runtime_error(model.error().message());
  return std::move(*model);
}

risk::MaterializedConstraints constraints(const PortfolioRecipe& recipe,
    const risk::FactorModel& geometry, const book::ReplayAllocationState& state,
    std::span<const double> held) {
  risk::ConstraintSet set;
  set.gross = risk::GrossNet{recipe.gross, true};
  set.pos = risk::PositionCap{recipe.name_cap};
  set.turn = risk::TurnoverBudget{recipe.turnover};
  auto result = set.materialize(geometry.exposures(), held, held.size());
  if (!result) throw std::runtime_error(result.error().message());
  // Native row order is dollar equality followed by identity position boxes.
  // Check the layout before tightening ineligible positions to exact zero.
  if (result->row_count() != held.size() + 1 || result->sparse())
    throw std::runtime_error("unexpected native portfolio constraint layout");
  for (std::size_t name = 0; name < held.size(); ++name) {
    const auto row = static_cast<Eigen::Index>(name + 1);
    for (std::size_t column = 0; column < held.size(); ++column)
      if (result->A(row, static_cast<Eigen::Index>(column)) != (name == column ? 1.0 : 0.0))
        throw std::runtime_error("native portfolio position box is not an identity row");
    if (!state.decision_eligibility[name]) result->l[row] = result->u[row] = 0;
  }
  return std::move(*result);
}

Json independently_check(std::span<const double> weights, std::span<const double> held,
    const PortfolioRecipe& recipe, const book::ReplayAllocationState& state) {
  double net = 0, gross = 0, maximum = 0, turnover = 0;
  if (weights.size() != held.size()) throw std::runtime_error("portfolio solver shape mismatch");
  for (std::size_t name = 0; name < weights.size(); ++name) {
    const auto value = weights[name];
    if (!std::isfinite(value) || (!state.decision_eligibility[name] && value != 0))
      throw std::runtime_error("portfolio solution violates finiteness or decision eligibility");
    net += value; gross += std::abs(value);
    maximum = std::max(maximum, std::abs(value));
    turnover += std::abs(value - held[name]);
  }
  if (std::abs(net) > tolerance || gross > recipe.gross + tolerance ||
      maximum > recipe.name_cap + tolerance || turnover > recipe.turnover + tolerance)
    throw std::runtime_error("portfolio solution failed independent hard-limit certification");
  return {{"target_net", net}, {"target_gross", gross}, {"target_max_name", maximum},
          {"target_turnover", turnover}};
}

std::vector<double> allocate(const PortfolioRecipe& recipe, const risk::FactorModel& geometry,
    const book::ReplayAllocationState& state, Json& diagnostics, std::size_t diagnostic_limit,
    std::stop_token stop) {
  checkpoint(stop, {}, 0, {});
  std::vector<double> held(state.marked_dollars.size()), q(held.size());
  for (std::size_t name = 0; name < held.size(); ++name) {
    held[name] = state.marked_dollars[name] / state.pretrade_nav;
    q[name] = -state.preference_weights[name];
  }
  double compulsory_exit = 0, eligible_net = 0;
  for (std::size_t name = 0; name < held.size(); ++name)
    if (state.decision_eligibility[name]) eligible_net += held[name];
    else compulsory_exit += std::abs(held[name]);
  if (compulsory_exit + std::abs(eligible_net) > recipe.turnover + tolerance)
    throw std::runtime_error("compulsory exit/dollar neutrality exceeds turnover budget");
  const auto surface = constraints(recipe, geometry, state, held);
  risk::ConstrainedQpSolver solver;
  solver.cfg.iters = 600;
  solver.cfg.rho = 1;
  solver.cfg.max_factor_bytes = 32ULL * 1024ULL * 1024ULL;
  const risk::QpProblem problem{geometry, 0.5, q, surface};
  // Adapt only at fixed iteration indices with power-of-two rho rounding.
  // Equality scaling and over-relaxation are the native deterministic schedule;
  // no wall-clock adaptation or early exit can change the bounded algorithm.
  auto solution = solver.solve_with_cert(problem, allocation_schedule);
  checkpoint(stop, {}, 0, {});
  if (!solution) throw std::runtime_error("portfolio constraints infeasible or uncertified: " +
                                        solution.error().message());
  if (solution->cert.primal_infeasible || solution->cert.dual_infeasible ||
      !std::isfinite(solution->cert.prim_res) || !std::isfinite(solution->cert.dual_res))
    throw std::runtime_error("portfolio solver did not produce a finite feasibility certificate");
  // Replay requires exact zeros. Remove only the numerical residue of explicit
  // zero boxes, then independently recertify neutrality and every hard budget.
  for (std::size_t name = 0; name < held.size(); ++name)
    if (!state.decision_eligibility[name]) {
      if (std::abs(solution->book[name]) > tolerance)
        throw std::runtime_error("portfolio solver violated a compulsory zero position");
      solution->book[name] = 0;
    }
  auto row = independently_check(solution->book, held, recipe, state);
  if (diagnostics.size() < diagnostic_limit) {
    row["decision_date"] = session_date(state.decision_session_key);
    row["execution_date"] = session_date(state.execution_session_key);
    row["target_weights"] = solution->book;
    row["pretrade_held_weights"] = held;
    row["solver_primal_residual"] = solution->cert.prim_res;
    row["solver_dual_residual"] = solution->cert.dual_res;
    row["solver_primal_infeasible"] = solution->cert.primal_infeasible;
    row["solver_polished"] = solution->cert.polished;
    row["solver_rho_final"] = solution->cert.rho_final;
    diagnostics.push_back(std::move(row));
  }
  return std::move(solution->book);
}

void enrich_allocations(Json& diagnostics, const book::ReplayPolicyResult& replay) {
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const auto& allocation = replay.allocations.at(i);
    auto& row = diagnostics[i];
    row["pretrade_nav"] = allocation.pretrade_nav;
    row["posttrade_nav"] = allocation.posttrade_nav;
    row["posttrade_cash"] = allocation.posttrade_cash;
    row["trade_cost"] = allocation.trade_cost;
    row["traded_dollars"] = allocation.traded_dollars;
    std::vector<double> achieved;
    double gross = 0, net = 0;
    for (const auto dollars : allocation.posttrade_marked_dollars) {
      const auto weight = dollars / allocation.posttrade_nav;
      achieved.push_back(weight); net += weight; gross += std::abs(weight);
    }
    row["achieved_posttrade_weights"] = std::move(achieved);
    row["achieved_posttrade_gross"] = gross;
    row["achieved_posttrade_net"] = net;
  }
}

PortfolioSizingSnapshot capture_sizing(const book::ReplayAllocationState& state,
                                      std::span<const double> weights) {
  PortfolioSizingSnapshot snapshot{{state.tri_units.begin(), state.tri_units.end()}, {}};
  snapshot.new_units.reserve(weights.size());
  for (std::size_t name = 0; name < weights.size(); ++name) {
    const auto mark = state.current_marks[name];
    // Match native TerminalReturn's unfilled-target branch before scalar sizing.
    if (state.tri_units[name] == 0 && weights[name] != 0 &&
        (!std::isfinite(mark) || mark <= 0)) {
      snapshot.new_units.push_back(0);
      continue;
    }
    const auto sized = book::resolve_replay_target(
        {book::ReplayTargetAction::TargetWeight, weights[name]}, state.tri_units[name],
        state.marked_dollars[name], mark, state.pretrade_nav, state.decision_eligibility[name] != 0);
    if (!sized) throw std::runtime_error(sized.error().message());
    snapshot.new_units.push_back(sized->tri_units);
  }
  return snapshot;
}

void provenance(Evidence& evidence, const DatasetPanel& data, const PortfolioRecipe& recipe,
                const book::ReplayPolicyResult& replay) {
  evidence.provenance = data.provenance;
  auto& out = evidence.provenance;
  out["components"] = Json::array();
  for (const auto& component : recipe.components)
    out["components"].push_back({{"strategy_dsl", component.expression},
        {"effective_strategy_dsl", component.effective_expression},
        {"expression_sha256", component.sha}, {"normalized_weight", component.weight}});
  out["portfolio_geometry"] = "identity_signal_projection_not_estimated_risk";
  out["blend_rule"] = "StandardizedFixedGrossV2";
  out["instrument_order"] = Json::array();
  for (const auto instrument : data.instruments)
    out["instrument_order"].push_back("SR:" + std::to_string(instrument));
  out["component_binding"] = "exact frozen expression hashes; no trusted alpha revision claims";
  out["engine"] = "atx::engine::book::replay_scheduled_targets";
  out["alpha_engine"] = "atx::engine::alpha::evaluate_reference";
  out["combiner"] = "atx::engine::combine::CombinedSignalSource";
  out["allocator"] = "atx::engine::risk::ConstrainedQpSolver";
  out["signal_decay_sessions"] = recipe.decay;
  out["execution_delay_sessions"] = recipe.data.delay;
  out["cadence"] = recipe.data.cadence;
  out["trade_bps"] = recipe.data.trade_bps;
  out["annual_borrow_bps"] = recipe.data.borrow_bps;
  out["gross_limit"] = recipe.gross; out["name_cap"] = recipe.name_cap;
  out["max_turnover"] = recipe.turnover;
  out["constraint_basis"] = "target fractions of marked pretrade NAV at delayed execution";
  out["turnover_basis"] = "absolute discretionary target changes versus current marked holdings; "
      "native mandatory missing-price liquidations precede allocation and are disclosed separately";
  out["constraint_tolerance"] = tolerance;
  out["solver_fixed_iterations"] = 600;
  out["solver_rho_initial"] = 1;
  out["solver_schedule"] = {{"refactor_at", allocation_schedule.refactor_at},
      {"rho_power_of_two", allocation_schedule.pow2_round},
      {"rho_min", allocation_schedule.rho_min}, {"rho_max", allocation_schedule.rho_max},
      {"adapt_ratio", allocation_schedule.adapt_ratio},
      {"equality_rho_scale", allocation_schedule.eq_rho_scale},
      {"over_relaxation", allocation_schedule.relax_alpha},
      {"early_exit", allocation_schedule.early_exit},
      {"check_every", allocation_schedule.check_every},
      {"eps_abs", allocation_schedule.eps_abs}, {"eps_rel", allocation_schedule.eps_rel}};
  out["geometry_objective"] = "minimize half squared weight distance from normalized blended signal";
  out["total_allocation_count"] = replay.allocations.size();
  out["portfolio_allocations_truncated"] =
      replay.allocations.size() > evidence.portfolio_allocations.size();
  out["total_trade_count"] = replay.replay.trades.size();
  out["trades_truncated"] = replay.replay.trades.size() > evidence.trades.size();
  out["effective_rebalances"] = replay.replay.effective_rebalances;
  out["assumed_liquidations"] = replay.replay.assumed_liquidations;
  out["assumed_liquidation_pnl"] = replay.replay.assumed_liquidation_pnl;
  out["nav_column"] = "engine interval.nav";
  out["warnings"] = Json::array({
      "Identity signal projection is not calibrated expected returns or an estimated risk model",
      "Limits govern delayed targets of pretrade NAV; fees and price drift change exposures",
      "Infeasible compulsory exits fail without relaxing turnover, neutrality or name limits",
      "Published TRAIN only; historical vintage, common-stock identity and borrow availability unverified",
      "No impact, capacity, sector neutrality, data-delivery or production execution qualification",
      "Missing or guarded held marks use explicit native adverse liquidation assumptions"});
}
} // namespace

PortfolioReplayOutput replay_portfolio_owned(const DatasetPanel& data, const PortfolioRecipe& recipe,
    const alpha::SignalSet& signal, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress, bool retain_sizing) {
  const auto panel = make_dataset_score_panel(data);
  const auto dates = panel.dates(), names = panel.instruments();
  const std::span<const std::int64_t> sessions(data.sessions.data() + data.score_begin, dates);
  std::vector<std::size_t> decisions;
  for (std::size_t day = 0; day + recipe.data.delay + 1 < dates; day += recipe.data.cadence)
    decisions.push_back(day);
  std::vector<double> weights(decisions.size() * names, 0);
  for (std::size_t i = 0; i < decisions.size(); ++i) {
    checkpoint(stop, {}, 0, {});
    preferences(data, signal, decisions[i], recipe.gross,
        std::span<double>{weights.data() + i * names, names});
  }
  const auto geometry = identity_geometry(names);
  Json diagnostics = Json::array();
  const auto diagnostic_limit = retain_sizing ? decisions.size() :
      std::min(options.max_result_points, std::size_t{65536} / names);
  std::vector<PortfolioSizingSnapshot> sizing;
  if (retain_sizing) sizing.reserve(decisions.size());
  const book::ReplayAllocationPolicy policy = [&](const book::ReplayAllocationState& state)
      -> atx::core::Result<std::vector<double>> {
    checkpoint(stop, progress, 0.6 + .2 * static_cast<double>(state.schedule_index + 1) /
        static_cast<double>(std::max(decisions.size(), std::size_t{1})),
        "Certifying delayed allocations against current marked holdings");
    auto target = allocate(recipe, geometry, state, diagnostics, diagnostic_limit, stop);
    if (retain_sizing) sizing.push_back(capture_sizing(state, target));
    return target;
  };
  book::ReplayConfig config;
  config.initial_nav = recipe.data.capital;
  config.execution_delay_periods = recipe.data.delay;
  config.trade_bps = recipe.data.trade_bps;
  config.annual_borrow_bps = recipe.data.borrow_bps;
  auto replay = book::replay_scheduled_targets(panel, sessions, decisions, weights, policy, config);
  if (!replay) throw std::runtime_error(replay.error().message());
  checkpoint(stop, progress, .85, "Collecting complete native portfolio accounting");
  enrich_allocations(diagnostics, *replay);
  Evidence evidence;
  // Only metadata is constructed here; ledger ownership does not depend on sampled research rows.
  provenance(evidence, data, recipe, *replay);
  evidence.provenance["portfolio_allocations_truncated"] =
      diagnostics.size() < replay->allocations.size();
  evidence.provenance["trades_truncated"] = false;
  return {std::move(*replay), std::move(diagnostics), std::move(sizing),
          std::move(evidence.provenance)};
}

Evidence replay_portfolio(const DatasetPanel& data, const PortfolioRecipe& recipe,
    const alpha::SignalSet& signal, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress) {
  auto owned = replay_portfolio_owned(data, recipe, signal, options, stop, progress, false);
  const auto panel = make_dataset_score_panel(data);
  const std::span<const std::int64_t> sessions(data.sessions.data() + data.score_begin, panel.dates());
  Evidence evidence;
  append_dataset_accounting(evidence, owned.replay.replay, panel, sessions, data, options, stop);
  evidence.portfolio_allocations = std::move(owned.diagnostics);
  evidence.provenance = std::move(owned.provenance);
  evidence.provenance["trades_truncated"] = owned.replay.replay.trades.size() > evidence.trades.size();
  return evidence;
}
} // namespace atx::server::research_detail

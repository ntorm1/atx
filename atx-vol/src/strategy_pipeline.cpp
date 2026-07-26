#include "atx/vol/strategy_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atx/vol/scenario_grid.hpp"
#include "atx/vol/universe.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

[[nodiscard]] bool finite(double value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool valid_side(Side side) noexcept {
  switch (side) {
  case Side::Call:
  case Side::Put:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_contract(const OptionContract &contract) noexcept {
  return contract.uid != 0u && finite(contract.K) && contract.K > 0.0 && finite(contract.T) &&
         contract.T > 0.0 && valid_side(contract.side);
}

[[nodiscard]] bool finite_greeks(const AmericanGreeks &greeks) noexcept {
  return finite(greeks.delta) && finite(greeks.gamma) && finite(greeks.vega) &&
         finite(greeks.theta) && finite(greeks.rho) && finite(greeks.vanna) &&
         finite(greeks.volga) && finite(greeks.charm) && finite(greeks.price);
}

[[nodiscard]] bool valid_named_position(const NamedPosition &value) noexcept {
  return !value.symbol.empty() && value.position.id != 0u &&
         valid_contract(value.position.contract) && finite(value.position.qty) &&
         finite(value.position.multiplier) && value.position.multiplier > 0.0;
}

struct OpportunityCandidate {
  const OptionOpportunity *opportunity{nullptr};
  std::string canonical;
};

[[nodiscard]] auto opportunity_tie(const OpportunityCandidate &candidate) noexcept {
  const OptionOpportunity &value = *candidate.opportunity;
  return std::tuple{std::string_view(candidate.canonical),
                    value.contract.uid,
                    value.contract.K,
                    value.contract.T,
                    static_cast<std::uint8_t>(value.contract.side),
                    value.position_id};
}

[[nodiscard]] std::vector<double> capped_equal_allocation(std::span<const double> capacities,
                                                          double target) {
  std::vector<double> allocation(capacities.size(), 0.0);
  std::vector<bool> active(capacities.size(), true);
  std::size_t active_count = capacities.size();
  double remaining = target;

  // At least one member is retired on every capped pass; otherwise the final
  // equal allocation terminates the loop. The bound is capacities.size()+1.
  for (std::size_t pass = 0; pass <= capacities.size() && active_count != 0u; ++pass) {
    const double fair_share = remaining / static_cast<double>(active_count);
    bool retired = false;
    for (std::size_t i = 0; i < capacities.size(); ++i) {
      if (!active[i] || capacities[i] > fair_share) {
        continue;
      }
      allocation[i] = capacities[i];
      remaining -= capacities[i];
      active[i] = false;
      --active_count;
      retired = true;
    }
    if (!retired) {
      for (std::size_t i = 0; i < capacities.size(); ++i) {
        if (active[i]) {
          allocation[i] = fair_share;
        }
      }
      remaining = 0.0;
      break;
    }
    if (remaining < 0.0 && remaining > -std::numeric_limits<double>::epsilon() * target) {
      remaining = 0.0;
    }
  }
  return allocation;
}

[[nodiscard]] Status validate_limit(const std::optional<double> &limit, std::string_view name) {
  if (limit.has_value() && (!finite(*limit) || *limit < 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: " + std::string(name) + " must be finite and nonnegative");
  }
  return Ok();
}

void observe_limit(double exposure, const std::optional<double> &limit, ScalarRiskLimitKind kind,
                   double &scale, std::vector<ScalarRiskLimitKind> &breaches) {
  if (!limit.has_value() || exposure <= *limit) {
    return;
  }
  breaches.push_back(kind);
  const double candidate = exposure > 0.0 ? *limit / exposure : 1.0;
  scale = std::min(scale, candidate);
}

[[nodiscard]] RiskVector scaled_risk(const RiskVector &risk, double scale) noexcept {
  return RiskVector{risk.delta * scale, risk.gamma * scale, risk.vega * scale, risk.theta * scale,
                    risk.gross_notional * scale};
}

struct OrderedScenarioPosition {
  const ScenarioRiskInput *input{nullptr};
};

[[nodiscard]] bool scenario_position_less(const OrderedScenarioPosition &lhs,
                                          const OrderedScenarioPosition &rhs) noexcept {
  if (lhs.input->position.contract.uid != rhs.input->position.contract.uid) {
    return lhs.input->position.contract.uid < rhs.input->position.contract.uid;
  }
  return lhs.input->position.id < rhs.input->position.id;
}

struct OrderedNamedPosition {
  const NamedPosition *input{nullptr};
};

[[nodiscard]] Result<std::vector<OrderedNamedPosition>>
order_named_positions(std::span<const NamedPosition> positions, std::string_view label) {
  std::vector<OrderedNamedPosition> result;
  result.reserve(positions.size());
  for (const NamedPosition &position : positions) {
    if (!valid_named_position(position)) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: invalid " + std::string(label) + " position");
    }
    result.push_back(OrderedNamedPosition{&position});
  }
  std::sort(result.begin(), result.end(),
            [](const OrderedNamedPosition &lhs, const OrderedNamedPosition &rhs) {
              return lhs.input->position.id < rhs.input->position.id;
            });
  for (std::size_t i = 1; i < result.size(); ++i) {
    if (result[i - 1u].input->position.id == result[i].input->position.id) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: duplicate " + std::string(label) + " position id");
    }
  }
  return Ok(std::move(result));
}

} // namespace

Status validate_strategy_universe(const StrategyUniverseProvenance &universe,
                                  std::int64_t decision_ts_ns) {
  if (decision_ts_ns <= 0 || universe.effective_ts_ns <= 0 || universe.knowledge_ts_ns <= 0 ||
      universe.effective_ts_ns > decision_ts_ns || universe.knowledge_ts_ns > decision_ts_ns) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: universe is not point-in-time available at the decision");
  }
  if (universe.index_symbol.empty() || universe.index_uid == 0u || universe.source.empty() ||
      universe.source_fingerprint == 0u || universe.constituents.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: universe identity/provenance is incomplete");
  }

  std::unordered_set<std::string> constituent_symbols;
  std::unordered_set<std::uint32_t> constituent_uids;
  constituent_symbols.reserve(universe.constituents.size() + 1u);
  constituent_uids.reserve(universe.constituents.size() + 1u);
  constituent_symbols.insert(canonical_symbol(universe.index_symbol));
  constituent_uids.insert(universe.index_uid);
  double total_weight = 0.0;
  for (const StrategyConstituent &member : universe.constituents) {
    const std::string canonical = canonical_symbol(member.symbol);
    if (member.symbol.empty() || member.uid == 0u || !finite(member.weight) ||
        member.weight <= 0.0 || !constituent_symbols.insert(canonical).second ||
        !constituent_uids.insert(member.uid).second) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: invalid or duplicate universe constituent");
    }
    total_weight += member.weight;
    if (!finite(total_weight)) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: constituent weights overflow");
    }
  }
  if (!(total_weight > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: constituent weights must have positive mass");
  }

  switch (universe.mode) {
  case StrategyUniverseMode::StrictConstituents:
    if (!universe.proxies.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: strict universe cannot carry proxy mappings");
    }
    return Ok();
  case StrategyUniverseMode::DirtyProxyBasket:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "strategy_pipeline: invalid strategy universe mode");
  }
  if (universe.proxies.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: dirty universe requires an explicit proxy mapping");
  }

  std::unordered_set<std::string> mapped_constituents;
  std::unordered_set<std::string> proxy_symbols;
  std::unordered_set<std::uint32_t> proxy_uids;
  mapped_constituents.reserve(universe.proxies.size());
  proxy_symbols.reserve(universe.proxies.size());
  proxy_uids.reserve(universe.proxies.size());
  for (const StrategyProxyMapping &proxy : universe.proxies) {
    const std::string constituent = canonical_symbol(proxy.constituent_symbol);
    const std::string trade_symbol = canonical_symbol(proxy.proxy_symbol);
    if (proxy.constituent_symbol.empty() || proxy.proxy_symbol.empty() || proxy.proxy_uid == 0u ||
        !finite(proxy.beta) || proxy.beta <= 0.0 || !constituent_symbols.contains(constituent) ||
        constituent == canonical_symbol(universe.index_symbol) ||
        !mapped_constituents.insert(constituent).second ||
        !proxy_symbols.insert(trade_symbol).second || !proxy_uids.insert(proxy.proxy_uid).second ||
        constituent_symbols.contains(trade_symbol) || constituent_uids.contains(proxy.proxy_uid)) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: invalid, duplicate, or ambiguous dirty proxy mapping");
    }
  }
  return Ok();
}

Result<LongShortVolatilityPortfolio>
construct_systematic_long_short_volatility(std::span<const OptionOpportunity> opportunities,
                                           const LongShortVolatilityConfig &config) {
  if (config.n_long == 0u || config.n_short == 0u || !finite(config.target_gross_vega_per_sleeve) ||
      config.target_gross_vega_per_sleeve <= 0.0 || !finite(config.max_abs_contracts_per_name) ||
      config.max_abs_contracts_per_name <= 0.0 || !finite(config.min_abs_edge) ||
      config.min_abs_edge < 0.0) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: invalid long/short construction config");
  }

  std::unordered_set<std::string> symbols;
  std::unordered_set<std::uint64_t> ids;
  symbols.reserve(opportunities.size());
  ids.reserve(opportunities.size());
  std::vector<OpportunityCandidate> longs;
  std::vector<OpportunityCandidate> shorts;
  longs.reserve(opportunities.size());
  shorts.reserve(opportunities.size());
  for (const OptionOpportunity &opportunity : opportunities) {
    const std::string canonical = canonical_symbol(opportunity.symbol);
    if (opportunity.symbol.empty() || opportunity.position_id == 0u ||
        !valid_contract(opportunity.contract) || !finite(opportunity.multiplier) ||
        opportunity.multiplier <= 0.0 || !finite(opportunity.valuation_edge) ||
        !finite(opportunity.vega_per_contract) || opportunity.vega_per_contract <= 0.0 ||
        !finite(opportunity.max_abs_contracts) || opportunity.max_abs_contracts <= 0.0 ||
        !symbols.insert(canonical).second || !ids.insert(opportunity.position_id).second) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: invalid or duplicate option opportunity");
    }
    if (opportunity.valuation_edge > 0.0 && opportunity.valuation_edge >= config.min_abs_edge) {
      longs.push_back(OpportunityCandidate{&opportunity, canonical});
    } else if (opportunity.valuation_edge < 0.0 &&
               -opportunity.valuation_edge >= config.min_abs_edge) {
      shorts.push_back(OpportunityCandidate{&opportunity, canonical});
    }
  }

  std::sort(longs.begin(), longs.end(),
            [](const OpportunityCandidate &lhs, const OpportunityCandidate &rhs) {
              if (lhs.opportunity->valuation_edge != rhs.opportunity->valuation_edge) {
                return lhs.opportunity->valuation_edge > rhs.opportunity->valuation_edge;
              }
              return opportunity_tie(lhs) < opportunity_tie(rhs);
            });
  std::sort(shorts.begin(), shorts.end(),
            [](const OpportunityCandidate &lhs, const OpportunityCandidate &rhs) {
              if (lhs.opportunity->valuation_edge != rhs.opportunity->valuation_edge) {
                return lhs.opportunity->valuation_edge < rhs.opportunity->valuation_edge;
              }
              return opportunity_tie(lhs) < opportunity_tie(rhs);
            });
  if (longs.size() < config.n_long || shorts.size() < config.n_short) {
    return Err(ErrorCode::Unavailable,
               "strategy_pipeline: insufficient qualified long/short opportunities");
  }
  longs.resize(config.n_long);
  shorts.resize(config.n_short);

  const auto capacities =
      [&config](std::span<const OpportunityCandidate> sleeve) -> Result<std::vector<double>> {
    std::vector<double> result;
    result.reserve(sleeve.size());
    for (const OpportunityCandidate &candidate : sleeve) {
      const OptionOpportunity &value = *candidate.opportunity;
      const double contracts = std::min(value.max_abs_contracts, config.max_abs_contracts_per_name);
      const double capacity = contracts * value.vega_per_contract;
      if (!finite(capacity) || capacity <= 0.0) {
        return Err(ErrorCode::InvalidArgument,
                   "strategy_pipeline: non-finite opportunity vega capacity");
      }
      result.push_back(capacity);
    }
    return Ok(std::move(result));
  };
  ATX_TRY(std::vector<double> long_capacity, capacities(longs));
  ATX_TRY(std::vector<double> short_capacity, capacities(shorts));

  const auto sum_capacity = [](std::span<const double> values) -> Result<double> {
    double total = 0.0;
    for (const double value : values) {
      total += value;
      if (!finite(total)) {
        return Err(ErrorCode::InvalidArgument,
                   "strategy_pipeline: aggregate vega capacity overflow");
      }
    }
    return Ok(total);
  };
  ATX_TRY(double long_total_capacity, sum_capacity(long_capacity));
  ATX_TRY(double short_total_capacity, sum_capacity(short_capacity));
  const double target = std::min(config.target_gross_vega_per_sleeve,
                                 std::min(long_total_capacity, short_total_capacity));
  if (!(target > 0.0) || !finite(target)) {
    return Err(ErrorCode::Unavailable, "strategy_pipeline: no balanced vega capacity is available");
  }

  const std::vector<double> long_allocation = capped_equal_allocation(long_capacity, target);
  const std::vector<double> short_allocation = capped_equal_allocation(short_capacity, target);

  LongShortVolatilityPortfolio output;
  output.positions.reserve(longs.size() + shorts.size());
  const auto emit = [&output](std::span<const OpportunityCandidate> sleeve,
                              std::span<const double> allocation, double sign,
                              double &gross_vega) -> Status {
    for (std::size_t i = 0; i < sleeve.size(); ++i) {
      const OptionOpportunity &value = *sleeve[i].opportunity;
      const double quantity = sign * allocation[i] / value.vega_per_contract;
      if (!finite(quantity)) {
        return Err(ErrorCode::InvalidArgument,
                   "strategy_pipeline: constructed quantity is non-finite");
      }
      output.positions.push_back(NamedPosition{
          value.symbol, Position{value.position_id, value.contract, quantity, value.multiplier}});
      gross_vega += allocation[i];
    }
    return Ok();
  };
  ATX_TRY_VOID(emit(longs, long_allocation, 1.0, output.long_gross_vega));
  ATX_TRY_VOID(emit(shorts, short_allocation, -1.0, output.short_gross_vega));
  output.net_vega = output.long_gross_vega - output.short_gross_vega;
  output.capacity_limited = target < config.target_gross_vega_per_sleeve;
  return Ok(std::move(output));
}

Result<RiskVector> aggregate_strategy_risk(std::span<const PositionRiskInput> positions) {
  std::vector<const PositionRiskInput *> ordered;
  ordered.reserve(positions.size());
  for (const PositionRiskInput &input : positions) {
    if (input.position.id == 0u || !valid_contract(input.position.contract) ||
        !finite(input.position.qty) || !finite(input.position.multiplier) ||
        input.position.multiplier <= 0.0 || !finite_greeks(input.greeks_per_share) ||
        input.greeks_per_share.price < 0.0) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: invalid position risk input");
    }
    ordered.push_back(&input);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const PositionRiskInput *lhs, const PositionRiskInput *rhs) {
              return lhs->position.id < rhs->position.id;
            });
  for (std::size_t i = 1; i < ordered.size(); ++i) {
    if (ordered[i - 1u]->position.id == ordered[i]->position.id) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: duplicate position risk id");
    }
  }

  RiskVector output;
  for (const PositionRiskInput *input : ordered) {
    const double weight = input->position.qty * input->position.multiplier;
    const AmericanGreeks &risk = input->greeks_per_share;
    output.delta += weight * risk.delta;
    output.gamma += weight * risk.gamma;
    output.vega += weight * risk.vega;
    output.theta += weight * risk.theta;
    output.gross_notional += std::fabs(weight * risk.price);
    if (!finite(output.delta) || !finite(output.gamma) || !finite(output.vega) ||
        !finite(output.theta) || !finite(output.gross_notional)) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: aggregate risk overflow");
    }
  }
  return Ok(output);
}

Result<ScalarRiskOverlayResult> apply_scalar_risk_overlay(const RiskVector &risk,
                                                          std::span<const double> scenario_pnl,
                                                          const ScalarRiskLimits &limits) {
  if (!finite(risk.delta) || !finite(risk.gamma) || !finite(risk.vega) || !finite(risk.theta) ||
      !finite(risk.gross_notional) || risk.gross_notional < 0.0) {
    return Err(ErrorCode::InvalidArgument, "strategy_pipeline: invalid aggregate risk vector");
  }
  ATX_TRY_VOID(validate_limit(limits.max_abs_delta, "max_abs_delta"));
  ATX_TRY_VOID(validate_limit(limits.max_abs_gamma, "max_abs_gamma"));
  ATX_TRY_VOID(validate_limit(limits.max_abs_vega, "max_abs_vega"));
  ATX_TRY_VOID(validate_limit(limits.max_abs_theta, "max_abs_theta"));
  ATX_TRY_VOID(validate_limit(limits.max_gross_notional, "max_gross_notional"));
  ATX_TRY_VOID(validate_limit(limits.max_worst_scenario_loss, "max_worst_scenario_loss"));
  switch (limits.action) {
  case ScalarRiskBreachAction::Clamp:
  case ScalarRiskBreachAction::Reject:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "strategy_pipeline: invalid scalar risk breach action");
  }

  double worst_loss = 0.0;
  for (const double pnl : scenario_pnl) {
    if (!finite(pnl)) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: scenario PnL must be finite");
    }
    if (pnl < 0.0) {
      worst_loss = std::max(worst_loss, -pnl);
    }
  }

  ScalarRiskOverlayResult output;
  observe_limit(std::fabs(risk.delta), limits.max_abs_delta, ScalarRiskLimitKind::Delta,
                output.scale, output.binding_limits);
  observe_limit(std::fabs(risk.gamma), limits.max_abs_gamma, ScalarRiskLimitKind::Gamma,
                output.scale, output.binding_limits);
  observe_limit(std::fabs(risk.vega), limits.max_abs_vega, ScalarRiskLimitKind::Vega, output.scale,
                output.binding_limits);
  observe_limit(std::fabs(risk.theta), limits.max_abs_theta, ScalarRiskLimitKind::Theta,
                output.scale, output.binding_limits);
  observe_limit(risk.gross_notional, limits.max_gross_notional, ScalarRiskLimitKind::GrossNotional,
                output.scale, output.binding_limits);
  observe_limit(worst_loss, limits.max_worst_scenario_loss, ScalarRiskLimitKind::WorstScenarioLoss,
                output.scale, output.binding_limits);
  if (output.scale < 1.0 && limits.action == ScalarRiskBreachAction::Reject) {
    return Err(ErrorCode::Unavailable,
               "strategy_pipeline: scalar risk overlay rejected the target");
  }

  output.risk = scaled_risk(risk, output.scale);
  output.scenario_pnl.reserve(scenario_pnl.size());
  for (const double pnl : scenario_pnl) {
    output.scenario_pnl.push_back(pnl * output.scale);
  }
  return Ok(std::move(output));
}

Result<ConditionalComponentScenarioResult>
conditional_component_scenario_pnl(std::span<const ScenarioRiskInput> positions,
                                   const ConditionalComponentScenario &scenario) {
  if (scenario.index_uid == 0u || !finite(scenario.index_spot_pct) ||
      scenario.index_spot_pct <= -1.0 || !finite(scenario.index_vol_bump) || !finite(scenario.dt) ||
      scenario.dt < 0.0 || !finite(scenario.dr)) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: invalid conditional scenario header");
  }

  std::vector<ComponentShockModel> shocks = scenario.components;
  std::sort(shocks.begin(), shocks.end(),
            [](const ComponentShockModel &lhs, const ComponentShockModel &rhs) {
              return lhs.uid < rhs.uid;
            });
  for (std::size_t i = 0; i < shocks.size(); ++i) {
    const ComponentShockModel &shock = shocks[i];
    const double spot_move = shock.spot_beta * scenario.index_spot_pct + shock.residual_spot_pct;
    if (shock.uid == 0u || shock.uid == scenario.index_uid || !finite(shock.spot_beta) ||
        !finite(shock.residual_spot_pct) || !finite(shock.vol_beta) ||
        !finite(shock.residual_vol_bump) || !finite(spot_move) || spot_move <= -1.0 ||
        (i != 0u && shocks[i - 1u].uid == shock.uid)) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: invalid or duplicate component shock");
    }
  }

  std::vector<OrderedScenarioPosition> ordered;
  ordered.reserve(positions.size());
  std::unordered_set<std::uint64_t> position_ids;
  position_ids.reserve(positions.size());
  for (const ScenarioRiskInput &input : positions) {
    if (input.position.id == 0u || !valid_contract(input.position.contract) ||
        !finite(input.position.qty) || !finite(input.position.multiplier) ||
        input.position.multiplier <= 0.0 || !finite(input.spot) || input.spot <= 0.0 ||
        !finite_greeks(input.greeks_per_share) || !position_ids.insert(input.position.id).second) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: invalid or duplicate scenario risk input");
    }
    ordered.push_back(OrderedScenarioPosition{&input});
  }
  std::sort(ordered.begin(), ordered.end(), scenario_position_less);
  std::vector<std::uint32_t> required_components;
  for (const OrderedScenarioPosition &entry : ordered) {
    const std::uint32_t uid = entry.input->position.contract.uid;
    if (uid != scenario.index_uid &&
        (required_components.empty() || required_components.back() != uid)) {
      required_components.push_back(uid);
    }
  }
  if (required_components.size() != shocks.size()) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: component shocks do not exactly cover non-index risk");
  }
  for (std::size_t i = 0; i < required_components.size(); ++i) {
    if (required_components[i] != shocks[i].uid) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: component shock uid mismatch");
    }
  }

  ConditionalComponentScenarioResult output;
  for (const OrderedScenarioPosition &entry : ordered) {
    const ScenarioRiskInput &input = *entry.input;
    const std::uint32_t uid = input.position.contract.uid;
    double spot_pct = scenario.index_spot_pct;
    double vol_bump = scenario.index_vol_bump;
    if (uid != scenario.index_uid) {
      const auto found = std::lower_bound(
          shocks.begin(), shocks.end(), uid,
          [](const ComponentShockModel &shock, std::uint32_t value) { return shock.uid < value; });
      if (found == shocks.end() || found->uid != uid) {
        return Err(ErrorCode::Internal,
                   "strategy_pipeline: validated component shock was not found");
      }
      spot_pct = found->spot_beta * scenario.index_spot_pct + found->residual_spot_pct;
      vol_bump = found->vol_beta * scenario.index_vol_bump + found->residual_vol_bump;
    }
    const double d_spot = input.spot * spot_pct;
    const double weight = input.position.qty * input.position.multiplier;
    const double pnl =
        scenario_taylor_leg(input.greeks_per_share, d_spot, vol_bump, scenario.dt, scenario.dr) *
        weight;
    if (!finite(pnl)) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: component scenario PnL overflow");
    }
    if (output.by_uid.empty() || output.by_uid.back().uid != uid) {
      output.by_uid.push_back(ComponentScenarioContribution{uid, 0.0});
    }
    output.by_uid.back().pnl += pnl;
    if (!finite(output.by_uid.back().pnl)) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: component scenario bucket overflow");
    }
  }
  for (const ComponentScenarioContribution &contribution : output.by_uid) {
    output.total_pnl += contribution.pnl;
    if (!finite(output.total_pnl)) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: component scenario total overflow");
    }
  }
  return Ok(std::move(output));
}

Status validate_algo_parameters(const AlgoParameters &parameters) {
  switch (parameters.style) {
  case AlgoStyle::Passive:
  case AlgoStyle::Adaptive:
  case AlgoStyle::Twap:
  case AlgoStyle::Vwap:
  case AlgoStyle::Immediate:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "strategy_pipeline: invalid algo style");
  }
  if (!finite(parameters.max_participation.fraction) ||
      parameters.max_participation.fraction <= 0.0 || parameters.max_participation.fraction > 1.0 ||
      parameters.horizon.value == 0u || !finite(parameters.limit_offset.value)) {
    return Err(ErrorCode::InvalidArgument, "strategy_pipeline: invalid typed algo parameters");
  }
  return Ok();
}

Result<BasketOrderIntent> make_basket_order_intent(std::uint64_t strategy_fingerprint,
                                                   std::int64_t decision_ts_ns,
                                                   std::span<const NamedPosition> target,
                                                   std::span<const NamedPosition> current,
                                                   std::span<const HedgeTarget> hedge_targets,
                                                   const AlgoParameters &algo,
                                                   IntentDisposition disposition) {
  if (strategy_fingerprint == 0u || decision_ts_ns <= 0) {
    return Err(ErrorCode::InvalidArgument,
               "strategy_pipeline: intent identity/decision clock is invalid");
  }
  ATX_TRY_VOID(validate_algo_parameters(algo));
  switch (disposition) {
  case IntentDisposition::ResearchOnly:
  case IntentDisposition::DryRun:
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "strategy_pipeline: invalid intent disposition");
  }

  ATX_TRY(std::vector<OrderedNamedPosition> targets, order_named_positions(target, "target"));
  ATX_TRY(std::vector<OrderedNamedPosition> currents, order_named_positions(current, "current"));

  BasketOrderIntent output;
  output.strategy_fingerprint = strategy_fingerprint;
  output.decision_ts_ns = decision_ts_ns;
  output.disposition = disposition;
  output.algo = algo;
  output.option_orders.reserve(targets.size() + currents.size());
  std::size_t target_index = 0u;
  std::size_t current_index = 0u;
  while (target_index < targets.size() || current_index < currents.size()) {
    const NamedPosition *target_position =
        target_index < targets.size() ? targets[target_index].input : nullptr;
    const NamedPosition *current_position =
        current_index < currents.size() ? currents[current_index].input : nullptr;
    const bool take_target = current_position == nullptr ||
                             (target_position != nullptr &&
                              target_position->position.id < current_position->position.id);
    const bool take_current = target_position == nullptr ||
                              (current_position != nullptr &&
                               current_position->position.id < target_position->position.id);

    const NamedPosition *identity = nullptr;
    double target_quantity = 0.0;
    double current_quantity = 0.0;
    if (take_target) {
      identity = target_position;
      target_quantity = target_position->position.qty;
      ++target_index;
    } else if (take_current) {
      identity = current_position;
      current_quantity = current_position->position.qty;
      ++current_index;
    } else {
      if (canonical_symbol(target_position->symbol) != canonical_symbol(current_position->symbol) ||
          !(target_position->position.contract == current_position->position.contract) ||
          target_position->position.multiplier != current_position->position.multiplier) {
        return Err(ErrorCode::InvalidArgument,
                   "strategy_pipeline: stable position id changed economic identity");
      }
      identity = target_position;
      target_quantity = target_position->position.qty;
      current_quantity = current_position->position.qty;
      ++target_index;
      ++current_index;
    }

    const double quantity_delta = target_quantity - current_quantity;
    if (!finite(quantity_delta)) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: option quantity delta overflow");
    }
    if (quantity_delta != 0.0) {
      output.option_orders.push_back(OptionOrderDelta{
          identity->position.id, identity->symbol, identity->position.contract,
          identity->position.multiplier, current_quantity, target_quantity, quantity_delta});
    }
  }

  std::vector<HedgeTarget> ordered_hedges(hedge_targets.begin(), hedge_targets.end());
  std::sort(ordered_hedges.begin(), ordered_hedges.end(),
            [](const HedgeTarget &lhs, const HedgeTarget &rhs) { return lhs.uid < rhs.uid; });
  output.hedges.reserve(ordered_hedges.size());
  for (std::size_t i = 0; i < ordered_hedges.size(); ++i) {
    const HedgeTarget &target_value = ordered_hedges[i];
    if (target_value.uid == 0u || !finite(target_value.current_shares) ||
        !finite(target_value.target_shares) ||
        (i != 0u && ordered_hedges[i - 1u].uid == target_value.uid)) {
      return Err(ErrorCode::InvalidArgument,
                 "strategy_pipeline: invalid or duplicate hedge target");
    }
    const double shares_to_trade = target_value.target_shares - target_value.current_shares;
    if (!finite(shares_to_trade)) {
      return Err(ErrorCode::InvalidArgument, "strategy_pipeline: hedge share delta overflow");
    }
    if (shares_to_trade != 0.0) {
      output.hedges.push_back(HedgeInstruction{kHedgeInstructionSchemaVersion, target_value.uid,
                                               target_value.current_shares,
                                               target_value.target_shares, shares_to_trade});
    }
  }
  return Ok(std::move(output));
}

} // namespace atx::vol

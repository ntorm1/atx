#pragma once

// Broker-neutral strategy-to-intent building blocks for quantitative volatility
// research. This module deliberately stops at immutable research/dry-run intent:
// it has no live transport, venue, broker, or SpiderRock dependency.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

// ---------------------------------------------------------------------------
// Point-in-time strategy universe provenance
// ---------------------------------------------------------------------------

enum class StrategyUniverseMode : std::uint8_t {
  StrictConstituents = 0,
  DirtyProxyBasket = 1,
};

struct StrategyConstituent {
  std::string symbol;
  std::uint32_t uid{0};
  double weight{0.0};

  [[nodiscard]] bool operator==(const StrategyConstituent &) const = default;
};

// One explicit dirty-basket substitution. The reference constituent remains in
// the benchmark composition; proxy_symbol/proxy_uid is the instrument traded in
// its place. beta is the documented exposure conversion and must be positive.
struct StrategyProxyMapping {
  std::string constituent_symbol;
  std::string proxy_symbol;
  std::uint32_t proxy_uid{0};
  double beta{1.0};

  [[nodiscard]] bool operator==(const StrategyProxyMapping &) const = default;
};

struct StrategyUniverseProvenance {
  StrategyUniverseMode mode{StrategyUniverseMode::StrictConstituents};
  std::string index_symbol;
  std::uint32_t index_uid{0};
  std::vector<StrategyConstituent> constituents;
  std::vector<StrategyProxyMapping> proxies;
  std::int64_t effective_ts_ns{0};
  std::int64_t knowledge_ts_ns{0};
  std::string source;
  std::uint64_t source_fingerprint{0};
};

// Validate authorship, uniqueness, strict/dirty invariants and PIT availability.
// Both effective_ts_ns and knowledge_ts_ns must be observable at decision_ts_ns;
// a future-effective or future-known composition is rejected as look-ahead.
[[nodiscard]] Status validate_strategy_universe(const StrategyUniverseProvenance &universe,
                                                std::int64_t decision_ts_ns);

// ---------------------------------------------------------------------------
// Systematic long/short volatility construction
// ---------------------------------------------------------------------------

// One independently tradeable option opportunity. valuation_edge is model value
// minus market value: positive candidates rank for the long sleeve, negative for
// the short sleeve. vega_per_contract is a positive absolute dollar vega.
struct OptionOpportunity {
  std::string symbol;
  std::uint64_t position_id{0};
  OptionContract contract{};
  double multiplier{100.0};
  double valuation_edge{0.0};
  double vega_per_contract{0.0};
  double max_abs_contracts{0.0};
};

struct NamedPosition {
  std::string symbol;
  Position position{};

  [[nodiscard]] bool operator==(const NamedPosition &other) const noexcept {
    return symbol == other.symbol && position.id == other.position.id &&
           position.contract == other.position.contract && position.qty == other.position.qty &&
           position.multiplier == other.position.multiplier;
  }
};

struct LongShortVolatilityConfig {
  std::size_t n_long{1};
  std::size_t n_short{1};
  double target_gross_vega_per_sleeve{10'000.0};
  double max_abs_contracts_per_name{1'000.0};
  double min_abs_edge{0.0};
};

struct LongShortVolatilityPortfolio {
  // Long sleeve first in descending edge rank; short sleeve follows in ascending
  // edge rank. Ties use canonical symbol, uid, contract and position id.
  std::vector<NamedPosition> positions;
  double long_gross_vega{0.0};
  double short_gross_vega{0.0};
  double net_vega{0.0};
  bool capacity_limited{false};
};

// Select and capacity-weight the requested sleeves. Each sleeve receives the
// same achievable gross vega via deterministic capped equal-vega water filling.
[[nodiscard]] Result<LongShortVolatilityPortfolio>
construct_systematic_long_short_volatility(std::span<const OptionOpportunity> opportunities,
                                           const LongShortVolatilityConfig &config = {});

// ---------------------------------------------------------------------------
// Generic scalar risk aggregation and overlay
// ---------------------------------------------------------------------------

struct PositionRiskInput {
  Position position{};
  AmericanGreeks greeks_per_share{};
};

struct RiskVector {
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0};
  double theta{0.0};
  double gross_notional{0.0};

  [[nodiscard]] bool operator==(const RiskVector &) const = default;
};

// Deterministically aggregate position-scaled risk in ascending position-id
// order. Duplicate IDs and every non-finite input are rejected.
[[nodiscard]] Result<RiskVector>
aggregate_strategy_risk(std::span<const PositionRiskInput> positions);

enum class ScalarRiskBreachAction : std::uint8_t {
  Clamp = 0,
  Reject = 1,
};

enum class ScalarRiskLimitKind : std::uint8_t {
  Delta = 0,
  Gamma = 1,
  Vega = 2,
  Theta = 3,
  GrossNotional = 4,
  WorstScenarioLoss = 5,
};

struct ScalarRiskLimits {
  std::optional<double> max_abs_delta;
  std::optional<double> max_abs_gamma;
  std::optional<double> max_abs_vega;
  std::optional<double> max_abs_theta;
  std::optional<double> max_gross_notional;
  std::optional<double> max_worst_scenario_loss;
  ScalarRiskBreachAction action{ScalarRiskBreachAction::Clamp};
};

struct ScalarRiskOverlayResult {
  double scale{1.0};
  RiskVector risk{};
  std::vector<double> scenario_pnl;
  std::vector<ScalarRiskLimitKind> binding_limits;

  [[nodiscard]] bool constrained() const noexcept { return scale < 1.0; }
};

// Compute the single scale in [0,1] that satisfies every enabled scalar limit.
// Scenario loss is max(0, -min(scenario_pnl)). Clamp scales risk and all scenario
// P&Ls; Reject returns Unavailable if any scale below one would be required.
[[nodiscard]] Result<ScalarRiskOverlayResult>
apply_scalar_risk_overlay(const RiskVector &risk, std::span<const double> scenario_pnl,
                          const ScalarRiskLimits &limits);

// ---------------------------------------------------------------------------
// Heterogeneous component scenarios conditioned on one index move
// ---------------------------------------------------------------------------

// A non-index component receives:
//   spot_pct = spot_beta * index_spot_pct + residual_spot_pct
//   vol_bump = vol_beta  * index_vol_bump + residual_vol_bump
struct ComponentShockModel {
  std::uint32_t uid{0};
  double spot_beta{1.0};
  double residual_spot_pct{0.0};
  double vol_beta{1.0};
  double residual_vol_bump{0.0};

  [[nodiscard]] bool operator==(const ComponentShockModel &) const = default;
};

struct ConditionalComponentScenario {
  std::uint32_t index_uid{0};
  double index_spot_pct{0.0};
  double index_vol_bump{0.0};
  double dt{0.0};
  double dr{0.0};
  // Exactly one entry for every non-index uid in the risk inputs.
  std::vector<ComponentShockModel> components;
};

struct ScenarioRiskInput {
  Position position{};
  double spot{0.0};
  AmericanGreeks greeks_per_share{};
};

struct ComponentScenarioContribution {
  std::uint32_t uid{0};
  double pnl{0.0};

  [[nodiscard]] bool operator==(const ComponentScenarioContribution &) const = default;
};

struct ConditionalComponentScenarioResult {
  double total_pnl{0.0};
  // Ascending uid, with each uid reduced in ascending position-id order.
  std::vector<ComponentScenarioContribution> by_uid;
};

// Apply the same second-order Taylor kernel used by scenario_grid to supplied
// per-share Greeks, but with a heterogeneous shock per component uid.
[[nodiscard]] Result<ConditionalComponentScenarioResult>
conditional_component_scenario_pnl(std::span<const ScenarioRiskInput> positions,
                                   const ConditionalComponentScenario &scenario);

// ---------------------------------------------------------------------------
// Versioned broker-neutral basket and hedge intents
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kBasketOrderIntentSchemaVersion = 1u;
inline constexpr std::uint32_t kHedgeInstructionSchemaVersion = 1u;

// There is intentionally no live disposition in this module. A separate,
// explicitly-authorized transport layer may translate an approved dry-run intent.
enum class IntentDisposition : std::uint8_t {
  ResearchOnly = 0,
  DryRun = 1,
};

enum class AlgoStyle : std::uint8_t {
  Passive = 0,
  Adaptive = 1,
  Twap = 2,
  Vwap = 3,
  Immediate = 4,
};

struct ParticipationRate {
  double fraction{0.10};

  [[nodiscard]] bool operator==(const ParticipationRate &) const = default;
};

struct TimeInForceSeconds {
  std::uint32_t value{300u};

  [[nodiscard]] bool operator==(const TimeInForceSeconds &) const = default;
};

struct LimitOffsetBps {
  double value{0.0};

  [[nodiscard]] bool operator==(const LimitOffsetBps &) const = default;
};

struct AlgoParameters {
  AlgoStyle style{AlgoStyle::Passive};
  ParticipationRate max_participation{};
  TimeInForceSeconds horizon{};
  LimitOffsetBps limit_offset{};
  bool allow_partial{true};

  [[nodiscard]] bool operator==(const AlgoParameters &) const = default;
};

[[nodiscard]] Status validate_algo_parameters(const AlgoParameters &parameters);

struct OptionOrderDelta {
  std::uint64_t position_id{0};
  std::string symbol;
  OptionContract contract{};
  double multiplier{100.0};
  double current_quantity{0.0};
  double target_quantity{0.0};
  double quantity_delta{0.0};

  [[nodiscard]] bool operator==(const OptionOrderDelta &) const = default;
};

struct HedgeTarget {
  std::uint32_t uid{0};
  double current_shares{0.0};
  double target_shares{0.0};
};

struct HedgeInstruction {
  std::uint32_t schema_version{kHedgeInstructionSchemaVersion};
  std::uint32_t uid{0};
  double current_shares{0.0};
  double target_shares{0.0};
  double shares_to_trade{0.0};

  [[nodiscard]] bool operator==(const HedgeInstruction &) const = default;
};

struct BasketOrderIntent {
  std::uint32_t schema_version{kBasketOrderIntentSchemaVersion};
  std::uint64_t strategy_fingerprint{0};
  std::int64_t decision_ts_ns{0};
  IntentDisposition disposition{IntentDisposition::ResearchOnly};
  AlgoParameters algo{};
  // Ascending position_id; unchanged positions are omitted.
  std::vector<OptionOrderDelta> option_orders;
  // Ascending uid; unchanged hedge targets are omitted.
  std::vector<HedgeInstruction> hedges;
};

// Generate target-minus-current quantity deltas. IDs are stable strategy keys:
// if one appears in both books, symbol/contract/multiplier must match exactly.
[[nodiscard]] Result<BasketOrderIntent> make_basket_order_intent(
    std::uint64_t strategy_fingerprint, std::int64_t decision_ts_ns,
    std::span<const NamedPosition> target, std::span<const NamedPosition> current,
    std::span<const HedgeTarget> hedge_targets = {}, const AlgoParameters &algo = {},
    IntentDisposition disposition = IntentDisposition::ResearchOnly);

} // namespace atx::vol

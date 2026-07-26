#pragma once

// Reusable, projection-backed option strategy templates for historical
// backtests. These templates describe model contracts (for example, a
// 3-calendar-month 40-delta put), never listed OPRA instruments. At each entry
// tick ProjectedTemplateStrategy resolves the complete cohort through
// project_option_contract and commits it only after every leg succeeds.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/contract_projection.hpp"
#include "atx/vol/strategy.hpp"

namespace atx::vol {

// Increment this salt whenever a change to projection, settlement, backtest
// accounting, or template serialization would make otherwise-identical inputs
// economically incomparable. It deliberately participates in every fingerprint.
inline constexpr std::uint64_t kBacktestTemplateEngineSchemaSalt =
    0x4154584254540001ULL; // "ATXBTT", schema/engine revision 1

struct BacktestTemplateLeg {
  ProjectedMaturitySpec maturity{};
  ProjectedStrikeSpec strike{};
  Side side{Side::Call};
  double quantity{1.0}; // signed, fixed number of contracts
  double multiplier{100.0};

  [[nodiscard]] bool operator==(const BacktestTemplateLeg &) const = default;
};

enum class BacktestHoldingRule : std::uint8_t {
  HoldToExpiry = 0,
};

// A theoretical option's target civil date is mapped to that NYSE trading date,
// or the following NYSE trading date for a weekend/standard holiday, while
// preserving the entry snapshot's projected UTC time-of-day. SurfaceDb archives
// use a fixed close-ish snapshot time (often 19:55Z), not Calendar::session_close;
// preserving it makes the engine's exact-expiry timestamp observable. Calendar
// is rule-based and does not model ad-hoc/emergency closures; a corpus spanning
// one needs an explicit override before this rule can guarantee an exact row.
enum class TheoreticalSettlementRule : std::uint8_t {
  FollowingNyseSessionSnapshot = 0,
};

struct BacktestProjectionSettings {
  bool analytic_greeks{true};
  double delta_tolerance{1.0e-7};
  QueryExecution query_execution{QueryExecution::Configured};

  [[nodiscard]] bool operator==(const BacktestProjectionSettings &) const = default;
};

struct BacktestStrategyTemplate {
  // Catalog metadata. Both fields are excluded from the economic fingerprint:
  // id is the caller/database lookup identity and name is a display label.
  std::string id;
  std::string name;
  std::vector<BacktestTemplateLeg> legs;
  unsigned entry_every_n{1}; // global backtest step cadence; 1 enters every step
  BacktestHoldingRule holding{BacktestHoldingRule::HoldToExpiry};
  HedgeSpec hedge{};
  BacktestProjectionSettings projection{};
  FrictionModel frictions{};
  TheoreticalSettlementRule settlement{TheoreticalSettlementRule::FollowingNyseSessionSnapshot};
};

// Validate all template invariants. Absolute expiries are checked only for
// positivity here; whether one lies after a particular valuation is checked
// atomically at projection time.
[[nodiscard]] Status validate_backtest_template(const BacktestStrategyTemplate &strategy_template);

// Stable, nonzero digest of every economic field plus
// kBacktestTemplateEngineSchemaSalt. Returns zero for an invalid template.
// Catalog id and display name are intentionally excluded.
[[nodiscard]] std::uint64_t
fingerprint_backtest_template(const BacktestStrategyTemplate &strategy_template);

// Build the requested long/short fixed-one-contract call+put strangle:
// 40-delta on each side, three calendar months, hold to expiry, and a daily
// delta-to-zero hedge. position_sign must be exactly +1 or -1.
[[nodiscard]] Result<BacktestStrategyTemplate>
make_40_delta_3_calendar_month_strangle_template(double position_sign = 1.0,
                                                 unsigned entry_every_n = 1u);

// IStrategy interpreter for one underlier uid. Entry cadence is keyed to the
// engine's global step_index, not to the number of cohorts already opened.
// Cohort preparation provides the strong guarantee for expected failures:
// book, next_lot_id, and the cohort counter are unchanged unless every projected
// leg has a concrete definition and finite model mark.
class ProjectedTemplateStrategy final : public IStrategy {
public:
  [[nodiscard]] static Result<ProjectedTemplateStrategy>
  create(BacktestStrategyTemplate strategy_template, std::uint32_t uid,
         std::uint32_t initial_cohort_counter = 0u);

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override;

  [[nodiscard]] HedgeSpec hedge_spec() const override { return strategy_template_.hedge; }
  [[nodiscard]] QueryExecution required_economic_execution() const noexcept override {
    return strategy_template_.projection.query_execution;
  }
  [[nodiscard]] std::span<const std::uint32_t> referenced_uids() const noexcept override {
    return referenced_uids_;
  }

  [[nodiscard]] const BacktestStrategyTemplate &template_spec() const noexcept {
    return strategy_template_;
  }
  [[nodiscard]] const FrictionModel &frictions() const noexcept {
    return strategy_template_.frictions;
  }
  [[nodiscard]] std::uint32_t next_cohort_counter() const noexcept { return cohort_counter_; }

private:
  ProjectedTemplateStrategy(BacktestStrategyTemplate strategy_template, std::uint32_t uid,
                            std::uint32_t initial_cohort_counter) noexcept;

  BacktestStrategyTemplate strategy_template_;
  std::uint32_t uid_{0};
  std::array<std::uint32_t, 1u> referenced_uids_{};
  std::uint32_t cohort_counter_{0};
};

} // namespace atx::vol

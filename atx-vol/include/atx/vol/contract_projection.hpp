#pragma once

// Projection of relative option templates onto an immutable historical
// PricedSurface. A projection turns "3 calendar month 40-delta call" into a
// concrete theoretical contract with an absolute expiry timestamp and strike.
// The concrete definition can then be held/repriced like any other OptionContract.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

enum class ProjectedMaturityKind : std::uint8_t {
  YearFraction = 0,
  CalendarDays = 1,
  CalendarMonths = 2,
  AbsoluteExpiry = 3,
};

struct ProjectedMaturitySpec {
  ProjectedMaturityKind kind{ProjectedMaturityKind::YearFraction};
  double year_fraction{0.25};
  std::int32_t calendar_count{0};
  std::int64_t expiry_ts_ns{0};

  [[nodiscard]] static ProjectedMaturitySpec years(double value) noexcept;
  [[nodiscard]] static ProjectedMaturitySpec days(std::int32_t value) noexcept;
  [[nodiscard]] static ProjectedMaturitySpec months(std::int32_t value) noexcept;
  [[nodiscard]] static ProjectedMaturitySpec absolute(std::int64_t expiry_ts_ns) noexcept;

  [[nodiscard]] bool operator==(const ProjectedMaturitySpec &) const = default;
};

enum class ProjectedStrikeKind : std::uint8_t {
  AtmForward = 0,
  Delta = 1,
  LogMoneyness = 2,
  AbsoluteStrike = 3,
};

struct ProjectedStrikeSpec {
  ProjectedStrikeKind kind{ProjectedStrikeKind::AtmForward};
  double value{0.0};

  [[nodiscard]] static ProjectedStrikeSpec atm_forward() noexcept;
  [[nodiscard]] static ProjectedStrikeSpec delta(double target_abs_delta) noexcept;
  [[nodiscard]] static ProjectedStrikeSpec log_moneyness(double k) noexcept;
  [[nodiscard]] static ProjectedStrikeSpec absolute(double strike) noexcept;

  [[nodiscard]] bool operator==(const ProjectedStrikeSpec &) const = default;
};

struct OptionProjectionSpec {
  std::uint32_t uid{0};
  ProjectedMaturitySpec maturity{};
  ProjectedStrikeSpec strike{};
  Side side{Side::Call};
  double multiplier{100.0};

  [[nodiscard]] bool operator==(const OptionProjectionSpec &) const = default;
};

enum class OptionProjectionOutput : std::uint8_t {
  DefinitionOnly = 0,
  Mark = 1,
  FullGreeks = 2,
};

enum class OptionProjectionStatus : std::uint8_t {
  Ok = 0,
  InvalidSpec = 1,
  SurfaceUnavailable = 2,
  MaturityUnavailable = 3,
  DeltaUnreachable = 4,
  PricingError = 5,
};

[[nodiscard]] const char *to_string(OptionProjectionStatus status) noexcept;

struct OptionProjectionConfig {
  OptionProjectionOutput output{OptionProjectionOutput::FullGreeks};
  bool analytic_greeks{true};
  double delta_tolerance{1.0e-7};
  // 0 uses the library worker policy (environment cap or hardware concurrency).
  // Scalar project_option_contract ignores this field.
  unsigned n_threads{0};
  // Route every American pricing query through the same execution policy used
  // by the surrounding backtest. Appended for aggregate-source compatibility.
  QueryExecution query_execution{QueryExecution::Configured};
};

// Immutable economic identity resolved at one historical valuation. `contract.T`
// is the residual year fraction at `valuation_ts_ns`; `expiry_ts_ns` is the
// absolute anchor used when the option is subsequently aged through a backtest.
struct ProjectedOptionDefinition {
  OptionContract contract{};
  std::int64_t valuation_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  double multiplier{100.0};
  std::uint64_t fingerprint{0};

  [[nodiscard]] bool operator==(const ProjectedOptionDefinition &) const = default;
};

struct ProjectedOption {
  ProjectedOptionDefinition definition{};
  double forward{0.0};
  double implied_vol{0.0};
  double requested_abs_delta{0.0};
  double achieved_delta{0.0};
  double model_mark{0.0};
  AmericanGreeks greeks{};
  std::uint16_t delta_evaluations{0};
  OptionProjectionStatus status{OptionProjectionStatus::Ok};

  [[nodiscard]] bool operator==(const ProjectedOption &) const = default;
};

// Resolve one template directly against one surface. On success the output is a
// concrete absolute-expiry definition plus the requested mark/risk materialization.
// WS-ZC1: takes a `SurfaceRef`, so the surface may be OWNED or a BORROWED mapped
// view. A `PricedSurface` / `PricedSurfaceView` lvalue converts implicitly, so
// existing call sites are unchanged.
[[nodiscard]] Result<ProjectedOption>
project_option_contract(const SurfaceRef &surface, const OptionProjectionSpec &spec,
                        const OptionProjectionConfig &config = {});

// Prepared projection plan for repeated use across historical SurfaceSets. The
// template vector and a uid-grouped execution permutation are built once. The
// hot `project_into` path writes caller-owned output slots, is deterministic
// across thread counts, and allocates nothing itself in serial steady state.
class PreparedOptionProjection {
public:
  [[nodiscard]] static Result<PreparedOptionProjection>
  create(std::span<const OptionProjectionSpec> specs);

  // BORROW of the template vector this plan owns (a COPY of the `specs` handed to
  // `create`, so the caller's input storage is not retained). Valid for the
  // plan's lifetime: the plan is immutable after `create` — no member function
  // rebuilds `specs_` — so only destroying the plan or assigning over it (copy or
  // move) invalidates the span, and a span taken from one plan never names a
  // copy's storage. Concurrent const readers are safe (`project_into` is const and
  // writes only the caller's `output`). Copy out to outlive the plan.
  [[nodiscard]] std::span<const OptionProjectionSpec> specs() const noexcept { return specs_; }
  [[nodiscard]] std::size_t size() const noexcept { return specs_.size(); }
  [[nodiscard]] std::uint64_t fingerprint() const noexcept { return fingerprint_; }

  // Per-row expected market failures are recorded in ProjectedOption::status and
  // do not fail the batch. Structural output/config errors fail the call.
  [[nodiscard]] Status project_into(const SurfaceSet &surfaces, std::span<ProjectedOption> output,
                                    const OptionProjectionConfig &config = {}) const;

private:
  std::vector<OptionProjectionSpec> specs_{};
  std::vector<std::uint32_t> execution_order_{};
  std::uint64_t fingerprint_{0};
};

} // namespace atx::vol

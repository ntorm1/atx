#pragma once

// Projection of relative option templates onto an immutable historical
// PricedSurface. A projection turns "3 calendar month 40-delta call" into a
// concrete theoretical contract with an absolute expiry timestamp and strike.
// The concrete definition can then be held/repriced like any other OptionContract.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/core/types.hpp"

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

// Delta-strike resolution policy. FastScreenColdConfirm may use a prepared
// marks tier only to propose a strike; every successful result is validated by
// the cold-reference American delta and falls back to the robust cold solver.
enum class OptionDeltaSolvePolicy : std::uint8_t {
  Direct = 0,
  FastScreenColdConfirm = 1,
  // Cross-sectional inverse-delta with cold confirm. In the scalar
  // project_option_contract entry point this behaves exactly as
  // FastScreenColdConfirm (a batch of one gains nothing); batch consumers
  // (PreparedVarPortfolio) select the cross-sectional group solver.
  CrossSectionalColdConfirm = 2,
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
  OptionDeltaSolvePolicy delta_solve_policy{OptionDeltaSolvePolicy::Direct};
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

// Absolute expiry timestamp for `spec` anchored at `valuation_ts_ns`. Public
// form of the maturity resolver used internally by project_option_contract.
[[nodiscard]] Result<std::int64_t> resolve_projected_expiry(std::int64_t valuation_ts_ns,
                                                            const ProjectedMaturitySpec &spec);

// Nonzero identity fingerprint for a resolved `definition`. Public form of the
// fingerprint helper used internally by project_option_contract.
[[nodiscard]] std::uint64_t
projected_definition_fingerprint(const ProjectedOptionDefinition &definition);

// Reusable, allocation-amortized workspace for solve_american_delta_batch.
// resize(n) grows all columns; steady-state reuse allocates nothing.
struct AmericanDeltaBatchScratch {
  std::vector<double> k_log{};    // current candidate log-moneyness per row
  std::vector<double> strike{};   // F(T) * exp(k_log)
  std::vector<double> residual{}; // |delta| - target at current candidate
  std::vector<double> prev_k{};   // previous point for the secant update
  std::vector<double> prev_residual{};
  std::vector<double> forward{};   // F(T) per row (seed-time resolution)
  std::vector<double> sigma{};     // smile-refreshed seed vol per row
  std::vector<double> signed_d1{}; // Black seed d1 (slope reuse, pass 1)
  std::vector<double> iv{};        // evaluate_batch iv column
  std::vector<double> price{};     // evaluate_batch price column
  std::vector<AmericanGreeks> greeks{};
  std::vector<Status> pass_status{};
  std::vector<std::uint32_t> active{};             // stable-ordered unconverged row ids
  std::vector<double> active_strike{}, active_t{}; // compacted pass inputs
  std::vector<Side> active_side{};
  // Rows the batch passes could not finish, handed to the scalar fallback
  // tail in ascending row-id order (sorted in place before the tail runs, an
  // allocation-free step). Owned here rather than as a solver-local vector so
  // even the rare fallback path allocates nothing once resize(n) has run;
  // empty on the (steady-state) happy path.
  std::vector<std::uint32_t> fallback_rows{};
  void resize(std::size_t n);
};

// Validate that a row count fits solve_american_delta_batch's row-id space:
// active/fallback row ids and evaluate_batch pack indices are std::uint32_t
// throughout. Exposed (rather than kept file-local) so the bound is directly
// unit-testable -- constructing a >= 2^32-row span to exercise it through
// solve_american_delta_batch itself is not practical.
[[nodiscard]] Result<std::uint32_t> checked_row_count(std::size_t n) noexcept;

// Cross-sectional inverse-delta solve on ONE surface (owned or view). For each
// row i: find strike_out[i] with | |cold American delta| - target_abs_delta[i] |
// <= tolerance, achieved via a Black-style inverse-delta seed, laned cold
// American delta passes (evaluate_batch FirstOrder + reduced GreekNeeds), a
// vectorized Newton/secant correction, and a robust scalar fallback for the
// unconverged tail (Task 4). Internal batch acceptance uses tolerance/2 so the
// scalar cold oracle holds at the full tolerance despite the documented
// laned-vs-scalar kernel gap. Spans all length n; structural violations
// (length mismatch, invalid tolerance, empty batch) fail the call and leave
// every output span untouched -- that guarantee holds only for these
// pre-flight validation failures, checked before any row is processed; a
// structural failure surfaced mid-run (e.g. evaluate_batch itself erroring
// inside a pass) still returns Err but rows already frozen by an earlier
// pass keep their written outputs. Per-row market/convergence failures land
// in row_status_out and never invent strikes. Deterministic: fixed row
// order, batch-composition-invariant kernels, no cross-call state.
// Thread-safe for concurrent calls on distinct scratch.
//
// `accepted_price_out` (optional; empty, or length n like every other span)
// harvests the COLD AMERICAN MARK at each row's accepted strike -- the quantity
// a caller would otherwise recompute in a dedicated EvalField::Price pass over
// the solved strikes. It costs no extra laned pass: every laned FirstOrder pass
// already materializes `AmericanGreeks::price` at the strike it evaluated, and a
// row is accepted at exactly the strike its accepting pass evaluated, so the
// harvested value is that pass's own price column. Rows accepted by a laned pass
// therefore carry the laned Greek bundle's mark; rows that fell through to the
// scalar fallback tail carry ONE scalar cold `evaluate(..., EvalField::Price,
// ...)` at the scalar solve's own strike (the tail's bracketing solver computes
// deltas only), which keeps the tail bit-identical to a pure scalar valuation
// route. Rows that failed (row_status_out Err) get NaN, as do all rows when a
// pass errors out mid-run. Harvesting is a pure function of results the solve
// already produced, so it changes no strike, delta, evaluation count or status.
[[nodiscard]] Status solve_american_delta_batch(
    const SurfaceRef &surface, std::span<const double> T, std::span<const Side> side,
    std::span<const double> target_abs_delta, double tolerance, AmericanDeltaBatchScratch &scratch,
    std::span<double> strike_out, std::span<double> achieved_delta_out,
    std::span<std::uint16_t> evaluations_out, std::span<Status> row_status_out,
    std::span<double> accepted_price_out = {});

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

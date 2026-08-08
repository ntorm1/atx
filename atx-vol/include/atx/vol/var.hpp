#pragma once

// Historical simulation VaR for characteristic-preserving option/stock books.
//
// A reference-date option is identified by relative time to expiry and absolute
// American spot delta. The delta string is its moneyness coordinate: every
// historical observation independently restrikes the option on its base surface
// to that same delta and relative expiry. Units are then scaled to the reference
// dollar delta and held while the resulting concrete strike/expiry is repriced
// on the shifted surface. Stock hedges follow the same rule (shares are sized on
// the base date and held through the transition).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "atx/vol/backtest.hpp"
#include "atx/vol/contract_projection.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

class SurfaceDb;

enum class VarLegKind : std::uint8_t {
  Option = 0,
  Stock = 1,
};

struct VarOptionPosition {
  std::string underlier{};
  ProjectedMaturitySpec time_to_expiry{};
  double target_abs_delta{0.40};
  Side side{Side::Call};
  double quantity{0.0};
  double multiplier{100.0};

  [[nodiscard]] bool operator==(const VarOptionPosition &) const = default;
};

struct VarStockPosition {
  std::string underlier{};
  double shares{0.0};

  [[nodiscard]] bool operator==(const VarStockPosition &) const = default;
};

using VarPosition = std::variant<VarOptionPosition, VarStockPosition>;

// Economic sizing boundary shared by options and stock hedges. `unit_delta` is
// signed (calls/stock positive, puts negative); `target_dollar_delta` carries the
// desired position sign. All inputs must be finite, spot/multiplier positive,
// and |unit_delta| in (0, 1].
struct VarSizingInput {
  double target_dollar_delta{0.0};
  double spot{0.0};
  double unit_delta{0.0};
  double multiplier{1.0};
};

struct VarSizingResult {
  double units{0.0};
  double achieved_dollar_delta{0.0};

  [[nodiscard]] bool operator==(const VarSizingResult &) const = default;
};

// Resolve signed units from a dollar-delta target. Overflow and invalid market
// inputs are rejected; the function never clamps or silently changes exposure.
[[nodiscard]] Result<VarSizingResult> resolve_var_sizing(const VarSizingInput &input);

enum class VarLegStatus : std::uint8_t {
  Ok = 0,
  SurfaceUnavailable = 1,
  TimestampMismatch = 2,
  ProjectionUnavailable = 3,
  PricingError = 4,
  InvalidDelta = 5,
  InvalidValue = 6,
  ProvenanceRejected = 7,
  ExpiredBeforeShift = 8,
};

[[nodiscard]] const char *to_string(VarLegStatus status) noexcept;

enum class VarScenarioStatus : std::uint8_t {
  Ok = 0,
  MarketUnavailable = 1,
  TimestampMismatch = 2,
  LegFailure = 3,
  // Snapshot load failed for a reason other than a genuinely absent surface
  // (corrupt/truncated archive, I/O error): a structural/infrastructure
  // fault, not a market condition. Distinguished from MarketUnavailable so
  // VarScenarioFailurePolicy::ExcludeFromDistribution cannot silently absorb
  // it into a shrunken-but-unflagged loss distribution.
  ArchiveError = 4,
};

[[nodiscard]] const char *to_string(VarScenarioStatus status) noexcept;

struct VarEvaluationConfig {
  // 0 uses the process pricing executor's configured worker count.
  unsigned n_threads{0};
  double delta_tolerance{1.0e-7};
  // Delta-moneyness and marks default to the cold reference route. Valuation
  // may independently opt into a configured marks accelerator without changing
  // the projected strike, but callers own its economic parity validation.
  QueryExecution projection_execution{QueryExecution::ColdReference};
  QueryExecution valuation_execution{QueryExecution::ColdReference};
  // Prepared query tiers are no longer consulted for the root: the
  // cross-sectional cold route is the default, and every successful
  // projection remains cold-confirmed to delta_tolerance with a robust cold
  // fallback.
  OptionDeltaSolvePolicy projection_solve_policy{OptionDeltaSolvePolicy::CrossSectionalColdConfirm};
};

// A single independent historical return observation. The base and shifted
// SurfaceSets are borrowed for the duration of replay_into and must contain the
// same required underliers at the stated valuation timestamps.
struct VarScenario {
  std::int64_t base_ts_ns{0};
  const SurfaceSet *base_surfaces{nullptr};
  std::int64_t shifted_ts_ns{0};
  const SurfaceSet *shifted_surfaces{nullptr};
};

struct VarReferenceLeg {
  VarLegKind kind{VarLegKind::Option};
  std::uint32_t uid{0};
  std::string underlier{};
  double reference_units{0.0};
  double reference_spot{0.0};
  double reference_mark{0.0};
  double reference_delta{0.0};
  double target_dollar_delta{0.0};
  // Requested absolute delta moneyness for an option; zero for stock.
  double target_abs_delta{0.0};
  // Reference-date forward log-moneyness, retained for audit; zero for stock.
  double log_moneyness{0.0};

  [[nodiscard]] bool operator==(const VarReferenceLeg &) const = default;
};

// One scenario/position result. For stock, mark is spot, delta is one, strike
// and time-to-expiry are zero, and definition_fingerprint identifies the
// normalized stock leg. Failed rows contain their status and NaN numeric fields.
struct VarLegFrame {
  VarLegKind kind{VarLegKind::Option};
  VarLegStatus status{VarLegStatus::Ok};
  std::uint32_t uid{0};
  double units{0.0};
  double base_spot{0.0};
  double shifted_spot{0.0};
  double base_mark{0.0};
  double shifted_mark{0.0};
  double base_delta{0.0};
  double dollar_delta{0.0};
  double base_value{0.0};
  double shifted_value{0.0};
  double pnl{0.0};
  double strike{0.0};
  double base_time_to_expiry{0.0};
  double shifted_time_to_expiry{0.0};
  std::uint64_t definition_fingerprint{0};

  [[nodiscard]] bool operator==(const VarLegFrame &) const = default;
};

struct VarScenarioFrame {
  std::int64_t base_ts_ns{0};
  std::int64_t shifted_ts_ns{0};
  VarScenarioStatus status{VarScenarioStatus::Ok};
  double base_value{0.0};
  double shifted_value{0.0};
  double pnl{0.0};
  double dollar_delta{0.0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint64_t definition_fingerprint{0};

  [[nodiscard]] bool operator==(const VarScenarioFrame &) const = default;
};

struct VarRiskStatistics {
  double confidence{0.0};
  double value_at_risk{0.0};
  double expected_shortfall{0.0};
  std::size_t n_scenarios{0};

  [[nodiscard]] bool operator==(const VarRiskStatistics &) const = default;
};

// Deterministic nearest-rank loss quantile over successful frames. Loss is
// -frame.pnl; expected shortfall is the inclusive average beginning at the VaR
// rank. Failed frames are excluded.
[[nodiscard]] Result<VarRiskStatistics>
historical_var_statistics(std::span<const VarScenarioFrame> frames, double confidence);

// Immutable, reference-anchored portfolio plan. Preparation owns all normalized
// definitions and retains no reference-surface borrow. Const replay calls are
// safe concurrently when their output spans do not overlap.
class PreparedVarPortfolio {
public:
  ~PreparedVarPortfolio();
  PreparedVarPortfolio(PreparedVarPortfolio &&) noexcept;
  PreparedVarPortfolio &operator=(PreparedVarPortfolio &&) noexcept;
  PreparedVarPortfolio(const PreparedVarPortfolio &) = delete;
  PreparedVarPortfolio &operator=(const PreparedVarPortfolio &) = delete;

  // Anchor positions to `reference_surfaces`. Underlier names use the archive's
  // stable uid_for_symbol mapping. Absolute-expiry positions are rejected: VaR
  // scenarios preserve relative time to expiry, not a historical calendar date.
  [[nodiscard]] static Result<PreparedVarPortfolio> create(std::span<const VarPosition> positions,
                                                           const SurfaceSet &reference_surfaces,
                                                           const VarEvaluationConfig &config = {});

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] double reference_value() const noexcept;
  [[nodiscard]] double reference_dollar_delta() const noexcept;
  [[nodiscard]] std::int64_t reference_ts_ns() const noexcept;
  [[nodiscard]] std::uint64_t fingerprint() const noexcept;
  [[nodiscard]] std::span<const VarReferenceLeg> reference_legs() const noexcept;

  // Evaluate independent base -> shifted observations. `frames` must match the
  // scenario count. `leg_frames` is either empty or scenario-major with exactly
  // scenarios.size()*size() elements. An empty leg span selects the vectorized
  // aggregate path; it is economically equivalent to retained-leg evaluation,
  // but floating-point reduction/kernel differences need not be byte-identical.
  // Expected market failures are recorded in frame statuses; structural
  // input/output errors fail the call.
  [[nodiscard]] Status replay_into(std::span<const VarScenario> scenarios,
                                   std::span<VarScenarioFrame> frames,
                                   std::span<VarLegFrame> leg_frames = {},
                                   const VarEvaluationConfig &config = {}) const;

private:
  PreparedVarPortfolio();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

enum class VarScenarioFailurePolicy : std::uint8_t {
  RejectRun = 0,
  ExcludeFromDistribution = 1,
};

struct VarRunConfig {
  // Empty reference_date selects the database's latest partition.
  std::string reference_date{};
  // Empty bounds select the full database range. Bounds are inclusive; N dates
  // produce N-1 independent adjacent-transition observations.
  std::string date_begin{};
  std::string date_end{};
  double confidence{0.99};
  VarEvaluationConfig evaluation{};
  VarScenarioFailurePolicy failure_policy{VarScenarioFailurePolicy::RejectRun};
  bool retain_leg_frames{false};
  ArchiveBacking archive_backing{ArchiveBacking::Sealed};
  // ColdReference is the one-pass correctness default. RepresentativeFast
  // prepares a certified accelerator during snapshot load for repeated replay.
  QueryPricingTier query_pricing_tier{QueryPricingTier::ColdReference};
  SurfaceProvenancePolicy provenance_policy{SurfaceProvenancePolicy::RequireAdmittedRisk};
};

struct HistoricalVarResult {
  std::string reference_date{};
  std::int64_t reference_ts_ns{0};
  double reference_value{0.0};
  double reference_dollar_delta{0.0};
  VarRiskStatistics risk{};
  std::vector<std::string> base_dates{};
  std::vector<std::string> shifted_dates{};
  std::vector<VarScenarioFrame> frames{};
  // Scenario-major and empty unless VarRunConfig::retain_leg_frames is true.
  std::vector<VarLegFrame> leg_frames{};
  std::size_t n_legs{0};
};

// End-to-end SurfaceDb replay. The database is read only. Dates are resolved
// through Clock, sorted ascending, split into balanced contiguous subranges, and
// evaluated on the persistent atx-vol pricing executor.
[[nodiscard]] Result<HistoricalVarResult> run_historical_var(const SurfaceDb &db,
                                                             std::span<const VarPosition> positions,
                                                             const VarRunConfig &config = {});

} // namespace atx::vol

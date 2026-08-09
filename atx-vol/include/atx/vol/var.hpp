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

// Where each option leg's BASE mark comes from. Both sources are cold American
// lattice marks on the base surface at the accepted strike and base T -- this
// knob does not open a prepared/fast marks tier, it only chooses whether that
// one number is computed twice.
//
// The cross-sectional delta solver already evaluates the full first-order
// Andersen-Lake bundle at every strike it tries, and it accepts a row at exactly
// the strike its accepting pass evaluated -- so `AmericanGreeks::price` from
// that pass IS the base mark, and the dedicated EvalField::Price pass that
// follows recomputes it. HarvestedFromSolver reuses the solver's value and skips
// that pass (~18-22 % of core time on the profiled SP100 fixture).
//
// NOT bit-identical to the dedicated pass, because the two marks come from
// different PRICING ENTRY POINTS, not merely from different ISA routes. The
// solver requests the analytic (`analytic=true`) FirstOrder bundle, so its mark
// is `american_greeks_al(...).price`; the dedicated pass requests
// `EvalField::Price` with `analytic=false`, so its mark is `american_price(...)`.
// Both are cold Andersen-Lake solves of the same contract and agree to ~1e-14
// relative, but they are not the same arithmetic. (A secondary, ISA-dependent
// difference rides on top: the dedicated pass's laned wrapper packs into the
// AVX2 kernel only when a same-T run fills a pack, and falls back to the same
// scalar `american_price` otherwise.) Measured bounds are pinned by
// ContractProjection.HarvestedSolverPriceMatchesDedicatedPricePassWithinPinnedGap.
// Default is therefore DedicatedPricePass.
enum class VarBaseMarkSource : std::uint8_t {
  DedicatedPricePass = 0,  // current behavior, default
  HarvestedFromSolver = 1, // reuse the solver's accepted-strike cold price
};

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
  // Restrike roots with |log-moneyness| beyond this bound fail the leg with
  // InvalidDelta instead of pricing on pure wing extrapolation.
  double max_restrike_abs_log_moneyness{5.0};
  // Harvesting is ADMISSIBLE only where the solver is actually running and
  // everything in sight is cold: projection_solve_policy ==
  // CrossSectionalColdConfirm (no other policy runs the batch solver to harvest
  // from) and valuation_execution == ColdReference (the harvested mark is a cold
  // lattice mark by construction, so it must not silently override a caller's
  // opt-in to a configured marks accelerator). Any other combination keeps the
  // dedicated pass -- the conservative direction, identical to today's numbers.
  // That is also what makes the whole-scenario downgrade to
  // FastScreenColdConfirm immune to this knob: the downgraded config no longer
  // satisfies the policy condition, so the scalar route computes its own marks
  // exactly as it does with harvesting off.
  VarBaseMarkSource base_mark_source{VarBaseMarkSource::DedicatedPricePass};
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
  // Bit 0: base-side tenor extrapolation; bit 1: shifted-side tenor
  // extrapolation; bit 2: restrike root beyond max_restrike_abs_log_moneyness.
  std::uint8_t diagnostic_flags{0};

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
  // Count of Ok legs in this scenario whose base or shifted side used tenor
  // extrapolation (VarLegFrame::diagnostic_flags bits 0/1's per-scenario
  // tally). Populated by both the aggregate and retained-leg routes, so it
  // is available regardless of VarRunConfig::retain_leg_frames.
  std::uint32_t n_tenor_extrapolated{0};

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

// Age/recency weighting for the weighted VaR/ES overload below.
struct VarWeighting {
  // 1.0 = equal weights (current behavior). Otherwise BRW/EWMA weight
  // lambda^(age) normalized, age 0 = most recent scenario by shifted_ts_ns.
  double ewma_lambda{1.0};
};

// Weighted quantile: sort losses ascending, accumulate normalized weights,
// VaR = first loss whose cumulative weight >= confidence; ES = weighted mean
// of losses >= VaR (weights renormalized over that tail). Applies the same
// frame-qualification filter as the unweighted overload above (Ok status, no
// failed legs, at least one ok leg, a real fingerprint, finite pnl), so with
// ewma_lambda == 1.0 the two overloads select the identical loss set --
// ewma_lambda == 1.0 (exact) is special-cased to delegate to the two-arg
// overload directly, guaranteeing bit-identical reproduction rather than
// merely numerically-close agreement (equal per-scenario weights make the
// general weighted-quantile arithmetic and the plain nearest-rank arithmetic
// mathematically equivalent, but not bit-identical, since they sum in a
// different order).
[[nodiscard]] Result<VarRiskStatistics>
historical_var_statistics(std::span<const VarScenarioFrame> frames, double confidence,
                          const VarWeighting &weighting);

// One VarRiskStatistics per confidence in `confidences`, same order as input,
// computed against the same weighted loss distribution. Every confidence must
// be finite and in (0, 1); the result is monotone non-decreasing in
// confidence (a higher confidence can only select an equal-or-larger loss
// under the same weighted quantile rule).
[[nodiscard]] Result<std::vector<VarRiskStatistics>>
historical_var_curve(std::span<const VarScenarioFrame> frames, std::span<const double> confidences,
                     const VarWeighting &weighting = {});

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

  // TEST-ONLY (Task 8, dynamic scenario scheduling). Same contract as
  // replay_into, but dispatches scenarios across workers via the pre-Task-8
  // static contiguous-range scheduler (PricingExecutor::run_blocks over
  // balanced ranges) instead of replay_into's PricingExecutor::run_dynamic.
  // Exists solely so the scheduling swap can be pinned bit-identical against
  // its predecessor; no production call site uses it and no
  // VarEvaluationConfig knob selects it.
  [[nodiscard]] Status replay_into_static_scheduling_for_test(
      std::span<const VarScenario> scenarios, std::span<VarScenarioFrame> frames,
      std::span<VarLegFrame> leg_frames = {}, const VarEvaluationConfig &config = {}) const;

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
  // Maximum calendar-day gap between base and shifted session for a transition
  // to enter the distribution. 0 disables the guard (current behavior).
  int max_session_gap_days{0};
  // Fail the run when more than this fraction of scenarios is excluded under
  // ExcludeFromDistribution. 1.0 disables the guard.
  double max_excluded_fraction{1.0};
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
  std::size_t n_gap_skipped{0};
  std::size_t n_excluded_from_distribution{0};
  // Legs whose solve or valuation used tenor extrapolation, either side.
  // Deterministic sum of VarScenarioFrame::n_tenor_extrapolated across
  // frames, in scenario order; populated on both the aggregate and
  // retained-leg routes (VarRunConfig::retain_leg_frames does not gate it).
  std::size_t n_tenor_extrapolated_legs{0};
};

// End-to-end SurfaceDb replay. The database is read only. Dates are resolved
// through Clock, sorted ascending, split into balanced contiguous subranges, and
// evaluated on the persistent atx-vol pricing executor.
[[nodiscard]] Result<HistoricalVarResult> run_historical_var(const SurfaceDb &db,
                                                             std::span<const VarPosition> positions,
                                                             const VarRunConfig &config = {});

} // namespace atx::vol

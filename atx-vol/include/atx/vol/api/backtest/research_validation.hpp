#pragma once

// Deterministic, point-in-time research validation for projection-backed
// strategy mining.
//
// This layer deliberately sits above BacktestDb. BacktestDb stores economic
// histories; this API controls when information was knowable, how candidate
// parameterizations are enumerated, which observations are genuinely
// out-of-sample, and whether statistical evidence is strong enough to promote.
// It performs no I/O and owns no mutable global state.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "atx/vol/api/storage/surface_archive.hpp" // ArchiveContentIdentity
#include "atx/vol/api/core/types.hpp"           // Result, Status

namespace atx::vol {

inline constexpr std::uint64_t kResearchValidationSchemaSalt =
    0x4154585256430001ULL; // "ATXRVC", schema/engine revision 1

// One point-in-time panel observation and its realized, unit-position outcome.
//
// Clock contract:
//   observed <= available <= decision < execution < label_end
//
// The strict decision/execution boundary prevents a signal computed from a
// close snapshot from being filled at that same modeled close. forward_pnl is
// the PnL of a +1 unit position over [execution,label_end); lagged_capital is
// the strictly positive capital known before that outcome and turns PnL into a
// dimensionless return. Rows are keyed by (decision_ts_ns, uid).
struct ResearchObservation {
  std::uint32_t uid{0};
  std::int64_t observed_ts_ns{0};
  std::int64_t available_ts_ns{0};
  std::int64_t decision_ts_ns{0};
  std::int64_t execution_ts_ns{0};
  std::int64_t label_end_ts_ns{0};
  double signal{0.0};
  double forward_pnl{0.0};
  double lagged_capital{0.0};
  ArchiveContentIdentity source_identity{};

  [[nodiscard]] bool operator==(const ResearchObservation &) const noexcept = default;
};

// Validate clocks, values, source identity, uid, and panel-key uniqueness, then
// return rows in canonical (decision timestamp, uid) order. Caller ordering
// never changes downstream fold or trial identity.
[[nodiscard]] Result<std::vector<ResearchObservation>>
canonicalize_research_observations(std::span<const ResearchObservation> observations);

using ResearchParameterValue = std::variant<std::int64_t, std::uint64_t, double, bool, std::string>;

struct ResearchParameterAxis {
  std::string name;
  std::vector<ResearchParameterValue> values;

  [[nodiscard]] bool operator==(const ResearchParameterAxis &) const = default;
};

struct ResearchGridParameter {
  std::string name;
  ResearchParameterValue value;

  [[nodiscard]] bool operator==(const ResearchGridParameter &) const = default;
};

struct ResearchParameterSet {
  std::vector<ResearchGridParameter> values;
  std::uint64_t identity{0};

  [[nodiscard]] bool operator==(const ResearchParameterSet &) const = default;
};

struct ResearchParameterGrid {
  std::vector<ResearchParameterAxis> axes;
  // Hard bound checked before allocation. Zero is invalid.
  std::uint64_t max_trials{100'000u};
};

// Canonical Cartesian expansion. Axes and values are sorted by their stable
// wire representation, so input permutation has no effect. Empty/duplicate
// axes or values, non-finite doubles, multiplication overflow, and a product
// above max_trials fail before allocation.
[[nodiscard]] Result<std::vector<ResearchParameterSet>>
enumerate_research_parameter_grid(const ResearchParameterGrid &grid);

enum class ResearchWalkForwardKind : std::uint8_t {
  Anchored = 0,
  Rolling = 1,
};

struct ResearchWalkForwardSpec {
  ResearchWalkForwardKind kind{ResearchWalkForwardKind::Anchored};
  std::size_t min_train_groups{0};
  std::size_t test_groups{0};
  std::size_t step_groups{0};
  // Required and positive for Rolling; ignored for Anchored.
  std::size_t max_train_groups{0};
  // Pre-test time gap. Candidate train rows inside
  // [test_observed_min-embargo_ns, test_observed_min) are excluded.
  std::int64_t embargo_ns{0};

  [[nodiscard]] bool operator==(const ResearchWalkForwardSpec &) const noexcept = default;
};

struct ResearchValidationFold {
  std::uint32_t id{0};
  std::vector<std::size_t> train_indices;
  std::vector<std::size_t> test_indices;
  std::vector<std::size_t> purged_indices;
  std::vector<std::size_t> embargoed_indices;

  [[nodiscard]] bool operator==(const ResearchValidationFold &) const = default;
};

struct ResearchValidationPlan {
  ResearchWalkForwardSpec spec{};
  std::vector<ResearchValidationFold> folds;

  [[nodiscard]] bool operator==(const ResearchValidationPlan &) const = default;
};

// Split whole decision-timestamp groups. Training outcomes whose
// [decision,label_end) interval reaches a test observation are purged; the
// explicit pre-test embargo is then applied to remaining rows. Test windows
// must not overlap (step_groups >= test_groups), making their OOS returns
// stitchable without duplicate observations.
[[nodiscard]] Result<ResearchValidationPlan>
make_purged_walk_forward_plan(std::span<const ResearchObservation> canonical_observations,
                              const ResearchWalkForwardSpec &spec);

// Audit an externally-loaded/stored plan against canonical observations.
// Checks indices, disjoint sets, timestamp grouping, chronology, outcome purge,
// embargo, and non-overlapping test membership.
[[nodiscard]] Status
validate_research_plan_no_leakage(std::span<const ResearchObservation> canonical_observations,
                                  const ResearchValidationPlan &plan);

enum class ResearchSignalTransform : std::uint8_t {
  Identity = 0,
  Difference = 1,
  RollingZScore = 2,
};

enum class ResearchSignalDirection : std::uint8_t {
  LongHigh = 0,
  ShortHigh = 1,
};

// Candidate transforms are evaluated independently per uid. lag and lookback
// are counts of prior observations for that uid, not calendar days. The
// transformed value is mapped to {-1,0,+1}; direction optionally flips it.
struct ResearchSignalCandidate {
  std::string id;
  ResearchSignalTransform transform{ResearchSignalTransform::Identity};
  std::size_t lag{0};
  std::size_t lookback{0};
  ResearchSignalDirection direction{ResearchSignalDirection::LongHigh};

  [[nodiscard]] bool operator==(const ResearchSignalCandidate &) const = default;
};

// One timestamp-level dimensionless strategy return. Panel PnL and capital are
// summed before division; capital is always finite and positive.
struct ResearchReturnObservation {
  std::int64_t decision_ts_ns{0};
  double pnl{0.0};
  double lagged_capital{0.0};
  double value{0.0};

  [[nodiscard]] bool operator==(const ResearchReturnObservation &) const noexcept = default;
};

struct ResearchSelectionAdjustment {
  bool family_sealed{false};
  // Includes successful, failed, and rejected configurations in the frozen
  // experiment family. It may not be zero.
  std::uint64_t attempted_trials{0};
  // Sample variance of the successful trials' unannualized Sharpe estimates.
  double successful_trial_sharpe_variance{0.0};
};

struct ResearchReturnStats {
  std::size_t n_observations{0};
  std::uint64_t attempted_trials{0};
  std::size_t newey_west_lag{0};
  double mean{0.0};
  double sample_standard_deviation{0.0};
  double skewness{0.0};
  double pearson_kurtosis{0.0};
  double sharpe{0.0}; // unannualized mean / sample standard deviation
  double newey_west_long_run_variance{0.0};
  double hac_mean_standard_error{0.0};
  double hac_t_statistic{0.0};
  double one_sided_p_value{1.0};
  double probabilistic_sharpe_probability{0.0};
  double deflated_sharpe_probability{0.0};
  double deflated_sharpe_threshold{0.0};
  double max_drawdown{0.0}; // additive cumulative-return peak-to-trough
};

// Element-order deterministic statistics. Newey-West uses a Bartlett kernel at
// the caller's explicit lag. PSR/DSR use the first four return moments and the
// sealed experiment family's complete attempted-trial count.
[[nodiscard]] Result<ResearchReturnStats>
compute_research_return_stats(std::span<const ResearchReturnObservation> returns,
                              std::size_t newey_west_lag,
                              const ResearchSelectionAdjustment &selection);

struct ResearchCandidateEvaluation {
  ResearchSignalCandidate candidate{};
  std::uint64_t candidate_identity{0};
  std::vector<ResearchReturnObservation> in_sample_returns;
  std::vector<ResearchReturnObservation> oos_returns;
  ResearchReturnStats in_sample_stats{};
  ResearchReturnStats oos_stats{};
};

[[nodiscard]] Result<ResearchCandidateEvaluation> evaluate_research_signal_candidate(
    std::span<const ResearchObservation> canonical_observations, const ResearchValidationPlan &plan,
    const ResearchSignalCandidate &candidate, std::size_t newey_west_lag,
    const ResearchSelectionAdjustment &selection);

struct ResearchTrialFamily {
  bool sealed{false};
  std::uint64_t attempted_trials{0};
};

struct ResearchMiningResult {
  std::vector<ResearchCandidateEvaluation> evaluations;
  std::string selected_candidate_id;
  std::uint64_t selected_candidate_identity{0};
};

// Evaluate every candidate, derive the successful-trial Sharpe variance, apply
// the sealed family's complete attempted count, then select on OOS HAC t only.
// Ties prefer higher OOS mean, lower lookback, lower lag, then stable identity.
[[nodiscard]] Result<ResearchMiningResult>
mine_research_signal_candidates(std::span<const ResearchObservation> canonical_observations,
                                const ResearchValidationPlan &plan,
                                std::span<const ResearchSignalCandidate> candidates,
                                std::size_t newey_west_lag, const ResearchTrialFamily &family);

[[nodiscard]] inline Result<ResearchMiningResult>
mine_research_signal_candidates(std::span<const ResearchObservation> canonical_observations,
                                const ResearchValidationPlan &plan,
                                std::initializer_list<ResearchSignalCandidate> candidates,
                                std::size_t newey_west_lag, const ResearchTrialFamily &family) {
  return mine_research_signal_candidates(
      canonical_observations, plan,
      std::span<const ResearchSignalCandidate>{candidates.begin(), candidates.size()},
      newey_west_lag, family);
}

[[nodiscard]] Result<std::vector<double>>
holm_adjusted_p_values(std::span<const double> raw_p_values);

// Benjamini-Yekutieli FDR adjustment, valid under arbitrary dependence.
[[nodiscard]] Result<std::vector<double>>
benjamini_yekutieli_adjusted_p_values(std::span<const double> raw_p_values);

enum class ResearchPromotionGateCode : std::uint8_t {
  SourceLineage = 0,
  FamilySealed = 1,
  IndependentValidation = 2,
  HoldoutConsumed = 3,
  CostStress = 4,
  Concentration = 5,
  MinimumObservations = 6,
  HacTStatistic = 7,
  DeflatedSharpe = 8,
  AdjustedPValue = 9,
  MaximumDrawdown = 10,
};

struct ResearchPromotionGateSpec {
  std::size_t min_oos_observations{504u};
  double min_hac_t_statistic{3.0};
  double min_deflated_sharpe_probability{0.95};
  double max_adjusted_p_value{0.05};
  double max_drawdown{0.25};
};

struct ResearchPromotionEvidence {
  bool source_lineage_complete{false};
  bool family_sealed{false};
  bool independent_validation_passed{false};
  bool holdout_consumed{false};
  bool cost_stress_passed{false};
  bool concentration_passed{false};
  ResearchReturnStats oos_stats{};
  double adjusted_p_value{1.0};
};

struct ResearchPromotionGateResult {
  ResearchPromotionGateCode code{ResearchPromotionGateCode::SourceLineage};
  bool passed{false};
  double observed{0.0};
  double threshold{0.0};
  std::string detail;
};

struct ResearchPromotionDecision {
  bool promoted{false};
  std::vector<ResearchPromotionGateResult> gates;
};

// Evaluate every mandatory gate in enum order. A false boolean, missing or
// non-finite statistic, or threshold failure blocks promotion; no short-circuit
// hides later failures.
[[nodiscard]] Result<ResearchPromotionDecision>
evaluate_research_promotion(const ResearchPromotionEvidence &evidence,
                            const ResearchPromotionGateSpec &spec);

} // namespace atx::vol

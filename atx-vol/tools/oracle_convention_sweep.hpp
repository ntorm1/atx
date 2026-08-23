#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "oracle_cohort_reader.hpp"
#include "oracle_conventions.hpp"

namespace atx::vol::oracle {

// The sweep's OWN absolute floor. It deliberately does not reuse the scorecard's
// `kGreekAbsFloor` tolerance even though it currently carries the same value:
// that constant is a REPORTING tolerance, and retuning it must never silently
// move what the scale SELECTION optimised.
//
// It performs two DIFFERENT jobs, one per objective. In the REPORTED metric it
// is the classic asymmetric denominator floor, max(|oracle|, floor). In the
// SELECTION objective it is only the degenerate-case floor of
// max(|model|, |oracle|, floor), reached solely when both sides are ~0. The
// selection objective is therefore bounded and carries no smallest-scale
// gradient, so selection runs on the FULL row population and excludes nothing.
inline constexpr double kSelectionAbsFloor = 1.0e-4;

// The BOUNDED no-regression rule, as a multiplier on the baseline value: a
// symmetric metric may end up worse than baseline only while
// `candidate <= baseline * kRegressionBoundMultiplier`. Beyond that the gate
// fails closed.
//
// Why a bound at all: the convention fit is multi-objective over ELEVEN
// targets that share a single map, and no point in the closed candidate grid
// strictly dominates every other on all eleven. A strict per-metric
// `candidate <= baseline` rule therefore cannot be satisfied by any candidate
// the search can reach, so it does not express "never get worse" — it
// expresses "never pick anything", and the only ways out are hand-tuning or a
// bypass flag, which is worse than a stated bound.
//
// Why 1%: the charter's own Mode A Greek tolerance is 1% relative. A regression
// that stays inside the tolerance the scorecard already accepts as a match
// cannot move a cell's verdict; one that exceeds it can, and that is exactly
// where the gate must stop.
//
// The bound is a licence to LOSE ground, never to hide it: every permitted
// regression is published in `accepted_regressions` below, and the validators
// cross-check the two directions so an empty array cannot cover a real
// regression.
//
// Stated as the MULTIPLIER and not as `1.0 + fraction`: five layers re-evaluate
// this comparison in three languages, and `1.0 + 0.01` is not required to be
// the same double as `1.01`.
inline constexpr double kRegressionBoundMultiplier = 1.01;

struct FloorMetric {
  std::string metric_id;
  double value = 0.0;
  // Rows behind `value`: every row both arms priced.
  std::int64_t count = 0;
  // Rows the scale SELECTION ran on. The symmetric selection objective is
  // well-conditioned on every row, so nothing is excluded and this now EQUALS
  // `count`. It is kept as an auditable record rather than deleted: five gate
  // layers assert selection_count >= count/10, and any future objective that
  // excluded rows again would have to move this published number instead of
  // narrowing the selection population silently.
  std::int64_t selection_count = 0;
  std::string unit;
};

// One PUBLISHED, permitted regression on the symmetric array. Emitting the
// entry is what makes the bound honest: a metric is allowed to lose ground, but
// never quietly, so the receipt carries both values and the size of the loss.
struct AcceptedRegression {
  std::string metric_id;
  double candidate = 0.0;
  double baseline = 0.0;
  // (candidate - baseline) / baseline, a FRACTION and never a percentage: the
  // 1% bound reads as 0.01 here. All five layers carry this same convention.
  double pct_of_baseline = 0.0;
};

struct CandidatePriceMetric {
  std::string candidate_id;
  double smoke_price_mae_ticks = 0.0;
  std::int64_t smoke_count = 0;
  double tune_sample_price_mae_ticks = 0.0;
  std::int64_t tune_sample_count = 0;
};

// RUN-LEVEL AGGREGATES of the dividend-schedule pre-pass, and nothing finer:
// the reconstruction groups are (date, bucket_et, underlier) snapshots —
// cohort MEMBERSHIP — and membership must never reach an aggregate receipt or
// its stderr, so no per-group breakdown exists here by design. Rows in refused
// groups are REFUSED by the DiscreteDividendTree arm (they surface as engine
// errors), never silently priced on a different model; these counts are what
// makes that refusal auditable from the receipt alone.
struct DividendReconstructionAggregate {
  std::int64_t rows_seen = 0;
  std::int64_t groups_seen = 0;
  std::int64_t groups_refused = 0;
  std::int64_t rows_in_refused_groups = 0;
  std::int64_t refusals_non_finite_input = 0;
  std::int64_t refusals_ambiguous_ddiv_at_expiry = 0;
  std::int64_t refusals_non_monotone_ddiv = 0;
  std::int64_t refusals_non_positive_jump = 0;
};

struct ConventionSweepResult {
  ConventionMap winner;
  // DEFINITION SITE 1 of the two published floor arrays.
  //
  // `metrics`/`baseline_metrics` are the STANDARD-RELATIVE floor,
  // |m - o| / max(|o|, kSelectionAbsFloor). They exist so the published number
  // stays directly comparable to the charter's "greeks within 1% rel" target,
  // which is stated against the oracle.
  //
  // `symmetric_metrics`/`baseline_symmetric_metrics` are the SYMMETRIC-RELATIVE
  // floor, |m - o| / max(|m|, |o|, kSelectionAbsFloor) — the same loss the scale
  // SELECTION minimises. It is bounded and has no smallest-scale gradient, so it
  // is the array the no-regression gate and the ratchet baseline run on: gating
  // on the standard array would reward the smaller multiplier on near-zero-oracle
  // rows and therefore contradict the selector by construction.
  //
  // Both arrays carry the same eleven metric ids over the same row population.
  // The two absolute floors (price, vol) have no relative denominator at all, so
  // their symmetric entry is the same number as their standard entry; only the
  // nine relative Greeks differ. A future reader must NOT unify the two arrays.
  std::vector<FloorMetric> metrics;
  std::vector<FloorMetric> baseline_metrics;
  std::vector<FloorMetric> symmetric_metrics;
  std::vector<FloorMetric> baseline_symmetric_metrics;
  // Every symmetric metric that ended up WORSE than baseline while staying
  // within kRegressionBoundMultiplier of it. Empty when nothing regressed. A
  // regression BEYOND the bound is deliberately absent: the gate layers fail
  // closed on it, and publishing it here would read as an endorsement of a
  // number no layer accepts.
  std::vector<AcceptedRegression> accepted_regressions;
  std::vector<CandidatePriceMetric> candidate_prices;
  // Metric ids of the Greeks on which the SELECTED input model is still worse
  // than baseline_convention() on the tune sample, each side at its own best
  // scale. Empty when it regresses on none. When BOTH finalists regressed the
  // lexicographic rank degenerates to price MAE, and this names what that cost,
  // so the trade-off is published rather than silently absorbed.
  std::vector<std::string> input_model_regressed_greeks;
  // The schedule pre-pass, in run-level aggregate. See the struct's banner.
  DividendReconstructionAggregate dividend_reconstruction;
  std::int64_t smoke_rows = 0;
  std::int64_t tune_rows = 0;
  std::int64_t rows_priced = 0;
  std::int64_t engine_errors = 0;
  double diagnostic_wall_seconds = 0.0;
  double diagnostic_rows_per_second = 0.0;
};

// Closed Stage 3 search: all input candidates on smoke, then the best two on a
// deterministic tune sample ALONE (smoke decides only the 8-way cut, so its
// evidence is never counted twice) ranked by `less_finalist` — Greeks first,
// price second — followed by full smoke+tune baseline/winner Greek attribution.
// Every observation is committed to the candidate and the baseline accumulator
// together or to neither, so the two floors always describe one row population.
//
// Returns Err on an empty cohort, and Err NAMING the offending metric or input
// candidate when one of them admitted no observation at all: an empty
// Accumulator means infinity, `%.17g` renders that as a bare `inf`, and the
// receipt would then be JSON that does not parse — diagnosed three layers away
// from its cause.
[[nodiscard]] Result<ConventionSweepResult> run_convention_sweep(std::span<const OracleRow> smoke,
                                                                 std::span<const OracleRow> tune);

// DEFINITION SITE of the bounded regression rule, exposed so the rule is
// testable without a sweep: a metric is published iff it is strictly worse than
// its baseline AND within kRegressionBoundMultiplier of it.
//
// A zero baseline is handled by the same comparison rather than by a special
// case: `candidate <= 0 * 1.01` is only true at candidate == 0, which is not a
// regression, so a metric that moved off zero is never published and never
// divides by it.
//
// PRECONDITION: both spans carry the same metric ids in the same order.
[[nodiscard]] std::vector<AcceptedRegression>
accepted_regressions(std::span<const FloorMetric> metrics,
                     std::span<const FloorMetric> baseline);

[[nodiscard]] std::string convention_map_json(const ConventionMap &map);
[[nodiscard]] std::string convention_sweep_json(const ConventionSweepResult &result,
                                                std::string_view git_sha);

// Mean absolute (or mean relative) error over the rows it admitted. Non-finite
// observations are skipped rather than poisoning the mean.
struct Accumulator {
  double sum = 0.0;
  std::int64_t count = 0;

  void absolute(double model, double oracle) noexcept;
  // REPORTED objective: |model - oracle| / max(|oracle|, kSelectionAbsFloor).
  // This is the number the charter's "greeks within 1% rel" target is stated
  // against, so it must stay exactly this function.
  void relative(double model, double oracle) noexcept;
  // SELECTION objective:
  //   |model - oracle| / max(|model|, |oracle|, kSelectionAbsFloor).
  // Symmetric, and bounded by 1 whenever either side clears the floor. The
  // asymmetric form pins its denominator on a near-zero oracle while the
  // numerator keeps growing with |model * scale|, which is a systematic
  // gradient toward the smallest candidate scale regardless of correctness;
  // this form has none, so no row needs excluding.
  //
  // Selection and reporting are DELIBERATELY different functions: the selection
  // loss is well-conditioned, the reported metric is the target. Do not unify
  // them.
  void symmetric_relative(double model, double oracle) noexcept;
  [[nodiscard]] double mean() const noexcept;
};

// The reported floor and the selection loss over the SAME rows. They differ
// only in objective (see Accumulator), never in population.
struct FloorAccumulators {
  Accumulator report;
  Accumulator selection;
};

// One (Greek source, signed scale) hypothesis in a bounded independent search.
struct ScaleCandidate {
  GreekSource source{};
  double scale = 1.0;
  FloorAccumulators error;
};

// The production per-Greek pick: lowest mean SELECTION error (the symmetric
// objective), ties broken on the stable candidate identity (source ID, then the
// signed scale numerically). Returns the winning index; `candidates` must be
// non-empty.
[[nodiscard]] std::size_t best_scale(std::span<const ScaleCandidate> candidates) noexcept;

// One finalist's rank key for the stage-2 input-model choice. LEXICOGRAPHIC,
// never a weighted score: the nine Greek errors are dimensionless ratios and
// the price MAE is in ticks, so any weighted sum would invent an exchange rate
// between incomparable units.
struct FinalistRank {
  // True when the finalist is worse than baseline_convention() on ANY of the
  // nine Greeks, both sides evaluated on the same tune sample at their own best
  // scale.
  bool regresses_any_greek = false;
  double tune_price_mae = 0.0;
  std::string_view candidate_id;
};

// Strict weak ordering, "better first": no Greek regression, then lower
// tune-sample price MAE, then the stable candidate identity — the last compared
// FIELD BY FIELD on '|', never as one flat string, so that adding a searched
// axis cannot reorder a tie on the axes that already existed. The
// '|'-sorts-above-'_' prefix trap that forces this is written out at
// `less_candidate_identity` in oracle_convention_sweep.cpp.
[[nodiscard]] bool less_finalist(const FinalistRank &left, const FinalistRank &right) noexcept;

} // namespace atx::vol::oracle

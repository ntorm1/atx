#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/american_markov.hpp"

namespace atx::vol {

inline constexpr std::size_t kAmericanMarkovFitMaxParameters = 128;
inline constexpr std::size_t kAmericanMarkovFitMaxQuotes = 16384;
inline constexpr std::size_t kAmericanMarkovFitMaxEvaluations = 8192;
inline constexpr std::size_t kAmericanMarkovFitMaxSweeps = 1024;
inline constexpr std::size_t kAmericanMarkovFitMaxRollbackWork = 1000000000;

struct AmericanMarkovFitInterval {
  double calendar_end = 0.0;
  double variance_end = 0.0;
  double rate = 0.0;
  double continuous_yield = 0.0;
  double cash_dividend_at_end = 0.0;
};

struct AmericanMarkovFitMarket {
  double spot = 0.0;
  std::span<const AmericanMarkovFitInterval> intervals;
  std::span<const double> state_grid;
  AmericanMarkovConfig model_config;
};

struct AmericanMarkovFitQuote {
  std::size_t expiry_index = 0;
  double strike = 0.0;
  Side side = Side::Call;
  double bid = 0.0; // Zero is an upper-bound observation, never a midpoint target.
  double ask = 0.0;
  double weight = 1.0;
};

enum class AmericanMarkovFitTimeShape { Shared, PerVarianceInterval, VarianceTimeKnots };
// ObjectiveTolerance/GradientTolerance/DampingLimit/JacobianFailure are
// reported only by the opt-in LevenbergMarquardt search; the default coordinate
// search never uses them. JacobianFailure: every probe of one Jacobian column
// was refused (see jacobian_log_step), so no gradient test was attempted.
enum class AmericanMarkovFitTermination {
  StepTolerance, EvaluationLimit, SweepLimit, ObjectiveTolerance, GradientTolerance, DampingLimit,
  JacobianFailure
};

// CoordinatePattern (default) is the historical derivative-free compass search
// in log sigma: +/-step per coordinate, step halved after a sweep without any
// improvement. LevenbergMarquardt is opt-in damped Gauss-Newton on the same
// objective's residual vector, with a bounded one-sided-difference price
// Jacobian (one objective evaluation per parameter per iteration, plus one
// retry per refused probe) and box projection.
// Neither certifies a global optimum.
enum class AmericanMarkovFitSearch { CoordinatePattern, LevenbergMarquardt };

// Fixed numerical admission threshold for the interval log-sigma basis. This
// is NOT a bound on price sensitivity or a calibration-identifiability proof.
inline constexpr double kAmericanMarkovFitMinTimeBasisReciprocalCondition = 1.0e-12;

struct AmericanMarkovFitConfig {
  // Empty means the single knot {0}: one flat sigma per selected time group.
  // Otherwise finite, strictly increasing log(X/initial spot) coordinates.
  std::span<const double> log_state_knots;
  AmericanMarkovFitTimeShape time_shape = AmericanMarkovFitTimeShape::Shared;
  // VarianceTimeKnots only: >=2 finite nonnegative strictly increasing absolute
  // cumulative variance-year coordinates. Caller chooses every knot; flat tails.
  // At each ORIGINAL interval's variance midpoint, interpolate log sigmas and
  // freeze that local-vol vector for the interval. This does NOT integrate a
  // continuous sigma(t) curve; compiler substep refinement cannot remove that
  // midpoint-freezing approximation. No schedule boundaries are inserted.
  // Inside one knot cell the represented/continuous interval variance ratio is
  // x/sinh(x), x = half the interval width times the slope of log(sigma^2);
  // intervals straddling a knot can be biased in either direction.
  // Parameters: knot count times state knots, within max_parameters, however
  // many expiries. Empty is required for Shared and PerVarianceInterval.
  std::span<const double> variance_time_knots;
  // Group-major absolute sigmas (a time group is one time knot in
  // VarianceTimeKnots). Empty initial means initial_sigma everywhere;
  // empty prior means the INITIAL vector, fixed throughout optimization.
  std::span<const double> initial_sigmas;
  std::span<const double> prior_sigmas;
  double initial_sigma = 0.25;
  double min_sigma = 0.01;
  double max_sigma = 3.0;
  double price_scale_floor = 0.01;
  // Weighted mean squared distances in units max(half spread, price floor).
  double midpoint_weight = 0.0; // Applies only when bid > 0.
  // Mean squared log-sigma distance to the prior and adjacent strike/time
  // groups. These are index differences, not coordinate-scaled derivatives.
  double prior_weight = 0.0;
  double strike_roughness_weight = 0.0;
  double time_roughness_weight = 0.0;
  double initial_log_step = 0.25;
  double min_log_step = 1.0e-4;
  std::size_t max_parameters = 32;
  std::size_t max_quotes = 4096;
  std::size_t max_evaluations = 256;
  // Coordinate search: completed sweeps. LevenbergMarquardt: completed
  // Jacobian iterations (each refreshes the price Jacobian once).
  std::size_t max_sweeps = 32;
  std::size_t batch_options = 128;
  std::size_t max_batch_values = 1048576; // Per workspace buffer.
  // State-pass-option products per evaluation: N explicit/cash, 2N resolvent.
  std::size_t max_rollback_work = 50000000;

  // Opt-in, parameter-scaled evaluation budget. Zero (default) keeps exactly
  // max_evaluations. Nonzero makes the budget
  //   clamp(evaluations_per_parameter * parameters, max_evaluations,
  //         max_scaled_evaluations)
  // where max_scaled_evaluations is the explicit hard cap: required in
  // [max_evaluations, kAmericanMarkovFitMaxEvaluations] when scaling is on and
  // required to be zero when it is off. Nothing else widens implicitly.
  std::size_t evaluations_per_parameter = 0;
  std::size_t max_scaled_evaluations = 0;

  AmericanMarkovFitSearch search = AmericanMarkovFitSearch::CoordinatePattern;
  // LevenbergMarquardt only (the coordinate search ignores all four, and uses
  // initial_log_step instead). min_log_step is the step tolerance: an iteration
  // whose projected step has max |d log sigma| below it terminates.
  // Accepted step with objective decrease <= objective_tolerance * objective
  // terminates (0 disables); max |d objective / d log sigma| <=
  // gradient_tolerance terminates (0 stops only on an exactly zero gradient).
  double objective_tolerance = 0.0;
  double gradient_tolerance = 0.0;
  // One-sided difference step in log sigma: forward, or backward when the
  // forward room to max_sigma is shorter than this step (the side with more
  // room when both are). A refused probe (e.g. over max_rollback_work) is
  // retried once in the other direction within the evaluation budget; if no
  // probe of a column succeeds the search stops with JacobianFailure rather
  // than test a gradient with a missing column.
  double jacobian_log_step = 1.0e-3;
  // Marquardt damping relative to diag(J'J); grows on rejected trial steps.
  // A rejection that would exceed max_damping terminates with DampingLimit.
  // A damped system that is not numerically positive definite grows the
  // damping without spending an evaluation.
  double initial_damping = 1.0e-3;
  double max_damping = 1.0e10;
};

struct AmericanMarkovFitDiagnostics {
  AmericanMarkovFitTermination termination = AmericanMarkovFitTermination::SweepLimit;
  std::size_t parameter_count = 0;
  std::size_t time_groups = 0;
  std::size_t quote_count = 0;
  std::size_t bidless_quotes = 0;
  std::size_t quotes_inside_interval = 0;
  std::size_t evaluations = 0; // Includes refused trial models.
  std::size_t best_evaluation = 0; // One-based evaluation yielding the stored model.
  std::size_t failed_evaluations = 0;
  std::size_t accepted_steps = 0;
  std::size_t completed_sweeps = 0;
  double initial_objective = 0.0;
  double objective = 0.0;
  double interval_loss = 0.0;
  double midpoint_loss = 0.0;
  double regularization_loss = 0.0;
  double max_interval_error = 0.0; // Price units, independent of weights.
  double mean_interval_error = 0.0;
  // Coordinate: final compass step. LevenbergMarquardt: max |d log sigma| of
  // the last proposed (projected) step, or zero if none was proposed.
  double final_log_step = 0.0;
  AmericanMarkovFitSearch search = AmericanMarkovFitSearch::CoordinatePattern;
  std::size_t evaluation_budget = 0; // Effective cap after optional scaling.
  std::size_t jacobian_evaluations = 0; // LevenbergMarquardt Jacobian columns.
  std::size_t rejected_steps = 0; // LevenbergMarquardt trial steps not accepted.
  // Every admitted evaluation charges the rollback work of the quotes it
  // actually prices (checked against max_rollback_work before the kernels
  // run). Exactly unaffected quote prefixes of a Jacobian column are reused,
  // not repriced, and appear in reused_quote_prices instead. Compilation and
  // factor construction are separate bounded compiler work, not counted here.
  std::size_t rollback_work = 0;
  std::size_t priced_quotes = 0;
  std::size_t reused_quote_prices = 0;
  double final_damping = 0.0;  // LevenbergMarquardt only.
  double gradient_norm = 0.0;  // LevenbergMarquardt: last max |d objective / d log sigma|.
};

struct AmericanMarkovFitTimeWeight {
  std::size_t left_group = 0;
  std::size_t right_group = 0;
  double right_weight = 0.0; // Left weight is 1-right_weight; equal groups mean one-hot.
  double variance_time = 0.0; // Represented original-interval variance midpoint.
  bool variance_active = false; // Paused intervals contribute no basis row.
};

struct AmericanMarkovFitTimeBasisDiagnostics {
  std::size_t quoted_variance_intervals = 0;
  std::vector<std::size_t> column_support_counts;
  std::vector<double> column_l2_norms;
  double common_normalization = 0.0; // sqrt(quoted_variance_intervals).
  // Observed reciprocal infinity condition of R/common_normalization, where
  // B=QR is the ordered interval-weight design. It is NOT cond_inf(B), a price
  // Jacobian condition, or a variance-weighted economic information measure.
  double qr_reciprocal_condition_inf = 0.0;
};

class AmericanMarkovFit;

// Effective evaluation cap for a resolved parameter count under a config that
// fit_american_markov_intervals accepts: max_evaluations without scaling, else
// clamp(evaluations_per_parameter * parameters, max_evaluations,
// max_scaled_evaluations), computed without overflow.
[[nodiscard]] std::size_t american_markov_fit_evaluation_budget(
    const AmericanMarkovFitConfig &config, std::size_t parameters) noexcept;

// Offline bounded deterministic calibration; allocates while searching.
// All spans are borrowed for this call only. Invalid input, a refused initial
// model or nonfinite initial objective returns an error, never a partial fit.
// Later refused trials leave the last finite best model intact. Allocation
// failure propagates std::bad_alloc; caller state is never modified.
[[nodiscard]] Result<AmericanMarkovFit> fit_american_markov_intervals(
    const AmericanMarkovFitMarket &market, std::span<const AmericanMarkovFitQuote> quotes,
    const AmericanMarkovFitConfig &config = {});

// Immutable owned best iterate, NOT a global optimizer/convergence certificate.
// One common positive Markov process produces every strike and expiry prefix;
// American Snell rollback preserves its finite Bermudan no-arbitrage model.
// Grid/time/tail and continuous-American approximation errors are NOT certified.
// A fit may miss quote intervals or exhaust its budget: inspect diagnostics.
class AmericanMarkovFit {
public:
  AmericanMarkovFit(AmericanMarkovFit &&) noexcept = default;
  AmericanMarkovFit &operator=(AmericanMarkovFit &&) noexcept = default;
  AmericanMarkovFit(const AmericanMarkovFit &) = default;
  AmericanMarkovFit &operator=(const AmericanMarkovFit &) = default;

  [[nodiscard]] const AmericanMarkovModel &model() const noexcept { return model_; }
  [[nodiscard]] std::span<const double> sigmas() const noexcept { return sigmas_; }
  [[nodiscard]] std::span<const double> log_state_knots() const noexcept { return knots_; }
  [[nodiscard]] AmericanMarkovFitTimeShape time_shape() const noexcept { return time_shape_; }
  [[nodiscard]] std::span<const double> variance_time_knots() const noexcept { return time_knots_; }
  // One owned row per original interval, including paused variance. Legacy
  // modes use equal groups; their paused intervals retain group zero.
  [[nodiscard]] std::span<const AmericanMarkovFitTimeWeight>
  interval_time_weights() const noexcept { return time_weights_; }
  // Present only for VarianceTimeKnots. Uses every positive-variance interval
  // through the latest positive-weight quoted expiry, including unquoted
  // intermediate intervals. Regularization never rescues a refused basis.
  [[nodiscard]] const std::optional<AmericanMarkovFitTimeBasisDiagnostics> &
  time_basis_diagnostics() const noexcept { return time_basis_; }
  // Marks in the original input quote order; sorting is internal to dispatch.
  [[nodiscard]] std::span<const double> quote_prices() const noexcept { return prices_; }
  [[nodiscard]] const AmericanMarkovFitDiagnostics &diagnostics() const noexcept { return diag_; }

private:
  friend Result<AmericanMarkovFit> fit_american_markov_intervals(
      const AmericanMarkovFitMarket &, std::span<const AmericanMarkovFitQuote>,
      const AmericanMarkovFitConfig &);
  AmericanMarkovFit(AmericanMarkovModel model, std::vector<double> sigmas,
                   std::vector<double> knots, AmericanMarkovFitTimeShape time_shape,
                   std::vector<double> time_knots,
                   std::vector<AmericanMarkovFitTimeWeight> time_weights,
                   std::optional<AmericanMarkovFitTimeBasisDiagnostics> time_basis,
                   std::vector<double> prices, AmericanMarkovFitDiagnostics diagnostics);
  AmericanMarkovModel model_;
  std::vector<double> sigmas_;
  std::vector<double> knots_;
  AmericanMarkovFitTimeShape time_shape_ = AmericanMarkovFitTimeShape::Shared;
  std::vector<double> time_knots_;
  std::vector<AmericanMarkovFitTimeWeight> time_weights_;
  std::optional<AmericanMarkovFitTimeBasisDiagnostics> time_basis_;
  std::vector<double> prices_;
  AmericanMarkovFitDiagnostics diag_;
};

} // namespace atx::vol

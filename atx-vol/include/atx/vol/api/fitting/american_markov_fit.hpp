#pragma once

#include <cstddef>
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

enum class AmericanMarkovFitTimeShape { Shared, PerVarianceInterval };
enum class AmericanMarkovFitTermination { StepTolerance, EvaluationLimit, SweepLimit };

struct AmericanMarkovFitConfig {
  // Empty means the single knot {0}: one flat sigma per selected time group.
  // Otherwise finite, strictly increasing log(X/initial spot) coordinates.
  std::span<const double> log_state_knots;
  AmericanMarkovFitTimeShape time_shape = AmericanMarkovFitTimeShape::Shared;
  // Group-major absolute sigmas. Empty initial means initial_sigma everywhere;
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
  std::size_t max_sweeps = 32;
  std::size_t batch_options = 128;
  std::size_t max_batch_values = 1048576; // Per workspace buffer.
  // State-pass-option products per evaluation: N explicit/cash, 2N resolvent.
  std::size_t max_rollback_work = 50000000;
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
  double final_log_step = 0.0;
};

class AmericanMarkovFit;

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
  // Input interval -> sigma-vector time group. Stopped-variance intervals map
  // to group zero; their sigma has no effect on the transition operator.
  [[nodiscard]] std::span<const std::size_t> interval_groups() const noexcept { return groups_; }
  // Marks in the original input quote order; sorting is internal to dispatch.
  [[nodiscard]] std::span<const double> quote_prices() const noexcept { return prices_; }
  [[nodiscard]] const AmericanMarkovFitDiagnostics &diagnostics() const noexcept { return diag_; }

private:
  friend Result<AmericanMarkovFit> fit_american_markov_intervals(
      const AmericanMarkovFitMarket &, std::span<const AmericanMarkovFitQuote>,
      const AmericanMarkovFitConfig &);
  AmericanMarkovFit(AmericanMarkovModel model, std::vector<double> sigmas,
                   std::vector<double> knots, std::vector<std::size_t> groups,
                   std::vector<double> prices, AmericanMarkovFitDiagnostics diagnostics);
  AmericanMarkovModel model_;
  std::vector<double> sigmas_;
  std::vector<double> knots_;
  std::vector<std::size_t> groups_;
  std::vector<double> prices_;
  AmericanMarkovFitDiagnostics diag_;
};

} // namespace atx::vol

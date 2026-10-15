#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

// A common underlying model, rather than an independent model at each strike.
// Inputs are constant over the interval ending at these cumulative clocks.
// Calendar endpoints strictly increase; variance time may remain unchanged.
// Both clocks are linearly subdivided between endpoints. The interior variance
// increment is volatility^2 * X^2 * d(variance_time); carry and discount use
// calendar time. Supply event endpoints to represent a nonuniform clock.
// Every endpoint is an available expiry prefix of the SAME compiled model.
struct AmericanMarkovInterval {
  double calendar_end = 0.0;
  double variance_end = 0.0;
  double rate = 0.0;
  double continuous_yield = 0.0;
  double volatility = 0.0;
  double cash_dividend_at_end = 0.0;
};

inline constexpr std::size_t kAmericanMarkovMaxStates = 4097;
inline constexpr std::size_t kAmericanMarkovMaxTransitions = 65536;
inline constexpr std::size_t kAmericanMarkovMaxStoredRows = 8000000;

struct AmericanMarkovConfig {
  // Used only when the caller supplies no grid. The generated uniform grid
  // includes zero and spot exactly (spot is inserted if necessary).
  std::size_t grid_nodes = 65;
  double upper_spot_multiple = 4.0;
  double max_calendar_step = 1.0 / 52.0;
  double max_variance_step = 1.0 / 52.0;
  double cfl_fraction = 0.9;
  std::size_t max_states = 1025;
  std::size_t max_transitions = 8192;
  std::size_t max_stored_rows = 1000000;
};

struct AmericanMarkovSpec {
  double spot = 0.0;
  std::span<const AmericanMarkovInterval> intervals;
  // Strictly increasing finite nonnegative X=S/G states. Must start at zero
  // and contain spot EXACTLY. No spot interpolation or hidden extrapolation.
  std::span<const double> state_grid;
  AmericanMarkovConfig config;
};

enum class AmericanMarkovStepKind : std::uint8_t { Diffusion, CashDividend };

// Rows are probabilities, not discounted weights. Entries with zero probability
// may repeat an index. Every destination belongs to the next layer.
struct AmericanMarkovTransitionRow {
  std::array<std::uint32_t, 3> destination{};
  std::array<double, 3> probability{};
};

struct AmericanMarkovLayer {
  double calendar_time = 0.0;
  double variance_time = 0.0;
  double growth = 1.0;        // G=exp(integral(rate-continuous_yield) d calendar time)
  std::vector<double> states; // X; actual stock at a node is growth * X
};

struct AmericanMarkovTransition {
  AmericanMarkovStepKind kind = AmericanMarkovStepKind::Diffusion;
  double discount = 1.0; // exp(-integral rate d calendar time), positive even at r<0
  double calendar_step = 0.0;
  double variance_step = 0.0;
  double volatility = 0.0;
  double cash_dividend = 0.0;
  std::vector<AmericanMarkovTransitionRow> rows;
};

struct AmericanMarkovDiagnostics {
  std::size_t diffusion_steps = 0;
  std::size_t cash_steps = 0;
  std::size_t stored_rows = 0;
  std::size_t max_states = 0;
  double max_probability_sum_error = 0.0;
  double max_relative_mean_error = 0.0;
  // Unconditional probabilities under the compiled process, not exercise
  // probabilities. Useful numerical-boundary diagnostics; not error bounds.
  double terminal_zero_probability = 0.0;
  double terminal_upper_probability = 0.0;
};

class AmericanMarkovModel;
class AmericanMarkovWorkspace;

[[nodiscard]] Result<AmericanMarkovModel> compile_american_markov(const AmericanMarkovSpec &spec);
[[nodiscard]] Result<double>
price_american_markov(const AmericanMarkovModel &model, double strike, Side side,
                      std::size_t expiry_index, AmericanMarkovWorkspace &workspace,
                      ExerciseStyle exercise = ExerciseStyle::American);

// Immutable after compilation. Transition n maps layer n to layer n+1.
// Const spans expose the complete operator for future exact PWL-strike Snell
// recursion; the scalar pricer does not own a different pricing implementation.
//
// This is a FINITE BERMUDAN model: exercise is allowed at every compiled layer.
// Diffusion has absorbing zero/upper X boundaries. Cash is deterministic on
// the shifted grid, with limited-liability paid_cash=min(D,S_before). These are
// model assumptions, not claims of continuous-American or unbounded-tail accuracy.
// The zero boundary absorbs the stock. The upper X boundary still follows
// deterministic growth G between layers and shifts down when cash is paid.
//
// Cash is applied before an endpoint expiry payoff. American rollback also
// allows exercise immediately BEFORE that cash event (including at expiry),
// and immediately after it. European control pricing uses only the post-cash
// terminal payoff. Valuation-time cash must already be reflected in input spot.
class AmericanMarkovModel {
public:
  AmericanMarkovModel(AmericanMarkovModel &&) noexcept = default;
  AmericanMarkovModel &operator=(AmericanMarkovModel &&) noexcept = default;
  AmericanMarkovModel(const AmericanMarkovModel &) = default;
  AmericanMarkovModel &operator=(const AmericanMarkovModel &) = default;

  [[nodiscard]] std::span<const AmericanMarkovLayer> layers() const noexcept { return layers_; }
  [[nodiscard]] std::span<const AmericanMarkovTransition> transitions() const noexcept {
    return transitions_;
  }
  [[nodiscard]] std::span<const std::size_t> expiry_layers() const noexcept {
    return expiry_layers_;
  }
  [[nodiscard]] std::size_t initial_state_index() const noexcept { return initial_state_index_; }
  [[nodiscard]] double spot() const noexcept { return spot_; }
  [[nodiscard]] const AmericanMarkovDiagnostics &diagnostics() const noexcept {
    return diagnostics_;
  }

private:
  AmericanMarkovModel() = default;
  friend Result<AmericanMarkovModel> compile_american_markov(const AmericanMarkovSpec &);
  double spot_ = 0.0;
  std::size_t initial_state_index_ = 0;
  std::vector<AmericanMarkovLayer> layers_;
  std::vector<AmericanMarkovTransition> transitions_;
  std::vector<std::size_t> expiry_layers_;
  AmericanMarkovDiagnostics diagnostics_;
};

// One workspace per concurrent caller; reusable across models and strikes.
// prepare() is the only allocation path. A successful price call never allocates.
class AmericanMarkovWorkspace {
public:
  [[nodiscard]] Status prepare(const AmericanMarkovModel &model);
  [[nodiscard]] std::size_t size() const noexcept { return first_.size(); }

private:
  friend Result<double> price_american_markov(const AmericanMarkovModel &, double, Side,
                                              std::size_t, AmericanMarkovWorkspace &,
                                              ExerciseStyle);
  std::vector<double> first_;
  std::vector<double> second_;
};

} // namespace atx::vol

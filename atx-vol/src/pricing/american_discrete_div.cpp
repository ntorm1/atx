#include "atx/vol/api/pricing/american.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

// Vellekoop-Nieuwenhuis (2006) spliced Cox-Ross-Rubinstein lattice for American
// (and European) options on an underlier paying DISCRETE CASH dividends. The
// contract, the measured evidence for it, and the three load-bearing details are
// documented at the declarations in api/pricing/american.hpp; this file states
// each one again at the line that implements it.
//
// Everything here is a pure function of its arguments — the only state is the
// per-call lattice buffers — so concurrent calls from any threads are safe.
//
// SCOPE: price, delta and gamma only. The seven remaining greeks of the
// `AmericanGreeks` bundle are deliberately absent (see the header's SCOPE note).

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// A `tau` this far past `T` (relatively) is still treated as landing AT expiry.
// A dividend ex-date reconstructed from the same year-fraction column as `T`
// must not be dropped by one ulp of that column.
inline constexpr double kTauAtExpiryRelTol = 1.0e-12;

// Turns "stock level minus strike" into the exercise payoff for either side.
[[nodiscard]] constexpr double payoff_sign(Side side) noexcept {
  return (side == Side::Call) ? 1.0 : -1.0;
}

[[nodiscard]] inline double exercise_value(double sgn, double level, double strike) noexcept {
  return std::max(sgn * (level - strike), 0.0);
}

// Price + the two lattice greeks; `greeks_valid` is false when `steps < 2`.
struct LatticeOutcome {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
};

// Boundary validation (agent profile §4): every external input is checked ONCE,
// here, and the kernel below assumes the validated invariants.
[[nodiscard]] Status validate(double S, double K, double T, double sigma, double r, double q,
                              std::span<const CashDividend> dividends, int steps) {
  if (!(S > 0.0) || !std::isfinite(S)) {
    return Err(ErrorCode::InvalidArgument, "spot must be finite and > 0");
  }
  if (!(K > 0.0) || !std::isfinite(K)) {
    return Err(ErrorCode::InvalidArgument, "strike must be finite and > 0");
  }
  if (!(T > 0.0) || !std::isfinite(T)) {
    return Err(ErrorCode::InvalidArgument, "year-fraction must be finite and > 0");
  }
  if (!(sigma > 0.0) || !std::isfinite(sigma)) {
    return Err(ErrorCode::InvalidArgument, "sigma must be finite and > 0");
  }
  if (!std::isfinite(r) || !std::isfinite(q)) {
    return Err(ErrorCode::InvalidArgument, "rate and yield must be finite");
  }
  if (steps < 1 || steps > kDiscreteDivMaxSteps) {
    return Err(ErrorCode::InvalidArgument,
               "steps must lie in [1, " + std::to_string(kDiscreteDivMaxSteps) + "]");
  }
  for (const CashDividend &dv : dividends) {
    if (!std::isfinite(dv.tau) || !std::isfinite(dv.amount)) {
      return Err(ErrorCode::InvalidArgument, "dividend tau and amount must be finite");
    }
    // Out-of-window events are IGNORED (a chain-wide schedule legitimately
    // carries them); a NEGATIVE amount is not an event outside the window, it is
    // a malformed one, so it fails closed rather than being silently skipped.
    if (dv.amount < 0.0) {
      return Err(ErrorCode::InvalidArgument, "dividend amount must be >= 0");
    }
  }
  return Ok();
}

// Detail 1 of 3 (header): at an ex-step BELOW expiry the continuation value is
// re-read on the shifted grid by LINEAR interpolation, clamped at both grid
// edges, and the American exercise test is then applied against the POST-
// dividend level ONLY — admitting cum-dividend exercise here was tried and made
// agreement with the vendor mark strictly worse.
//
// Post-dividend levels are the same monotone sequence shifted down, so one
// forward sweep of the bracket index `j` serves every node: a two-pointer merge,
// O(nodes), not a binary search per node.
//
// Detail 3 of 3: the post-dividend level is FLOORED at zero. A cash amount
// exceeding the stock level at a low node would otherwise price a negative
// stock, which for a put reads back as a payoff above the strike.
void splice_dividend(std::span<double> level, std::span<double> value, std::span<double> scratch,
                     std::size_t last, double amount, double sgn, double strike,
                     bool american) noexcept {
  const double lo = level[0];
  const double hi = level[last];
  std::size_t j = 0;
  for (std::size_t i = 0; i <= last; ++i) {
    const double post = std::max(level[i] - amount, 0.0);
    double cont = 0.0;
    if (post <= lo) {
      cont = value[0];
    } else if (post >= hi) {
      cont = value[last];
    } else {
      while (j + 1U <= last && level[j + 1U] <= post) {
        ++j;
      }
      const double slope = (value[j + 1U] - value[j]) / (level[j + 1U] - level[j]);
      cont = slope * (post - level[j]) + value[j];
    }
    if (american) {
      cont = std::max(cont, exercise_value(sgn, post, strike));
    }
    scratch[i] = cont;
  }
  // Written through a scratch buffer, not in place: every node reads the WHOLE
  // pre-splice value curve, so an in-place write would feed already-shifted
  // values back into a later node's interpolation.
  for (std::size_t i = 0; i <= last; ++i) {
    value[i] = scratch[i];
  }
}

[[nodiscard]] Result<LatticeOutcome> run_lattice(double S, double K, double T, double sigma,
                                                 double r, double q, Side side,
                                                 std::span<const CashDividend> dividends, int steps,
                                                 ExerciseStyle exercise) {
  const int n_steps = steps;
  const std::size_t n_nodes = static_cast<std::size_t>(n_steps) + 1U;
  const bool american = (exercise == ExerciseStyle::American);
  const double sgn = payoff_sign(side);

  const double dt = T / static_cast<double>(n_steps);
  const double sqrt_dt = std::sqrt(dt);
  const double u = std::exp(sigma * sqrt_dt);
  const double d = 1.0 / u;
  const double disc = std::exp(-r * dt);
  const double p = (std::exp((r - q) * dt) - d) / (u - d);
  if (!(p > 0.0) || !(p < 1.0)) {
    return Err(ErrorCode::OutOfRange,
               "CRR risk-neutral probability left (0, 1); the step is too coarse for this "
               "(r - q, sigma, T) — raise `steps`");
  }
  const double pu = disc * p;
  const double pd = disc * (1.0 - p);

  // Dividend step map: total cash landing on each lattice step. Built by
  // accumulation, so an UNSORTED span and duplicate ex-dates need no
  // pre-sort — two events rounding to the same step simply add.
  std::vector<double> step_amount(n_nodes, 0.0);
  bool any_dividend = false;
  const double tau_at_expiry = T * (1.0 + kTauAtExpiryRelTol);
  for (const CashDividend &dv : dividends) {
    if (!(dv.tau > 0.0) || dv.tau > tau_at_expiry || !(dv.amount > 0.0)) {
      continue; // outside (0, T], or a zero amount: a no-op, not an error
    }
    // Round-half-to-even, then clamp into [1, n_steps]: an ex-date inside the
    // window always lands on a real rollback step.
    const double nearest = std::nearbyint(dv.tau / dt);
    const double clamped = std::clamp(nearest, 1.0, static_cast<double>(n_steps));
    step_amount[static_cast<std::size_t>(clamped)] += dv.amount;
    any_dividend = true;
  }

  // Terminal levels S_N[i] = S*exp(sigma*sqrt(dt)*(2i - N)), ascending in i.
  std::vector<double> level(n_nodes);
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const int rung = 2 * static_cast<int>(i) - n_steps;
    level[i] = S * std::exp(sigma * sqrt_dt * static_cast<double>(rung));
  }

  // Detail 2 of 3 (header): a dividend on the TERMINAL step is applied to the
  // analytic payoff, never interpolated. Linear interpolation across the payoff
  // KINK is the one place this scheme carries avoidable O(grid-spacing) error,
  // and it is exactly the case that matters here — SPY's ex-dates ARE expiry
  // dates. Applying the payoff directly is what makes the equivalent-strike
  // identity hold BIT-EXACTLY.
  std::vector<double> value(n_nodes);
  const double terminal_amount = step_amount[static_cast<std::size_t>(n_steps)];
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const double post = std::max(level[i] - terminal_amount, 0.0);
    value[i] = exercise_value(sgn, post, K);
  }

  std::vector<double> scratch;
  if (any_dividend) {
    scratch.resize(n_nodes);
  }

  // Step-1 and step-2 lattice state, captured on the way past for the greeks.
  double level_1[2] = {0.0, 0.0};
  double value_1[2] = {0.0, 0.0};
  double level_2[3] = {0.0, 0.0, 0.0};
  double value_2[3] = {0.0, 0.0, 0.0};
  // The rollback below visits steps N-1 .. 0, so at N == 2 step 2 IS the
  // terminal state and the loop never sees it. Capturing it here is what keeps
  // gamma a number instead of 0/0 at the smallest lattice that can carry one.
  if (n_steps == 2) {
    for (std::size_t i = 0; i < 3U; ++i) {
      level_2[i] = level[i];
      value_2[i] = value[i];
    }
  }

  for (int k = n_steps - 1; k >= 0; --k) {
    const std::size_t last = static_cast<std::size_t>(k);
    // Only nodes 0..k survive to step k, and each has now been scaled by u
    // exactly (N - k) times — which is what S*u^(2i-k) requires.
    for (std::size_t i = 0; i <= last; ++i) {
      level[i] *= u;
    }
    for (std::size_t i = 0; i <= last; ++i) {
      value[i] = pu * value[i + 1U] + pd * value[i];
    }

    const double amount = step_amount[last];
    if (amount > 0.0) {
      splice_dividend(level, value, scratch, last, amount, sgn, K, american);
    } else if (american) {
      for (std::size_t i = 0; i <= last; ++i) {
        value[i] = std::max(value[i], exercise_value(sgn, level[i], K));
      }
    }

    if (k == 2) {
      for (std::size_t i = 0; i < 3U; ++i) {
        level_2[i] = level[i];
        value_2[i] = value[i];
      }
    } else if (k == 1) {
      for (std::size_t i = 0; i < 2U; ++i) {
        level_1[i] = level[i];
        value_1[i] = value[i];
      }
    }
  }

  LatticeOutcome out;
  out.price = value[0];
  if (n_steps >= 2) {
    out.delta = (value_1[1] - value_1[0]) / (level_1[1] - level_1[0]);
    const double slope_up = (value_2[2] - value_2[1]) / (level_2[2] - level_2[1]);
    const double slope_dn = (value_2[1] - value_2[0]) / (level_2[1] - level_2[0]);
    out.gamma = (slope_up - slope_dn) / (0.5 * (level_2[2] - level_2[0]));
  }
  return Ok(out);
}

} // namespace

Result<double> american_discrete_div_price(double S, double K, double T, double sigma, double r,
                                           double q, Side side,
                                           std::span<const CashDividend> dividends, int steps,
                                           ExerciseStyle exercise) {
  const Status ok = validate(S, K, T, sigma, r, q, dividends, steps);
  if (!ok) {
    return Err(ok.error());
  }
  const Result<LatticeOutcome> out =
      run_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise);
  if (!out) {
    return Err(out.error());
  }
  return Ok(out->price);
}

Result<DiscreteDivGreeks> american_discrete_div_greeks(double S, double K, double T, double sigma,
                                                       double r, double q, Side side,
                                                       std::span<const CashDividend> dividends,
                                                       int steps, ExerciseStyle exercise) {
  const Status ok = validate(S, K, T, sigma, r, q, dividends, steps);
  if (!ok) {
    return Err(ok.error());
  }
  // Gamma is a central second difference over step 2's three nodes; with one
  // step there is no step 2, so this fails closed rather than reporting a
  // fabricated 0.
  if (steps < 2) {
    return Err(ErrorCode::InvalidArgument, "lattice greeks need steps >= 2");
  }
  const Result<LatticeOutcome> out =
      run_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise);
  if (!out) {
    return Err(out.error());
  }
  DiscreteDivGreeks greeks;
  greeks.price = out->price;
  greeks.delta = out->delta;
  greeks.gamma = out->gamma;
  return Ok(greeks);
}

} // namespace atx::vol

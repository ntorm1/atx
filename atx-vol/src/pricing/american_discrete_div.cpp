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
// Three tiers share ONE kernel (`run_lattice`): the price, the price plus the
// two greeks the rollback already carries, and the nine-greek oracle bundle at
// eight rollbacks. The bundle's solve accounting lives at its declaration in the
// header; `run_lattice` is what makes five of those nine free.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Turns "stock level minus strike" into the exercise payoff for either side.
[[nodiscard]] constexpr double payoff_sign(Side side) noexcept {
  return (side == Side::Call) ? 1.0 : -1.0;
}

[[nodiscard]] inline double exercise_value(double sgn, double level, double strike) noexcept {
  return std::max(sgn * (level - strike), 0.0);
}

// Price + every greek ONE rollback can carry. `delta`/`gamma`/`theta` need
// `steps >= 2` and `charm` needs `steps >= 3`; below those counts the field
// stays 0 and the public entry that would expose it refuses first, so a
// fabricated 0 never reaches a caller.
struct LatticeOutcome {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double theta = 0.0; // dP/dt, calendar convention, per year
  double charm = 0.0; // d(delta)/dt, calendar convention
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
// re-read on the shifted grid by LINEAR interpolation. Above the top node it is
// clamped; BELOW the bottom node it is EXTRAPOLATED — quadratically through the
// three lowest nodes, linearly when only two survive — because
// `post = level[i] - amount` sits under `level[0]` for the low nodes at EVERY
// splice with a positive amount, so the bottom edge is the common case here and
// not a corner. A flat clamp there says the option stops responding to spot
// below the grid; an American put's exercise floor hides that, a EUROPEAN put
// does not. Measured: the ruinous-dividend European put moved 96.146830 ->
// 97.044553, the latter being K*exp(-r*T) exactly, and the flat clamp's deficit
// was level[0]*exp(-r*tau_grid) to the last digit.
//
// THREE properties of this branch are load-bearing and each is measured at the
// header's declaration:
//   * NOTHING is clamped in it. The splice is one LINEAR operator on the value
//     vector and cont_call - cont_put is affine in the stock level, so a linear
//     extrapolation reproduces put-call parity exactly and hands both legs the
//     same absolute error. A per-node max(., 0) is not linear; it desynchronises
//     the legs and is the ONLY thing that can break parity here.
//   * Consequently PUT-CALL PARITY CANNOT TEST THIS BRANCH. An absolute
//     reference must (tests/discrete_div_american_test.cpp).
//   * Quadratic, because option value is convex in spot: a linear extension from
//     the lowest segment is a rigorous lower bound and undershoots one-sidedly.
//     The divided-difference term is >= 0 on convex data, so it only adds to
//     that bound.
//
// Post-dividend levels are the same monotone sequence shifted down, so one
// forward sweep of the bracket index `j` serves every node: a two-pointer merge,
// O(nodes), not a binary search per node.
//
// Detail 2 of 3: the American exercise test is applied against BOTH stock
// levels. The holder standing at this step before the cash comes off may
// exercise at the CUM-dividend level `level[i]`, or wait an instant and exercise
// at the POST-dividend level `post`; the option is worth the better of the two
// against continuation. Taking only `post` understates a CALL's exercise value
// by exactly `amount` wherever exercise binds, which is where American call
// exercise happens at all (Roll/Geske/Whaley; Itkin arXiv:2510.18159 §6.4). It
// is a no-op on a PUT, whose cum-dividend exercise is dominated by the same
// exercise one instant later.
//
// Detail 3 of 3: the post-dividend level is FLOORED at zero. A cash amount
// exceeding the stock level at a low node would otherwise price a negative
// stock, which for a put reads back as a payoff above the strike.
void splice_dividend(std::span<double> level, std::span<double> value, std::span<double> scratch,
                     std::size_t last, double amount, double sgn, double strike,
                     bool american) noexcept {
  const double lo = level[0];
  const double hi = level[last];
  // The lowest segment's slope, held for the extrapolation below `lo`. At a
  // single surviving node there is no segment and the flat clamp is all that
  // exists; that step is the root, where `post <= lo` cannot bind on a node the
  // caller reads (`value[0]` is the answer either way).
  // Newton divided differences over the three lowest surviving nodes. At two
  // nodes the second difference does not exist and `lo_curv` stays 0, which
  // degrades the expression below to the linear extension; at one node there is
  // no segment either and it degrades to the flat value, which is the whole
  // answer at that step anyway (`value[0]` is what the caller reads).
  const double lo_slope = (last >= 1U) ? ((value[1] - value[0]) / (level[1] - level[0])) : 0.0;
  double lo_curv = 0.0;
  double lo_next = lo;
  if (last >= 2U) {
    const double d12 = (value[2] - value[1]) / (level[2] - level[1]);
    lo_curv = (d12 - lo_slope) / (level[2] - level[0]);
    lo_next = level[1];
  }
  std::size_t j = 0;
  for (std::size_t i = 0; i <= last; ++i) {
    const double post = std::max(level[i] - amount, 0.0);
    double cont = 0.0;
    if (post <= lo) {
      // No max(., 0) here, deliberately: see the note above. Both factors of the
      // quadratic term are negative below `lo`, so on convex data the term is
      // >= 0 and the result never falls below the linear lower bound.
      cont = value[0] + lo_slope * (post - lo) + lo_curv * (post - lo) * (post - lo_next);
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
      cont = std::max({cont, exercise_value(sgn, post, strike),
                       exercise_value(sgn, level[i], strike)});
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
  //
  // The same cum-dividend right the splice admits applies here: an AMERICAN
  // holder facing an ex-date on the terminal step may exercise the instant
  // before it against the un-shifted level. Without this the terminal ex-date
  // is the one splice the step below cannot rescue — there is no later step to
  // carry the exercise — and an American call in front of a large terminal
  // dividend is mispriced by the whole amount rather than by O(dt).
  std::vector<double> value(n_nodes);
  const double terminal_amount = step_amount[static_cast<std::size_t>(n_steps)];
  const bool terminal_cum_exercise = american && (terminal_amount > 0.0);
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const double post = std::max(level[i] - terminal_amount, 0.0);
    value[i] = exercise_value(sgn, post, K);
    if (terminal_cum_exercise) {
      value[i] = std::max(value[i], exercise_value(sgn, level[i], K));
    }
  }

  std::vector<double> scratch;
  if (any_dividend) {
    scratch.resize(n_nodes);
  }

  // Step-1, step-2 and step-3 lattice state, captured on the way past for the
  // greeks. Step 3 is what makes charm free: because u*d == 1 its inner pair
  // (i = 1, 2) sits at exactly the same two stock levels as step 1's pair
  // (S*d and S*u), so the two deltas differ ONLY in remaining time and their
  // difference is a clean d(delta)/dt with no second spot stencil.
  double level_1[2] = {0.0, 0.0};
  double value_1[2] = {0.0, 0.0};
  double level_2[3] = {0.0, 0.0, 0.0};
  double value_2[3] = {0.0, 0.0, 0.0};
  double level_3[4] = {0.0, 0.0, 0.0, 0.0};
  double value_3[4] = {0.0, 0.0, 0.0, 0.0};
  // The rollback below visits steps N-1 .. 0, so at N == 2 step 2 (and at
  // N == 3 step 3) IS the terminal state and the loop never sees it. Capturing
  // it here is what keeps gamma and charm numbers instead of 0/0 at the
  // smallest lattice that can carry each.
  if (n_steps == 2) {
    for (std::size_t i = 0; i < 3U; ++i) {
      level_2[i] = level[i];
      value_2[i] = value[i];
    }
  } else if (n_steps == 3) {
    for (std::size_t i = 0; i < 4U; ++i) {
      level_3[i] = level[i];
      value_3[i] = value[i];
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

    if (k == 3) {
      for (std::size_t i = 0; i < 4U; ++i) {
        level_3[i] = level[i];
        value_3[i] = value[i];
      }
    } else if (k == 2) {
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
    // theta = dP/dt at fixed spot, calendar convention (decay is negative).
    // level_2[1] == S to within rounding — u*d == 1, so the middle step-2 node
    // is the ROOT's own stock level two steps later — which is what makes this
    // a pure time derivative and not a mixed spot/time one. It estimates theta
    // at t = dt rather than t = 0, an O(dt) offset shared with every
    // two-step-forward tree theta; the ripple that dominates a BUMPED greek
    // largely cancels here because both nodes come from the SAME lattice.
    out.theta = (value_2[1] - out.price) / (2.0 * dt);
  }
  if (n_steps >= 3) {
    // charm = d(delta)/dt = -d^2P/dS dT. Step 3's inner pair spans the same two
    // levels as step 1's, so this differences two deltas of the same spot
    // stencil two steps apart; each keeps its own denominator rather than a
    // shared S*u - S*d so the expression stays honest about rounding.
    const double delta_3 = (value_3[2] - value_3[1]) / (level_3[2] - level_3[1]);
    out.charm = (delta_3 - out.delta) / (2.0 * dt);
  }
  return Ok(out);
}

// Solve 8 of 8 — the bumped leg of the SpiderRock secant theta, P(T - horizon).
//
// The schedule is passed UNCHANGED and `run_lattice` re-applies its own
// (0, T'] window, so an ex-date past the bumped expiry is DROPPED, never clipped
// onto the new terminal step: the option no longer lives to receive that cash,
// and moving it inside the window would price a flow that does not exist.
//
// At `T - horizon <= 0` there is NO eighth solve. SpiderRock's companion rule
// (American -> European with rate = sdiv = carry = 0) taken to its limit at zero
// remaining time IS the intrinsic payoff, so the intrinsic is the leg — not an
// epsilon-maturity lattice, which would re-derive the same number at O(N^2) cost
// and with grid error on top. `exercise_value` resolves the at-the-money kink to
// the OUT-of-the-money side: at S == K it is 0. The whole schedule falls outside
// the empty window (0, T'] there, which is the same drop rule taken to its own
// limit rather than a second convention.
[[nodiscard]] Result<double> secant_bumped_leg(double S, double K, double T, double sigma, double r,
                                               double q, Side side,
                                               std::span<const CashDividend> dividends, int steps,
                                               ExerciseStyle exercise, double horizon) {
  const double bumped_T = T - horizon;
  if (!(bumped_T > 0.0)) {
    return Ok(exercise_value(payoff_sign(side), S, K));
  }
  const Result<LatticeOutcome> leg =
      run_lattice(S, K, bumped_T, sigma, r, q, side, dividends, steps, exercise);
  if (!leg) {
    return Err(leg.error());
  }
  return Ok(leg->price);
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

Result<DiscreteDivGreekBundle> american_discrete_div_greek_bundle(
    double S, double K, double T, double sigma, double r, double q, Side side,
    std::span<const CashDividend> dividends, int steps, ExerciseStyle exercise,
    double theta_secant_horizon) {
  const Status ok = validate(S, K, T, sigma, r, q, dividends, steps);
  if (!ok) {
    return Err(ok.error());
  }
  // Charm differences the step-1 and step-3 deltas; with two steps there is no
  // step 3, so this fails closed rather than reporting a fabricated 0.
  if (steps < 3) {
    return Err(ErrorCode::InvalidArgument, "the greek bundle needs steps >= 3");
  }
  if (!std::isfinite(theta_secant_horizon) || !(theta_secant_horizon > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "theta secant horizon must be finite and > 0");
  }

  // The seven (sigma, r, q) states, tabulated so the solve COUNT is readable
  // rather than inferred from a wall of named locals. Row 0 is the unbumped
  // base and carries five of the nine greeks on its own.
  struct BumpState {
    double dsigma;
    double dr;
    double dq;
  };
  const double hv = discrete_div_sigma_bump(sigma);
  constexpr double hr = kDiscreteDivRateBump;
  constexpr double hq = kDiscreteDivYieldBump;
  constexpr std::size_t kStates = 7U;
  const BumpState bumps[kStates] = {{0.0, 0.0, 0.0},  {+hv, 0.0, 0.0}, {-hv, 0.0, 0.0},
                                    {0.0, +hr, 0.0},  {0.0, -hr, 0.0}, {0.0, 0.0, +hq},
                                    {0.0, 0.0, -hq}};
  LatticeOutcome state[kStates];
  for (std::size_t i = 0; i < kStates; ++i) {
    const Result<LatticeOutcome> res =
        run_lattice(S, K, T, sigma + bumps[i].dsigma, r + bumps[i].dr, q + bumps[i].dq, side,
                    dividends, steps, exercise);
    if (!res) {
      return Err(res.error());
    }
    state[i] = *res;
  }
  const LatticeOutcome &base = state[0];
  const LatticeOutcome &vol_up = state[1];
  const LatticeOutcome &vol_dn = state[2];
  const LatticeOutcome &rate_up = state[3];
  const LatticeOutcome &rate_dn = state[4];
  const LatticeOutcome &yield_up = state[5];
  const LatticeOutcome &yield_dn = state[6];

  const Result<double> bumped = secant_bumped_leg(S, K, T, sigma, r, q, side, dividends, steps,
                                                  exercise, theta_secant_horizon);
  if (!bumped) {
    return Err(bumped.error());
  }

  DiscreteDivGreekBundle out;
  out.price = base.price;
  out.delta = base.delta;
  out.gamma = base.gamma;
  out.theta = base.theta;
  out.charm = base.charm;
  out.vega = (vol_up.price - vol_dn.price) / (2.0 * hv);
  out.volga = (vol_up.price - 2.0 * base.price + vol_dn.price) / (hv * hv);
  // vanna IS d(delta)/d(sigma): the two sigma-bumped rollbacks already carry
  // their own lattice delta, so no spot x vol cross stencil is built.
  out.vanna = (vol_up.delta - vol_dn.delta) / (2.0 * hv);
  out.rho = (rate_up.price - rate_dn.price) / (2.0 * hr);
  out.phi = (yield_up.price - yield_dn.price) / (2.0 * hq);
  // A ONE-PERIOD dollar amount, positive as decay. It is NOT divided by a day
  // count: `theta_secant_horizon` is already the day, and dividing again is the
  // double-scaling trap the header names.
  out.theta_secant = base.price - *bumped;
  return Ok(out);
}

// Per-event dP/dD_i. The declaration states WHY this cannot be composed from a
// carry sensitivity and a forward Jacobian; this is the arithmetic.
Status american_discrete_div_dividend_sensitivities(double S, double K, double T, double sigma,
                                                    double r, double q, Side side,
                                                    std::span<const CashDividend> dividends,
                                                    std::span<double> dP_dDiv_out, int steps,
                                                    ExerciseStyle exercise) {
  const Status ok = validate(S, K, T, sigma, r, q, dividends, steps);
  if (!ok) {
    return Err(ok.error());
  }
  if (dP_dDiv_out.size() != dividends.size()) {
    return Err(ErrorCode::InvalidArgument,
               "dP_dDiv_out must carry exactly one slot per dividend event");
  }
  if (dividends.empty()) {
    return Ok();
  }

  // One mutable copy of the schedule, re-pointed per event: the bumped legs
  // differ from the base in a single amount, so nothing else is rebuilt and the
  // loop allocates nothing.
  std::vector<CashDividend> bumped(dividends.begin(), dividends.end());
  const double tau_at_expiry = T * (1.0 + kTauAtExpiryRelTol);
  for (std::size_t i = 0; i < dividends.size(); ++i) {
    const CashDividend &dv = dividends[i];
    // The SAME window `run_lattice` applies. An event the price never sees
    // cannot move it, so its sensitivity is exactly 0 rather than lattice noise.
    if (!(dv.tau > 0.0) || dv.tau > tau_at_expiry) {
      dP_dDiv_out[i] = 0.0;
      continue;
    }
    const double h = discrete_div_amount_bump(dv.amount);
    // A negative cash amount is not a dividend, so the down leg is clamped at 0
    // and the difference silently becomes one-sided over the TRUE interval —
    // hence the explicit denominator rather than a hard-coded 2h.
    const double lo_amount = std::max(dv.amount - h, 0.0);
    const double hi_amount = dv.amount + h;
    const double span = hi_amount - lo_amount;

    bumped[i].amount = hi_amount;
    const Result<LatticeOutcome> up =
        run_lattice(S, K, T, sigma, r, q, side, bumped, steps, exercise);
    if (!up) {
      return Err(up.error());
    }
    bumped[i].amount = lo_amount;
    const Result<LatticeOutcome> dn =
        run_lattice(S, K, T, sigma, r, q, side, bumped, steps, exercise);
    if (!dn) {
      return Err(dn.error());
    }
    bumped[i].amount = dv.amount;
    dP_dDiv_out[i] = (up->price - dn->price) / span;
  }
  return Ok();
}

} // namespace atx::vol

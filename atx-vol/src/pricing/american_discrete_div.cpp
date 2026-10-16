#include "atx/vol/api/pricing/american.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/pricing/black76.hpp"

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
// fabricated 0 never reaches a caller. Under the BBS final step only `price` is
// meaningful (no public entry exposes smoothed greeks).
struct LatticeOutcome {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double theta = 0.0; // dP/dt, calendar convention, per year
  double charm = 0.0; // d(delta)/dt, calendar convention
};

// The per-call lattice workspace: the dividend step map, the node levels, the
// node values and the splice scratch. A public entry owns ONE of these and every
// rollback it runs reuses it — the greek bundle's eight rollbacks and BBSR's
// coarse rollback included — so a call allocates these buffers once, sized by
// its first (largest) rollback. `scratch` is grown only when a dividend lands
// inside the window, exactly as before the workspace existed.
struct LatticeBuffers {
  std::vector<double> step_amount;
  std::vector<double> level;
  std::vector<double> value;
  std::vector<double> scratch;
};

// The analytic continuation over the LAST lattice step (Broadie-Detemple BBS):
// the European value at t_{N-1} of the expiry payoff, as a function of the
// stock level `s` at t_{N-1}. Cash on the terminal step is applied exactly:
//   call  (max(S - D, 0) - K)+ == (S - (K + D))+            -> call at K + D
//   put   (K - max(S - D, 0))+ == (K + D - S)+ - (D - S)+   -> put(K + D) - put(D)
// and an AMERICAN call may exercise cum-dividend at expiry, so its terminal
// payoff is max((S-D-K)+, (S-K)+) == (S - K)+: the cash cannot move it. A put's
// cum-dividend exercise is dominated, so the American and European puts agree.
struct FinalStepValue {
  double strike = 0.0;
  double terminal_amount = 0.0; // cash on the expiry step, >= 0
  double dt = 0.0;
  double sigma = 0.0;
  double growth = 1.0; // exp((r - q) * dt)
  double df = 1.0;     // exp(-r * dt)
  Side side = Side::Call;
  bool american = true;

  [[nodiscard]] double operator()(double s) const noexcept {
    if (!(s > 0.0)) {
      // A stock at zero stays at zero: the call is worthless, the put pays K.
      return (side == Side::Call) ? 0.0 : df * strike;
    }
    const double forward = s * growth;
    if (!(terminal_amount > 0.0)) {
      return black76_price(forward, strike, dt, sigma, df, side);
    }
    if (side == Side::Call) {
      const double call_strike = american ? strike : strike + terminal_amount;
      return black76_price(forward, call_strike, dt, sigma, df, Side::Call);
    }
    return black76_price(forward, strike + terminal_amount, dt, sigma, df, Side::Put) -
           black76_price(forward, terminal_amount, dt, sigma, df, Side::Put);
  }
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

// BBS step N-1: the analytic continuation replaces the rollback of the kinked
// payoff. Cash landing on THIS step is applied at each node's own post-dividend
// level — the continuation is known in closed form at any level, so this splice
// interpolates nothing and has no grid bottom — with the same cum- and
// post-dividend exercise test `splice_dividend` applies. With no cash here
// `post == level[i]` and the test is the plain American max.
void bbs_final_step(std::span<const double> level, std::span<double> value, std::size_t last,
                    double amount, const FinalStepValue &final_step, double sgn, double strike,
                    bool american) noexcept {
  for (std::size_t i = 0; i <= last; ++i) {
    const double post = std::max(level[i] - amount, 0.0);
    double cont = final_step(post);
    if (american) {
      cont = std::max({cont, exercise_value(sgn, post, strike),
                       exercise_value(sgn, level[i], strike)});
    }
    value[i] = cont;
  }
}

// `bbs` selects the Broadie-Detemple final step (header, DiscreteDivSmoothing):
// the step N-1 continuation becomes `FinalStepValue` evaluated at each node's
// own (post-dividend) level. Every other step is the plain V&N rollback, and
// with `bbs == false` the arithmetic is the historical kernel's, operation for
// operation.
[[nodiscard]] Result<LatticeOutcome> run_lattice(double S, double K, double T, double sigma,
                                                 double r, double q, Side side,
                                                 std::span<const CashDividend> dividends, int steps,
                                                 ExerciseStyle exercise, bool bbs,
                                                 LatticeBuffers &buffers) {
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
  buffers.step_amount.assign(n_nodes, 0.0);
  const std::span<double> step_amount(buffers.step_amount.data(), n_nodes);
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
  buffers.level.resize(n_nodes);
  const std::span<double> level(buffers.level.data(), n_nodes);
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
  buffers.value.resize(n_nodes);
  const std::span<double> value(buffers.value.data(), n_nodes);
  const double terminal_amount = step_amount[static_cast<std::size_t>(n_steps)];
  const bool terminal_cum_exercise = american && (terminal_amount > 0.0);
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const double post = std::max(level[i] - terminal_amount, 0.0);
    value[i] = exercise_value(sgn, post, K);
    if (terminal_cum_exercise) {
      value[i] = std::max(value[i], exercise_value(sgn, level[i], K));
    }
  }

  std::span<double> scratch;
  if (any_dividend) {
    if (buffers.scratch.size() < n_nodes) {
      buffers.scratch.resize(n_nodes);
    }
    scratch = std::span<double>(buffers.scratch.data(), n_nodes);
  }

  // The BBS step N-1 continuation (unused when `bbs` is false).
  const FinalStepValue final_step{K,      terminal_amount, dt,   sigma, std::exp((r - q) * dt),
                                  disc,   side,            american};

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
    const double amount = step_amount[last];
    if (bbs && k == n_steps - 1) {
      bbs_final_step(level, value, last, amount, final_step, sgn, K, american);
    } else {
      for (std::size_t i = 0; i <= last; ++i) {
        value[i] = pu * value[i + 1U] + pd * value[i];
      }
      if (amount > 0.0) {
        splice_dividend(level, value, scratch, last, amount, sgn, K, american);
      } else if (american) {
        for (std::size_t i = 0; i <= last; ++i) {
          value[i] = std::max(value[i], exercise_value(sgn, level[i], K));
        }
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

// ── The escrowed-dividend lattices (header, EscrowedBbs / EscrowedLeisenReimer)
//
// The lattice carries the escrowed stock X = S - (cash still to come,
// discounted at the carry r - q), lognormal with vol `sigma` and drift r - q, so
// its European converges to Black76 at the dated-cash forward. The exercise
// test at step k values the stock as X + escrow(k), where escrow(k) is the cash
// on steps after k carried back to t_k; on an ex-step the holder may also
// exercise CUM-dividend against X + escrow(k) + (this step's cash) — the same
// two-level test the V&N splice applies. Nothing is interpolated, so there is
// no grid bottom.
//
// `buffers.step_amount[k]` holds the cash landing on step k discounted to t = 0
// at r - q, so escrow(k) = exp((r - q) t_k) * (the map summed above k): one
// running sum and one exponential per step, no extra buffer.

// Fills the escrow step map (same window and round-half-to-even step rule as
// the V&N kernel) and returns the cash's present value at the carry.
double escrow_step_map(std::span<double> step_cash, std::span<const CashDividend> dividends,
                       double T, double dt, double carry, int n_steps) noexcept {
  double escrow_0 = 0.0;
  const double tau_at_expiry = T * (1.0 + kTauAtExpiryRelTol);
  for (const CashDividend &dv : dividends) {
    if (!(dv.tau > 0.0) || dv.tau > tau_at_expiry || !(dv.amount > 0.0)) {
      continue;
    }
    const double nearest = std::nearbyint(dv.tau / dt);
    const double clamped = std::clamp(nearest, 1.0, static_cast<double>(n_steps));
    const double discounted = dv.amount * std::exp(-carry * std::min(dv.tau, T));
    step_cash[static_cast<std::size_t>(clamped)] += discounted;
    escrow_0 += discounted;
  }
  return escrow_0;
}

// The escrowed tree's geometry: up/down factors, the up probability, and the
// per-step factor that walks a node level back one step (1/d).
struct EscrowedTree {
  double up = 1.0;
  double down = 1.0;
  double p = 0.5;
};

// Peizer-Pratt method-2 inversion, the binomial-to-normal map of Leisen-Reimer.
[[nodiscard]] double peizer_pratt(double z, int n) noexcept {
  const double nd = static_cast<double>(n);
  const double t = z / (nd + 1.0 / 3.0 + 0.1 / (nd + 1.0));
  const double root = std::sqrt(1.0 - std::exp(-t * t * (nd + 1.0 / 6.0)));
  return 0.5 + std::copysign(0.5 * root, z);
}

// Leisen-Reimer (1996): the up probability matches N(d2) and the up factor
// N(d1)/N(d2) at THIS strike, so the tree is centred on K and its European
// converges smoothly at O(1/n^2) for odd n. A strike-aligned lattice: every
// strike gets its own tree, and the tree moves continuously with (K, sigma).
[[nodiscard]] EscrowedTree leisen_reimer_tree(double x0, double K, double T, double sigma,
                                              double carry, int n_steps) noexcept {
  const double vol = sigma * std::sqrt(T);
  const double d1 = (std::log(x0 / K) + (carry + 0.5 * sigma * sigma) * T) / vol;
  const double d2 = d1 - vol;
  const double dt = T / static_cast<double>(n_steps);
  const double growth = std::exp(carry * dt);
  EscrowedTree tree;
  tree.p = peizer_pratt(d2, n_steps);
  tree.up = growth * peizer_pratt(d1, n_steps) / tree.p;
  tree.down = (growth - tree.p * tree.up) / (1.0 - tree.p);
  return tree;
}

// The exercise test of the escrowed lattices at step k (see the block above).
void escrowed_exercise(std::span<const double> level, std::span<double> value, std::size_t last,
                       double stock_at_root, double escrow_post, double escrow_cum,
                       bool ex_step, double sgn, double strike) noexcept {
  for (std::size_t i = 0; i <= last; ++i) {
    // The root values the stock at S itself, not X0 + escrow re-summed.
    const double stock = (last == 0U) ? stock_at_root : level[i] + escrow_post;
    double best = std::max(value[i], exercise_value(sgn, stock, strike));
    if (ex_step) {
      best = std::max(best, exercise_value(sgn, level[i] + escrow_cum, strike));
    }
    value[i] = best;
  }
}

// `leisen_reimer` selects the strike-centred Leisen-Reimer tree (an even
// `steps` is rounded up to the next odd count, Leisen-Reimer's own convention)
// with the plain terminal payoff; otherwise a CRR tree with the BBS final step.
[[nodiscard]] Result<LatticeOutcome>
run_escrowed_lattice(double S, double K, double T, double sigma, double r, double q, Side side,
                     std::span<const CashDividend> dividends, int steps, ExerciseStyle exercise,
                     bool leisen_reimer, LatticeBuffers &buffers) {
  const int n_steps = (leisen_reimer && steps % 2 == 0) ? steps + 1 : steps;
  const std::size_t n_nodes = static_cast<std::size_t>(n_steps) + 1U;
  const bool american = (exercise == ExerciseStyle::American);
  const double sgn = payoff_sign(side);
  const double carry = r - q;
  const double dt = T / static_cast<double>(n_steps);
  const double disc = std::exp(-r * dt);

  buffers.step_amount.assign(n_nodes, 0.0);
  const std::span<double> step_cash(buffers.step_amount.data(), n_nodes);
  const double x0 = S - escrow_step_map(step_cash, dividends, T, dt, carry, n_steps);
  if (!(x0 > 0.0)) {
    return Err(ErrorCode::OutOfRange, "dated cash exhausts the spot; no escrowed stock remains");
  }
  const auto usable = [](const EscrowedTree &t) {
    return t.p > 0.0 && t.p < 1.0 && t.down > 0.0 && t.up > t.down && std::isfinite(t.up);
  };
  EscrowedTree tree;
  if (leisen_reimer) {
    tree = leisen_reimer_tree(x0, K, T, sigma, carry, n_steps);
  }
  if (!leisen_reimer || !usable(tree)) {
    // The CRR geometry. Under Leisen-Reimer this is the DEGENERATE regime only:
    // the strike sits so many standard deviations away (|d2| beyond ~200 at
    // 301 steps, sigma*sqrt(T) -> 0 at an IV bracket's lower end) that the
    // Peizer-Pratt probability rounds to exactly 0 or 1 and the centred tree
    // does not exist; the option is then worth its forward intrinsic on any
    // tree, and the CRR one still prices it.
    tree.up = std::exp(sigma * std::sqrt(dt));
    tree.down = 1.0 / tree.up;
    tree.p = (std::exp(carry * dt) - tree.down) / (tree.up - tree.down);
  }
  if (!usable(tree)) {
    return Err(ErrorCode::OutOfRange,
               "escrowed tree probability left (0, 1); the step is too coarse for this "
               "(r - q, sigma, T) — raise `steps`");
  }
  const double pu = disc * tree.p;
  const double pd = disc * (1.0 - tree.p);
  const double back = 1.0 / tree.down; // level at step k = level at step k+1 / down
  const double log_up = std::log(tree.up);
  const double log_down = std::log(tree.down);

  buffers.level.resize(n_nodes);
  const std::span<double> level(buffers.level.data(), n_nodes);
  for (std::size_t i = 0; i < n_nodes; ++i) {
    const double ups = static_cast<double>(i);
    level[i] = x0 * std::exp(ups * log_up + (static_cast<double>(n_steps) - ups) * log_down);
  }
  buffers.value.resize(n_nodes);
  const std::span<double> value(buffers.value.data(), n_nodes);

  // Cash on the expiry step, valued at T. An AMERICAN call may exercise
  // cum-dividend at expiry, so its payoff is (X + D - K)+. A put's cum-dividend
  // exercise is dominated.
  const double terminal_cash = std::exp(carry * T) * step_cash[n_nodes - 1U];
  const bool terminal_cum = american && terminal_cash > 0.0;
  for (std::size_t i = 0; i < n_nodes; ++i) {
    value[i] = exercise_value(sgn, level[i], K);
    if (terminal_cum) {
      value[i] = std::max(value[i], exercise_value(sgn, level[i] + terminal_cash, K));
    }
  }
  // BBS (CRR tree only): the analytic European over the last step of that
  // payoff — a call struck at K - D when the American call may exercise
  // cum-dividend at expiry (a forward when K <= D).
  const double call_strike = (terminal_cum && side == Side::Call) ? K - terminal_cash : K;
  const double growth = std::exp(carry * dt);
  const auto final_step = [&](double x) {
    const double forward = x * growth;
    if (side == Side::Put) {
      return black76_price(forward, K, dt, sigma, disc, Side::Put);
    }
    if (!(call_strike > 0.0)) {
      return disc * (forward - call_strike);
    }
    return black76_price(forward, call_strike, dt, sigma, disc, Side::Call);
  };

  // Cash strictly after step k, discounted to t = 0; grows as k walks back.
  double later_cash = 0.0;
  for (int k = n_steps - 1; k >= 0; --k) {
    const std::size_t last = static_cast<std::size_t>(k);
    later_cash += step_cash[last + 1U];
    for (std::size_t i = 0; i <= last; ++i) {
      level[i] *= back;
    }
    if (!leisen_reimer && k == n_steps - 1) {
      for (std::size_t i = 0; i <= last; ++i) {
        value[i] = final_step(level[i]);
      }
    } else {
      for (std::size_t i = 0; i <= last; ++i) {
        value[i] = pu * value[i + 1U] + pd * value[i];
      }
    }
    if (american) {
      const double to_t = std::exp(carry * dt * static_cast<double>(k));
      const double post = to_t * later_cash;
      escrowed_exercise(level, value, last, S, post, post + to_t * step_cash[last],
                        step_cash[last] > 0.0, sgn, K);
    }
  }
  LatticeOutcome out;
  out.price = value[0];
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
                                               ExerciseStyle exercise, double horizon,
                                               LatticeBuffers &buffers) {
  const double bumped_T = T - horizon;
  if (!(bumped_T > 0.0)) {
    return Ok(exercise_value(payoff_sign(side), S, K));
  }
  const Result<LatticeOutcome> leg =
      run_lattice(S, K, bumped_T, sigma, r, q, side, dividends, steps, exercise, false, buffers);
  if (!leg) {
    return Err(leg.error());
  }
  return Ok(leg->price);
}

// The schemes that are ONE rollback: the spliced CRR with or without the BBS
// final step, and the escrowed lattice on a CRR+BBS or Leisen-Reimer tree.
[[nodiscard]] Result<LatticeOutcome>
run_single_rollback(DiscreteDivScheme scheme, double S, double K, double T, double sigma, double r,
                    double q, Side side, std::span<const CashDividend> dividends, int steps,
                    ExerciseStyle exercise, LatticeBuffers &buffers) {
  switch (scheme) {
  case DiscreteDivScheme::SplicedCrr:
    return run_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise, false, buffers);
  case DiscreteDivScheme::SplicedBbs:
    return run_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise, true, buffers);
  case DiscreteDivScheme::EscrowedBbs:
    return run_escrowed_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise, false,
                                buffers);
  case DiscreteDivScheme::EscrowedLeisenReimer:
    return run_escrowed_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise, true,
                                buffers);
  case DiscreteDivScheme::SplicedBbsr:
    break; // two rollbacks: `richardson_price`
  }
  return Err(ErrorCode::InvalidArgument, "not a single-rollback lattice scheme");
}

// SplicedBbsr (header): two-point Richardson on the BBS lattice's leading 1/N
// term. The coarse rollback runs SECOND so it reuses the fine one's buffers.
[[nodiscard]] Result<double> richardson_price(double S, double K, double T, double sigma,
                                              double r, double q, Side side,
                                              std::span<const CashDividend> dividends, int steps,
                                              ExerciseStyle exercise) {
  LatticeBuffers buffers;
  const Result<LatticeOutcome> fine =
      run_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise, true, buffers);
  if (!fine) {
    return Err(fine.error());
  }
  const int coarse_steps = discrete_div_richardson_coarse_steps(steps);
  const Result<LatticeOutcome> coarse = run_lattice(S, K, T, sigma, r, q, side, dividends,
                                                    coarse_steps, exercise, true, buffers);
  if (!coarse) {
    return Err(coarse.error());
  }
  const double n = static_cast<double>(steps);
  const double m = static_cast<double>(coarse_steps);
  const double extrapolated = (n * fine->price - m * coarse->price) / (n - m);
  // Extrapolation can step under an exercise plateau (P_N exactly intrinsic,
  // P_M a little above it) or, in a far wing, under zero; neither is a price.
  const double floor = (exercise == ExerciseStyle::American)
                           ? exercise_value(payoff_sign(side), S, K)
                           : 0.0;
  return Ok(std::max(extrapolated, floor));
}

} // namespace

const char *to_string(DiscreteDivScheme value) noexcept {
  switch (value) {
  case DiscreteDivScheme::SplicedCrr:
    return "spliced";
  case DiscreteDivScheme::SplicedBbs:
    return "spliced-bbs";
  case DiscreteDivScheme::SplicedBbsr:
    return "spliced-bbsr";
  case DiscreteDivScheme::EscrowedBbs:
    return "escrowed-bbs";
  case DiscreteDivScheme::EscrowedLeisenReimer:
    return "escrowed-lr";
  }
  return "unknown";
}

Result<double> american_discrete_div_price(double S, double K, double T, double sigma, double r,
                                           double q, Side side,
                                           std::span<const CashDividend> dividends, int steps,
                                           ExerciseStyle exercise, DiscreteDivScheme scheme) {
  const Status ok = validate(S, K, T, sigma, r, q, dividends, steps);
  if (!ok) {
    return Err(ok.error());
  }
  if (!discrete_div_scheme_admits(scheme, steps)) {
    return Err(ErrorCode::InvalidArgument,
               "unknown discrete-dividend lattice scheme, or too few steps for it");
  }
  if (scheme == DiscreteDivScheme::SplicedBbsr) {
    return richardson_price(S, K, T, sigma, r, q, side, dividends, steps, exercise);
  }
  LatticeBuffers buffers;
  const Result<LatticeOutcome> out =
      run_single_rollback(scheme, S, K, T, sigma, r, q, side, dividends, steps, exercise, buffers);
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
  LatticeBuffers buffers;
  const Result<LatticeOutcome> out =
      run_lattice(S, K, T, sigma, r, q, side, dividends, steps, exercise, false, buffers);
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
  LatticeBuffers buffers;
  for (std::size_t i = 0; i < kStates; ++i) {
    const Result<LatticeOutcome> res =
        run_lattice(S, K, T, sigma + bumps[i].dsigma, r + bumps[i].dr, q + bumps[i].dq, side,
                    dividends, steps, exercise, false, buffers);
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
                                                  exercise, theta_secant_horizon, buffers);
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
  // differ from the base in a single amount, so nothing else is rebuilt, and
  // every rollback shares one lattice workspace.
  std::vector<CashDividend> bumped(dividends.begin(), dividends.end());
  LatticeBuffers buffers;
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
        run_lattice(S, K, T, sigma, r, q, side, bumped, steps, exercise, false, buffers);
    if (!up) {
      return Err(up.error());
    }
    bumped[i].amount = lo_amount;
    const Result<LatticeOutcome> dn =
        run_lattice(S, K, T, sigma, r, q, side, bumped, steps, exercise, false, buffers);
    if (!dn) {
      return Err(dn.error());
    }
    bumped[i].amount = dv.amount;
    dP_dDiv_out[i] = (up->price - dn->price) / span;
  }
  return Ok();
}

} // namespace atx::vol

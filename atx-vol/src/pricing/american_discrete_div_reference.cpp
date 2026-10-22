#include "pricing/american_discrete_div_reference.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The independent cash-dividend reference (contract, model, uncertainty rule and
// the two methods: the header). This file states each load-bearing choice again at
// the line that implements it.

namespace atx::vol::ddiv_ref {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kEps = std::numeric_limits<double>::epsilon();

// Howard iteration converges in at most n + 1 policies on an M-matrix LCP
// (Bokanowski-Maroso-Zidani 2009); in practice 1-4. The cap only turns a defect
// into a typed error instead of an unbounded loop.
constexpr int kMaxPolicyIterations = 200;

// An iterate solves the discrete LCP "to rounding" when no row violates it by more
// than this many ulps of the row's own term scale (the solves are backward stable).
constexpr double kLcpRoundingUlps = 64.0;

// The rounding-tie rule. Where the payoff itself nearly solves the discrete PDE
// (r = q = 0 deep in the money: the residual is O(dt h^2)), both branches of a row
// are feasible to rounding and exact Howard can flip those rows on rounding noise
// forever. Measured (matrix case G4p, finest space level): 261 deep-ITM rows
// switching every iteration at a worst violation of 70 ulps. From iteration
// kLcpTieIteration on, an iterate whose worst violation is within this bound is
// accepted as a rounding tie (a stall test on the switch count instead took up to
// 31 iterations on that grid: the count drifts down on noise); the largest
// violation accepted is reported on the result.
constexpr double kLcpTieUlps = 1024.0;
constexpr int kLcpTieIteration = 3;

// The PDE domain's lower edge never sizes off a spot net of cash below this
// fraction of spot (ruinous schedules would otherwise ask for ln 0).
constexpr double kLowestNetSpotFraction = 0.05;

// Every segment starts from non-smooth data (the payoff, or a jump), so even a
// segment far shorter than one nominal step gets a few steps of its own; measured:
// with one, the 0.0013-year last segment of matrix case B2p held the time axis to
// order 1.2.
constexpr int kMinSegmentSteps = 4;

// Rounding floor of an estimate, relative to max(|value|, 1): the tridiagonal
// solves are backward stable and a ladder runs O(1e4) of them.
constexpr double kRoundingFloorRel = 1.0e-12;

[[nodiscard]] double payoff(Side side, double s, double K) noexcept {
  return std::max(side == Side::Call ? s - K : K - s, 0.0);
}

[[nodiscard]] bool is_american(const Problem &p) noexcept {
  return p.exercise == ExerciseStyle::American;
}

// Cash in (0, T], discounted to 0 at the carry r - q: what the escrowed state
// nets out of spot (the kernel's `escrow_step_map`, with the TRUE ex-date).
[[nodiscard]] double escrow_at_zero(const Problem &p) noexcept {
  const double tau_at_expiry = p.T * (1.0 + kTauAtExpiryRelTol);
  double escrow = 0.0;
  for (const CashDividend &d : p.dividends) {
    if (d.tau > 0.0 && d.tau <= tau_at_expiry && d.amount > 0.0) {
      escrow += d.amount * std::exp(-(p.r - p.q) * std::min(d.tau, p.T));
    }
  }
  return escrow;
}

// ── The model at one (segment, time): obstacle, payoff, boundaries ──────────
//
// Segment j of the time grid is [bounds[j], bounds[j + 1]] with bounds = {0,
// interior ex-dates..., T}; the cash STRICTLY AFTER any time of segment j is the
// interior events k >= j plus the terminal cash. Passing the segment index rather
// than comparing times keeps "after" exact at an ex-date (where the step time and
// the event time are the same double).
struct Model {
  double S = 0.0;
  double K = 0.0;
  double T = 0.0;
  double r = 0.0;
  double carry = 0.0; // r - q
  Side side = Side::Put;
  bool american = true;
  CashProcess process = CashProcess::SpotLiquidator;
  EventSchedule events;
  double state0 = 0.0; // S, or X0 under Escrowed

  [[nodiscard]] bool escrowed() const noexcept { return process == CashProcess::Escrowed; }

  // Escrowed: cash after `t` (segment `seg`) carried back to t at the carry.
  [[nodiscard]] double escrow_after(int seg, double t) const noexcept {
    double e = events.terminal_amount * std::exp(-carry * (T - t));
    for (std::size_t k = static_cast<std::size_t>(seg); k < events.interior.size(); ++k) {
      e += events.interior[k].amount * std::exp(-carry * (events.interior[k].t - t));
    }
    return e;
  }

  // Spot: the cash after segment `seg`'s times, carried FORWARD to T — what the
  // forward S e^{c tau} loses: F = S e^{c tau} - this.
  [[nodiscard]] double cash_forward_after(int seg) const noexcept {
    double c = events.terminal_amount;
    for (std::size_t k = static_cast<std::size_t>(seg); k < events.interior.size(); ++k) {
      c += events.interior[k].amount * std::exp(carry * (T - events.interior[k].t));
    }
    return c;
  }

  // What exercise at state `z` delivers is the stock z + shift: shift 0 for the
  // spot conventions, escrow(t) under Escrowed. Computed once per time level so
  // the per-node obstacle is one add and one max.
  [[nodiscard]] double exercise_shift(int seg, double t) const noexcept {
    return escrowed() ? escrow_after(seg, t) : 0.0;
  }

  // The exercise value at state `z`.
  [[nodiscard]] double obstacle(int seg, double t, double z) const noexcept {
    return payoff(side, z + exercise_shift(seg, t), K);
  }

  // Post-dividend stock for a spot convention (the `CashProcess` block, header).
  [[nodiscard]] double post_level(double s, double amount) const noexcept {
    if (process == CashProcess::SpotSurvivor) {
      return (s >= amount) ? s - amount : s;
    }
    return std::max(s - amount, 0.0); // liquidator: absorbed at zero
  }

  // Expiry value at state `z`, the terminal cash applied analytically: the payoff
  // at the post-dividend stock, and — American — the cum-dividend exercise the
  // instant before it.
  [[nodiscard]] double terminal(double z) const noexcept {
    const double cash = events.terminal_amount;
    if (!(cash > 0.0)) {
      return payoff(side, z, K);
    }
    if (escrowed()) {
      const double post = payoff(side, z, K);
      return american ? std::max(post, payoff(side, z + cash, K)) : post;
    }
    const double post = payoff(side, post_level(z, cash), K);
    return american ? std::max(post, payoff(side, z, K)) : post;
  }

  // Every state level where `terminal` has a kink or a jump.
  [[nodiscard]] std::vector<double> terminal_kinks() const {
    const double cash = events.terminal_amount;
    std::vector<double> kinks{K};
    if (cash > 0.0) {
      if (escrowed()) {
        kinks.push_back(K - cash);
      } else {
        kinks.push_back(K + cash);
        kinks.push_back(cash);
      }
    }
    return kinks;
  }

  // A stock absorbed at zero stays there: the call is worthless, the put pays K
  // now (American, r >= 0) or K at expiry discounted (European; American r < 0).
  [[nodiscard]] double value_at_zero(double tau) const noexcept {
    if (side == Side::Call) {
      return 0.0;
    }
    const double held = K * std::exp(-r * tau);
    return american ? std::max(K, held) : held;
  }

  // The forward of state `z` at time-to-expiry `tau` (segment `seg`): X e^{c tau}
  // under Escrowed, S e^{c tau} less the cash still to come carried to T otherwise.
  [[nodiscard]] double forward_at(int seg, double tau, double z) const noexcept {
    const double grown = z * std::exp(carry * tau);
    return escrowed() ? grown : grown - cash_forward_after(seg);
  }

  // Far-field values (Dirichlet rows). Deep out of the money: 0. Deep in the money:
  // the linear forward value, maxed with exercise for an American. These rows sit
  // `width_sd` standard deviations from the spot and strike; the width study in the
  // fast suite (8 vs 11) bounds what they contribute.
  [[nodiscard]] double far_low(int seg, double t, double z) const noexcept {
    if (side == Side::Call) {
      return 0.0;
    }
    const double tau = T - t;
    const double forward = forward_at(seg, tau, z);
    const double linear = std::exp(-r * tau) * (K - std::max(forward, 0.0));
    return american ? std::max(linear, obstacle(seg, t, z)) : linear;
  }

  [[nodiscard]] double far_high(int seg, double t, double z) const noexcept {
    if (side == Side::Put) {
      return 0.0;
    }
    const double tau = T - t;
    const double forward = forward_at(seg, tau, z);
    const double linear = std::exp(-r * tau) * (forward - K);
    return american ? std::max(linear, obstacle(seg, t, z)) : std::max(linear, 0.0);
  }
};

[[nodiscard]] Model make_model(const Problem &p) {
  Model m;
  m.S = p.S;
  m.K = p.K;
  m.T = p.T;
  m.r = p.r;
  m.carry = p.r - p.q;
  m.side = p.side;
  m.american = is_american(p);
  m.process = p.process;
  m.events = event_schedule(p.T, p.dividends);
  m.state0 = m.escrowed() ? p.S - escrow_at_zero(p) : p.S;
  return m;
}

// ── Cell average of the terminal payoff ────────────────────────────────────
//
// Each node starts from the mean of the payoff over its control volume
// [x - w/2, x + w/2] in log state, the volume split at every kink so each piece is
// smooth (4-point Gauss-Legendre per piece, exact to rounding on e^x pieces of
// this width). The projection is what keeps the leading error term smooth in the
// grid spacing — without it the strike's position between nodes makes the
// second-order error oscillate and Richardson meaningless.
constexpr std::array<double, 4> kGl4X = {-0.8611363115940526, -0.3399810435848563,
                                         0.3399810435848563, 0.8611363115940526};
constexpr std::array<double, 4> kGl4W = {0.3478548451374538, 0.6521451548625461,
                                         0.6521451548625461, 0.3478548451374538};

[[nodiscard]] double cell_average(const Model &m, std::span<const double> log_kinks, double x,
                                  double w) {
  const double a = x - 0.5 * w;
  const double b = x + 0.5 * w;
  std::array<double, 8> cuts{};
  std::size_t n_cuts = 0;
  cuts[n_cuts++] = a;
  for (const double k : log_kinks) {
    if (k > a && k < b && n_cuts + 1 < cuts.size()) {
      cuts[n_cuts++] = k;
    }
  }
  cuts[n_cuts++] = b;
  std::sort(cuts.begin(), cuts.begin() + static_cast<std::ptrdiff_t>(n_cuts));
  double sum = 0.0;
  for (std::size_t i = 0; i + 1 < n_cuts; ++i) {
    const double mid = 0.5 * (cuts[i] + cuts[i + 1]);
    const double half = 0.5 * (cuts[i + 1] - cuts[i]);
    for (std::size_t g = 0; g < kGl4X.size(); ++g) {
      sum += kGl4W[g] * half * m.terminal(std::exp(mid + half * kGl4X[g]));
    }
  }
  return sum / w;
}

[[nodiscard]] std::vector<double> log_kinks_of(const Model &m) {
  std::vector<double> out;
  for (const double k : m.terminal_kinks()) {
    if (k > 0.0) {
      out.push_back(std::log(k));
    }
  }
  return out;
}

// Value of the post-jump curve (nodal `values` on x0 + i h) at stock `post`.
// Below the grid the curve is continued LINEARLY IN THE STOCK to its exact value
// at zero (`value_at_zero`): deep in the money a put is linear in S and a call is
// flat at 0, so this is the asymptote, not a guess; the grid bottom sits
// `width_sd` deviations below every relevant stock.
[[nodiscard]] double post_value(const Model &m, std::span<const double> values, double x0,
                                double h, double post, double tau) noexcept {
  const double s_lo = std::exp(x0);
  if (post >= s_lo) {
    return interp_uniform_cubic(values, x0, h, std::log(post));
  }
  const double at_zero = m.value_at_zero(tau);
  if (!(post > 0.0)) {
    return at_zero;
  }
  return at_zero + (values[0] - at_zero) * (post / s_lo);
}

// The curve just BEFORE an interior ex-date (backward in time: from the
// post-dividend curve V+ to V-), as a function of log state x:
//   spot      V-(S) = V+(post(S)) — the cash jump, read by interpolation;
//   escrowed  V-(X) = V+(X)       — the state is continuous;
// and, American, the better of that and the CUM-dividend exercise (against the
// undiluted S, or X + escrow(t+) + D). Post-dividend exercise is already inside
// V+ (the constraint holds at t+).
struct JumpCurve {
  const Model *m = nullptr;
  std::span<const double> post_curve; // V+ on x0 + i h (read only)
  double x0 = 0.0;
  double h = 0.0;
  double amount = 0.0;
  double tau = 0.0;        // time to expiry at the ex-date
  double escrow_cum = 0.0; // escrowed: escrow(t+) + amount

  [[nodiscard]] double cont(double x) const noexcept {
    if (m->escrowed()) {
      return interp_uniform_cubic(post_curve, x0, h, x);
    }
    return post_value(*m, post_curve, x0, h, m->post_level(std::exp(x), amount), tau);
  }
  [[nodiscard]] double cum(double x) const noexcept {
    const double z = std::exp(x);
    return payoff(m->side, m->escrowed() ? z + escrow_cum : z, m->K);
  }
  [[nodiscard]] double value(double x) const noexcept {
    return m->american ? std::max(cont(x), cum(x)) : cont(x);
  }
  // Log levels where V- has a kink or a jump that is not a crossing: the cum
  // payoff's strike, and the spot conventions' cash level (liquidator floor /
  // survivor skip).
  [[nodiscard]] std::array<double, 2> fixed_kinks() const noexcept {
    constexpr double kNone = -std::numeric_limits<double>::infinity();
    if (m->escrowed()) {
      const double k = m->K - escrow_cum;
      return {(m->american && k > 0.0) ? std::log(k) : kNone, kNone};
    }
    return {m->american ? std::log(m->K) : kNone, std::log(amount)};
  }
};

// The jump onto the nodes of `values` (overwritten; `scratch` same size). A node
// whose control volume [x - h/2, x + h/2] contains a kink of V- — a fixed one, or
// the crossing of continuation and cum-dividend exercise, located by bisection —
// gets the volume's MEAN (pieces split at the kinks, Gauss-Legendre per piece);
// every other node its point value. Same reason as the payoff's cell average: a
// kink between nodes otherwise leaves an O(h^2) error whose coefficient oscillates
// with the kink's position, which defeats the space extrapolation.
void apply_jump(const JumpCurve &jc, std::span<double> values, std::span<double> scratch) {
  const std::array<double, 2> kinks = jc.fixed_kinks();
  const std::size_t n = values.size();
  for (std::size_t i = 0; i < n; ++i) {
    const double x = jc.x0 + jc.h * static_cast<double>(i);
    const double a = x - 0.5 * jc.h;
    const double b = x + 0.5 * jc.h;
    std::array<double, 5> cuts{a, b, 0.0, 0.0, 0.0};
    std::size_t n_cuts = 2;
    for (const double k : kinks) {
      if (k > a && k < b) {
        cuts[n_cuts++] = k;
      }
    }
    if (jc.m->american) {
      double lo = a;
      double hi = b;
      const bool lo_exercise = jc.cum(lo) > jc.cont(lo);
      if (lo_exercise != (jc.cum(hi) > jc.cont(hi))) {
        for (int it = 0; it < 60; ++it) {
          const double mid = 0.5 * (lo + hi);
          ((jc.cum(mid) > jc.cont(mid)) == lo_exercise ? lo : hi) = mid;
        }
        cuts[n_cuts++] = 0.5 * (lo + hi);
      }
    }
    if (n_cuts == 2) {
      scratch[i] = jc.value(x);
      continue;
    }
    std::sort(cuts.begin(), cuts.begin() + static_cast<std::ptrdiff_t>(n_cuts));
    double sum = 0.0;
    for (std::size_t c = 0; c + 1 < n_cuts; ++c) {
      const double mid = 0.5 * (cuts[c] + cuts[c + 1]);
      const double half = 0.5 * (cuts[c + 1] - cuts[c]);
      for (std::size_t g = 0; g < kGl4X.size(); ++g) {
        sum += kGl4W[g] * half * jc.value(mid + half * kGl4X[g]);
      }
    }
    scratch[i] = sum / jc.h;
  }
  std::copy(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n), values.begin());
}

// The jump of interior event `seg - 1` at time `t` (its ex-date), onto the nodes
// of `values` on x0 + i h.
void jump_at(const Model &m, int seg, double t, std::span<double> values,
             std::span<double> scratch, double x0, double h) {
  const double amount = m.events.interior[static_cast<std::size_t>(seg) - 1U].amount;
  JumpCurve jc;
  jc.m = &m;
  jc.post_curve = std::span<const double>(scratch.data(), values.size());
  jc.x0 = x0;
  jc.h = h;
  jc.amount = amount;
  jc.tau = m.T - t;
  jc.escrow_cum = m.escrowed() ? m.escrow_after(seg, t) + amount : 0.0;
  // V+ is read from a copy while V- is written: every node's interpolation reads
  // the WHOLE pre-jump curve.
  std::copy(values.begin(), values.end(), scratch.begin());
  std::vector<double> out(values.size());
  apply_jump(jc, values, out);
}

// ── A. The PDE ─────────────────────────────────────────────────────────────

struct LogGrid {
  double x_min = 0.0;
  double h = 0.0;
  int n = 0;  // intervals; nodes 0 .. n
  int i0 = 0; // the centre node
};

[[nodiscard]] LogGrid make_log_grid(const Model &m, const Problem &p, const PdeGrid &g) {
  const double sd = ((g.domain_sigma > 0.0) ? g.domain_sigma : p.sigma) * std::sqrt(p.T);
  const double centre = std::log(m.state0);
  double lo = std::min(centre, std::log(p.K));
  const double hi = std::max(centre, std::log(p.K));
  if (!m.escrowed()) {
    double cash = m.events.terminal_amount;
    for (const ExDate &e : m.events.interior) {
      cash += e.amount;
    }
    lo = std::min(lo, std::log(std::max(p.S - cash, kLowestNetSpotFraction * p.S)));
  }
  const double x_lo = lo - g.width_sd * sd;
  const double x_hi = hi + g.width_sd * sd;
  const double h0 = (x_hi - x_lo) / static_cast<double>(g.space_base);
  const int i0_base = std::clamp(static_cast<int>(std::lround((centre - x_lo) / h0)), 2,
                                 g.space_base - 2);
  const int scale = 1 << g.space_level;
  LogGrid out;
  out.h = h0 / static_cast<double>(scale);
  out.n = g.space_base * scale;
  out.i0 = i0_base * scale;
  out.x_min = centre - static_cast<double>(out.i0) * out.h;
  return out;
}

// The constant-coefficient operator A (time-to-expiry form V_tau = A V) on the
// uniform log grid: central differences, a V_{i-1} + b V_i + c V_{i+1}.
struct Operator {
  double a = 0.0;
  double b = 0.0;
  double c = 0.0;
};

[[nodiscard]] Operator make_operator(double sigma, double r, double q, double h) noexcept {
  const double nu = r - q - 0.5 * sigma * sigma;
  const double diff = 0.5 * sigma * sigma / (h * h);
  const double conv = 0.5 * nu / h;
  return Operator{diff - conv, -2.0 * diff - r, diff + conv};
}

// Per-call PDE state: the value curve and the tridiagonal scratch.
struct PdeState {
  const Model *model = nullptr; // non-owning; the caller's Model outlives the solve
  LogGrid grid;
  Operator op;
  std::vector<double> v;       // values at the current time level
  std::vector<double> rhs;     // explicit part
  std::vector<double> obstacle;
  std::vector<double> work;    // the iterate of the policy loop
  std::vector<double> cp;      // Thomas forward sweep
  std::vector<double> dp;
  std::vector<unsigned char> active; // 1 = obstacle row
  std::vector<double> level;         // state level e^{x_i} of every node
  int max_policy_iterations = 0;
  double max_policy_time = 0.0;
  int implicit_solves = 0;
  double max_lcp_violation = 0.0; // worst accepted relative LCP violation
  int lcp_rounding_ties = 0;      // steps accepted by the rounding-tie rule
  bool fresh_data = false;        // v is a payoff or a jump, not an LCP solution

  [[nodiscard]] double state_at(int i) const noexcept {
    return level[static_cast<std::size_t>(i)];
  }
};

// Thomas sweep over rows (lo, di, up | rhs): interior rows the implicit operator or
// (0, 1, 0 | obstacle) where `active`, rows 0 and n the Dirichlet values. Every row
// is diagonally dominant (an M-matrix row or an identity row), so the sweep is
// stable without pivoting.
void thomas_solve(PdeState &s, double lo, double di, double up, double bottom, double top) {
  const int n = s.grid.n;
  const auto at = [](std::vector<double> &vec, int i) -> double & {
    return vec[static_cast<std::size_t>(i)];
  };
  at(s.cp, 0) = 0.0;
  at(s.dp, 0) = bottom;
  for (int i = 1; i <= n; ++i) {
    const bool identity = (i == n) || (s.active[static_cast<std::size_t>(i)] != 0U);
    const double l = identity ? 0.0 : lo;
    const double d = identity ? 1.0 : di;
    const double u = identity ? 0.0 : up;
    const double rv = (i == n) ? top
                      : identity ? s.obstacle[static_cast<std::size_t>(i)]
                                 : s.rhs[static_cast<std::size_t>(i)];
    const double m = d - l * at(s.cp, i - 1);
    at(s.cp, i) = u / m;
    at(s.dp, i) = (rv - l * at(s.dp, i - 1)) / m;
  }
  at(s.work, n) = at(s.dp, n);
  for (int i = n - 1; i >= 0; --i) {
    at(s.work, i) = at(s.dp, i) - at(s.cp, i) * at(s.work, i + 1);
  }
}

// The implicit rows of one step: the M-matrix stencil and the two Dirichlet values.
struct Rows {
  double lo = 0.0;
  double di = 0.0;
  double up = 0.0;
  double bottom = 0.0;
  double top = 0.0;
};

// The discrete LCP min(M V - rhs, V - g) = 0 at `t_new`, solved by Howard
// iteration: each iteration a tridiagonal solve with the obstacle rows pinned, the
// policy then re-chosen row by row as the branch with the smaller residual.
Status solve_lcp(PdeState &s, const Rows &rows, int seg, double t_new) {
  const Model &m = *s.model;
  const int n = s.grid.n;
  const double lo = rows.lo;
  const double di = rows.di;
  const double up = rows.up;
  // Initial policy: the obstacle rows are the nodes where exercise binds on a
  // guess of the new level AND pays something (a zero payoff never strictly beats
  // holding; starting those nodes on the obstacle made Howard release them one
  // node per iteration — measured: 28 iterations on the first half step after
  // expiry at the matrix's finest grid). The guess is the previous level, which
  // solved the same LCP one step earlier — except on a segment's first step,
  // where the previous level is the payoff or a jump: there the guess is one
  // unconstrained (European) step. Measured, matrix case G4p (r = 0 put): seeding
  // from the cell-averaged payoff, which lies below g at every in-the-money node,
  // released the band one node per iteration from the strike down, 31 iterations.
  const double shift = m.exercise_shift(seg, t_new);
  for (int i = 0; i <= n; ++i) {
    const auto k = static_cast<std::size_t>(i);
    s.obstacle[k] = payoff(m.side, s.level[k] + shift, m.K);
    s.active[k] = 0U;
  }
  const std::vector<double> *guess = &s.v;
  if (s.fresh_data) {
    thomas_solve(s, lo, di, up, rows.bottom, rows.top);
    guess = &s.work;
    s.fresh_data = false;
  }
  for (int i = 0; i <= n; ++i) {
    const auto k = static_cast<std::size_t>(i);
    s.active[k] = (s.obstacle[k] > 0.0 && (*guess)[k] <= s.obstacle[k]) ? 1U : 0U;
  }
  double worst = 0.0; // the largest relative LCP violation of the last iterate
  int worst_row = 0;
  int flips = 0; // rows the last iteration switched
  for (int iter = 1; iter <= kMaxPolicyIterations; ++iter) {
    thomas_solve(s, lo, di, up, rows.bottom, rows.top);
    bool changed = false;
    worst = 0.0;
    worst_row = 0;
    flips = 0;
    for (int i = 1; i < n; ++i) {
      const auto k = static_cast<std::size_t>(i);
      const double w = s.work[k];
      const double pde_res = lo * s.work[k - 1] + di * w + up * s.work[k + 1] - s.rhs[k];
      const double obs_res = w - s.obstacle[k];
      // EXACT Howard: each row takes the branch with the smaller residual (a tie
      // keeps the current one). A hysteresis margin here made the policy
      // non-minimising, which voids Howard's monotone convergence — measured: a
      // 200-iteration cycle on the r = q = 0 put, where the payoff satisfies the
      // discrete PDE to O(dt h^2) deep in the money.
      const bool on_obstacle = s.active[k] != 0U;
      const bool want = on_obstacle ? !(pde_res < obs_res) : (obs_res < pde_res);
      if (want != on_obstacle) {
        s.active[k] = want ? 1U : 0U;
        changed = true;
        ++flips;
      }
      // How far does the iterate violate the LCP, relative to the rounding of the
      // row's OWN terms (on a fine grid the implicit entries are ~ theta dt sigma^2
      // / h^2 >> 1, far above |V|)?
      const double row_scale = std::abs(lo * s.work[k - 1]) + std::abs(di * w) +
                               std::abs(up * s.work[k + 1]) + std::abs(s.rhs[k]) +
                               std::abs(s.obstacle[k]) + 1.0;
      const double violation = on_obstacle ? -pde_res : -obs_res;
      if (violation / row_scale > worst) {
        worst = violation / row_scale;
        worst_row = i;
      }
    }
    const bool solves_lcp = worst <= kLcpRoundingUlps * kEps;
    const bool rounding_tie = iter >= kLcpTieIteration && worst <= kLcpTieUlps * kEps;
    if (!changed || solves_lcp || rounding_tie) {
      if (iter > s.max_policy_iterations) {
        s.max_policy_iterations = iter;
        s.max_policy_time = t_new;
      }
      if (changed && !solves_lcp) {
        ++s.lcp_rounding_ties;
      }
      s.max_lcp_violation = std::max(s.max_lcp_violation, worst);
      s.v.swap(s.work);
      return Ok();
    }
  }
  std::array<char, 256> why{};
  std::snprintf(why.data(), why.size(),
                "discrete-dividend reference PDE: policy iteration did not settle within its "
                "cap (t = %.9g; last iteration: %d rows switched, worst relative LCP violation "
                "%.3e at node %d of %d, state %.6g)",
                t_new, flips, worst, worst_row, n, s.state_at(worst_row));
  return Err(ErrorCode::Internal, std::string(why.data()));
}

// One theta step to `t_new` (theta 1: implicit Euler, 0.5: Crank-Nicolson) on
// segment `seg`: the explicit half into `rhs`, then the implicit half — a plain
// tridiagonal solve (European) or the discrete LCP (American, `solve_lcp`).
Status theta_step(PdeState &s, double dt, double theta, int seg, double t_new) {
  const Model &m = *s.model;
  const int n = s.grid.n;
  const double ex = (1.0 - theta) * dt;
  for (int i = 1; i < n; ++i) {
    const auto k = static_cast<std::size_t>(i);
    s.rhs[k] = s.v[k] + ex * (s.op.a * s.v[k - 1] + s.op.b * s.v[k] + s.op.c * s.v[k + 1]);
  }
  const Rows rows{-theta * dt * s.op.a, 1.0 - theta * dt * s.op.b, -theta * dt * s.op.c,
                  m.far_low(seg, t_new, s.state_at(0)), m.far_high(seg, t_new, s.state_at(n))};
  ++s.implicit_solves;
  if (!m.american) {
    thomas_solve(s, rows.lo, rows.di, rows.up, rows.bottom, rows.top);
    s.v.swap(s.work);
    return Ok();
  }
  return solve_lcp(s, rows, seg, t_new);
}

// One segment, backward from its end `t_end` to its start on the graded levels of
// `segment_time`: the first `rannacher` steps as two implicit-Euler half steps
// each (the data entering a segment is non-smooth — the payoff, or a jump),
// Crank-Nicolson after that. The last level IS the segment start (no accumulated
// dt), so the ex-date the next jump happens at is the event's own double.
Status run_segment(PdeState &s, int seg, double t_start, double t_end, int steps,
                   int rannacher, double grading) {
  double t_old = t_end;
  s.fresh_data = true; // the segment starts from the payoff or a jump
  for (int k = 0; k < steps; ++k) {
    const double t_new = segment_time(t_start, t_end, steps, k + 1, grading);
    const double dt = t_old - t_new;
    if (k < rannacher) {
      const double t_mid = t_old - 0.5 * dt;
      if (Status st = theta_step(s, 0.5 * dt, 1.0, seg, t_mid); !st) {
        return st;
      }
      if (Status st = theta_step(s, t_mid - t_new, 1.0, seg, t_new); !st) {
        return st;
      }
    } else if (Status st = theta_step(s, dt, 0.5, seg, t_new); !st) {
      return st;
    }
    t_old = t_new;
  }
  return Ok();
}

[[nodiscard]] Status validate_pde_grid(const PdeGrid &g) {
  if (g.space_base < 8 || g.time_base < 1 || g.space_level < 0 || g.space_level >= kMaxLevels ||
      g.time_level < 0 || g.time_level >= kMaxLevels || !(g.width_sd > 0.0) ||
      !std::isfinite(g.width_sd) || g.rannacher_steps < 0 || !std::isfinite(g.domain_sigma) ||
      g.domain_sigma < 0.0 || !(g.time_grading >= 1.0 && g.time_grading <= 4.0)) {
    return Err(ErrorCode::InvalidArgument, "discrete-dividend reference PDE: grid outside its "
                                           "range");
  }
  return Ok();
}

// Every refinement value of one axis is a full solve; collect them into an Axis.
[[nodiscard]] Estimate combine(Axis space, Axis time, double finest, int policy_iterations) {
  Estimate e;
  e.space = space;
  e.time = time;
  e.finest = finest;
  e.value = finest + space.correction + time.correction;
  e.uncertainty = space.uncertainty + time.uncertainty +
                  kRoundingFloorRel * std::max(std::abs(e.value), 1.0);
  e.max_policy_iterations = policy_iterations;
  return e;
}

// ── B. The trinomial lattice ───────────────────────────────────────────────

struct Branches {
  double up = 0.0;
  double mid = 0.0;
  double down = 0.0;
  double disc = 1.0;
};

// Moment-matched branches of ln-state increments over dt on spacing dx: mean
// nu dt and second moment sigma^2 dt + nu^2 dt^2.
[[nodiscard]] Branches make_branches(double sigma, double r, double q, double dt,
                                     double dx) noexcept {
  const double nu = r - q - 0.5 * sigma * sigma;
  const double mu = nu * dt / dx;
  const double lam2 = sigma * sigma * dt / (dx * dx);
  Branches b;
  b.up = 0.5 * (lam2 + mu * mu + mu);
  b.down = 0.5 * (lam2 + mu * mu - mu);
  b.mid = 1.0 - lam2 - mu * mu;
  b.disc = std::exp(-r * dt);
  return b;
}

[[nodiscard]] Status validate_trinomial_grid(const TrinomialGrid &g) {
  if (g.steps_base < 1 || g.level < 0 || g.level >= kMaxLevels || !(g.pad_sd > 0.0) ||
      !std::isfinite(g.pad_sd)) {
    return Err(ErrorCode::InvalidArgument, "discrete-dividend reference lattice: grid outside "
                                           "its range");
  }
  return Ok();
}

// The lattice's geometry: the event-aligned segments, their steps and branches,
// one log spacing for all of them, and the padding band.
struct TrinomialLattice {
  std::vector<double> bounds;     // 0, interior ex-dates, T
  std::vector<int> steps;         // per segment
  std::vector<double> dts;        // per segment
  std::vector<Branches> branches; // per segment
  double dx = 0.0;
  double centre = 0.0; // ln of the state at the root
  int total = 0;       // steps over [0, T]
  int pad = 0;
  int width = 0; // total + pad: the terminal level spans index 0 .. 2 width
};

[[nodiscard]] Result<TrinomialLattice> make_trinomial(const Model &m, const Problem &p,
                                                      const TrinomialGrid &grid) {
  TrinomialLattice t;
  t.steps = segment_steps(m.events, p.T, grid.steps_base, grid.level);
  t.bounds.push_back(0.0);
  for (const ExDate &e : m.events.interior) {
    t.bounds.push_back(e.t);
  }
  t.bounds.push_back(p.T);
  double dt_max = 0.0;
  for (std::size_t j = 0; j < t.steps.size(); ++j) {
    t.dts.push_back((t.bounds[j + 1U] - t.bounds[j]) / static_cast<double>(t.steps[j]));
    dt_max = std::max(dt_max, t.dts[j]);
    t.total += t.steps[j];
  }
  // One spacing for the whole lattice (it must recombine across segments); the
  // longest step sets it at the classical lambda^2 = 1/3, shorter steps only
  // lower lambda^2 and lift the middle branch.
  t.dx = p.sigma * std::sqrt(3.0 * dt_max);
  for (const double dt : t.dts) {
    const Branches b = make_branches(p.sigma, p.r, p.q, dt, t.dx);
    if (b.up < 0.0 || b.down < 0.0 || b.mid < 0.0) {
      return Err(ErrorCode::OutOfRange, "discrete-dividend reference lattice: a branch "
                                        "probability left [0, 1] (|r - q - sigma^2/2| dx > "
                                        "sigma^2)");
    }
    t.branches.push_back(b);
  }
  // The padding band below (and, symmetrically, above) the root's own fan: room for
  // the post-dividend levels of every node, so the splice reads the curve and not a
  // boundary. No boundary condition is ever applied.
  double cash = m.events.terminal_amount;
  for (const ExDate &e : m.events.interior) {
    cash += e.amount;
  }
  const double net = m.escrowed() ? m.state0 : std::max(p.S - cash, kLowestNetSpotFraction * p.S);
  const double band = grid.pad_sd * p.sigma * std::sqrt(p.T) + std::log(m.state0 / net);
  t.pad = static_cast<int>(std::ceil(band / t.dx)) + 3;
  t.width = t.total + t.pad;
  t.centre = std::log(m.state0);
  return Ok(t);
}

// Backward induction: time level k from total - 1 down to 0; node j of level k is
// index width + j, |j| <= k + pad. Bermudan exercise at every level; at an ex-date
// (a segment's first level) the jump splices the level's band.
[[nodiscard]] double trinomial_rollback(const Model &m, const TrinomialLattice &t) {
  const auto nodes = static_cast<std::size_t>(2 * t.width + 1);
  const auto x_of = [&](std::size_t i) {
    return t.centre + t.dx * (static_cast<double>(i) - static_cast<double>(t.width));
  };
  std::vector<double> v(nodes);
  std::vector<double> next(nodes);
  std::vector<double> level(nodes);
  const std::vector<double> kinks = log_kinks_of(m);
  for (std::size_t i = 0; i < nodes; ++i) {
    level[i] = std::exp(x_of(i));
    v[i] = cell_average(m, kinks, x_of(i), t.dx);
  }
  int seg = static_cast<int>(t.steps.size()) - 1;
  int seg_first = t.total - t.steps[static_cast<std::size_t>(seg)];
  for (int k = t.total - 1; k >= 0; --k) {
    while (k < seg_first) {
      --seg;
      seg_first -= t.steps[static_cast<std::size_t>(seg)];
    }
    const auto sj = static_cast<std::size_t>(seg);
    const Branches &b = t.branches[sj];
    const double t_k = (k == seg_first)
                           ? t.bounds[sj]
                           : t.bounds[sj] + t.dts[sj] * static_cast<double>(k - seg_first);
    const auto lo = static_cast<std::size_t>(t.width - (k + t.pad));
    const auto hi = static_cast<std::size_t>(t.width + (k + t.pad));
    const double shift = m.exercise_shift(seg, t_k);
    for (std::size_t i = lo; i <= hi; ++i) {
      double val = b.disc * (b.up * v[i + 1U] + b.mid * v[i] + b.down * v[i - 1U]);
      if (m.american) {
        val = std::max(val, payoff(m.side, level[i] + shift, m.K));
      }
      next[i] = val;
    }
    v.swap(next);
    if (k == seg_first && seg > 0) {
      const std::span<double> band(v.data() + lo, hi - lo + 1U);
      const std::span<double> scratch(next.data() + lo, hi - lo + 1U);
      jump_at(m, seg, t_k, band, scratch, x_of(lo), t.dx);
    }
  }
  return v[static_cast<std::size_t>(t.width)];
}

} // namespace

// ── Public: names, validation, schedule ────────────────────────────────────

const char *to_string(CashProcess value) noexcept {
  switch (value) {
  case CashProcess::SpotLiquidator:
    return "spot-liquidator";
  case CashProcess::SpotSurvivor:
    return "spot-survivor";
  case CashProcess::Escrowed:
    return "escrowed";
  }
  return "unknown";
}

Status validate(const Problem &p) {
  const auto positive = [](double x) { return x > 0.0 && std::isfinite(x); };
  if (!positive(p.S) || !positive(p.K) || !positive(p.T) || !positive(p.sigma)) {
    return Err(ErrorCode::InvalidArgument,
               "discrete-dividend reference: S, K, T and sigma must be finite and > 0");
  }
  if (!std::isfinite(p.r) || !std::isfinite(p.q)) {
    return Err(ErrorCode::InvalidArgument, "discrete-dividend reference: r and q must be finite");
  }
  for (const CashDividend &d : p.dividends) {
    if (!std::isfinite(d.tau) || !std::isfinite(d.amount) || d.amount < 0.0) {
      return Err(ErrorCode::InvalidArgument,
                 "discrete-dividend reference: dividend tau / amount must be finite, amount >= 0");
    }
  }
  if (p.process == CashProcess::Escrowed && !(p.S - escrow_at_zero(p) > 0.0)) {
    return Err(ErrorCode::OutOfRange,
               "discrete-dividend reference: dated cash exhausts the spot; no escrowed stock");
  }
  return Ok();
}

EventSchedule event_schedule(double T, std::span<const CashDividend> dividends) {
  EventSchedule out;
  const double tau_at_expiry = T * (1.0 + kTauAtExpiryRelTol);
  const double expiry_floor = T * (1.0 - kTauAtExpiryRelTol);
  std::vector<ExDate> raw;
  for (const CashDividend &d : dividends) {
    if (!(d.tau > 0.0) || d.tau > tau_at_expiry || !(d.amount > 0.0)) {
      continue; // outside (0, T] or a zero amount: the kernel ignores it too
    }
    if (d.tau >= expiry_floor) {
      out.terminal_amount += d.amount;
    } else {
      raw.push_back(ExDate{d.tau, d.amount});
    }
  }
  std::sort(raw.begin(), raw.end(), [](const ExDate &a, const ExDate &b) { return a.t < b.t; });
  for (const ExDate &e : raw) {
    if (!out.interior.empty() && out.interior.back().t == e.t) {
      out.interior.back().amount += e.amount;
    } else {
      out.interior.push_back(e);
    }
  }
  return out;
}

std::vector<int> segment_steps(const EventSchedule &events, double T, int base, int level) {
  std::vector<double> bounds{0.0};
  for (const ExDate &e : events.interior) {
    bounds.push_back(e.t);
  }
  bounds.push_back(T);
  std::vector<int> steps;
  steps.reserve(bounds.size() - 1U);
  for (std::size_t j = 0; j + 1 < bounds.size(); ++j) {
    const double share = static_cast<double>(base) * (bounds[j + 1] - bounds[j]) / T;
    // The 1e-9 keeps an exact multiple from rounding up through the division.
    const int n0 = std::max(kMinSegmentSteps, static_cast<int>(std::ceil(share - 1.0e-9)));
    steps.push_back(n0 << level);
  }
  return steps;
}

double segment_time(double t_start, double t_end, int n, int k, double grading) noexcept {
  if (k >= n) {
    return t_start;
  }
  if (k <= 0) {
    return t_end;
  }
  const double frac = static_cast<double>(k) / static_cast<double>(n);
  return t_end - (t_end - t_start) * std::pow(frac, grading);
}

std::vector<double> time_grid(const EventSchedule &events, double T, int base, int level,
                              double grading) {
  const std::vector<int> steps = segment_steps(events, T, base, level);
  std::vector<double> bounds{0.0};
  for (const ExDate &e : events.interior) {
    bounds.push_back(e.t);
  }
  bounds.push_back(T);
  std::vector<double> grid{0.0};
  for (std::size_t j = 0; j < steps.size(); ++j) {
    for (int k = steps[j] - 1; k >= 1; --k) {
      grid.push_back(segment_time(bounds[j], bounds[j + 1], steps[j], k, grading));
    }
    grid.push_back(bounds[j + 1]);
  }
  return grid;
}

double interp_uniform_cubic(std::span<const double> values, double x0, double h,
                            double x) noexcept {
  const int n = static_cast<int>(values.size()) - 1;
  const double u = std::clamp((x - x0) / h, 0.0, static_cast<double>(n));
  const int k = std::min(static_cast<int>(std::floor(u)), n - 1);
  const int base = std::clamp(k - 1, 0, n - 3);
  const double t = u - static_cast<double>(base);
  const auto v = [&](int i) { return values[static_cast<std::size_t>(base + i)]; };
  const double l0 = -(t - 1.0) * (t - 2.0) * (t - 3.0) / 6.0;
  const double l1 = t * (t - 2.0) * (t - 3.0) / 2.0;
  const double l2 = -t * (t - 1.0) * (t - 3.0) / 2.0;
  const double l3 = t * (t - 1.0) * (t - 2.0) / 6.0;
  return l0 * v(0) + l1 * v(1) + l2 * v(2) + l3 * v(3);
}

// ── Public: the PDE ────────────────────────────────────────────────────────

Result<PdeResult> pde_price(const Problem &p, const PdeGrid &grid) {
  if (Status st = validate(p); !st) {
    return Err(st.error());
  }
  if (Status st = validate_pde_grid(grid); !st) {
    return Err(st.error());
  }
  const Model m = make_model(p);
  PdeState s;
  s.model = &m;
  s.grid = make_log_grid(m, p, grid);
  s.op = make_operator(p.sigma, p.r, p.q, s.grid.h);
  const std::size_t nodes = static_cast<std::size_t>(s.grid.n) + 1U;
  s.v.resize(nodes);
  s.rhs.assign(nodes, 0.0);
  s.obstacle.assign(nodes, 0.0);
  s.work.assign(nodes, 0.0);
  s.cp.assign(nodes, 0.0);
  s.dp.assign(nodes, 0.0);
  s.active.assign(nodes, 0U);
  s.level.resize(nodes);
  for (std::size_t i = 0; i < nodes; ++i) {
    s.level[i] = std::exp(s.grid.x_min + s.grid.h * static_cast<double>(i));
  }

  const std::vector<double> kinks = log_kinks_of(m);
  for (std::size_t i = 0; i < nodes; ++i) {
    s.v[i] = cell_average(m, kinks, s.grid.x_min + s.grid.h * static_cast<double>(i), s.grid.h);
  }

  const std::vector<int> steps = segment_steps(m.events, p.T, grid.time_base, grid.time_level);
  const int segments = static_cast<int>(steps.size());
  std::vector<double> scratch(nodes);
  for (int seg = segments - 1; seg >= 0; --seg) {
    const auto j = static_cast<std::size_t>(seg);
    const double t_start = (seg == 0) ? 0.0 : m.events.interior[j - 1U].t;
    const double t_end = (seg + 1 == segments) ? p.T : m.events.interior[j].t;
    if (Status st = run_segment(s, seg, t_start, t_end, steps[j], grid.rannacher_steps,
                                grid.time_grading);
        !st) {
      return Err(st.error());
    }
    if (seg > 0) {
      jump_at(m, seg, t_start, s.v, scratch, s.grid.x_min, s.grid.h);
    }
  }

  const auto i0 = static_cast<std::size_t>(s.grid.i0);
  const double h = s.grid.h;
  const double vx = (s.v[i0 + 1U] - s.v[i0 - 1U]) / (2.0 * h);
  const double vxx = (s.v[i0 + 1U] - 2.0 * s.v[i0] + s.v[i0 - 1U]) / (h * h);
  PdeResult out;
  out.price = s.v[i0];
  out.delta = vx / m.state0;
  out.gamma = (vxx - vx) / (m.state0 * m.state0);
  out.space_intervals = s.grid.n;
  out.time_steps = s.implicit_solves;
  out.max_policy_iterations = s.max_policy_iterations;
  out.max_policy_time = s.max_policy_time;
  out.max_lcp_violation = s.max_lcp_violation;
  out.lcp_rounding_ties = s.lcp_rounding_ties;
  return Ok(out);
}

Axis richardson_axis(std::span<const double> values, double theory_order) noexcept {
  Axis axis;
  axis.n = static_cast<int>(std::min(values.size(), axis.values.size()));
  for (int i = 0; i < axis.n; ++i) {
    axis.values[static_cast<std::size_t>(i)] = values[static_cast<std::size_t>(i)];
  }
  if (axis.n < 2) {
    return axis;
  }
  const auto at = [&](int i) { return axis.values[static_cast<std::size_t>(i)]; };
  const double d2 = at(axis.n - 1) - at(axis.n - 2);
  if (axis.n == 2) {
    axis.uncertainty = std::abs(d2);
    return axis;
  }
  const double d1 = at(axis.n - 2) - at(axis.n - 3);
  if (d2 == 0.0) {
    // Converged to the last bit on the last level; the previous move bounds it.
    axis.uncertainty = (d1 == 0.0) ? 0.0 : std::abs(d1) * kEps;
    axis.asymptotic = (d1 == 0.0);
    return axis;
  }
  const double ratio = d1 / d2;
  if (ratio > 1.0) {
    axis.observed_order = std::log2(ratio);
  }
  axis.asymptotic = (ratio > 1.0) && (axis.observed_order >= 0.5 * theory_order) &&
                    (axis.observed_order <= theory_order + 1.0);
  if (!axis.asymptotic) {
    axis.uncertainty = std::max(std::abs(d1), std::abs(d2));
    return axis;
  }
  const double order = std::min(axis.observed_order, theory_order);
  axis.correction = d2 / (std::exp2(order) - 1.0);
  axis.uncertainty = std::abs(axis.correction);
  return axis;
}

Result<Estimate> pde_reference(const Problem &p, const PdeLadder &ladder) {
  if (ladder.levels < 3 || ladder.levels > kMaxLevels) {
    return Err(ErrorCode::InvalidArgument,
               "discrete-dividend reference PDE: ladder levels outside [3, kMaxLevels]");
  }
  const int top = ladder.levels - 1;
  std::array<double, kMaxLevels> space{};
  std::array<double, kMaxLevels> time{};
  int policy = 0;
  double lcp_violation = 0.0;
  int ties = 0;
  for (int level = 0; level < ladder.levels; ++level) {
    PdeGrid g = ladder.base;
    g.space_level = level;
    g.time_level = top;
    const Result<PdeResult> out = pde_price(p, g);
    if (!out) {
      return Err(out.error());
    }
    space[static_cast<std::size_t>(level)] = out->price;
    policy = std::max(policy, out->max_policy_iterations);
    lcp_violation = std::max(lcp_violation, out->max_lcp_violation);
    ties += out->lcp_rounding_ties;
  }
  // The shared corner (finest space, finest time) is solved once and read twice.
  time[static_cast<std::size_t>(top)] = space[static_cast<std::size_t>(top)];
  for (int level = 0; level < top; ++level) {
    PdeGrid g = ladder.base;
    g.space_level = top;
    g.time_level = level;
    const Result<PdeResult> out = pde_price(p, g);
    if (!out) {
      return Err(out.error());
    }
    time[static_cast<std::size_t>(level)] = out->price;
    policy = std::max(policy, out->max_policy_iterations);
    lcp_violation = std::max(lcp_violation, out->max_lcp_violation);
    ties += out->lcp_rounding_ties;
  }
  const auto n = static_cast<std::size_t>(ladder.levels);
  Estimate e = combine(richardson_axis(std::span<const double>(space.data(), n), 2.0),
                       richardson_axis(std::span<const double>(time.data(), n), 2.0),
                       space[static_cast<std::size_t>(top)], policy);
  e.max_lcp_violation = lcp_violation;
  e.lcp_rounding_ties = ties;
  return Ok(e);
}

// ── Public: the trinomial lattice ──────────────────────────────────────────

Result<double> trinomial_price(const Problem &p, const TrinomialGrid &grid) {
  if (Status st = validate(p); !st) {
    return Err(st.error());
  }
  if (Status st = validate_trinomial_grid(grid); !st) {
    return Err(st.error());
  }
  const Model m = make_model(p);
  const Result<TrinomialLattice> lattice = make_trinomial(m, p, grid);
  if (!lattice) {
    return Err(lattice.error());
  }
  return Ok(trinomial_rollback(m, *lattice));
}

Result<Estimate> trinomial_reference(const Problem &p, const TrinomialLadder &ladder) {
  if (ladder.levels < 3 || ladder.levels > kMaxLevels) {
    return Err(ErrorCode::InvalidArgument,
               "discrete-dividend reference lattice: ladder levels outside [3, kMaxLevels]");
  }
  std::array<double, kMaxLevels> values{};
  for (int level = 0; level < ladder.levels; ++level) {
    TrinomialGrid g = ladder.base;
    g.level = level;
    const Result<double> out = trinomial_price(p, g);
    if (!out) {
      return Err(out.error());
    }
    values[static_cast<std::size_t>(level)] = *out;
  }
  const auto n = static_cast<std::size_t>(ladder.levels);
  return Ok(combine(Axis{}, richardson_axis(std::span<const double>(values.data(), n), 1.0),
                    values[n - 1U], 0));
}

Result<Reference> reference_price(const Problem &p, const ReferenceOpts &opts) {
  const Result<Estimate> pde = pde_reference(p, opts.pde);
  if (!pde) {
    return Err(pde.error());
  }
  const Result<Estimate> lattice = trinomial_reference(p, opts.lattice);
  if (!lattice) {
    return Err(lattice.error());
  }
  Reference out;
  out.pde = *pde;
  out.lattice = *lattice;
  out.value = pde->value;
  const double gap = std::abs(pde->value - lattice->value);
  out.methods_agree = gap <= pde->uncertainty + lattice->uncertainty;
  out.uncertainty = out.methods_agree ? pde->uncertainty
                                      : gap + pde->uncertainty + lattice->uncertainty;
  return Ok(out);
}

} // namespace atx::vol::ddiv_ref

// Target tracking (platform v8 R-6): the ADMM tracker on synthetic factor-model problems --
// the target itself without costs or limits, the closed-form soft threshold of a diagonal
// linear-cost problem, one-name brute force over every kink (w0, 0, the dead zone),
// monotonicity in the cost, boxes / trade limits / net / beta held, the trade-limit share,
// a clipped indefinite covariance, bit-for-bit repeats and a warm dual, refusals.
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/book/target_tracking.hpp"

namespace atx_test_target_tracking {
namespace book = atx::engine::book;
using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
constexpr f64 inf = std::numeric_limits<f64>::infinity();

struct Lcg {
  u64 state;
  f64 next() { // uniform [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11U) * 0x1.0p-53;
  }
  f64 uniform(f64 lo, f64 hi) { return lo + (hi - lo) * next(); }
};

// n names on intercept + 4 groups (every fifth name has none) + 3 styles; F = v v' + diag
// (PSD); d = (1..3%)^2; costs on; boxes, trade limits and limits open.
book::TrackingProblem random_problem(usize n, u64 seed) {
  Lcg rng{seed};
  book::TrackingProblem p;
  p.factors = book::TrackingFactors{4, 3};
  p.n = n;
  const usize k = p.factors.count();
  std::vector<f64> v(k);
  for (f64& x : v) x = rng.uniform(-2e-3, 2e-3);
  p.covariance.assign(k * k, 0.0);
  for (usize a = 0; a < k; ++a) {
    const f64 var = a == 0 ? 1e-4 : a <= 4 ? 2.5e-5 : 1e-5;
    for (usize b = 0; b < k; ++b) p.covariance[a * k + b] = v[a] * v[b] + (a == b ? var : 0.0);
  }
  p.external_gap.assign(k, 0.0);
  p.group.resize(n); p.styles.resize(n * 3); p.specific.resize(n); p.target.resize(n);
  p.current.resize(n); p.impact_cost.resize(n); p.borrow_cost.resize(n); p.beta.resize(n);
  p.linear_cost.assign(n, 3e-5);
  p.trade_limit.assign(n, inf); p.lower.assign(n, -inf); p.upper.assign(n, inf);
  for (usize i = 0; i < n; ++i) {
    p.group[i] = static_cast<u32>(i % 5);
    for (usize c = 0; c < 3; ++c) p.styles[i * 3 + c] = rng.uniform(-1.5, 1.5);
    const f64 vol = rng.uniform(0.01, 0.03);
    p.specific[i] = vol * vol;
    p.target[i] = rng.uniform(-0.02, 0.02);
    p.current[i] = rng.uniform(-0.01, 0.01);
    p.impact_cost[i] = rng.uniform(0.0, 2e-3);
    p.borrow_cost[i] = rng.uniform(2e-5, 6e-5);
    p.beta[i] = rng.uniform(0.5, 1.5);
  }
  p.gamma = 40.0;
  return p;
}
void zero_costs(book::TrackingProblem& p) {
  p.linear_cost.assign(p.n, 0.0); p.impact_cost.assign(p.n, 0.0); p.borrow_cost.assign(p.n, 0.0);
}
f64 max_abs_diff(std::span<const f64> a, std::span<const f64> b) {
  f64 m = 0.0;
  for (usize i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
  return m;
}
f64 unit_trade_cost(const book::TrackingProblem& p, std::span<const f64> w) {
  f64 c = 0.0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 move = std::abs(w[i] - p.current[i]);
    c += p.linear_cost[i] * move + p.impact_cost[i] * move * std::sqrt(move);
  }
  return c;
}
f64 objective_at(const book::TrackingProblem& p, std::span<const f64> w) {
  return book::tracking_terms(p, w).value().objective;
}

// No cost, no limit, no outside gap: the tracking optimum is the target itself, and the
// registered constants (default options) reach it to 1e-8.
TEST(TargetTracking, ZeroCostNoLimitsReturnsTheTargetTo1e8) {
  auto p = random_problem(50, 11);
  zero_costs(p);
  const auto sol = book::solve_tracking(p);
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  EXPECT_TRUE(sol->limits_met);
  EXPECT_LT(max_abs_diff(sol->w, p.target), 1e-8);
  EXPECT_EQ(sol->clipped_eigenvalues, 0U);
  EXPECT_LE(sol->iterations, book::tracking_max_iterations);
}

// F = 0, linear cost only, nothing else: each name solves (gamma d/2)(w - a)^2 + s|w - w0|,
// so w = w0 + soft(a - w0, s / (gamma d)). Half the names sit in the dead zone (no trade,
// exactly w0), half trade to the threshold.
TEST(TargetTracking, DiagonalLinearCostIsTheSoftThreshold) {
  auto p = random_problem(40, 23);
  zero_costs(p);
  std::fill(p.covariance.begin(), p.covariance.end(), 0.0);
  std::vector<f64> expected(p.n);
  usize dead = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 gap = p.target[i] - p.current[i], curvature = p.gamma * p.specific[i];
    p.linear_cost[i] = curvature * std::abs(gap) * (i % 2 == 0 ? 0.5 : 1.5);
    const f64 threshold = p.linear_cost[i] / curvature;
    const f64 shrunk = std::max(std::abs(gap) - threshold, 0.0);
    expected[i] = p.current[i] + (gap > 0 ? shrunk : -shrunk);
    dead += shrunk == 0.0 ? 1U : 0U;
  }
  ASSERT_EQ(dead, p.n / 2);
  const auto sol = book::solve_tracking(p);
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  EXPECT_LT(max_abs_diff(sol->w, expected), 1e-8);
  for (usize i = 1; i < p.n; i += 2) EXPECT_EQ(sol->w[i], p.current[i]) << i; // exact no-trade
  EXPECT_EQ(sol->no_trade, dead);
}

// One name, every kink of the prox: the buy, the sale into a short (borrow on), the linear
// cost's dead zone (w = w0 exactly) and the borrow kink (w = 0 exactly), against a golden-
// section search of the objective.
TEST(TargetTracking, OneNameMatchesAGoldenSectionSearchAtEveryKink) {
  struct Case {
    f64 target, current, impact, borrow;
    const char* name;
  };
  const Case cases[] = {{0.03, 0.004, 3e-3, 5e-5, "buy"},
                        {-0.03, 0.004, 3e-3, 5e-5, "sell into a short"},
                        {0.0045, 0.004, 3e-3, 5e-5, "dead zone"},
                        {-0.004, 0.001, 1e-4, 1e-3, "borrow kink"}};
  for (const auto& c : cases) {
    book::TrackingProblem p;
    p.factors = book::TrackingFactors{0, 0}; // the intercept only
    p.n = 1;
    p.group = {0}; p.covariance = {1e-4}; p.external_gap = {0.0}; p.specific = {4e-4};
    p.target = {c.target}; p.current = {c.current}; p.linear_cost = {2e-5};
    p.impact_cost = {c.impact}; p.borrow_cost = {c.borrow}; p.trade_limit = {inf};
    p.lower = {-inf}; p.upper = {inf}; p.beta = {1.0}; p.gamma = 40.0;
    book::TrackingOptions o;
    o.tolerance = 1e-13;
    o.max_iterations = 100000;
    const auto sol = book::solve_tracking(p, o);
    ASSERT_TRUE(sol) << c.name << ": " << sol.error().to_string();
    const auto f = [&](f64 u) { return objective_at(p, std::vector<f64>{u}); };
    f64 lo = -0.05, hi = 0.05;
    const f64 g = 0.5 * (std::sqrt(5.0) - 1.0);
    f64 a = hi - g * (hi - lo), b = lo + g * (hi - lo), fa = f(a), fb = f(b);
    for (int step = 0; step < 300; ++step) {
      if (fa < fb) { hi = b; b = a; fb = fa; a = hi - g * (hi - lo); fa = f(a); }
      else { lo = a; a = b; fa = fb; b = lo + g * (hi - lo); fb = f(b); }
    }
    const f64 u = 0.5 * (lo + hi);
    EXPECT_NEAR(sol->w[0], u, 1e-8) << c.name;
    EXPECT_LE(f(sol->w[0]), f(u) + 1e-15) << c.name;
    if (std::string(c.name) == "dead zone") EXPECT_EQ(sol->w[0], c.current);
    if (std::string(c.name) == "borrow kink") EXPECT_EQ(sol->w[0], 0.0);
    if (std::string(c.name) == "sell into a short") EXPECT_LT(sol->w[0], 0.0);
  }
}

// min f(w) + kappa h(w) with h the (unit) trade cost: h at the optimum is nonincreasing in
// kappa (compare the two optimality inequalities). With the net limit and boxes on.
TEST(TargetTracking, HigherCostTradesLess) {
  auto p = random_problem(60, 31);
  p.net = book::TrackingLimit{0.0, 0.0};
  for (usize i = 0; i < p.n; ++i) { p.lower[i] = -0.02; p.upper[i] = 0.02; }
  const auto base_linear = p.linear_cost, base_impact = p.impact_cost;
  f64 previous = inf, first = 0.0, last = 0.0;
  for (const f64 kappa : {0.25, 1.0, 4.0, 16.0}) {
    for (usize i = 0; i < p.n; ++i) {
      p.linear_cost[i] = kappa * base_linear[i];
      p.impact_cost[i] = kappa * base_impact[i];
    }
    const auto sol = book::solve_tracking(p);
    ASSERT_TRUE(sol) << sol.error().to_string();
    EXPECT_TRUE(sol->converged) << kappa;
    auto unit = p;
    unit.linear_cost = base_linear; unit.impact_cost = base_impact;
    const f64 h = unit_trade_cost(unit, sol->w);
    EXPECT_LE(h, previous + 1e-12) << kappa;
    if (kappa == 0.25) first = h;
    last = h;
    previous = h;
  }
  EXPECT_LT(last, 0.5 * first);
}

// Holding box, locate floor, trade limits, a net equality away from 0 and a beta band that
// excludes the beta-free optimum (so, by strict convexity, the optimum is on its edge).
TEST(TargetTracking, LimitsAndBoxesHoldToTolerance) {
  auto p = random_problem(60, 5);
  Lcg rng{77};
  for (usize i = 0; i < p.n; ++i) {
    p.trade_limit[i] = rng.uniform(0.002, 0.006);
    p.lower[i] = i % 7 == 0 ? std::min(p.current[i], 0.0) : -0.02; // locate floor
    p.upper[i] = 0.02;
    p.target[i] += 3e-3 * (p.beta[i] - 1.0); // a beta tilt for the band to cut
  }
  p.net = book::TrackingLimit{0.003, 0.003};
  const auto free_beta = book::solve_tracking(p);
  ASSERT_TRUE(free_beta) << free_beta.error().to_string();
  f64 b_free = 0.0;
  for (usize i = 0; i < p.n; ++i) b_free += p.beta[i] * free_beta->w[i];
  ASSERT_GT(std::abs(b_free), 1e-5);
  p.beta_limit = book::TrackingLimit{-0.5 * std::abs(b_free), 0.5 * std::abs(b_free)};
  const f64 edge = b_free > 0 ? p.beta_limit.hi : p.beta_limit.lo;
  const auto sol = book::solve_tracking(p);
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  EXPECT_TRUE(sol->limits_met) << sol->limit_violation;
  f64 net = 0.0, beta = 0.0;
  usize at_limit = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 w = sol->w[i], move = std::abs(w - p.current[i]);
    EXPECT_GE(w, p.lower[i]) << i;
    EXPECT_LE(w, p.upper[i]) << i;
    EXPECT_LE(move, p.trade_limit[i] * (1.0 + 1e-12)) << i;
    at_limit += move >= p.trade_limit[i] * (1.0 - 1e-9) ? 1U : 0U;
    net += w; beta += p.beta[i] * w;
  }
  EXPECT_NEAR(net, 0.003, 1e-12);
  EXPECT_GE(beta, p.beta_limit.lo - 1e-12);
  EXPECT_LE(beta, p.beta_limit.hi + 1e-12);
  EXPECT_NEAR(beta, edge, 1e-8);
  EXPECT_NE(sol->beta_multiplier, 0.0);
  EXPECT_EQ(sol->at_trade_limit, at_limit);
  EXPECT_DOUBLE_EQ(sol->trade_limit_share, static_cast<f64>(at_limit) / static_cast<f64>(p.n));
}

// From flat with a target far beyond one period's trade limit, every name trades exactly
// its limit: the share at the trade limit is 1.
TEST(TargetTracking, TradeLimitShareCountsNamesAtTheirLimit) {
  auto p = random_problem(30, 41);
  zero_costs(p);
  for (usize i = 0; i < p.n; ++i) {
    p.current[i] = 0.0;
    p.target[i] = i % 2 == 0 ? 0.02 : -0.02;
    p.trade_limit[i] = 1e-4;
  }
  const auto sol = book::solve_tracking(p);
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_EQ(sol->at_trade_limit, p.n);
  EXPECT_EQ(sol->trade_limit_share, 1.0);
  for (usize i = 0; i < p.n; ++i) EXPECT_NEAR(std::abs(sol->w[i]), 1e-4, 1e-15) << i;
}

// A covariance with a negative eigenvalue (zeroed entries can do that) is used through its
// PSD part; the clipped eigenvalue is counted.
TEST(TargetTracking, IndefiniteCovarianceIsClippedAndCounted) {
  auto p = random_problem(30, 43);
  const usize k = p.factors.count();
  std::fill(p.covariance.begin(), p.covariance.end(), 0.0);
  for (usize a = 0; a < k; ++a) p.covariance[a * k + a] = a == 2 ? -1e-5 : 2e-5;
  const auto sol = book::solve_tracking(p);
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_EQ(sol->clipped_eigenvalues, 1U);
  EXPECT_TRUE(sol->converged) << sol->iterations;
}

// Same inputs, same bits; the solution's dual as a warm start reaches the same book.
TEST(TargetTracking, RepeatsBitForBitAndAWarmDualAgrees) {
  auto p = random_problem(50, 17);
  p.net = book::TrackingLimit{0.0, 0.0};
  const auto first = book::solve_tracking(p);
  const auto again = book::solve_tracking(p);
  ASSERT_TRUE(first && again);
  ASSERT_EQ(first->w.size(), again->w.size());
  for (usize i = 0; i < p.n; ++i)
    EXPECT_EQ(std::bit_cast<u64>(first->w[i]), std::bit_cast<u64>(again->w[i])) << i;
  EXPECT_EQ(first->iterations, again->iterations);
  const auto warm = book::solve_tracking(p, {}, first->dual);
  ASSERT_TRUE(warm) << warm.error().to_string();
  EXPECT_TRUE(warm->converged);
  EXPECT_LT(max_abs_diff(warm->w, first->w), 1e-8);
}

TEST(TargetTracking, RefusesMalformedProblems) {
  const auto p = random_problem(10, 3);
  ASSERT_TRUE(book::validate_tracking_problem(p));
  const auto refused = [](const book::TrackingProblem& q) {
    const auto s = book::solve_tracking(q);
    return !s && s.error().code() == atx::core::ErrorCode::InvalidArgument;
  };
  auto q = p; q.target.pop_back();                          EXPECT_TRUE(refused(q));
  q = p; q.specific[3] = 0.0;                               EXPECT_TRUE(refused(q));
  q = p; q.linear_cost[2] = -1e-5;                          EXPECT_TRUE(refused(q));
  q = p; q.lower[1] = 0.1; q.upper[1] = 0.0;                EXPECT_TRUE(refused(q));
  q = p; q.trade_limit[0] = std::nan("");                   EXPECT_TRUE(refused(q));
  q = p; q.gamma = 0.0;                                     EXPECT_TRUE(refused(q));
  q = p; q.net = book::TrackingLimit{0.1, -0.1};            EXPECT_TRUE(refused(q));
  q = p; q.group[4] = 9;                                    EXPECT_TRUE(refused(q));
  q = p; q.covariance[5] = inf;                             EXPECT_TRUE(refused(q));
  const std::vector<f64> short_dual(p.n - 1, 0.0);
  EXPECT_FALSE(book::solve_tracking(p, {}, short_dual));
  book::TrackingOptions none;
  none.max_iterations = 0;
  EXPECT_FALSE(book::solve_tracking(p, none));
}
} // namespace atx_test_target_tracking

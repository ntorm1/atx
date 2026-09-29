// spo-v1 / spo-v2 (platform v7 W1): the solver on synthetic factor-model problems (KKT
// certificate, closed-form Markowitz, constraint feasibility, warm/cold determinism, a 1-D
// brute-force check of the 3/2 cost), the pinned atx-risk-v1 store (refusals), and the NAV
// hook (flag-off identity, CLI routing, an end-to-end replay on a synthetic role + risk
// model). Fix-up 2 (the rejected spo-v1 TRAIN cell): one test per root cause -- alpha
// orientation and horizon (SpoAlpha, SpoSolver.HorizonConsistentAlpha...), the gamma rule
// (SpoCalibration), the risk-model plausibility ceiling (SpoRisk.Implausible...,
// SpoHook.ACorrupt...) -- and the aim-partial-v5 shadow book (SpoHook.ShadowBook...).

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"

namespace {
using namespace atx;
namespace co = atx::core;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace v7 = atx::impl::strategy::v7;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();

u64 bits(f64 x) { return std::bit_cast<u64>(x); }
struct Lcg {
  u64 state;
  f64 next() { // uniform [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11U) * 0x1.0p-53;
  }
  f64 uniform(f64 lo, f64 hi) { return lo + (hi - lo) * next(); }
};

// ---- synthetic problems ----------------------------------------------------------------------
// n names on market + 4 industries (every fifth name has none) + 3 styles; F = diag + v v'
// (PSD); D_i = (1..3%)^2; GK alpha; a gross budget that binds; boxes around w0.
sp::Problem random_problem(usize n, u64 seed) {
  Lcg rng{seed};
  sp::Problem p;
  p.layout = sp::FactorLayout{4, 3};
  p.n = n;
  const usize k = p.layout.factors();
  p.industry.resize(n); p.styles.resize(n * 3); p.specific.resize(n); p.alpha.resize(n);
  p.beta.resize(n); p.w0.resize(n); p.lower.resize(n); p.upper.resize(n);
  p.linear_cost.assign(n, 5e-5); p.impact_cost.resize(n); p.long_rate.assign(n, 1e-5);
  p.short_rate.resize(n);
  std::vector<f64> v(k);
  for (f64& x : v) x = rng.uniform(-2e-3, 2e-3);
  p.covariance.assign(k * k, 0.0);
  for (usize a = 0; a < k; ++a) {
    const f64 var = a == 0 ? 1e-4 : a <= 4 ? 2.5e-5 : 1e-5;
    for (usize b = 0; b < k; ++b) p.covariance[a * k + b] = v[a] * v[b] + (a == b ? var : 0.0);
  }
  p.fixed_exposure.assign(k, 0.0);
  for (usize i = 0; i < n; ++i) {
    p.industry[i] = static_cast<u32>(i % 5);
    for (usize c = 0; c < 3; ++c) p.styles[i * 3 + c] = rng.uniform(-1.5, 1.5);
    const f64 vol = rng.uniform(0.01, 0.03);
    p.specific[i] = vol * vol;
    p.alpha[i] = 0.02 * vol * rng.uniform(-1.7, 1.7);
    p.beta[i] = rng.uniform(0.5, 1.5);
    p.w0[i] = rng.uniform(-0.01, 0.01);
    p.lower[i] = std::max(-0.02, p.w0[i] - 0.006);
    p.upper[i] = std::min(0.02, p.w0[i] + 0.006);
    p.impact_cost[i] = rng.uniform(0.0, 2e-3);
    p.short_rate[i] = rng.uniform(2e-5, 6e-5);
  }
  p.gamma = 40; p.net = 0; p.gross = 0.25; p.beta_lo = -0.03; p.beta_hi = 0.03;
  return p;
}
sp::SolverOptions options(usize iterations, f64 tolerance) {
  sp::SolverOptions o;
  o.max_iterations = iterations; o.tolerance = tolerance;
  return o;
}
f64 max_abs_diff(std::span<const f64> a, std::span<const f64> b) {
  f64 m = 0;
  for (usize i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
  return m;
}
// Dense Sigma = X F X' + D of a problem (tests only).
std::vector<f64> dense_sigma(const sp::Problem& p) {
  const usize n = p.n, k = p.layout.factors(), first = 1 + p.layout.industries;
  std::vector<f64> x(n * k, 0.0), sigma(n * n, 0.0);
  for (usize i = 0; i < n; ++i) {
    x[i * k] = 1.0;
    if (p.industry[i] != 0) x[i * k + p.industry[i]] = 1.0;
    for (usize c = 0; c < p.layout.styles; ++c)
      x[i * k + first + c] = p.styles[i * p.layout.styles + c];
  }
  for (usize i = 0; i < n; ++i)
    for (usize j = 0; j < n; ++j) {
      f64 s = i == j ? p.specific[i] : 0.0;
      for (usize a = 0; a < k; ++a)
        for (usize b = 0; b < k; ++b) s += x[i * k + a] * p.covariance[a * k + b] * x[j * k + b];
      sigma[i * n + j] = s;
    }
  return sigma;
}
// Solves A x = b for symmetric positive definite A (Cholesky).
std::vector<f64> spd_solve(std::vector<f64> a, std::vector<f64> b) {
  const usize n = b.size();
  for (usize j = 0; j < n; ++j) {
    for (usize k = 0; k < j; ++k) a[j * n + j] -= a[j * n + k] * a[j * n + k];
    if (!(a[j * n + j] > 0)) throw std::runtime_error("not positive definite");
    a[j * n + j] = std::sqrt(a[j * n + j]);
    for (usize i = j + 1; i < n; ++i) {
      for (usize k = 0; k < j; ++k) a[i * n + j] -= a[i * n + k] * a[j * n + k];
      a[i * n + j] /= a[j * n + j];
    }
  }
  for (usize i = 0; i < n; ++i) {
    for (usize k = 0; k < i; ++k) b[i] -= a[i * n + k] * b[k];
    b[i] /= a[i * n + i];
  }
  for (usize ii = n; ii-- > 0;) {
    for (usize k = ii + 1; k < n; ++k) b[ii] -= a[k * n + ii] * b[k];
    b[ii] /= a[ii * n + ii];
  }
  return b;
}

// ---- solver --------------------------------------------------------------------------------
TEST(SpoSolver, KktResidualBelowToleranceOnFiftyNames) {
  const auto p = random_problem(50, 11);
  const auto o = options(20000, 1e-9);
  const auto sol = sp::solve(p, p.w0, o);
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  EXPECT_TRUE(sol->coupling_met);
  EXPECT_LE(sol->residual, o.tolerance);
  EXPECT_TRUE(sol->multipliers.gross_binding); // the budget binds: the coupled prox is exercised
  const auto r = sp::kkt_residual(p, sol->w, sol->metric_scale);
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_LT(*r, 1.01 * o.tolerance);
  // The certificate is not vacuous: w0 itself is far from optimal.
  const auto r0 = sp::kkt_residual(p, p.w0, sol->metric_scale);
  ASSERT_TRUE(r0);
  EXPECT_GT(*r0, 1e3 * o.tolerance);
}
TEST(SpoSolver, CostFreeUnboundedSolutionIsTheClosedFormMarkowitz) {
  auto p = random_problem(50, 23);
  const usize n = p.n;
  p.linear_cost.assign(n, 0.0); p.impact_cost.assign(n, 0.0);
  p.long_rate.assign(n, 0.0); p.short_rate.assign(n, 0.0);
  p.w0.assign(n, 0.0); p.lower.assign(n, -inf); p.upper.assign(n, inf);
  p.gross = inf; p.beta_lo = -inf; p.beta_hi = inf; p.net = 0; p.gamma = 100;
  const auto sol = sp::solve(p, std::vector<f64>(n, 0.0), options(50000, 1e-13));
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  // max a'w - gamma/2 w'Sigma w s.t. 1'w = 0: w = Sigma^-1 (a - nu 1) / gamma,
  // nu = 1'Sigma^-1 a / 1'Sigma^-1 1.
  const auto sigma = dense_sigma(p);
  const auto x = spd_solve(sigma, p.alpha);
  const auto y = spd_solve(sigma, std::vector<f64>(n, 1.0));
  f64 sx = 0, sy = 0;
  for (usize i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; }
  std::vector<f64> w(n);
  f64 scale = 0;
  for (usize i = 0; i < n; ++i) {
    w[i] = (x[i] - sx / sy * y[i]) / p.gamma;
    scale = std::max(scale, std::abs(w[i]));
  }
  ASSERT_GT(scale, 1e-4);
  EXPECT_LT(max_abs_diff(sol->w, w), 1e-7 * scale) << "scale " << scale;
  f64 net = 0;
  for (const f64 v : sol->w) net += v;
  EXPECT_LT(std::abs(net), 1e-12);
}
f64 beta_of(const sp::Problem& p, std::span<const f64> w) {
  f64 b = 0;
  for (usize i = 0; i < p.n; ++i) b += p.beta[i] * w[i];
  return b;
}
TEST(SpoSolver, NetGrossBetaAndBoxesHoldTo1e10) {
  auto p = random_problem(60, 5);
  // Fixed positions outside the problem: net -0.004 held there, so the problem's net is +0.004.
  p.net = 0.004; p.gross = 0.2; p.beta_lo = -inf; p.beta_hi = inf;
  for (usize i = 0; i < p.n; ++i) p.alpha[i] += 3e-4 * (p.beta[i] - 1.0);
  // Beta free first: its optimum's beta b_free. A band of half |b_free| around 0 excludes that
  // optimum, so (strict convexity) the constrained optimum lies on the band's edge.
  const auto free = sp::solve(p, p.w0, options(20000, 1e-9));
  ASSERT_TRUE(free) << free.error().to_string();
  EXPECT_EQ(free->multipliers.rho, 0.0); // no band, no beta multiplier
  const f64 b_free = beta_of(p, free->w);
  ASSERT_GT(std::abs(b_free), 1e-6);
  p.beta_lo = -0.5 * std::abs(b_free); p.beta_hi = 0.5 * std::abs(b_free);
  const f64 edge = b_free > 0 ? p.beta_hi : p.beta_lo;
  const auto sol = sp::solve(p, p.w0, options(20000, 1e-9));
  ASSERT_TRUE(sol) << sol.error().to_string();
  EXPECT_TRUE(sol->converged) << sol->iterations;
  EXPECT_TRUE(sol->coupling_met);
  f64 net = 0, gross = 0, beta = 0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 w = sol->w[i];
    EXPECT_GE(w, p.lower[i]) << i;
    EXPECT_LE(w, p.upper[i]) << i;
    net += w; gross += std::abs(w); beta += p.beta[i] * w;
  }
  EXPECT_LT(std::abs(net - p.net), 1e-10);
  EXPECT_LE(gross, p.gross + 1e-10);
  EXPECT_GE(beta, p.beta_lo - 1e-10);
  EXPECT_LE(beta, p.beta_hi + 1e-10);
  // The gross budget binds with a positive multiplier mu = (alpha+ + alpha-) / 2 and is met
  // with equality; the beta band binds on the side of b_free with the multiplier's sign.
  EXPECT_TRUE(sol->multipliers.gross_binding);
  EXPECT_GT(sol->multipliers.pos + sol->multipliers.neg, 0.0);
  EXPECT_NEAR(gross, p.gross, 1e-10);
  EXPECT_NEAR(beta, edge, 1e-10);
  EXPECT_NE(sol->multipliers.rho, 0.0);
  EXPECT_EQ(sol->multipliers.rho > 0, b_free > 0);
}
// The quantities the gamma calibration bisects on the cost-free aim (no gross cap, so no gross
// multiplier): its variance is nonincreasing in gamma (Markowitz), and its gross crosses a target
// inside the bracket (near the caps at low gamma, shrunk at high gamma).
TEST(SpoSolver, CostFreeAimVarianceFallsAndGrossCrossesWithGamma) {
  auto p = random_problem(50, 29);
  const usize n = p.n;
  p.linear_cost.assign(n, 0.0); p.impact_cost.assign(n, 0.0);
  p.long_rate.assign(n, 0.0); p.short_rate.assign(n, 0.0);
  p.w0.assign(n, 0.0); p.lower.assign(n, -0.01); p.upper.assign(n, 0.01);
  p.gross = inf; p.net = 0; p.beta_lo = -0.02; p.beta_hi = 0.02;
  f64 previous = inf;
  for (const f64 gamma : {1.0, 10.0, 100.0, 1000.0, 10000.0}) {
    p.gamma = gamma;
    const auto sol = sp::solve(p, std::vector<f64>(n, 0.0), options(20000, 1e-10));
    ASSERT_TRUE(sol) << sol.error().to_string();
    EXPECT_TRUE(sol->converged) << gamma;
    EXPECT_FALSE(sol->multipliers.gross_binding) << gamma;
    f64 gross = 0;
    for (const f64 w : sol->w) gross += std::abs(w);
    const f64 variance = sp::objective_terms(p, sol->w).variance;
    EXPECT_GT(gross, 0.0) << gamma;
    EXPECT_LE(variance, previous * (1.0 + 1e-6)) << gamma;
    if (gamma == 1.0) EXPECT_GT(gross, 0.3) << "near the caps at low gamma";
    if (gamma == 10000.0) EXPECT_LT(gross, 0.3) << "shrunk at high gamma";
    previous = variance;
  }
}
TEST(SpoSolver, WarmAndColdStartsAgreeAndRepeatBitForBit) {
  const auto p = random_problem(50, 17);
  const auto o = options(50000, 1e-11);
  const auto warm = sp::solve(p, p.w0, o);
  const auto cold = sp::solve(p, std::vector<f64>(p.n, 0.0), o);
  ASSERT_TRUE(warm && cold);
  EXPECT_TRUE(warm->converged && cold->converged);
  EXPECT_LT(max_abs_diff(warm->w, cold->w), 1e-8);
  const auto again = sp::solve(p, p.w0, o);
  ASSERT_TRUE(again);
  ASSERT_EQ(again->w.size(), warm->w.size());
  for (usize i = 0; i < p.n; ++i) EXPECT_EQ(bits(again->w[i]), bits(warm->w[i])) << i;
  EXPECT_EQ(again->iterations, warm->iterations);
}
TEST(SpoSolver, TwoNamesMatchAOneDimensionalSearchWithTheThreeHalvesCost) {
  for (const u64 seed : {3ULL, 8ULL, 21ULL}) {
    auto p = random_problem(2, seed);
    p.gross = inf; p.beta_lo = -inf; p.beta_hi = inf; p.net = 0;
    p.lower = {-0.05, -0.05}; p.upper = {0.05, 0.05};
    p.w0 = {0.004, -0.001};
    p.linear_cost = {2e-5, 4e-5}; p.impact_cost = {3e-2, 1e-2};
    p.alpha[0] = 4e-4; p.alpha[1] = -2e-4;
    const auto sol = sp::solve(p, p.w0, options(50000, 1e-13));
    ASSERT_TRUE(sol) << sol.error().to_string();
    // Net 0: w = (u, -u); golden section on the strictly convex -objective(u).
    const auto cost = [&](f64 u) {
      const std::vector<f64> w{u, -u};
      return -sp::objective_terms(p, w).objective;
    };
    f64 lo = -0.05, hi = 0.05;
    const f64 g = 0.5 * (std::sqrt(5.0) - 1.0);
    f64 a = hi - g * (hi - lo), b = lo + g * (hi - lo), fa = cost(a), fb = cost(b);
    for (int k = 0; k < 400; ++k) {
      if (fa < fb) { hi = b; b = a; fb = fa; a = hi - g * (hi - lo); fa = cost(a); }
      else { lo = a; a = b; fa = fb; b = lo + g * (hi - lo); fb = cost(b); }
    }
    const f64 u = 0.5 * (lo + hi);
    EXPECT_NEAR(sol->w[0], u, 1e-7) << seed;
    EXPECT_NEAR(sol->w[1], -sol->w[0], 1e-12) << seed;
    EXPECT_LE(cost(sol->w[0]), cost(u) + 1e-15) << seed;
  }
}

// ---- fix-up 2: root causes of the rejected spo-v1 TRAIN cell ----------------------------------
// (1) No sign defect: a_i has the sign of desired_i. The scale defect: a_i = IC sigma z is the
// forecast over the IC's horizon h; spo-v1 (h = 1) charged all of it to every session while
// amortizing costs over H sessions -- sqrt(h) too much alpha per session.
TEST(SpoAlpha, OrientationFollowsDesiredAndTheHorizonScalesThePerSessionForecast) {
  const std::vector<f64> desired{0.3, -0.1, 0.0, 9.0, -0.2};
  const std::vector<u8> member{1, 1, 1, 0, 1};
  const f64 mean = (0.3 - 0.1 + 0.0 - 0.2) / 4.0;
  f64 ss = 0;
  for (usize i = 0; i < desired.size(); ++i)
    if (member[i]) ss += (desired[i] - mean) * (desired[i] - mean);
  const f64 sd = sp::member_sd(desired, member);
  EXPECT_NEAR(sd, std::sqrt(ss / 3.0), 1e-15); // the members' sample SD; nonmember 9.0 ignored
  EXPECT_EQ(sp::member_sd(desired, std::vector<u8>{0, 0, 0, 1, 0}), 0.0);
  const f64 specific = 0.02 * 0.02, ic = 0.02, h = 20.0;
  for (const f64 horizon : {1.0, h})
    for (usize i = 0; i < desired.size(); ++i) {
      const f64 a = sp::gk_alpha(ic, specific, desired[i] / sd, horizon);
      EXPECT_EQ(a > 0, desired[i] > 0) << i;
      EXPECT_EQ(a < 0, desired[i] < 0) << i;
    }
  const f64 z = 1.5;
  // h sessions of the per-session alpha are the h-session forecast IC (sigma sqrt(h)) z.
  EXPECT_NEAR(sp::gk_alpha(ic, specific, z, h) * h, ic * (std::sqrt(specific) * std::sqrt(h)) * z,
              1e-17);
  EXPECT_NEAR(sp::gk_alpha(ic, specific, z, 1.0) / sp::gk_alpha(ic, specific, z, h), std::sqrt(h),
              1e-12);
  EXPECT_EQ(sp::gk_alpha(ic, specific, z, 1.0), ic * std::sqrt(specific) * z); // spo-v1's alpha
}
// (3) The amortization defect is (1) seen from the cost side: a / sqrt(h) with gamma / sqrt(h)
// (the same cost-free aim) is spo-v1 with costs and financing weighted sqrt(h) -- the objective
// divided by sqrt(h), so the same solution -- and the penalized cost of a solution is
// nonincreasing in that weight: spo-v1 buys more trading for the same aim.
TEST(SpoSolver, HorizonConsistentAlphaIsASqrtHCostWeightAndTradesLess) {
  const auto base = random_problem(50, 37);
  const f64 root_h = std::sqrt(20.0);
  auto scaled = base;
  for (f64& a : scaled.alpha) a /= root_h;
  scaled.gamma = base.gamma / root_h;
  auto weighted = base;
  for (f64& c : weighted.linear_cost) c *= root_h;
  for (f64& c : weighted.impact_cost) c *= root_h;
  for (f64& c : weighted.long_rate) c *= root_h;
  for (f64& c : weighted.short_rate) c *= root_h;
  const auto o = options(50000, 1e-11);
  const auto v2 = sp::solve(scaled, base.w0, o);
  const auto same = sp::solve(weighted, base.w0, o);
  const auto v1 = sp::solve(base, base.w0, o);
  ASSERT_TRUE(v2 && same && v1);
  EXPECT_TRUE(v2->converged && same->converged && v1->converged);
  EXPECT_LT(max_abs_diff(v2->w, same->w), 1e-8);
  const auto penalty = [&](std::span<const f64> w) {
    const auto t = sp::objective_terms(base, w);
    return t.cost + t.financing;
  };
  EXPECT_LT(penalty(v2->w), penalty(v1->w));
}
// (2) spo-v1's gamma = max(gamma_vol, gamma_bind) >= gamma_bind puts the cost-free aim at gross
// <= L; from flat a trade cost only shrinks the book (the L1 norm of a lasso-penalized
// solution is nonincreasing in the penalty), so the gross budget cannot bind. spo-v2's
// gamma_vol < gamma_bind leaves the aim above L and the budget binds.
TEST(SpoCalibration, V1GammaKeepsTheCostedBookInsideTheBudgetAndV2Binds) {
  const auto p = random_problem(60, 41);
  const usize n = p.n;
  const f64 budget = 0.2, scale = sp::estimate_metric_scale(p);
  std::vector<f64> size(n);
  for (usize i = 0; i < n; ++i) size[i] = std::abs(p.alpha[i]);
  std::nth_element(size.begin(), size.begin() + static_cast<std::ptrdiff_t>(n / 2), size.end());
  const f64 median = size[n / 2];
  sp::SpoParams v1;
  v1.w_max = 0.02;
  v1.target_vol = 1.0; // out of reach at the caps: gamma = gamma_bind
  sp::Calibration c1;
  ASSERT_TRUE(sp::calibrate_gamma(p, v1, budget, scale, c1));
  EXPECT_TRUE(c1.done);
  EXPECT_FALSE(c1.vol_reached);
  EXPECT_EQ(c1.gamma, c1.gamma_bind);
  EXPECT_NEAR(c1.aim_gross, budget, 1e-4 * budget);
  // The live problem: the calibration aim's constraints plus the budget and a trade cost.
  auto live = p;
  live.w0.assign(n, 0.0); live.lower.assign(n, -v1.w_max); live.upper.assign(n, v1.w_max);
  live.impact_cost.assign(n, 0.0); live.long_rate.assign(n, 0.0); live.short_rate.assign(n, 0.0);
  live.net = 0; live.beta_lo = -v1.beta_max; live.beta_hi = v1.beta_max; live.gross = budget;
  live.gamma = c1.gamma;
  live.linear_cost.assign(n, 0.3 * median);
  const auto gross_of = [](std::span<const f64> w) {
    f64 g = 0;
    for (const f64 v : w) g += std::abs(v);
    return g;
  };
  const auto inside = sp::solve(live, std::vector<f64>(n, 0.0), options(20000, 1e-10));
  ASSERT_TRUE(inside) << inside.error().to_string();
  EXPECT_FALSE(inside->multipliers.gross_binding);
  EXPECT_LT(gross_of(inside->w), 0.9 * budget);
  // spo-v2: gamma = gamma_vol for a vol target twice the aim's vol at gamma_bind.
  sp::SpoParams v2 = sp::v2_params();
  v2.w_max = v1.w_max;
  v2.target_vol = 2.0 * c1.aim_vol;
  sp::Calibration c2;
  ASSERT_TRUE(sp::calibrate_gamma(p, v2, budget, scale, c2));
  EXPECT_TRUE(c2.done);
  EXPECT_EQ(c2.gamma, c2.gamma_vol);
  EXPECT_LT(c2.gamma_vol, c2.gamma_bind);
  EXPECT_NEAR(c2.aim_vol, v2.target_vol, 1e-3 * v2.target_vol);
  EXPECT_GT(c2.aim_gross, budget);
  live.gamma = c2.gamma;
  live.linear_cost.assign(n, 0.01 * median);
  const auto binding = sp::solve(live, std::vector<f64>(n, 0.0), options(20000, 1e-10));
  ASSERT_TRUE(binding) << binding.error().to_string();
  EXPECT_TRUE(binding->multipliers.gross_binding);
  EXPECT_NEAR(gross_of(binding->w), budget, 1e-10);
  // spo-v2 refuses a vol target the aim cannot reach instead of using a bracket end.
  v2.target_vol = 1.0;
  sp::Calibration c3;
  const auto refused = sp::calibrate_gamma(p, v2, budget, scale, c3);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), co::ErrorCode::Unavailable);
  EXPECT_FALSE(c3.done);
}

// ---- synthetic atx-risk-v1 output ------------------------------------------------------------
struct Directory {
  std::filesystem::path path;
  Directory() {
    static std::atomic<u64> counter{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
        ("atx_spo_" + std::to_string(tick) + "_" + std::to_string(counter.fetch_add(1)));
    if (!std::filesystem::create_directory(path)) throw std::runtime_error("fixture directory");
  }
  ~Directory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
  Directory(const Directory&) = delete;
  Directory& operator=(const Directory&) = delete;
};
template <class T>
Json write_payload(const std::filesystem::path& file, const std::vector<T>& values) {
  std::ofstream out(file, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(values));
  // SAFETY: char writes the object representation of contiguous arithmetic fixture data.
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) throw std::runtime_error("fixture payload write");
  return Json{{"bytes", bytes.size()}, {"sha256", co::sha256_file(file.string()).value()}};
}
// The files the `risk` verb writes with --emit-exposures all, for `sessions` x `names`;
// forecast[d] == 0 leaves date d unforecast (NaN covariance and specific variance);
// specific_overrides sets entries (d x names + i, value) of the specific variance.
// Returns the manifest's SHA-256.
std::string write_risk_model(const std::filesystem::path& dir, std::span<const i64> sessions,
                             usize names, std::span<const u8> forecast, const std::string& role,
                             u64 seed,
                             std::span<const std::pair<usize, f64>> specific_overrides = {}) {
  Lcg rng{seed};
  const usize dates = sessions.size(), k = sp::risk_factors, s = sp::risk_styles;
  std::vector<f64> cov(dates * k * k, missing), spec(dates * names, missing);
  std::vector<f32> styles(dates * names * s);
  std::vector<u8> slots(dates * names);
  std::vector<f64> v(k);
  for (f64& x : v) x = rng.uniform(-1e-3, 1e-3);
  for (usize d = 0; d < dates; ++d) {
    for (usize i = 0; i < names; ++i) {
      slots[d * names + i] = static_cast<u8>(i % 5);
      for (usize c = 0; c < s; ++c)
        styles[(d * names + i) * s + c] = static_cast<f32>(rng.uniform(-1.5, 1.5));
      if (forecast[d]) {
        const f64 vol = 0.012 + 0.004 * static_cast<f64>(i % 4);
        spec[d * names + i] = vol * vol;
      }
    }
    if (!forecast[d]) continue;
    for (usize a = 0; a < k; ++a) {
      const f64 var = a == 0 ? 1e-4 : a <= sp::risk_industry_slots ? 2.5e-5 : 1e-5;
      for (usize b = 0; b < k; ++b)
        cov[(d * k + a) * k + b] = v[a] * v[b] + (a == b ? var : 0.0);
    }
  }
  for (const auto& [cell, value] : specific_overrides) spec.at(cell) = value;
  Json files = Json::object();
  files["factor_covariance.f64"] = write_payload(dir / "factor_covariance.f64", cov);
  files["specific_variance.f64"] = write_payload(dir / "specific_variance.f64", spec);
  files["style_exposures.f32"] = write_payload(dir / "style_exposures.f32", styles);
  files["industry_slot.u8"] = write_payload(dir / "industry_slot.u8", slots);
  {
    std::ofstream csv(dir / "diagnostics.csv", std::ios::binary);
    csv << "session,forecast,lambda2_factor,lambda2_specific,bias_factor,bias_specific,"
           "structural_names,specific_names\n";
    for (usize d = 0; d < dates; ++d)
      csv << sessions[d] << ',' << (forecast[d] ? 1 : 0) << ",1,1,nan,nan,0," << names << '\n';
  }
  files["diagnostics.csv"] =
      Json{{"bytes", std::filesystem::file_size(dir / "diagnostics.csv")},
           {"sha256", co::sha256_file((dir / "diagnostics.csv").string()).value()}};
  const Json manifest{{"schema", "atx.risk-model/v1"}, {"status", "complete"},
                      {"role", {{"path", "role/manifest.json"}, {"manifest_sha256", role}}},
                      {"geometry", {{"dates", dates}, {"instruments", names}, {"factors", k},
                                    {"styles", s}}},
                      {"files", files}};
  {
    std::ofstream out(dir / "manifest.json", std::ios::binary);
    out << manifest.dump(2) << '\n';
  }
  return co::sha256_file((dir / "manifest.json").string()).value();
}
std::vector<i64> weekdays(usize count) {
  std::vector<i64> out;
  auto date = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2};
  while (out.size() < count) {
    const std::chrono::weekday wd{date};
    if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
      out.push_back(static_cast<i64>(date.time_since_epoch().count()) * day_ns);
    date += std::chrono::days{1};
  }
  return out;
}

TEST(SpoRisk, RefusesADateWithoutForecastAnotherRoleAndAnotherPin) {
  const Directory dir;
  const auto sessions = weekdays(3);
  const std::vector<u8> forecast{0, 1, 1};
  const auto sha = write_risk_model(dir.path, sessions, 4, forecast, "role-sha", 7);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  EXPECT_EQ(store->dates(), 3U);
  EXPECT_EQ(store->instruments(), 4U);
  sp::RiskSlice slice;
  const auto lacking = store->read(0, slice);
  ASSERT_FALSE(lacking);
  EXPECT_EQ(lacking.error().code(), co::ErrorCode::Unavailable);
  EXPECT_NE(lacking.error().message().find("no forecast"), std::string::npos);
  ASSERT_TRUE(store->read(1, slice));
  EXPECT_EQ(slice.session, sessions[1]);
  EXPECT_EQ(slice.covariance.size(), sp::risk_factors * sp::risk_factors);
  EXPECT_EQ(slice.nan_covariance_entries, 0U);
  EXPECT_EQ(slice.slot[3], 3U);
  EXPECT_FALSE(store->read(3, slice)); // beyond the model
  EXPECT_TRUE(store->check_axes(sessions, 4));
  EXPECT_FALSE(store->check_axes(sessions, 5));
  EXPECT_FALSE(sp::RiskStore::open(dir.path.string(), sha, "another-role"));
  EXPECT_FALSE(sp::RiskStore::open(dir.path.string(), std::string(64, '0'), "role-sha"));
  ASSERT_TRUE(std::filesystem::remove(dir.path / "style_exposures.f32")); // not "all"
  EXPECT_FALSE(sp::RiskStore::open(dir.path.string(), sha, "role-sha"));
}

// (4) exante_vol_current was computed correctly from a corrupt risk model (TRAIN: daily
// specific variance 1e0..9e12 on 177-179 names for 20 sessions). The plausibility ceiling
// clamps and counts such entries; inf (spo-v1) leaves the slice alone.
TEST(SpoRisk, ImplausibleSpecificVarianceIsCappedAndCounted) {
  sp::RiskSlice slice;
  slice.specific = {4e-4, 1e12, missing, 0.9, 1.5e4, 1.0};
  sp::RiskSlice untouched = slice;
  EXPECT_EQ(sp::cap_specific(untouched, inf), 0U);
  EXPECT_EQ(untouched.capped_specific, 0U);
  EXPECT_EQ(untouched.specific[1], 1e12);
  EXPECT_EQ(sp::cap_specific(slice, 1.0), 2U);
  EXPECT_EQ(slice.capped_specific, 2U);
  EXPECT_EQ(slice.specific[0], 4e-4);
  EXPECT_EQ(slice.specific[1], 1.0);
  EXPECT_TRUE(std::isnan(slice.specific[2]));
  EXPECT_EQ(slice.specific[3], 0.9);
  EXPECT_EQ(slice.specific[4], 1.0);
  EXPECT_EQ(slice.specific[5], 1.0); // at the ceiling: kept, not counted
}

// ---- the NAV hook ------------------------------------------------------------------------------
// Random-walk role: every name present, ADV rising with the index, name n-1 leaves membership
// for a stretch (the exit path). Signals random per cell.
struct Role {
  usize d{}, n{};
  std::vector<f64> signal, close, raw, volume;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Role(usize dates, usize names, u64 seed)
      : d(dates), n(names), signal(dates * names), close(dates * names), raw(dates * names),
        volume(dates * names), member(dates * names, 1), present(dates * names, 1),
        sessions(weekdays(dates)), ids(names) {
    Lcg rng{seed};
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
    for (usize t = 0; t < d; ++t)
      for (usize i = 0; i < n; ++i) {
        const usize k = t * n + i;
        close[k] = t == 0 ? 100.0 : close[k - n] * (1.0 + 0.04 * (rng.next() - 0.5));
        raw[k] = close[k];
        volume[k] = 1e5 * static_cast<f64>(1 + 2 * i) * (0.5 + rng.next());
        signal[k] = rng.next();
      }
    for (usize t = d / 3; t < d / 2; ++t) {
      member[t * n + n - 1] = 0; signal[t * n + n - 1] = missing;
    }
  }
  [[nodiscard]] st::TargetReplayInput target() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present, volume};
  }
  [[nodiscard]] st::NavReplayInput nav() const { return {target(), volume}; }
};
st::NavReplayConfig nav_config() {
  st::NavReplayConfig c;
  c.target.rule = st::TargetReplayRule::AimPartialV5; c.target.cadence = 1;
  c.target.trade_fraction = 0.25; c.target.dust_multiple = 0.1; c.target.aim_leverage = 1.2;
  c.target.exit_rate = 0.05;
  c.scenario = st::fixed_nav_scenarios()[st::nav_primary_scenario_index];
  c.initial_nav = 1e8; c.liquidity_window = 4; c.min_vol_pairs = 2;
  return c;
}
atx::core::Result<v7::NavV7Command> parse_v7(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
}
bool claims(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::claims_nav_args(static_cast<int>(argv.size()), argv.data());
}

TEST(SpoHook, FlagOffKeepsAimPartialV5BitForBit) {
  const Role role(40, 12, 31);
  const auto cfg = nav_config();
  // The seam with the new tier / locate spans and no extension is update_weights bit for bit.
  const auto x = role.target();
  Lcg rng{9};
  const std::vector<u8> tier(role.n, u8{3}), no_locate(role.n, u8{1});
  for (const usize d : {5ULL, 17ULL, 30ULL}) {
    std::vector<f64> desired(role.n, 0.0), current(role.n);
    f64 mean = 0;
    for (usize i = 0; i < role.n; ++i) {
      desired[i] = role.member[d * role.n + i] ? rng.next() : 0.0;
      mean += desired[i];
      current[i] = 0.02 * (rng.next() - 0.5);
    }
    for (f64& v : desired) v -= mean / static_cast<f64>(role.n);
    std::vector<f64> plain = current, hooked = current;
    st::TargetReplayDay a, b;
    ASSERT_TRUE(st::detail::update_weights(x, cfg.target, d, true, 0.0, desired, plain, a));
    ASSERT_TRUE(v7::plan(x, cfg, d, true, 0.0, 1e8, desired, hooked, b, {}, tier, no_locate));
    for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(plain[i]), bits(hooked[i])) << d << ' ' << i;
    EXPECT_EQ(bits(a.turnover), bits(b.turnover));
    EXPECT_EQ(bits(a.gross), bits(b.gross));
    EXPECT_EQ(a.construction.banded_names, b.construction.banded_names);
  }
  // A whole replay with an extension that only observes (no rule) keeps every planned weight.
  const auto plain = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(plain) << plain.error().to_string();
  const v7::ScopedNavExtension extension(v7::NavV7Options{});
  EXPECT_EQ(extension.spo_engine(), nullptr);
  const auto hooked = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(hooked) << hooked.error().to_string();
  ASSERT_EQ(plain->days.size(), hooked->days.size());
  for (usize t = 0; t < plain->days.size(); ++t) {
    const auto& p = plain->days[t]; const auto& h = hooked->days[t];
    EXPECT_EQ(bits(p.planned_gross), bits(h.planned_gross)) << t;
    EXPECT_EQ(bits(p.planned_net), bits(h.planned_net)) << t;
    EXPECT_EQ(bits(p.planned_turnover), bits(h.planned_turnover)) << t;
    EXPECT_EQ(bits(p.net_return), bits(h.net_return)) << t;
    EXPECT_EQ(p.construction.banded_names, h.construction.banded_names) << t;
  }
  // Without --rule spo-v1 the parse leaves the rule and every token alone.
  const auto parsed = parse_v7({"nav", "--rule", "aim-partial-v5", "--output", "x"});
  ASSERT_TRUE(parsed);
  EXPECT_FALSE(parsed->options.spo_v1);
  EXPECT_EQ(parsed->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5", "--output",
                                                    "x"}));
}
TEST(SpoHook, ParseRoutesTheRuleAndRefusesBadCombinations) {
  const std::vector<std::string> base{"nav", "--rule", "spo-v1", "--risk-model", "risk",
                                      "--risk-model-sha256", "abc", "--trade-fraction", ".05",
                                      "--output", "x"};
  const auto with = [&](std::initializer_list<std::string> extra) {
    auto args = base;
    args.insert(args.end(), extra.begin(), extra.end());
    return args;
  };
  const auto parsed =
      parse_v7(with({"--gamma", "5", "--spo-books", "primary", "--spo-iters", "300"}));
  ASSERT_TRUE(parsed) << parsed.error().to_string();
  const auto& o = parsed->options;
  EXPECT_TRUE(o.spo_v1); EXPECT_FALSE(o.aim_v6);
  EXPECT_EQ(o.spo_params.gamma, 5.0);
  EXPECT_FALSE(o.spo_params.all_books);
  EXPECT_EQ(o.spo_params.max_iterations, 300U);
  EXPECT_EQ(o.spo_params.ic_book, 0.02); EXPECT_EQ(o.spo_params.w_max, 0.01);
  EXPECT_EQ(o.spo_params.adv_cap_q, 0.05); EXPECT_EQ(o.spo_params.adv_trade_p, 0.01);
  EXPECT_EQ(o.spo_params.tolerance, 1e-8); EXPECT_EQ(o.spo_params.target_vol, 0.05);
  EXPECT_TRUE(std::isnan(o.spo_params.horizon)); // 1 / theta at the first decision
  EXPECT_EQ(parsed->risk_model, "risk");
  EXPECT_EQ(parsed->risk_model_sha256, "abc");
  EXPECT_EQ(parsed->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5",
                                                    "--trade-fraction", ".05", "--output", "x"}));
  EXPECT_TRUE(claims({"nav", "--rule", "spo-v1"}));
  EXPECT_TRUE(claims({"nav", "--gamma", "1"}));
  EXPECT_FALSE(parse_v7({"nav", "--rule", "spo-v1", "--output", "x"})); // no risk model
  EXPECT_FALSE(parse_v7({"nav", "--gamma", "5", "--output", "x"}));     // no rule
  EXPECT_FALSE(parse_v7(with({"--capacity-curve"})));
  EXPECT_FALSE(parse_v7(with({"--rate", "per-name-v1"})));
  EXPECT_FALSE(parse_v7(with({"--spo-books", "some"})));
  EXPECT_FALSE(parse_v7(with({"--spo-iters", "-1"})));
  EXPECT_FALSE(parse_v7(with({"--ic-book", "0"})));
  EXPECT_FALSE(parse_v7(with({"--gamma", "1", "--gamma", "2"})));
  std::vector<std::string> both = base;
  both.insert(both.end(), {"--rule", "aim-partial-v6"});
  EXPECT_FALSE(parse_v7(both));
  // spo-v1 keeps its pre-registered semantics.
  EXPECT_EQ(o.spo_params.version, 1U);
  EXPECT_EQ(o.spo_params.alpha_horizon, 1.0);
  EXPECT_TRUE(std::isinf(o.spo_params.specific_ceiling));
  EXPECT_EQ(o.spo_params.gamma_rule, sp::GammaRule::VolAndBind);
  EXPECT_STREQ(sp::rule_name(o.spo_params), "spo-v1");
}
TEST(SpoHook, ParseSpoV2AppliesTheFixUpDefaultsUnderTheFlagsGiven) {
  const std::vector<std::string> tail{"--risk-model", "risk", "--risk-model-sha256", "abc",
                                      "--output", "x"};
  const auto parse = [&](std::vector<std::string> head) {
    head.insert(head.end(), tail.begin(), tail.end());
    return parse_v7(std::move(head));
  };
  const auto v2 = parse({"nav", "--rule", "spo-v2"});
  ASSERT_TRUE(v2) << v2.error().to_string();
  const auto& p = v2->options.spo_params;
  EXPECT_TRUE(v2->options.spo_v1);
  EXPECT_EQ(p.version, 2U);
  EXPECT_TRUE(std::isnan(p.alpha_horizon)); // h = H
  EXPECT_EQ(p.specific_ceiling, 1.0);
  EXPECT_EQ(p.gamma_rule, sp::GammaRule::Vol);
  EXPECT_EQ(p.ic_book, 0.02); EXPECT_EQ(p.w_max, 0.01); EXPECT_EQ(p.target_vol, 0.05);
  EXPECT_STREQ(sp::rule_name(p), "spo-v2");
  EXPECT_EQ(v2->args, (std::vector<std::string>{"nav", "--rule", "aim-partial-v5", "--output",
                                                "x"}));
  // A flag overrides the rule's default wherever it stands.
  const auto early = parse({"nav", "--alpha-horizon", "5", "--specific-ceiling", "inf", "--rule",
                            "spo-v2"});
  ASSERT_TRUE(early) << early.error().to_string();
  EXPECT_EQ(early->options.spo_params.alpha_horizon, 5.0);
  EXPECT_TRUE(std::isinf(early->options.spo_params.specific_ceiling));
  EXPECT_EQ(early->options.spo_params.version, 2U);
  EXPECT_TRUE(claims({"nav", "--rule", "spo-v2"}));
  EXPECT_TRUE(claims({"nav", "--alpha-horizon", "20"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v2", "--alpha-horizon", "0.5"}));
  EXPECT_FALSE(parse({"nav", "--rule", "spo-v2", "--specific-ceiling", "0"}));
  EXPECT_FALSE(parse_v7({"nav", "--alpha-horizon", "5", "--output", "x"})); // no rule
  Json recipe{{"rule", "aim-partial-v5+neutral-price-risk-v1"}};
  {
    const v7::ScopedNavExtension extension(v2->options);
    v7::extend_recipe(recipe);
  }
  EXPECT_EQ(recipe["rule"], "spo-v2+neutral-price-risk-v1");
}
TEST(SpoHook, ReplayPlansNeutralBudgetedBooksAndRelabelsTheRule) {
  const Directory dir;
  const Role role(40, 12, 53);
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 3);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const auto cfg = nav_config();
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(result) << result.error().to_string();
  const auto* engine = extension.spo_engine();
  ASSERT_NE(engine, nullptr);
  EXPECT_TRUE(engine->calibration().done);
  EXPECT_GT(engine->calibration().gamma, 0.0);
  EXPECT_EQ(engine->horizon(), 4.0); // 1 / theta
  const auto rows = engine->rows();
  ASSERT_FALSE(rows.empty());
  f64 traded = 0;
  for (const auto& r : rows) {
    EXPECT_TRUE(r.converged) << r.session;
    EXPECT_TRUE(r.coupling_met) << r.session;
    EXPECT_LE(r.gross, cfg.target.aim_leverage + 1e-9) << r.session;
    EXPECT_LT(std::abs(r.net), 1e-9) << r.session;
    EXPECT_LE(r.abs_beta, 0.02 + 1e-9) << r.session;
    EXPECT_TRUE(std::isfinite(r.exante_vol)) << r.session;
    traded += r.turnover;
  }
  EXPECT_GT(traded, 0.0);
  for (const auto& day : result->days)
    if (day.decision && day.rebalance) EXPECT_LT(std::abs(day.planned_net), 1e-9) << day.session;
  EXPECT_FALSE(extension.tc_records().empty()); // the L4 transfer coefficient is still recorded
  const auto csv = sp::diagnostics_csv(rows);
  EXPECT_EQ(static_cast<usize>(std::count(csv.begin(), csv.end(), '\n')), rows.size() + 1);
  Json recipe{{"rule", "aim-partial-v5+neutral-price-risk-v1"}};
  v7::extend_recipe(recipe);
  EXPECT_EQ(recipe["rule"], "spo-v1+neutral-price-risk-v1");
  ASSERT_TRUE(recipe.contains("v7"));
  EXPECT_TRUE(recipe["v7"].contains("spo_v1"));
  // Deterministic: a second replay under a fresh extension plans the same weights.
  const v7::ScopedNavExtension again(o);
  const auto repeat = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(repeat);
  ASSERT_EQ(repeat->days.size(), result->days.size());
  for (usize t = 0; t < result->days.size(); ++t)
    EXPECT_EQ(bits(repeat->days[t].planned_gross), bits(result->days[t].planned_gross)) << t;
}
TEST(SpoHook, CalibrationMeetsTheBindingOfTheVolTargetAndTheGrossBudget) {
  const Directory dir;
  const Role role(30, 12, 71);
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 9);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_params.w_max = 0.5; // 12 names at 0.5 reach gross 6 > L = 1.2: both targets reachable
  o.spo_risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const auto cfg = nav_config();
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(result) << result.error().to_string();
  const auto& c = extension.spo_engine()->calibration();
  ASSERT_TRUE(c.done);
  EXPECT_FALSE(c.from_flag);
  EXPECT_TRUE(c.vol_reached);
  EXPECT_TRUE(c.bind_reached);
  EXPECT_EQ(c.gamma, std::max(c.gamma_vol, c.gamma_bind));
  const f64 gross_ratio = c.aim_gross / cfg.target.aim_leverage;
  const f64 vol_ratio = c.aim_vol / o.spo_params.target_vol;
  EXPECT_LE(vol_ratio, 1.0 + 1e-4); // gamma >= gamma_vol and the aim's vol falls with gamma
  if (c.gamma_bind >= c.gamma_vol) {
    EXPECT_NEAR(gross_ratio, 1.0, 1e-4); // the gross budget binds
  } else {
    EXPECT_NEAR(vol_ratio, 1.0, 1e-4); // the vol target binds
  }
}
TEST(SpoHook, ReplayRefusesWhenTheRiskModelLacksADecisionDate) {
  const Directory dir;
  const Role role(30, 10, 61);
  std::vector<u8> forecast(role.d, u8{1});
  forecast[12] = 0;
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 5);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), nav_config());
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().message().find("no forecast"), std::string::npos)
      << result.error().to_string();
}
// The shadow book: the plan-level aim-partial-v5 book beside the optimiser, scored with the same
// alpha. From flat its first plan is aim-partial-v5's own first plan.
TEST(SpoHook, ShadowBookScoresTheAimPartialPlanBesideTheOptimiser) {
  const Directory dir;
  const Role role(40, 12, 53);
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 3);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  const auto cfg = nav_config();
  const auto plain = st::replay_nav(role.nav(), cfg); // aim-partial-v5, no extension
  ASSERT_TRUE(plain) << plain.error().to_string();
  v7::NavV7Options o;
  o.spo_v1 = true;
  o.spo_risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const v7::ScopedNavExtension extension(o);
  const auto result = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(result) << result.error().to_string();
  const auto rows = extension.spo_engine()->rows();
  ASSERT_FALSE(rows.empty());
  const st::NavReplayDay* first = nullptr;
  for (const auto& day : plain->days)
    if (day.decision && day.rebalance) { first = &day; break; }
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(rows.front().session, first->session);
  EXPECT_DOUBLE_EQ(rows.front().gross_shadow, first->planned_gross);
  EXPECT_DOUBLE_EQ(rows.front().turnover_shadow, first->planned_turnover);
  for (const auto& r : rows) {
    EXPECT_TRUE(std::isfinite(r.alpha_shadow)) << r.session;
    EXPECT_LE(r.gross_shadow, cfg.target.aim_leverage + 1e-9) << r.session;
    EXPECT_GE(r.turnover_shadow, 0.0) << r.session;
    EXPECT_GE(r.trade_cost_shadow, 0.0) << r.session;
    EXPECT_TRUE(std::isfinite(r.exante_vol_shadow)) << r.session;
  }
  const auto summary = sp::summary_json(rows);
  ASSERT_FALSE(summary.empty());
  for (const auto& entry : summary) {
    EXPECT_TRUE(entry.contains("shadow"));
    EXPECT_TRUE(entry.contains("alpha_capture")); // null when the shadow's alpha sums to <= 0
    EXPECT_TRUE(entry.at("holding_sessions").is_number());
    EXPECT_TRUE(entry.at("shadow").at("holding_sessions").is_number());
  }
  const auto csv = sp::diagnostics_csv(rows);
  EXPECT_NE(csv.find(",capped_specific,alpha_shadow,gross_shadow,turnover_shadow,"
                     "trade_cost_shadow,exante_vol_shadow\n"),
            std::string::npos);
}
// spo-v2 end to end: gamma is the vol-target gamma and the alpha is the per-session share of the
// h = H forecast (H = 1 / theta = 4 here): the shadow book is the same plan in both rules, so
// its score halves.
TEST(SpoHook, SpoV2CalibratesToTheVolTargetAndScalesAlphaToTheHorizon) {
  const Directory dir;
  const Role role(30, 12, 71);
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 9);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  const auto risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const auto cfg = nav_config();
  struct Run {
    std::vector<sp::DiagnosticRow> rows;
    sp::Calibration calibration;
    f64 horizon{};
  };
  const auto run = [&](sp::SpoParams params) {
    params.w_max = 0.5; // the vol target is reachable (see the spo-v1 calibration test)
    v7::NavV7Options o;
    o.spo_v1 = true;
    o.spo_params = params;
    o.spo_risk = risk;
    const v7::ScopedNavExtension extension(o);
    const auto result = st::replay_nav(role.nav(), cfg);
    EXPECT_TRUE(result) << result.error().to_string();
    const auto* engine = extension.spo_engine();
    const auto rows = engine->rows();
    return Run{std::vector<sp::DiagnosticRow>(rows.begin(), rows.end()), engine->calibration(),
               engine->horizon()};
  };
  const Run v1 = run(sp::SpoParams{});
  const Run v2 = run(sp::v2_params());
  ASSERT_FALSE(v1.rows.empty());
  ASSERT_FALSE(v2.rows.empty());
  EXPECT_EQ(v2.horizon, 4.0);
  const auto& c = v2.calibration;
  ASSERT_TRUE(c.done);
  EXPECT_EQ(c.rule, sp::GammaRule::Vol);
  EXPECT_EQ(c.gamma, c.gamma_vol);
  EXPECT_NEAR(c.aim_vol, 0.05, 1e-3 * 0.05);
  EXPECT_EQ(sp::parameters_json(sp::v2_params(), v2.horizon)["alpha_horizon"], 4.0);
  EXPECT_EQ(sp::parameters_json(sp::SpoParams{}, v1.horizon)["alpha_horizon"], 1.0);
  EXPECT_EQ(v1.rows.front().session, v2.rows.front().session);
  EXPECT_DOUBLE_EQ(v1.rows.front().gross_shadow, v2.rows.front().gross_shadow);
  EXPECT_DOUBLE_EQ(2.0 * v2.rows.front().alpha_shadow, v1.rows.front().alpha_shadow);
  for (const auto& r : v2.rows) EXPECT_EQ(r.capped_specific, 0U) << r.session; // clean model
}
// (4) end to end: a daily specific variance of 1e12 on a held name drives spo-v1's
// exante_vol_current (a correct formula on a corrupt input) far above any plausible book vol;
// the ceiling clamps it, counts it on exactly the corrupt decisions and bounds the column.
TEST(SpoHook, ACorruptSpecificVarianceIsCappedAndCountedInTheDiagnostics) {
  const Directory dir;
  const Role role(30, 12, 71);
  const std::vector<u8> forecast(role.d, u8{1});
  std::vector<std::pair<usize, f64>> corrupt;
  for (usize d = 10; d < 15; ++d) corrupt.emplace_back(d * role.n + 3, 1e12);
  const auto sha =
      write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", 9, corrupt);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  ASSERT_TRUE(store) << store.error().to_string();
  const auto risk = std::make_shared<const sp::RiskStore>(std::move(*store));
  const auto cfg = nav_config();
  const auto rows_with = [&](f64 ceiling) {
    v7::NavV7Options o;
    o.spo_v1 = true;
    o.spo_params.specific_ceiling = ceiling;
    o.spo_risk = risk;
    const v7::ScopedNavExtension extension(o);
    const auto result = st::replay_nav(role.nav(), cfg);
    EXPECT_TRUE(result) << result.error().to_string();
    const auto rows = extension.spo_engine()->rows();
    return std::vector<sp::DiagnosticRow>(rows.begin(), rows.end());
  };
  const auto raw = rows_with(inf), capped = rows_with(1.0);
  ASSERT_FALSE(raw.empty());
  ASSERT_EQ(raw.size(), capped.size());
  const auto corrupt_session = [&](i64 session) {
    for (usize d = 10; d < 15; ++d)
      if (role.sessions[d] == session) return true;
    return false;
  };
  usize corrupt_rows = 0;
  f64 worst = 0;
  for (usize k = 0; k < raw.size(); ++k) {
    const bool bad = corrupt_session(raw[k].session);
    corrupt_rows += bad ? 1U : 0U;
    EXPECT_EQ(raw[k].capped_specific, 0U) << k;
    EXPECT_EQ(capped[k].capped_specific, bad ? 1U : 0U) << k;
    EXPECT_LT(capped[k].exante_vol_current, 1.0) << k;
    EXPECT_LT(capped[k].exante_vol, 1.0) << k;
    if (bad) worst = std::max(worst, raw[k].exante_vol_current);
  }
  EXPECT_GT(corrupt_rows, 0U);
  EXPECT_GT(worst, 1.0);
}
} // namespace

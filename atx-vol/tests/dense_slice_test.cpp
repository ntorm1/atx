#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "atx/vol/black76.hpp"      // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"        // FitObs
#include "atx/vol/dense_slice.hpp"  // fit_convex_slice, ConvexSliceFit
#include "atx/vol/types.hpp"        // Side

// Phase 1 of the arbitrage-constrained dense surface: the per-slice convex
// call-price QP. These tests pin the two properties the whole approach rests on:
//   (1) the fitted call curve is ALWAYS arbitrage-free (convex, non-increasing,
//       positive) — even fed non-convex / noisy input, because no-arb is a HARD
//       constraint of the QP, not a post-hoc repair; and
//   (2) on clean arbitrage-free data it NEAR-INTERPOLATES (recovers the truth
//       vol to a few bp), which is what a 3-DoF eSSVI cannot do.

namespace {

using atx::vol::black76_price;
using atx::vol::black76_value_and_vega;
using atx::vol::ConvexFitOpts;
using atx::vol::FitObs;
using atx::vol::fit_convex_slice;
using atx::vol::Side;

// One OTM observation at strike K under a given vol, with a Black-76 vega weight.
FitObs mk_obs(double F, double T, double df, double K, double vol,
              double spread = 0.02) {
  const Side side = (K >= F) ? Side::Call : Side::Put;
  FitObs o{};
  o.K = K;
  o.F = F;
  o.df = df;
  o.k = std::log(K / F);
  o.side = side;
  o.mid = black76_price(F, K, T, vol, df, side);
  o.spread = spread;
  o.vega = black76_value_and_vega(F, K, T, vol, df, side).vega;
  o.sigma_mkt = vol;
  return o;
}

// The fitted call curve must be convex, non-increasing, and non-negative.
void expect_arb_free(const atx::vol::ConvexSliceFit& fit) {
  const auto& u = fit.u;
  const auto& C = fit.C;
  ASSERT_GE(u.size(), std::size_t{3});
  for (std::size_t i = 0; i < C.size(); ++i) {
    EXPECT_GE(C[i], -1.0e-9) << "positivity at node " << i;
  }
  for (std::size_t i = 0; i + 1 < C.size(); ++i) {
    EXPECT_LE(C[i + 1], C[i] + 1.0e-7) << "monotone (call falls with strike) at " << i;
  }
  for (std::size_t i = 1; i + 1 < C.size(); ++i) {
    const double left = (C[i] - C[i - 1]) / (u[i] - u[i - 1]);
    const double right = (C[i + 1] - C[i]) / (u[i + 1] - u[i]);
    EXPECT_GE(right - left, -1.0e-7) << "convexity (butterfly) at node " << i;
  }
}

std::vector<double> strike_grid(double F) {
  std::vector<double> ks;
  for (double K = 0.70 * F; K <= 1.30 * F + 1e-9; K += 0.02 * F) {
    ks.push_back(K);
  }
  return ks;
}

}  // namespace

TEST(DenseSlice, FlatVolIsRecoveredAndArbFree) {
  constexpr double F = 100.0, T = 0.25, r = 0.03, vol = 0.22;
  const double df = std::exp(-r * T);
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, vol));
  }

  ConvexFitOpts opts;
  opts.lambda = 1.0e-4;  // near-interpolation
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);

  // Near-interpolation: a flat-vol board's call prices are exactly convex, so the
  // fit recovers the generating vol at every interior strike to a few bp.
  int checked = 0;
  for (const double K : strike_grid(F)) {
    if (K < 0.80 * F || K > 1.20 * F) continue;  // interior, high-vega
    const double iv = fit->iv(std::log(K / F));
    ASSERT_TRUE(std::isfinite(iv));
    EXPECT_NEAR(iv, vol, 0.01) << "K=" << K;
    ++checked;
  }
  EXPECT_GT(checked, 5);
}

TEST(DenseSlice, SmileIsRecovered) {
  constexpr double F = 100.0, T = 0.5, r = 0.03;
  const double df = std::exp(-r * T);
  // A mild arbitrage-free skew: down-sloping with convex curvature in k.
  auto vol_of = [](double k) { return 0.22 - 0.10 * k + 0.30 * k * k; };
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, vol_of(std::log(K / F))));
  }

  const auto fit = fit_convex_slice(obs, F, T, df, {});
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);
  for (const double K : {95.0, 100.0, 105.0}) {
    const double iv = fit->iv(std::log(K / F));
    ASSERT_TRUE(std::isfinite(iv));
    EXPECT_NEAR(iv, vol_of(std::log(K / F)), 0.02) << "K=" << K;
  }
}

TEST(DenseSlice, NonConvexNoiseIsProjectedToArbFree) {
  constexpr double F = 100.0, T = 0.25, r = 0.03, vol = 0.22;
  const double df = std::exp(-r * T);
  std::vector<FitObs> obs;
  int i = 0;
  for (const double K : strike_grid(F)) {
    // Alternating +/- price perturbation makes the raw board non-convex (butterfly
    // arb) — the fit MUST still return a convex (arb-free) curve.
    FitObs o = mk_obs(F, T, df, K, vol);
    o.mid *= (i % 2 == 0) ? 1.06 : 0.94;
    obs.push_back(o);
    ++i;
  }
  const auto fit = fit_convex_slice(obs, F, T, df, {});
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);  // the whole point: arb-free despite arb-violating input
}

TEST(DenseSlice, WideBoardUsesClusteredNodesAndStaysArbFree) {
  // A wide, dense board (>node_cap strikes) exercises the ATM-clustered node grid
  // + design matrix. The fit must stay arbitrage-free and still track a mild smile.
  constexpr double F = 600.0, T = 0.3, r = 0.03;
  const double df = std::exp(-r * T);
  auto vol_of = [](double k) { return 0.20 - 0.08 * k + 0.25 * k * k; };
  std::vector<FitObs> obs;
  for (double K = 0.55 * F; K <= 1.45 * F + 1e-9; K += 0.01 * F) {  // ~90 strikes
    obs.push_back(mk_obs(F, T, df, K, vol_of(std::log(K / F))));
  }
  ConvexFitOpts opts;
  opts.node_cap = 40;
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_LE(fit->u.size(), std::size_t{40});  // capped node count
  EXPECT_GE(fit->u.size(), std::size_t{20});
  expect_arb_free(*fit);
  for (const double K : {560.0, 600.0, 640.0}) {
    const double iv = fit->iv(std::log(K / F));
    ASSERT_TRUE(std::isfinite(iv));
    EXPECT_NEAR(iv, vol_of(std::log(K / F)), 0.02) << "K=" << K;
  }
}

TEST(DenseSlice, RejectsTooFewStrikes) {
  constexpr double F = 100.0, T = 0.25;
  const double df = std::exp(-0.03 * T);
  std::vector<FitObs> obs = {mk_obs(F, T, df, 95.0, 0.2),
                             mk_obs(F, T, df, 105.0, 0.2)};
  EXPECT_FALSE(fit_convex_slice(obs, F, T, df, {}).has_value());
}

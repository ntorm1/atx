#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/greeks.hpp"

// Analytic Black-76 Greeks vs centred finite differences of the price,
// ported from the C ats-vol test_greeks_b76.c. Step sizes balance truncation
// against round-off (h ≈ ε^(1/3) for 1st order, ε^(1/4) for 2nd).

namespace {

using atx::vol::black76_greeks;
using atx::vol::black76_price;
using atx::vol::Greeks;
using atx::vol::Side;

// B76 price holding df = exp(-rT); r/T bumps must refresh df.
double price_b76(double F, double K, double T, double sigma, double r,
                 Side side) {
  return black76_price(F, K, T, sigma, std::exp(-r * T), side);
}

Greeks greeks_at(double F, double K, double T, double sigma, double r,
                 Side side) {
  return black76_greeks(F, K, T, sigma, r, std::exp(-r * T), side).greeks;
}

TEST(Greeks, DeltaCall_Atm_MatchesFd) {
  const double F = 100.0, K = 100.0, T = 0.25, sigma = 0.25, r = 0.04;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-3;
  const double fd = (price_b76(F + h, K, T, sigma, r, Side::Call) -
                     price_b76(F - h, K, T, sigma, r, Side::Call)) /
                    (2.0 * h);
  EXPECT_LT(std::fabs(g.delta - fd), 1.0e-7);
}

TEST(Greeks, DeltaPut_Otm_MatchesFd) {
  const double F = 100.0, K = 90.0, T = 0.5, sigma = 0.30, r = 0.03;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Put);
  const double h = 1.0e-3;
  const double fd = (price_b76(F + h, K, T, sigma, r, Side::Put) -
                     price_b76(F - h, K, T, sigma, r, Side::Put)) /
                    (2.0 * h);
  EXPECT_LT(std::fabs(g.delta - fd), 1.0e-7);
}

TEST(Greeks, Gamma_MatchesFd) {
  const double F = 100.0, K = 100.0, T = 0.25, sigma = 0.25, r = 0.04;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-2;
  const double up = price_b76(F + h, K, T, sigma, r, Side::Call);
  const double mid = price_b76(F, K, T, sigma, r, Side::Call);
  const double dn = price_b76(F - h, K, T, sigma, r, Side::Call);
  EXPECT_LT(std::fabs(g.gamma - (up - 2.0 * mid + dn) / (h * h)), 1.0e-4);
}

TEST(Greeks, Vega_MatchesFd) {
  const double F = 100.0, K = 110.0, T = 0.5, sigma = 0.30, r = 0.04;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-5;
  const double fd = (price_b76(F, K, T, sigma + h, r, Side::Call) -
                     price_b76(F, K, T, sigma - h, r, Side::Call)) /
                    (2.0 * h);
  EXPECT_LT(std::fabs(g.vega - fd), 1.0e-6);
}

TEST(Greeks, Rho_MatchesFd) {
  const double F = 100.0, K = 100.0, T = 1.0, sigma = 0.20, r = 0.05;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-5;
  const double fd = (price_b76(F, K, T, sigma, r + h, Side::Call) -
                     price_b76(F, K, T, sigma, r - h, Side::Call)) /
                    (2.0 * h);
  EXPECT_LT(std::fabs(g.rho - fd), 1.0e-6);
}

TEST(Greeks, Theta_MatchesFd) {
  const double F = 100.0, K = 105.0, T = 0.5, sigma = 0.25, r = 0.03;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-5;
  const double dpdT = (price_b76(F, K, T + h, sigma, r, Side::Call) -
                       price_b76(F, K, T - h, sigma, r, Side::Call)) /
                      (2.0 * h);
  EXPECT_LT(std::fabs(g.theta - (-dpdT)), 1.0e-5);
}

TEST(Greeks, Vanna_MatchesFd) {
  const double F = 100.0, K = 95.0, T = 0.4, sigma = 0.28, r = 0.04;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-4;
  const double fd = (greeks_at(F, K, T, sigma + h, r, Side::Call).delta -
                     greeks_at(F, K, T, sigma - h, r, Side::Call).delta) /
                    (2.0 * h);
  EXPECT_LT(std::fabs(g.vanna - fd), 1.0e-6);
}

TEST(Greeks, Volga_MatchesFd) {
  const double F = 100.0, K = 110.0, T = 0.5, sigma = 0.30, r = 0.04;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-3;
  const double mid = price_b76(F, K, T, sigma, r, Side::Call);
  const double up = price_b76(F, K, T, sigma + h, r, Side::Call);
  const double dn = price_b76(F, K, T, sigma - h, r, Side::Call);
  EXPECT_LT(std::fabs(g.volga - (up - 2.0 * mid + dn) / (h * h)), 1.0e-3);
}

TEST(Greeks, Charm_MatchesFd) {
  const double F = 100.0, K = 105.0, T = 0.5, sigma = 0.25, r = 0.03;
  const Greeks g = greeks_at(F, K, T, sigma, r, Side::Call);
  const double h = 1.0e-5;
  const double dDelta_dT =
      (greeks_at(F, K, T + h, sigma, r, Side::Call).delta -
       greeks_at(F, K, T - h, sigma, r, Side::Call).delta) /
      (2.0 * h);
  EXPECT_LT(std::fabs(g.charm - (-dDelta_dT)), 1.0e-4);
}

TEST(Greeks, SecondOrder_PutCallSymmetry) {
  const double F = 100.0, K = 110.0, T = 0.5, sigma = 0.30, r = 0.04;
  const double df = std::exp(-r * T);
  const Greeks gc = black76_greeks(F, K, T, sigma, r, df, Side::Call).greeks;
  const Greeks gp = black76_greeks(F, K, T, sigma, r, df, Side::Put).greeks;
  EXPECT_LT(std::fabs(gc.gamma - gp.gamma), 1.0e-14);
  EXPECT_LT(std::fabs(gc.vega - gp.vega), 1.0e-14);
  EXPECT_LT(std::fabs(gc.vanna - gp.vanna), 1.0e-14);
  EXPECT_LT(std::fabs(gc.volga - gp.volga), 1.0e-14);
  EXPECT_LT(std::fabs((gc.delta - gp.delta) - df), 1.0e-14);
}

// ── Plan item 2.5: the bundle's put price must use Φ(−d), not 1−Φ(d) ──────
//
// `black76_greeks` returns a price alongside the sensitivities, and callers
// (portfolio_price.cpp, bulk.cpp, the AVX2 batch kernels) consume it as THE
// Black-76 price. It computed the put leg from the 1−Φ(d) complement, which
// cancels catastrophically once d1, d2 ≫ 0: Φ(d) lands within an ulp of 1, so
// 1−Φ(d2) and 1−Φ(d1) round to the same multiple u of ε and the price
// degenerates to df·(K−F)·u — NEGATIVE for K < F, and disagreeing with
// `black76_price` by 100%+ relative on a genuinely positive premium. See
// black76_test.cpp for the same pin on the aux / value+vega entries.
TEST(Greeks, Price_WideGridIncludingDeepWings_MatchesBlack76Price) {
  constexpr double F = 100.0;
  constexpr double r = 0.03;
  constexpr double kFloor = -std::numeric_limits<double>::min();
  const double Ks[] = {60.0, 62.0, 64.0, 66.0,  68.0,  70.0,  72.0,
                       74.0, 76.0, 78.0, 80.0,  84.0,  88.0,  92.0,
                       96.0, 98.0, 100.0, 105.0, 110.0, 140.0, 200.0};
  const double Ts[] = {0.002, 0.01, 0.05, 0.25, 1.0};
  const double sigmas[] = {0.02, 0.04, 0.08, 0.12, 0.25};

  for (double K : Ks)
    for (double T : Ts)
      for (double sigma : sigmas) {
        const double df = std::exp(-r * T);
        for (Side side : {Side::Call, Side::Put}) {
          const double base = black76_price(F, K, T, sigma, df, side);
          const double got = black76_greeks(F, K, T, sigma, r, df, side).price;
          // A few dozen ULP of the base price: post-fix both call sites run the
          // same operation sequence, while the complement form misses by ≥ 100%
          // relative in the wing. The additive DBL_MIN absorbs the denormal
          // floor, where both Φ(−d) products are themselves denormals.
          const double tol =
              64.0 * std::numeric_limits<double>::epsilon() * std::fabs(base) +
              std::numeric_limits<double>::min();
          EXPECT_NEAR(got, base, tol)
              << "K=" << K << " T=" << T << " sig=" << sigma
              << " put=" << (side == Side::Put);
        }
        // A premium is never negative (the complement form returns ~-5e-15 in
        // the wing, 293 orders of magnitude past this floor).
        EXPECT_GE(black76_greeks(F, K, T, sigma, r, df, Side::Put).price, kFloor)
            << "K=" << K << " T=" << T << " sig=" << sigma;
      }
}

TEST(Greeks, SweepAllGreeksVsFd) {
  const double Ks[] = {85.0, 95.0, 100.0, 110.0};
  const double Ts[] = {0.10, 0.50, 1.00};
  const double sigmas[] = {0.15, 0.40};
  const Side sides[] = {Side::Call, Side::Put};
  const double F = 100.0, r = 0.03;

  int failures = 0;
  for (double K : Ks)
    for (double T : Ts)
      for (double sigma : sigmas)
        for (Side side : sides) {
          const Greeks g = greeks_at(F, K, T, sigma, r, side);

          const double hF = 1.0e-3;
          const double pF_up = price_b76(F + hF, K, T, sigma, r, side);
          const double pF_dn = price_b76(F - hF, K, T, sigma, r, side);
          const double pF_md = price_b76(F, K, T, sigma, r, side);
          if (std::fabs(g.delta - (pF_up - pF_dn) / (2.0 * hF)) > 1.0e-6)
            failures++;
          if (std::fabs(g.gamma - (pF_up - 2.0 * pF_md + pF_dn) / (hF * hF)) >
              1.0e-3)
            failures++;

          const double hS = 1.0e-4;
          const double pS_up = price_b76(F, K, T, sigma + hS, r, side);
          const double pS_dn = price_b76(F, K, T, sigma - hS, r, side);
          const double pS_md = price_b76(F, K, T, sigma, r, side);
          if (std::fabs(g.vega - (pS_up - pS_dn) / (2.0 * hS)) > 1.0e-5)
            failures++;
          if (std::fabs(g.volga - (pS_up - 2.0 * pS_md + pS_dn) / (hS * hS)) >
              1.0e-1)
            failures++;

          const double hR = 1.0e-5;
          if (std::fabs(g.rho - (price_b76(F, K, T, sigma, r + hR, side) -
                                 price_b76(F, K, T, sigma, r - hR, side)) /
                                    (2.0 * hR)) > 1.0e-5)
            failures++;

          const double hT = 1.0e-5;
          const double theta_fd = -(price_b76(F, K, T + hT, sigma, r, side) -
                                    price_b76(F, K, T - hT, sigma, r, side)) /
                                  (2.0 * hT);
          if (std::fabs(g.theta - theta_fd) > 1.0e-3)
            failures++;

          const double vanna_fd =
              (greeks_at(F, K, T, sigma + hS, r, side).delta -
               greeks_at(F, K, T, sigma - hS, r, side).delta) /
              (2.0 * hS);
          if (std::fabs(g.vanna - vanna_fd) > 1.0e-5)
            failures++;

          const double charm_fd =
              -(greeks_at(F, K, T + hT, sigma, r, side).delta -
                greeks_at(F, K, T - hT, sigma, r, side).delta) /
              (2.0 * hT);
          if (std::fabs(g.charm - charm_fd) > 1.0e-3)
            failures++;
        }
  EXPECT_EQ(failures, 0);
}

} // namespace

#include "atx/engine/quant/black_scholes.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace {
namespace bs = atx::engine::quant;

TEST(BlackScholes, AtmCallReference) {
  // S=K=100, T=1, r=q=0, sigma=0.2 -> 7.9655674...
  const double c = bs::bs_price(100, 100, 1.0, 0.0, 0.0, 0.2, true);
  EXPECT_NEAR(c, 7.9655674, 1e-4);
}

TEST(BlackScholes, PutCallParity) {
  const double S = 105, K = 100, T = 0.75, r = 0.043, q = 0.0, sig = 0.25;
  const double c = bs::bs_price(S, K, T, r, q, sig, true);
  const double p = bs::bs_price(S, K, T, r, q, sig, false);
  const double lhs = c - p;
  const double rhs = S * std::exp(-q * T) - K * std::exp(-r * T);
  EXPECT_NEAR(lhs, rhs, 1e-9);
}

TEST(BlackScholes, ImpliedVolRecoversSigma) {
  const double S = 98, K = 100, T = 0.5, r = 0.043, q = 0.0;
  for (const bool call : {true, false}) {
    for (const double sig : {0.1, 0.2, 0.45, 0.9}) {
      const double price = bs::bs_price(S, K, T, r, q, sig, call);
      const double iv = bs::implied_vol(price, S, K, T, r, q, call);
      EXPECT_NEAR(iv, sig, 1e-4) << "call=" << call << " sig=" << sig;
    }
  }
}

TEST(BlackScholes, GreeksMatchFiniteDifference) {
  const double S = 100, K = 95, T = 0.6, r = 0.043, q = 0.0, sig = 0.3;
  const bool call = true;
  const bs::Greeks g = bs::bs_greeks(S, K, T, r, q, sig, call);
  const double h = 1e-4;
  const double dS =
      (bs::bs_price(S + h, K, T, r, q, sig, call) - bs::bs_price(S - h, K, T, r, q, sig, call)) /
      (2 * h);
  const double d2S = (bs::bs_price(S + h, K, T, r, q, sig, call) -
                      2 * bs::bs_price(S, K, T, r, q, sig, call) +
                      bs::bs_price(S - h, K, T, r, q, sig, call)) /
                     (h * h);
  const double dSig =
      (bs::bs_price(S, K, T, r, q, sig + h, call) - bs::bs_price(S, K, T, r, q, sig - h, call)) /
      (2 * h);
  // theta per calendar day == -dPrice/dT / 365
  const double dT =
      (bs::bs_price(S, K, T + h, r, q, sig, call) - bs::bs_price(S, K, T - h, r, q, sig, call)) /
      (2 * h);
  EXPECT_NEAR(g.delta, dS, 1e-4);
  EXPECT_NEAR(g.gamma, d2S, 1e-3);
  EXPECT_NEAR(g.vega, dSig, 1e-3);
  EXPECT_NEAR(g.theta, -dT / 365.0, 1e-5);
}

TEST(BlackScholes, ImpliedVolGuards) {
  EXPECT_TRUE(std::isnan(bs::implied_vol(5.0, 100, 100, 0.0, 0.043, 0.0, true)));   // T=0
  EXPECT_TRUE(std::isnan(bs::implied_vol(-1.0, 100, 100, 0.5, 0.043, 0.0, true)));  // price<=0
  // price below intrinsic (deep ITM call worth < S-K*df) -> no solution
  const double df = std::exp(-0.043 * 0.5);
  const double intrinsic = 100 - 50 * df;
  EXPECT_TRUE(std::isnan(bs::implied_vol(intrinsic - 1.0, 100, 50, 0.5, 0.043, 0.0, true)));
}
} // namespace

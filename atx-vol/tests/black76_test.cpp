#include <gtest/gtest.h>

#include <cmath>

#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp" // black76_greeks — F8 vega bit-parity guard

// Black-76 pricer coverage, ported from the C ats-vol test_pricer_b76.c:
// put-call parity, intrinsic collapse, the aux/lnfk/value+vega variants
// agreeing with the base kernel, and vega via finite difference.

namespace {

using atx::vol::black76_aux;
using atx::vol::black76_greeks;
using atx::vol::black76_price;
using atx::vol::black76_price_from_lnfk;
using atx::vol::black76_value_and_vega;
using atx::vol::Side;

TEST(Black76, PutCallParity_Atm) {
  const double F = 100.0, K = 100.0, T = 0.25, sigma = 0.25, df = 0.99;
  const double c = black76_price(F, K, T, sigma, df, Side::Call);
  const double p = black76_price(F, K, T, sigma, df, Side::Put);
  EXPECT_LT(std::fabs(c - p), 1.0e-12); // df·(F-K) = 0 at ATM
  EXPECT_GT(c, 0.0);
  EXPECT_GT(p, 0.0);
}

TEST(Black76, PutCallParity_Otm) {
  const double F = 100.0, K = 110.0, T = 0.5, sigma = 0.30, df = 0.98;
  const double c = black76_price(F, K, T, sigma, df, Side::Call);
  const double p = black76_price(F, K, T, sigma, df, Side::Put);
  EXPECT_LT(std::fabs((c - p) - df * (F - K)), 1.0e-10);
}

TEST(Black76, Intrinsic_AtZeroSigma) {
  const double F = 110.0, K = 100.0, T = 0.25, df = 0.99;
  const double c = black76_price(F, K, T, 0.0, df, Side::Call);
  EXPECT_LT(std::fabs(c - df * (F - K)), 1.0e-12);
  const double p = black76_price(F, K, T, 0.0, df, Side::Put);
  EXPECT_LT(std::fabs(p), 1.0e-12);
}

TEST(Black76, Intrinsic_AtZeroT) {
  const double F = 90.0, K = 100.0, df = 0.99;
  const double p = black76_price(F, K, 0.0, 0.25, df, Side::Put);
  EXPECT_LT(std::fabs(p - df * (K - F)), 1.0e-12);
}

TEST(Black76, Aux_PriceMatchesBase) {
  const double F = 100.0, K = 108.0, T = 0.75, sigma = 0.22, df = 0.97;
  for (Side side : {Side::Call, Side::Put}) {
    const auto aux = black76_aux(F, K, T, sigma, df, side);
    EXPECT_NEAR(aux.price, black76_price(F, K, T, sigma, df, side), 1e-12);
    EXPECT_NEAR(aux.d2, aux.d1 - sigma * std::sqrt(T), 1e-12);
  }
}

TEST(Black76, FromLnfk_MatchesBase) {
  const double F = 100.0, K = 95.0, T = 0.4, sigma = 0.28, df = 0.985;
  const double ln_fk = std::log(F / K);
  const double sqrt_t = std::sqrt(T);
  for (Side side : {Side::Call, Side::Put}) {
    const double a =
        black76_price_from_lnfk(F, K, T, sigma, df, ln_fk, sqrt_t, side);
    const double b = black76_price(F, K, T, sigma, df, side);
    EXPECT_NEAR(a, b, 1e-13);
  }
}

TEST(Black76, ValueAndVega_MatchesFiniteDiff) {
  const double F = 100.0, K = 100.0, T = 0.25, sigma = 0.25, df = 0.99;
  const auto vv = black76_value_and_vega(F, K, T, sigma, df, Side::Call);
  EXPECT_NEAR(vv.price, black76_price(F, K, T, sigma, df, Side::Call), 1e-12);

  const double h = 1.0e-5;
  const double up = black76_price(F, K, T, sigma + h, df, Side::Call);
  const double dn = black76_price(F, K, T, sigma - h, df, Side::Call);
  EXPECT_LT(std::fabs(vv.vega - (up - dn) / (2.0 * h)), 1.0e-6);
}

// F8 (perf review): american_vega and the fused american_price_and_vega_cached
// use black76_value_and_vega's vega instead of the 9-output black76_greeks
// bundle's. That swap is only bit-identical if the two vegas agree to the last
// bit — same d1, φ(d1) = norm_pdf(d1) = (1/√2π)·exp(-½d1²), and F·df·φ(d1)·√T with
// F·df == df·F (IEEE commutative). Assert EXACT equality across a moneyness /
// maturity / vol grid, both sides. r feeds only black76_greeks' rho/theta, never
// its vega, so it is fixed arbitrarily here.
TEST(Black76, ValueAndVega_VegaBitIdenticalToGreeksBundle) {
  constexpr double r = 0.037;
  for (double F : {50.0, 100.0, 250.0}) {
    for (double K : {60.0, 100.0, 140.0, 300.0}) {
      for (double T : {0.02, 0.25, 1.0, 2.5}) {
        for (double sigma : {0.08, 0.20, 0.65}) {
          const double df = std::exp(-r * T);
          for (Side side : {Side::Call, Side::Put}) {
            const double fused = black76_value_and_vega(F, K, T, sigma, df, side).vega;
            const double bundle = black76_greeks(F, K, T, sigma, r, df, side).greeks.vega;
            EXPECT_EQ(fused, bundle) << "F=" << F << " K=" << K << " T=" << T << " sig=" << sigma;
          }
        }
      }
    }
  }
}

TEST(Black76, ValueAndVega_SqrtTSentinelMatches) {
  const double F = 100.0, K = 103.0, T = 0.6, sigma = 0.2, df = 0.98;
  const auto with_internal =
      black76_value_and_vega(F, K, T, sigma, df, Side::Put, -1.0);
  const auto with_supplied =
      black76_value_and_vega(F, K, T, sigma, df, Side::Put, std::sqrt(T));
  EXPECT_NEAR(with_internal.price, with_supplied.price, 1e-15);
  EXPECT_NEAR(with_internal.vega, with_supplied.vega, 1e-15);
}

} // namespace

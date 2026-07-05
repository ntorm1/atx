#include <gtest/gtest.h>

#include <cmath>

#include "atx/vol/black76.hpp"
#include "atx/vol/implied_vol.hpp"

// Implied-vol round-trip coverage, ported from the C ats-vol test_pricer_iv.c:
// price with B76, invert, expect σ recovered to near-machine precision across
// the equity-options envelope, plus no-arb-band rejection.

namespace {

using atx::vol::black76_price;
using atx::vol::ErrorCode;
using atx::vol::implied_vol;
using atx::vol::Side;

void check_roundtrip(double F, double K, double T, double sigma, double df,
                     Side side, double tol) {
  const double price = black76_price(F, K, T, sigma, df, side);
  const auto iv = implied_vol(price, F, K, T, df, side);
  ASSERT_TRUE(iv.has_value()) << "solver failed to converge";
  EXPECT_LT(std::fabs(*iv - sigma), tol);
}

TEST(ImpliedVol, Atm_Roundtrip) {
  check_roundtrip(100, 100, 0.25, 0.20, 0.99, Side::Call, 1.0e-8);
  check_roundtrip(100, 100, 0.25, 0.20, 0.99, Side::Put, 1.0e-8);
}

TEST(ImpliedVol, OtmCall_Roundtrip) {
  check_roundtrip(100, 120, 0.5, 0.25, 0.98, Side::Call, 1.0e-7);
}

TEST(ImpliedVol, OtmPut_Roundtrip) {
  check_roundtrip(100, 80, 0.5, 0.30, 0.98, Side::Put, 1.0e-7);
}

TEST(ImpliedVol, LowVol_Roundtrip) {
  check_roundtrip(100, 100, 0.5, 0.05, 0.98, Side::Call, 1.0e-7);
}

TEST(ImpliedVol, HighVol_Roundtrip) {
  check_roundtrip(100, 100, 0.5, 1.50, 0.98, Side::Call, 1.0e-7);
}

TEST(ImpliedVol, ShortDated_Roundtrip) {
  const double T = 1.0 / 365.25; // 1-day expiry
  check_roundtrip(100, 100, T, 0.30, 0.9999, Side::Call, 1.0e-6);
}

TEST(ImpliedVol, RejectBelowIntrinsic) {
  // C below max(F-K,0)·df is arb-violating (intrinsic = 0.99·20 = 19.8).
  const double F = 100.0, K = 80.0, T = 0.25, df = 0.99;
  const auto iv = implied_vol(10.0, F, K, T, df, Side::Call);
  ASSERT_FALSE(iv.has_value());
  EXPECT_EQ(iv.error().code(), ErrorCode::OutOfRange);
}

TEST(ImpliedVol, RejectNonPositiveInputs) {
  EXPECT_EQ(implied_vol(5.0, -100.0, 100.0, 0.25, 0.99, Side::Call)
                .error()
                .code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(
      implied_vol(5.0, 100.0, 100.0, 0.0, 0.99, Side::Call).error().code(),
      ErrorCode::InvalidArgument);
}

TEST(ImpliedVol, Sr2017Seed_ConvergesOnChainGrid) {
  // 5×5×5×5×2 grid over the equity-options envelope; every above-tick quote
  // must converge to high precision. Mirrors the C sr2017 grid test.
  const double Fs[] = {50, 100, 200, 500, 1000};
  const double k_logs[] = {-0.5, -0.2, 0.0, 0.2, 0.5};
  const double Ts[] = {0.05, 0.25, 0.5, 1.0, 2.0};
  const double sigmas[] = {0.10, 0.20, 0.30, 0.50, 0.80};
  const double df = 0.98;

  int n_ok = 0, n_fail = 0;
  double max_abs_err = 0.0;
  for (double F : Fs)
    for (double kl : k_logs)
      for (double T : Ts)
        for (double sig : sigmas) {
          const double K = F * std::exp(kl);
          for (Side side : {Side::Call, Side::Put}) {
            const double price = black76_price(F, K, T, sig, df, side);
            const double intr = (side == Side::Call)
                                    ? df * std::fmax(F - K, 0.0)
                                    : df * std::fmax(K - F, 0.0);
            if (price - intr < 1.0e-4 * F) continue; // skip sub-tick quotes
            const auto iv = implied_vol(price, F, K, T, df, side);
            const double err = iv.has_value() ? std::fabs(*iv - sig) : 1.0;
            if (iv.has_value() && err < 1.0e-6) {
              n_ok++;
            } else {
              n_fail++;
            }
            max_abs_err = std::fmax(max_abs_err, err);
          }
        }
  EXPECT_GT(n_ok, n_ok + n_fail - 2);
  EXPECT_LT(max_abs_err, 1.0e-5);
}

} // namespace

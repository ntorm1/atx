#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "atx/vol/black76.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/types.hpp" // kIvMin

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

// A4 (core-review finding 4): notional-scaled no-arb tolerances.
//
// At index notional (F≈5000) a legitimately at-intrinsic quote carries ~1e-12
// forward-unit rounding noise (the price/df division of a ~1e3-magnitude value).
// The pre-fix absolute 1e-15 no-arb band could never admit it — the acceptance
// band was effectively zero-width at this scale — so the quote was rejected
// OutOfRange instead of clamped to the vol floor. The band and the intrinsic
// clamp now scale with the price's own rounding-noise floor (ε·max(F,K), the
// same max(F,K) scaling the American front door and the K1 residual floor use),
// so the quote is admitted and clamps to kIvMin.
TEST(ImpliedVol, IndexScaleAtIntrinsic_ClampsToFloor) {
  const double F = 5000.0, K = 4000.0, T = 1.0, df = std::exp(-0.03);
  const double intr = F - K; // forward intrinsic = 1000
  // A quote 1e-12 (forward units) below intrinsic: inside the notional-scaled
  // band (ε·max(F,K) ≈ 8.9e-12) but far outside the old absolute 1e-15 band.
  const double price = df * (intr - 1.0e-12);
  const auto iv = implied_vol(price, F, K, T, df, Side::Call);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_DOUBLE_EQ(*iv, atx::vol::kIvMin);
}

// A4: the notional-scaled band must leave tiny-notional quotes unchanged. At
// F≈0.5 the band ε·max(F,K) ≈ 9e-16 tracks the old 1e-15 floor, so a genuine
// in-the-money quote still round-trips and a clearly sub-intrinsic quote is
// still rejected OutOfRange.
TEST(ImpliedVol, SmallNotionalBehaviorUnchanged) {
  const double F = 0.5, K = 0.45, T = 0.5, df = 0.99, sigma = 0.30;
  const double price = black76_price(F, K, T, sigma, df, Side::Call);
  const auto iv = implied_vol(price, F, K, T, df, Side::Call);
  ASSERT_TRUE(iv.has_value()) << (iv ? std::string{} : iv.error().to_string());
  EXPECT_NEAR(*iv, sigma, 1.0e-7);

  const double intr_disc = df * (F - K); // discounted intrinsic ≈ 0.0495
  const auto bad = implied_vol(intr_disc - 0.01, F, K, T, df, Side::Call);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::OutOfRange);
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

// Item 1.5: a true IV below the floor must REPORT the floor, not fail.
//
// types.hpp documents kIvMin as "the unified IV floor ... BOTH the reported floor
// of every inverter and the lower bound of the American IV search bracket, so no
// representable IV sits below it and there is no bracket-vs-report
// discontinuity". The Halley loop clamps σ into [kIvMin, kIvMax] after every
// step but tested the PRE-clamp `step` for termination, so a sub-floor quote
// pinned σ at kIvMin while `|step|` stayed large: the vol-step test never fired
// and the loop exhausted kIvMaxIter into a spurious Unavailable — the exact
// bracket-vs-report discontinuity the floor is documented to prevent.
//
// The grid stays at-the-money on purpose: away from ATM a sub-floor quote's time
// value falls under the no-arb noise floor and is handled earlier by the
// at-intrinsic clamp (already correct), so only ATM actually drives the solver
// into the floor.
TEST(ImpliedVol, TrueVolBelowFloor_ClampsToFloor) {
  const double df = 0.99;
  const double sigmas[] = {1.0e-3, 2.0e-3, 4.9e-3}; // all < kIvMin = 0.005
  for (double F : {100.0, 5000.0})
    for (double T : {0.05, 0.25, 1.0})
      for (double sig : sigmas)
        for (Side side : {Side::Call, Side::Put}) {
          const double K = F;
          const double price = black76_price(F, K, T, sig, df, side);
          const auto iv = implied_vol(price, F, K, T, df, side);
          ASSERT_TRUE(iv.has_value())
              << "F=" << F << " T=" << T << " sig=" << sig
              << " side=" << (side == Side::Call ? "C" : "P") << ": " << iv.error().to_string();
          EXPECT_DOUBLE_EQ(*iv, atx::vol::kIvMin) << "F=" << F << " T=" << T << " sig=" << sig;
        }
}

// Boundary: a quote priced exactly AT the floor inverts to the floor.
TEST(ImpliedVol, TrueVolAtFloor_ReturnsFloor) {
  const double F = 100.0, K = 100.0, T = 0.25, df = 0.99;
  for (Side side : {Side::Call, Side::Put}) {
    const double price = black76_price(F, K, T, atx::vol::kIvMin, df, side);
    const auto iv = implied_vol(price, F, K, T, df, side);
    ASSERT_TRUE(iv.has_value()) << iv.error().to_string();
    EXPECT_GE(*iv, atx::vol::kIvMin); // the floor is a hard lower bound
    EXPECT_NEAR(*iv, atx::vol::kIvMin, 1.0e-12);
  }
}

// The other side of the boundary must be untouched: a true IV just ABOVE the
// floor still round-trips to its own value, never collapsing onto the floor.
TEST(ImpliedVol, TrueVolJustAboveFloor_RoundTripsUnchanged) {
  const double F = 100.0, K = 100.0, T = 0.25, df = 0.99;
  const double sigmas[] = {5.001e-3, 6.0e-3, 1.0e-2, 5.0e-2};
  for (double sig : sigmas)
    for (Side side : {Side::Call, Side::Put}) {
      const double price = black76_price(F, K, T, sig, df, side);
      const auto iv = implied_vol(price, F, K, T, df, side);
      ASSERT_TRUE(iv.has_value()) << "sig=" << sig << ": " << iv.error().to_string();
      EXPECT_NEAR(*iv, sig, 1.0e-12) << "sig=" << sig;
      EXPECT_GT(*iv, atx::vol::kIvMin) << "sig=" << sig;
    }
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

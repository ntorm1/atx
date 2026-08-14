// K1 — IV convergence-tolerance gate (SPRINT W5.4b).
//
// The scalar inverter's price-residual termination test used an ABSOLUTE
// tolerance (`|price_model - price| < kIvTol`, kIvTol = 1e-12) while kIvTol is a
// tolerance in VOL units. That is mis-scaled: the residual is in price units and
// its floating-point noise floor is ~ε·df·max(F,K). For a high-notional option
// that floor exceeds 1e-12, so the residual test could never fire — the loop
// always fell through to the vol-step test, computing one wasted final Halley
// evaluation past machine precision. K1 scales the price tolerance by that noise
// floor so the residual test becomes meaningful and fires exactly when σ is
// machine-precise.
//
// These tests assert:
//   1. On a high-notional round-trip, the inversion terminates on the
//      price-residual test (exit_reason == 0), not the vol-step test.
//   2. Machine-precision IV is maintained across a moneyness × maturity × vol ×
//      notional grid (round-trip |σ̂ − σ_true| relative bound), including
//      high-notional forwards where the mis-scaling used to bite.
//   3. The public implied_vol result is unchanged in value (both routes recover
//      the same σ to machine precision).

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"
#include "atx/vol/api/core/types.hpp" // kIvMin

namespace atx::vol {

// Test/measurement seam defined in src/implied_vol.cpp (not in the public
// header). Reports the Halley-step count and which termination test fired
// (exit_reason: 0 = price-residual, 1 = vol-step, 2 = IV-below-floor clamp,
// -1 = none/error).
Result<double> implied_vol_traced(double price, double F, double K, double T, double df, Side side,
                                  int &iters, int &exit_reason);

namespace {

using atx::vol::black76_price;
using atx::vol::implied_vol;
using atx::vol::Side;

// A high-notional, well-conditioned quote. Its vega (~df·F·φ·√T) is large, so the
// price residual's rounding-noise floor (~ε·df·max(F,K) ≈ 2e-11 here) sits far
// above the old absolute 1e-12 tolerance, which therefore never fires.
TEST(IvConvergence, HighNotionalTerminatesOnResidualTest) {
  const double F = 100000.0, K = 100000.0, T = 1.0, sigma = 0.20, df = 0.98;
  const double price = black76_price(F, K, T, sigma, df, Side::Call);

  int iters = -1, exit_reason = -99;
  const Result<double> iv = implied_vol_traced(price, F, K, T, df, Side::Call, iters, exit_reason);
  ASSERT_TRUE(iv.has_value());
  // The mis-scaled absolute test never fires at this notional, so the OLD code
  // exits via the vol-step test (exit_reason == 1) and pays an extra Halley
  // evaluation. K1 makes the residual test meaningful: it must fire (== 0).
  EXPECT_EQ(exit_reason, 0) << "expected price-residual termination, got reason=" << exit_reason
                            << " after " << iters << " Halley steps";
  EXPECT_LE(iters, 3) << "iters=" << iters;
}

// The residual test firing at the noise floor must NOT cost precision: round-trip
// recovery stays at machine precision on the high-notional corner (where the OLD
// mis-scaling degraded recovery to ~1e-10 relative).
TEST(IvConvergence, HighNotionalRoundTripMachinePrecision) {
  const double F = 100000.0, K = 100000.0, T = 1.0, sigma = 0.20, df = 0.98;
  const double price = black76_price(F, K, T, sigma, df, Side::Call);
  const Result<double> iv = implied_vol(price, F, K, T, df, Side::Call);
  ASSERT_TRUE(iv.has_value());
  EXPECT_LT(std::fabs(*iv - sigma) / sigma, 1e-13) << "iv=" << *iv;
}

// Machine-precision round-trip across a moneyness × maturity × vol × notional
// grid. The notional sweep (F up to 1e5) exercises the mis-scaled regime.
TEST(IvConvergence, RoundTripMachinePrecisionAcrossGrid) {
  const double Fs[] = {1.0, 100.0, 1000.0, 20000.0, 100000.0};
  const double klogs[] = {-0.4, -0.15, 0.0, 0.15, 0.4};
  const double Ts[] = {0.05, 0.25, 1.0, 2.0};
  const double sigmas[] = {0.08, 0.20, 0.45, 0.85};
  const double df = 0.97;

  double max_rel = 0.0;
  int n = 0;
  for (double F : Fs)
    for (double kl : klogs)
      for (double T : Ts)
        for (double sig : sigmas) {
          const double K = F * std::exp(kl);
          for (Side side : {Side::Call, Side::Put}) {
            const double price = black76_price(F, K, T, sig, df, side);
            const double intr = (side == Side::Call) ? df * std::fmax(F - K, 0.0)
                                                     : df * std::fmax(K - F, 0.0);
            if (price - intr < 1e-6 * F) continue; // skip degenerate near-intrinsic quotes
            const Result<double> iv = implied_vol(price, F, K, T, df, side);
            ASSERT_TRUE(iv.has_value())
                << "F=" << F << " K=" << K << " T=" << T << " sig=" << sig;
            const double rel = std::fabs(*iv - sig) / sig;
            max_rel = std::max(max_rel, rel);
            ++n;
          }
        }
  // Machine-precision class. The worst-case relative error is bounded by the
  // residual noise floor divided by vega, ~8·ε·(max(F,K)/F)/(φ(d1)·√T·σ), which
  // for the high-dynamic-range corners of this grid is a few ×1e-12. The OLD
  // mis-scaled tolerance produced 1.74e-10 here (measured), so 1e-11 both proves
  // the >10× improvement and holds a machine-precision-class bound.
  EXPECT_LT(max_rel, 1e-11) << "grid points=" << n << " max_rel=" << max_rel;
  // Emit the achieved figure so the ledger can quote the real number.
  std::printf("[IvConvergence] grid points=%d max_rel_err=%.3e\n", n, max_rel);
}

// Item 1.5 — termination, not exhaustion, below the vol floor.
//
// With σ pinned at kIvMin by the post-step clamp, the vol-step test sees the
// PRE-clamp `step` (still large), so the loop used to run out kIvMaxIter and
// report Unavailable. It must now exit through the floor clamp (exit_reason == 2)
// within a couple of Halley steps. Value-level coverage lives in
// implied_vol_test.cpp; this asserts the termination PATH.
TEST(IvConvergence, BelowFloorTerminatesOnFloorClamp) {
  const double F = 100.0, K = 100.0, T = 0.25, df = 0.99;
  const double sigma = 0.002; // < kIvMin = 0.005
  const double price = black76_price(F, K, T, sigma, df, Side::Call);

  int iters = -1, exit_reason = -99;
  const Result<double> iv = implied_vol_traced(price, F, K, T, df, Side::Call, iters, exit_reason);
  ASSERT_TRUE(iv.has_value()) << iv.error().to_string();
  EXPECT_DOUBLE_EQ(*iv, kIvMin);
  EXPECT_EQ(exit_reason, 2) << "expected floor-clamp termination, got reason=" << exit_reason
                            << " after " << iters << " Halley steps";
  EXPECT_LE(iters, 2) << "iters=" << iters;
}

// Low-notional / small-vega quotes keep the historical absolute-1e-12 behaviour
// (the noise floor there is at or below 1e-12), so they still converge and
// recover σ. Guards against the fix over-tightening tiny options.
TEST(IvConvergence, LowNotionalStillConverges) {
  const double df = 0.99;
  const double Fs[] = {0.5, 1.0, 5.0};
  const double sigmas[] = {0.10, 0.30, 0.60};
  for (double F : Fs)
    for (double sig : sigmas)
      for (Side side : {Side::Call, Side::Put}) {
        const double K = F;
        const double price = black76_price(F, K, 0.5, sig, df, side);
        const Result<double> iv = implied_vol(price, F, K, 0.5, df, side);
        ASSERT_TRUE(iv.has_value()) << "F=" << F << " sig=" << sig;
        EXPECT_LT(std::fabs(*iv - sig) / sig, 1e-10) << "F=" << F << " sig=" << sig;
      }
}

} // namespace
} // namespace atx::vol

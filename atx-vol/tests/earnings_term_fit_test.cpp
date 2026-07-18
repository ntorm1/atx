#include "atx/vol/earnings_term_fit.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>

// Coverage for the earnings term-fit core's one pure primitive so far:
// `censored_atm_vol` (per-expiry censored total-variance -> censored ATM vol,
// see the header's own model comment). The value-vocabulary types
// (`EarningsFitConfig`, `EarningsTermFit`, etc.) declared alongside it are
// exercised by later tasks' tests, not here.

namespace {

using atx::vol::censored_atm_vol;
using atx::vol::CensorObsInput;

TEST(EarningsTermFit_CensoredAtmVol, StripsEventVariance) {
  // w_dirty = sigma_C^2*T + n*e^2 ; recover sigma_C.
  const double T = 0.25, sigmaC = 0.30, e = 0.06;
  const std::size_t n = 1;
  const double w = sigmaC * sigmaC * T + static_cast<double>(n) * e * e;
  const CensorObsInput o{T, w, n};
  EXPECT_NEAR(censored_atm_vol(o, e, 1e-10), sigmaC, 1e-9);
}

TEST(EarningsTermFit_CensoredAtmVol, FloorsOnOvershoot) {
  const CensorObsInput o{0.02, 1e-6, 3}; // n*e^2 >> w
  const double v = censored_atm_vol(o, 0.10, 1e-10);
  EXPECT_TRUE(std::isfinite(v));
  EXPECT_GE(v, 0.0);
}

} // namespace

// atx::engine::alpha — unit tests for state_ops.hpp step kernels (P3d C2/C3).
//
// Tests the pure scalar recurrence kernels KalmanLevelState / kalman_level_step
// and ou_filter_step directly, without any VM or registry wiring.

#include "atx/engine/alpha/state_ops.hpp"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

using namespace atx::engine::alpha::detail;

// ---------------------------------------------------------------------------
// KalmanLevelStep — C2
// ---------------------------------------------------------------------------

TEST(KalmanLevelStep, SeedAndOneUpdate) {
  KalmanLevelState s{};
  bool seeded = false;
  double o0 = kalman_level_step(s, seeded, 2.0, 0.1, 1.0); // seeds: x=2, P=R=1
  EXPECT_DOUBLE_EQ(o0, 2.0);
  EXPECT_TRUE(seeded);
  // step z1=4: P-=P+Q=1.1; K=1.1/2.1; x=2 + K*(4-2)
  const double K = 1.1 / 2.1;
  const double expect = 2.0 + K * 2.0;
  double o1 = kalman_level_step(s, seeded, 4.0, 0.1, 1.0);
  EXPECT_DOUBLE_EQ(o1, expect);
}

TEST(KalmanLevelStep, NanObsCarriesEstimate) {
  KalmanLevelState s{};
  bool seeded = false;
  (void)kalman_level_step(s, seeded, 2.0, 0.1, 1.0);
  const double x_before = s.x;
  double o = kalman_level_step(s, seeded, std::numeric_limits<double>::quiet_NaN(), 0.1, 1.0);
  EXPECT_DOUBLE_EQ(o, x_before); // estimate carried, no update
}

TEST(KalmanLevelStep, UnseededNanStaysUnseeded) {
  KalmanLevelState s{};
  bool seeded = false;
  double o = kalman_level_step(s, seeded, std::numeric_limits<double>::quiet_NaN(), 0.1, 1.0);
  EXPECT_TRUE(std::isnan(o));
  EXPECT_FALSE(seeded);
}

// ---------------------------------------------------------------------------
// OuFilterStep — C3
// ---------------------------------------------------------------------------

TEST(OuFilterStep, PullsTowardMu) {
  const double theta = std::log(2.0); // phi = 0.5
  const double mu = 10.0;
  atx::f64 xhat = 0.0;
  bool seeded = false;
  double o0 = ou_filter_step(xhat, seeded, 2.0, theta, mu); // seed -> 2.0
  EXPECT_DOUBLE_EQ(o0, 2.0);
  double o1 = ou_filter_step(xhat, seeded, 99.0 /*ignored after seed*/, theta, mu);
  EXPECT_DOUBLE_EQ(o1, 6.0); // 10 + 0.5*(2-10) = 6
}

TEST(OuFilterStep, UnseededNanStaysUnseeded) {
  atx::f64 xhat = 0.0;
  bool seeded = false;
  double o = ou_filter_step(xhat, seeded, std::numeric_limits<double>::quiet_NaN(), 0.3, 1.0);
  EXPECT_TRUE(std::isnan(o));
  EXPECT_FALSE(seeded);
}

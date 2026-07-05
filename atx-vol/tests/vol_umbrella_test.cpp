#include <gtest/gtest.h>

// Umbrella compile check: this translation unit includes ONLY the umbrella
// header. It proves (a) the aggregate public surface is self-consistent — no
// ODR clashes across the co-included headers (e.g. the PricingRoute enum,
// formerly duplicated in portfolio.hpp and profile.hpp, now single-sourced from
// types.hpp), and (b) the quickstart symbols are reachable through the one
// include.
#include "atx/vol/vol.hpp"

namespace {

TEST(VolUmbrella, PublicSurfaceIsReachableAndConsistent) {
  using namespace atx::vol;

  // Core vocabulary (types.hpp) — including the de-duplicated PricingRoute.
  EXPECT_EQ(static_cast<int>(Side::Call), 0);
  EXPECT_EQ(static_cast<int>(PricingRoute::B76AlCache), 1);

  // European primitive (black76.hpp).
  const double px = black76_price(100.0, 100.0, 1.0, 0.2, 1.0, Side::Call);
  EXPECT_GT(px, 0.0);

  // Named preset factory (session.hpp — config / API layer).
  const SessionInputs in = make_session_inputs(FitPreset::Robust, 100.0, 0.03);
  EXPECT_EQ(in.calendar_repair, CalendarRepair::MonotoneFit);

  // Calibration-grade slice type (vol_surface.hpp) is reachable.
  EssviParams slice{};
  slice.theta = 0.04;
  EXPECT_GT(slice.theta, 0.0);
}

}  // namespace

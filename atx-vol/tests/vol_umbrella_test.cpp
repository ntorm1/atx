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

// E5 / AN-W. The analytics flagship and its neighbours were absent from the
// umbrella, so the one-include public API could not reach
// `compute_surface_analytics`, the RND/BKM stack, the earnings/event-vol model,
// the vol-time clock, the SR tenor grid, the tearsheet/run-report writers,
// dense slices, the strangle DSL or the strategy adapters. Ten headers were
// added; this test names a symbol from EACH of them, so a future re-ordering of
// vol.hpp that drops one fails here rather than silently shrinking the public
// surface again.
//
// S4-T18: the tearsheet / run_report assertions left this test with their
// headers — those are `atx-vol-tools` CLI-support headers now, outside the
// frozen umbrella surface.
TEST(VolUmbrella, AnalyticsAndReportingSurfaceIsReachable) {
  using namespace atx::vol;

  // analytics.hpp — the flagship bundle + E5's delta-convention contract.
  const AnalyticsConfig acfg;
  EXPECT_EQ(acfg.delta_convention, DeltaConvention::American);
  EXPECT_FALSE(acfg.tenors.tenors_years.empty());

  // event_vol.hpp — the SpiderRock event decomposition + E3a's joint-solve tag.
  EXPECT_EQ(static_cast<int>(EmoveMethod::TwoPillar), 0);
  EXPECT_GT(censored_total_variance(0.04, 1, 0.05), 0.0);

  // earnings_term_fit.hpp — the joint {eMove, st, lt, decay} fit vocabulary.
  const EarningsFitConfig efc;
  EXPECT_GT(efc.emove_hi, efc.emove_lo);

  // vol_time.hpp — the hybrid business/vol-time clock.
  EXPECT_GT(kCalendarYearNs, 0.0);

  // sr_tenor_grid.hpp — SpiderRock's 12-point native tenor grid.
  EXPECT_EQ(SrTenorGrid::kTradingDays.size(), std::size_t{12});
  EXPECT_EQ(SrTenorGrid::kTradingDays.front(), 5);

  // dense_slice.hpp — the densified convex slice fit.
  const ConvexFitOpts dopts;
  EXPECT_GE(kMaxIntervalSlackRows, 1);
  EXPECT_GE(sizeof dopts, sizeof(double));

  // dispersion_strangle.hpp — the strangle DSL.
  const DispersionStrangleConfig strangle;
  EXPECT_GE(sizeof strangle, sizeof(double));

  // strategy.hpp — the lifecycle/roll vocabulary the dispersion strategies use.
  const LifecycleSpec lifecycle;
  EXPECT_GE(sizeof lifecycle, sizeof(double));
}

}  // namespace

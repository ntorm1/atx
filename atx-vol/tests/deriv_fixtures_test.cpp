#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "atx/vol/arb.hpp" // arb_check_butterfly_slice
#include "deriv_fixtures.hpp"

// Task 0 (vol-derivs production sprint): the shared vol-derivatives fixture
// builders in `deriv_fixtures.hpp` are new, largely-static production-adjacent
// code with no coverage of their own -- these tests pin the skew fixture's
// numeric output (so a later edit to `make_skew_surface`'s internal formula
// cannot silently drift the oracle every downstream correctness task relies
// on) and gate the MC realized-variance harness against its closed-form
// flat-BS truth per the sprint brief's acceptance criterion.

namespace {

using atx::vol::CurveSet;
using atx::vol::EssviSurface;
using atx::vol::deriv_testkit::kFixturePillarsT;
using atx::vol::deriv_testkit::make_curves;
using atx::vol::deriv_testkit::make_flat_surface;
using atx::vol::deriv_testkit::make_skew_surface;
using atx::vol::deriv_testkit::mc_realized_variance;
using atx::vol::deriv_testkit::McModelParams;
using atx::vol::deriv_testkit::McRvResult;

// ── make_flat_surface sanity ──────────────────────────────────────────────

TEST(DerivFixtures, FlatSurface_IvEqualsSigmaAtEveryPillar) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma);
  for (const double T : kFixturePillarsT) {
    EXPECT_NEAR(surf.iv(0.0, T), sigma, 1.0e-9);
    // Flat: also true well off ATM.
    EXPECT_NEAR(surf.iv(-0.3, T), sigma, 1.0e-6);
    EXPECT_NEAR(surf.iv(0.3, T), sigma, 1.0e-6);
  }
}

// ── make_skew_surface: fixture pin (Task 0 acceptance) ────────────────────
//
// `make_skew_surface(0.20, -0.10, 0.03)` is the brief's own worked example.
// ATM iv equals atm_vol exactly at every pillar (the w(0) == theta identity
// documented in deriv_fixtures.hpp), independent of skew_slope/convexity --
// pinned first as a structural invariant. The off-ATM values encode the
// actual skew/convexity shape and are pinned to the literal doubles this
// build produced (recorded once, see task-0-report.md), so any future change
// to the fixture's internal formula is caught here rather than silently
// drifting every downstream oracle.

TEST(DerivFixtures, SkewSurface_AtmIvEqualsAtmVolAtEveryPillar) {
  const EssviSurface surf = make_skew_surface(0.20, -0.10, 0.03);
  for (const double T : kFixturePillarsT) {
    EXPECT_NEAR(surf.iv(0.0, T), 0.20, 1.0e-12);
  }
}

TEST(DerivFixtures, SkewSurface_PinnedOffAtmValues) {
  const EssviSurface surf = make_skew_surface(0.20, -0.10, 0.03);

  // 3M (kSkewRefT), the tenor `skew_slope` is calibrated at directly.
  EXPECT_NEAR(surf.iv(0.0, 0.25), 0.20, 1.0e-12);
  EXPECT_NEAR(surf.iv(-0.10, 0.25), 0.20998670686955731, 1.0e-12);
  EXPECT_NEAR(surf.iv(0.10, 0.25), 0.19004019870737834, 1.0e-12);

  // 1M: convexity > 0 steepens the short end relative to the 3M anchor.
  EXPECT_NEAR(surf.iv(-0.10, 1.0 / 12.0), 0.21032013922729328, 1.0e-12);

  // 1Y: convexity > 0 flattens the long end relative to the 3M anchor.
  EXPECT_NEAR(surf.iv(0.10, 1.0), 0.19044309323379133, 1.0e-12);

  // Put side (negative k) reads a higher vol than the call side (positive k)
  // -- the negative skew shape the sprint brief asks for.
  EXPECT_GT(surf.iv(-0.10, 0.25), surf.iv(0.0, 0.25));
  EXPECT_LT(surf.iv(0.10, 0.25), surf.iv(0.0, 0.25));
}

TEST(DerivFixtures, SkewSurface_ButterflyFreeAtEveryPillar) {
  const EssviSurface surf = make_skew_surface(0.20, -0.10, 0.03);
  for (const double T : kFixturePillarsT) {
    const auto viol = atx::vol::arb_check_butterfly_slice(
        [&](double k) { return surf.w(k, T); }, T, -1.5, 1.5, 129);
    ASSERT_TRUE(viol.has_value()) << viol.error().to_string();
    EXPECT_TRUE(viol->empty())
        << "T=" << T << " has " << viol->size() << " butterfly violations";
  }
}

TEST(DerivFixtures, SkewSurface_CalendarOrderedAtmLevel) {
  // theta_i = atm_vol^2 * T_i is strictly increasing in T by construction;
  // w(0, T) == theta_i, so the ATM level alone is trivially calendar-ordered.
  const EssviSurface surf = make_skew_surface(0.20, -0.10, 0.03);
  double prev_w = 0.0;
  for (const double T : kFixturePillarsT) {
    const double w = surf.w(0.0, T);
    EXPECT_GT(w, prev_w);
    prev_w = w;
  }
}

// ── make_curves ────────────────────────────────────────────────────────────

TEST(DerivFixtures, MakeCurves_NontrivialCarryMatchesForwardFormula) {
  const double spot = 100.0;
  const double r = 0.05;
  const double q = 0.02;
  const CurveSet cs = make_curves(spot, r, q);
  ASSERT_NE(r, q); // nontrivial carry, per the Task 0 brief
  for (const double T : kFixturePillarsT) {
    const double expected_f = spot * std::exp((r - q) * T);
    const double expected_df = std::exp(-r * T);
    // Query resolve_forward's own pillar reads via the ForwardCurve directly.
    bool found = false;
    for (const auto &pt : cs.forward.points()) {
      if (pt.T == T) {
        EXPECT_NEAR(pt.F, expected_f, 1.0e-9);
        found = true;
      }
    }
    EXPECT_TRUE(found) << "no forward point at T=" << T;
    EXPECT_NEAR(cs.yield.disc(T), expected_df, 1.0e-9);
  }
}

// ── MC realized-variance oracle ───────────────────────────────────────────

TEST(DerivFixtures, McHarness_DeterministicUnderTheSameSeed) {
  const McModelParams p{100.0, 0.03, 0.01, 0.25, 0.5};
  const McRvResult a = mc_realized_variance(p, 2000, 8, 42);
  const McRvResult b = mc_realized_variance(p, 2000, 8, 42);
  EXPECT_EQ(a.mean_rv, b.mean_rv);
  EXPECT_EQ(a.stderr_rv, b.stderr_rv);
  EXPECT_EQ(a.n_paths, b.n_paths);

  const McRvResult c = mc_realized_variance(p, 2000, 8, 43);
  EXPECT_NE(a.mean_rv, c.mean_rv);
}

// Task 0 acceptance: MC harness reproduces flat-BS E[RV] = sigma^2 + (T/n) *
// (r - q - sigma^2/2)^2 within 3 MC standard errors at n_paths = 2e5.
TEST(DerivFixtures, McHarness_RecoversFlatBsExpectedRealizedVariance) {
  const McModelParams p{100.0, 0.05, 0.02, 0.20, 1.0};
  const std::uint32_t n_steps = 12;
  const McRvResult q = mc_realized_variance(p, 200000, n_steps, 7);

  const double mu = p.r - p.q - 0.5 * p.sigma * p.sigma;
  const double truth =
      p.sigma * p.sigma + (p.T / static_cast<double>(n_steps)) * mu * mu;

  ASSERT_GT(q.stderr_rv, 0.0);
  EXPECT_NEAR(q.mean_rv, truth, 3.0 * q.stderr_rv)
      << "mean_rv=" << q.mean_rv << " truth=" << truth
      << " stderr=" << q.stderr_rv;
}

} // namespace

#include "fitting/convex_recovery.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

using detail::make_strict_repair_spec;
using detail::should_attempt_strict_recovery;
using detail::strict_promotion_ks;
using detail::validation_grid_k;

ValidationFailure mask(std::uint32_t bits) { return static_cast<ValidationFailure>(bits); }

// The five masks observed across all 181 backfill rejections must qualify;
// anything carrying a non-geometric bit (beyond CarryGap) must not.
TEST(ConvexRecovery, ShouldAttemptMatchesObservedRejectionMasks) {
  EXPECT_TRUE(should_attempt_strict_recovery(mask(2080))); // CarryGap|Calendar
  EXPECT_TRUE(should_attempt_strict_recovery(mask(2064))); // CarryGap|Butterfly
  EXPECT_TRUE(should_attempt_strict_recovery(mask(2096))); // CarryGap|Cal|Bfly
  EXPECT_TRUE(should_attempt_strict_recovery(mask(32)));   // Calendar
  EXPECT_TRUE(should_attempt_strict_recovery(mask(16)));   // Butterfly

  EXPECT_FALSE(should_attempt_strict_recovery(mask(0)));
  EXPECT_FALSE(should_attempt_strict_recovery(mask(2048)));     // CarryGap alone
  EXPECT_FALSE(should_attempt_strict_recovery(mask(1024)));     // InsufficientData
  EXPECT_FALSE(should_attempt_strict_recovery(mask(2080 | 1))); // +InvalidDomain
  EXPECT_FALSE(should_attempt_strict_recovery(mask(16 | 64)));  // +Wing
  EXPECT_FALSE(should_attempt_strict_recovery(mask(32 | 256))); // +TimedOut
}

TEST(ConvexRecovery, StrictSpecPinsOracleCalendarGridAndTightensTolerance) {
  RiskSurfaceValidationConfig config; // defaults: +/-0.50, 129 pts, 1e-8
  const ConvexRepairSpec spec = make_strict_repair_spec(config);
  EXPECT_EQ(spec.k_min, config.k_min);
  EXPECT_EQ(spec.k_max, config.k_max);
  EXPECT_EQ(spec.grid_points, config.calendar_grid_points);
  EXPECT_DOUBLE_EQ(spec.tolerance, 0.1 * config.calendar_total_variance_tolerance);
  EXPECT_TRUE(spec.extra_node_ks.empty());
}

TEST(ConvexRecovery, PromotionTakesCalendarKVerbatim) {
  RiskSurfaceValidationConfig config;
  ValidationDigest digest;
  digest.n_calendar_violations = 3;
  digest.first_calendar_k = -0.50;
  const std::vector<double> ks = strict_promotion_ks(digest, config);
  ASSERT_EQ(ks.size(), 1u);
  EXPECT_EQ(ks[0], -0.50);
}

TEST(ConvexRecovery, PromotionStraddlesButterflyKWithStrikeGridNeighbors) {
  RiskSurfaceValidationConfig config; // strike grid: 129 pts over [-0.5, 0.5]
  ValidationDigest digest;
  digest.n_butterfly_violations = 1;
  // Between grid points 63 and 64: neighbors 62..65 plus the k itself.
  digest.first_butterfly_k = validation_grid_k(config, 63, 129) + 1.0e-4;
  const std::vector<double> ks = strict_promotion_ks(digest, config);
  ASSERT_EQ(ks.size(), 5u);
  EXPECT_EQ(ks[0], validation_grid_k(config, 62, 129));
  EXPECT_EQ(ks[1], validation_grid_k(config, 63, 129));
  EXPECT_EQ(ks[2], digest.first_butterfly_k);
  EXPECT_EQ(ks[3], validation_grid_k(config, 64, 129));
  EXPECT_EQ(ks[4], validation_grid_k(config, 65, 129));
}

TEST(ConvexRecovery, PromotionClampsAtBandEdgeAndDropsNonFinite) {
  RiskSurfaceValidationConfig config;
  ValidationDigest digest;
  digest.n_butterfly_violations = 1;
  digest.first_butterfly_k = config.k_min; // index 0: neighbors -1 dropped
  std::vector<double> ks = strict_promotion_ks(digest, config);
  ASSERT_GE(ks.size(), 2u);
  EXPECT_EQ(ks.front(), config.k_min);
  for (std::size_t i = 1; i < ks.size(); ++i) {
    EXPECT_GT(ks[i], ks[i - 1]); // sorted, deduplicated
  }

  // ValidationDigest's first_calendar_k/first_butterfly_k default to 0.0, not
  // NaN, so the guard here is the violation COUNTS (both zero), not a
  // finiteness check on the default-constructed value.
  ValidationDigest empty; // counts zero => nothing to promote
  EXPECT_TRUE(strict_promotion_ks(empty, config).empty());
}

} // namespace
} // namespace atx::vol

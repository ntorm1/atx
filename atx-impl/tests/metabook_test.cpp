// metabook_test.cpp — p8 Sprint 2: stage_metabook (the fund:: mega-alpha layer wired into
// atx-impl). S2-0/S2-1/S2-2/S2-4/S2-5 land their accept tests here (S2-3's netting-specific
// tests live in metabook_netting_test.cpp per the sprint's test-home split).
//
// S2-0: config-surface + FROZEN-signature confirmation only (no stage behavior yet).

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/fund/meta_allocator.hpp"

#include "stage_metabook.hpp"

namespace atxtest_metabook_test {

using atx::impl::MetaBookStageConfig;
using atx::impl::SleeveAssignment;

// SleeveAssignment::SingleSleeve MUST be 0 -- the inert R7-pin default (also enforced by a
// static_assert in stage_metabook.hpp; re-checked here as a runtime regression net).
TEST(MetabookConfig, SingleSleeveIsZero) {
  EXPECT_EQ(static_cast<atx::u8>(SleeveAssignment::SingleSleeve), 0U);
}

// MetaBookStageConfig default-constructs to SingleSleeve + the engine MetaBookConfig's own
// defaults (untouched) + the stage's own gross/name_cap/risk_aversion defaults (1.0 each,
// mirroring RunConfig's resolved gross_val/name_cap_val/risk_aversion in stage_optimize.cpp).
TEST(MetabookConfig, DefaultsAreInert) {
  const MetaBookStageConfig cfg;
  EXPECT_EQ(cfg.assignment, SleeveAssignment::SingleSleeve);
  EXPECT_EQ(cfg.max_sleeves, 8U);
  EXPECT_DOUBLE_EQ(cfg.gross, 1.0);
  EXPECT_DOUBLE_EQ(cfg.name_cap, 1.0);
  EXPECT_DOUBLE_EQ(cfg.risk_aversion, 1.0);

  // The wrapped engine MetaBookConfig's own defaults (meta_allocator.hpp / meta_book.hpp),
  // untouched by S2-0 -- confirms the field is plumbed by VALUE, not reinterpreted.
  EXPECT_EQ(cfg.meta.alloc.method, atx::engine::fund::RiskBudgetMethod::EqualRiskContribution);
  EXPECT_DOUBLE_EQ(cfg.meta.alloc.fractional_kelly, 0.3);
  EXPECT_DOUBLE_EQ(cfg.meta.alloc.target_vol, 0.0);
  EXPECT_DOUBLE_EQ(cfg.meta.alloc.max_gross, 4.0);
  EXPECT_EQ(cfg.meta.alloc.solve_iters, 64U);
  EXPECT_EQ(cfg.meta.risk_lookback, 60U);
}

} // namespace atxtest_metabook_test

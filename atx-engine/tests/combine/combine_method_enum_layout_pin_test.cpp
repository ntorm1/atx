// combine_method_enum_layout_pin_test.cpp — p8 S3-0: CombineMethod enum-layout
// pin + CombinerConfig inert-default pin.
//
// Sprint 3 appends two stage-dispatched CombineMethod enumerators (Stack=5,
// RegimeStack=6) AFTER the five frozen §5.3 methods (mirrors S1's
// RiskModelKind::Diagonal==0 discipline: existing indices never move). This
// suite is the frozen-order guard: any future edit that reorders/renumbers the
// enum, or widens its underlying type, breaks this file's static_asserts at
// COMPILE TIME — the earliest possible signal. It also pins CombinerConfig's
// new S3 knobs to their documented inert defaults (a fresh config must be a
// complete no-op until a caller explicitly sets method to Stack/RegimeStack).
//
// Suite: CombineMethodEnumLayout

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/combine/combiner.hpp"

namespace atxtest_combine_method_enum_layout_pin {

namespace combine = atx::engine::combine;
using combine::CombineMethod;
using combine::CombinerConfig;

// ===========================================================================
//  Frozen-order pin (S3-0, compile-time).
// ===========================================================================
static_assert(static_cast<atx::u8>(CombineMethod::EqualWeight) == 0U,
              "EqualWeight must stay the frozen zero index");
static_assert(static_cast<atx::u8>(CombineMethod::RankAverage) == 1U,
              "RankAverage index frozen");
static_assert(static_cast<atx::u8>(CombineMethod::IcWeighted) == 2U,
              "IcWeighted index frozen");
static_assert(static_cast<atx::u8>(CombineMethod::ShrinkageMv) == 3U,
              "ShrinkageMv index frozen (the default method)");
static_assert(static_cast<atx::u8>(CombineMethod::BoundedRegression) == 4U,
              "BoundedRegression index frozen");
static_assert(static_cast<atx::u8>(CombineMethod::Stack) == 5U,
              "Stack (S3) must be appended immediately after BoundedRegression");
static_assert(static_cast<atx::u8>(CombineMethod::RegimeStack) == 6U,
              "RegimeStack (S3) must be appended immediately after Stack");
static_assert(sizeof(CombineMethod) == 1U,
              "CombineMethod must stay a `: atx::u8` single-byte enum");

TEST(CombineMethodEnumLayout, FrozenIndicesRuntimeCheck) {
  // Runtime mirror of the static_asserts above (belt-and-suspenders — a
  // static_assert alone can silently stop being evaluated if the enum's
  // underlying-type declaration is ever dropped).
  EXPECT_EQ(static_cast<atx::u8>(CombineMethod::EqualWeight), 0U);
  EXPECT_EQ(static_cast<atx::u8>(CombineMethod::RankAverage), 1U);
  EXPECT_EQ(static_cast<atx::u8>(CombineMethod::IcWeighted), 2U);
  EXPECT_EQ(static_cast<atx::u8>(CombineMethod::ShrinkageMv), 3U);
  EXPECT_EQ(static_cast<atx::u8>(CombineMethod::BoundedRegression), 4U);
  EXPECT_EQ(static_cast<atx::u8>(CombineMethod::Stack), 5U);
  EXPECT_EQ(static_cast<atx::u8>(CombineMethod::RegimeStack), 6U);
  EXPECT_EQ(sizeof(CombineMethod), 1U);
}

// ===========================================================================
//  CombinerConfig — inert defaults (S3-0's appended knobs).
// ===========================================================================
TEST(CombineMethodEnumLayout, CombinerConfigDefaultsAreInert) {
  const CombinerConfig cfg;
  EXPECT_EQ(cfg.method, CombineMethod::ShrinkageMv)
      << "method_from_string(\"\")/\"shrinkage-mv\" must keep resolving to the "
         "linear default — Stack/RegimeStack are opt-in only";
  EXPECT_EQ(cfg.stack_master_seed, 0U);
  EXPECT_EQ(cfg.stack_cpcv_groups, 6U);
  EXPECT_EQ(cfg.stack_cpcv_test_groups, 2U);
  EXPECT_DOUBLE_EQ(cfg.stack_cpcv_embargo, 0.01);
  EXPECT_EQ(cfg.stack_horizon, 1U);
  EXPECT_EQ(cfg.regime_n_states, 3U);
}

// ===========================================================================
//  AlphaCombiner::fit — Stack/RegimeStack are stage-dispatched, never fit here.
// ===========================================================================
TEST(CombineMethodEnumLayout, FitRejectsStackAndRegimeStackAsStageDispatched) {
  // A 2-alpha pool with a trivial 3-period PnL stream — just enough to clear
  // AlphaCombiner::fit's n>=1 / T>=2 guards so the CombineMethod switch itself
  // (not an earlier guard) is what is under test.
  combine::AlphaStore pool;
  const std::vector<atx::f64> pnl_a{0.01, -0.02, 0.03};
  const std::vector<atx::f64> pnl_b{0.02, 0.01, -0.01};
  const std::vector<atx::f64> pos{1.0, 1.0}; // 1 instrument x 3 periods, flattened lazily below
  std::vector<atx::f64> pos_a(3, 1.0);
  std::vector<atx::f64> pos_b(3, 1.0);
  combine::AlphaMetrics m{};
  ASSERT_TRUE(pool.insert(nullptr, pnl_a, pos_a, m).has_value());
  ASSERT_TRUE(pool.insert(nullptr, pnl_b, pos_b, m).has_value());

  for (const CombineMethod cm : {CombineMethod::Stack, CombineMethod::RegimeStack}) {
    combine::AlphaCombiner combiner;
    combiner.cfg.method = cm;
    const auto r = combiner.fit(pool, /*fit_begin=*/0U, /*fit_end=*/3U);
    ASSERT_FALSE(r.has_value())
        << "AlphaCombiner::fit must reject Stack/RegimeStack (stage-dispatched) method="
        << static_cast<int>(cm);
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

} // namespace atxtest_combine_method_enum_layout_pin

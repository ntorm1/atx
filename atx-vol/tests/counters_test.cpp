// counters_test.cpp — the ATX_VOL_COUNTERS facility (P0.2) contract.
//
// The default test build compiles with ATX_VOL_COUNTERS OFF, so the primary
// assertions here are the OFF-path guarantees: the macros compile to nothing, the
// snapshot API returns the "disabled" sentinel, and counters_enabled() is a
// constexpr false. When the suite is (re)built with -DATX_VOL_COUNTERS=ON the same
// test flips to verifying the counters actually count.

#include <gtest/gtest.h>

#include "atx/vol/counters.hpp"

namespace {

using atx::vol::counters::Counter;
using atx::vol::counters::counters_enabled;
using atx::vol::counters::snapshot;

// Compile-time zero-cost proof for the default build: when the definition is
// absent, counters_enabled() folds to false, so every `if constexpr` counter
// block (and the header's atomic storage) is discarded.
#if !defined(ATX_VOL_COUNTERS)
static_assert(!counters_enabled(),
              "OFF build must report counters disabled (zero-cost sentinel)");
#endif

// The macros must be valid statements even in an unbraced `if` — the OFF
// expansion is `((void)0)`, the ON expansion a function call; both parse here.
TEST(Counters, MacrosAreStatementSafe) {
  int taken = 0;
  if (taken == 0)
    ATX_VOL_COUNT(BoundarySolves);
  else
    ATX_VOL_COUNT_N(FrameBytes, 7);
  for (int i = 0; i < 2; ++i) ATX_VOL_COUNT(NormCdfCalls);
  SUCCEED();
}

TEST(Counters, SnapshotContractMatchesBuildMode) {
  if constexpr (!counters_enabled()) {
    // OFF: disabled sentinel, and the macros never touch any state.
    const auto s = snapshot();
    EXPECT_FALSE(s.enabled);
    ATX_VOL_COUNT(BoundarySolves);
    ATX_VOL_COUNT_N(FrameBytes, 999);
    const auto s2 = snapshot();
    EXPECT_FALSE(s2.enabled);
    EXPECT_EQ(s2.get(Counter::BoundarySolves), 0u);
    EXPECT_EQ(s2.get(Counter::FrameBytes), 0u);
  } else {
    // ON: the counters actually accumulate and the snapshot is enabled.
    atx::vol::counters::reset();
    ATX_VOL_COUNT(BoundarySolves);
    ATX_VOL_COUNT(BoundarySolves);
    ATX_VOL_COUNT_N(FrameBytes, 40);
    const auto s = snapshot();
    EXPECT_TRUE(s.enabled);
    EXPECT_EQ(s.get(Counter::BoundarySolves), 2u);
    EXPECT_EQ(s.get(Counter::FrameBytes), 40u);
    atx::vol::counters::reset();
    const auto s2 = snapshot();
    EXPECT_EQ(s2.get(Counter::BoundarySolves), 0u);
  }
}

}  // namespace

// sim_clock_test.cpp — behavioural tests for atx::engine::SimClock (P1-4).
//
// SimClock is the engine's monotonic point-in-time gate: it advances forward
// only (the DataHandler is the sole advancer in Phase 1 backtest mode) and
// admits a record as visible iff knowledge_ts <= now().
//
// Tests follow the Subject_Condition_Expected naming convention and cover:
//   - Default state: now() == epoch.
//   - Forward advance: now() tracks the last advance_to() call.
//   - Monotonic precondition: advance_to a past timestamp → EXPECT_DEATH.
//   - Advance to same timestamp (non-decreasing, not strictly increasing): OK.
//   - is_visible: future knowledge_ts → invisible; now == knowledge_ts → visible;
//     past knowledge_ts → visible.
//   - Restatement window: two records share event_ts but differ in knowledge_ts;
//     the gate drives "original visible, restatement invisible" before the
//     restatement's knowledge time, and "both visible" after.
//   - Boundary: epoch-zero record visible at epoch clock.

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/clock/sim_clock.hpp"

namespace {

using atx::core::time::Timestamp;
using atx::engine::SimClock;

// Convenience: nanosecond factory so test values read as plain integers.
constexpr Timestamp ts(atx::i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// ============================================================================
//  Default state
// ============================================================================

TEST(SimClock, DefaultConstruct_NowIsEpoch) {
  const SimClock clock;
  EXPECT_EQ(clock.now(), Timestamp::epoch());
}

// ============================================================================
//  advance_to — forward movement
// ============================================================================

TEST(SimClock, AdvanceTo_ForwardTimestamp_NowUpdated) {
  SimClock clock;
  clock.advance_to(ts(1'000));
  EXPECT_EQ(clock.now(), ts(1'000));
}

TEST(SimClock, AdvanceTo_MultipleForwardSteps_NowTracksLast) {
  SimClock clock;
  clock.advance_to(ts(100));
  clock.advance_to(ts(200));
  clock.advance_to(ts(300));
  EXPECT_EQ(clock.now(), ts(300));
}

TEST(SimClock, AdvanceTo_SameTimestamp_Allowed) {
  // Advancing to the current timestamp is non-decreasing (not strictly
  // increasing), so it must NOT trigger the monotonic assert.
  SimClock clock;
  clock.advance_to(ts(500));
  clock.advance_to(ts(500)); // idempotent re-advance — must not abort
  EXPECT_EQ(clock.now(), ts(500));
}

// ============================================================================
//  Monotonic precondition — backward advance must abort in debug builds.
// ============================================================================

TEST(SimClock, AdvanceTo_BackwardTimestamp_Aborts) {
  SimClock clock;
  clock.advance_to(ts(1'000));
  // ATX_ASSERT in advance_to fires on backward movement; EXPECT_DEATH captures
  // the resulting std::abort(). The regex matches any output (the assert may
  // or may not emit a message to stderr depending on the logging init state).
  EXPECT_DEATH({ clock.advance_to(ts(999)); }, ".*");
}

// ============================================================================
//  is_visible — the point-in-time look-ahead gate
// ============================================================================

TEST(SimClock, IsVisible_FutureKnowledgeTs_ReturnsFalse) {
  SimClock clock;
  clock.advance_to(ts(1'000));
  EXPECT_FALSE(clock.is_visible(ts(1'001)));
}

TEST(SimClock, IsVisible_KnowledgeTsEqualToNow_ReturnsTrue) {
  SimClock clock;
  clock.advance_to(ts(1'000));
  EXPECT_TRUE(clock.is_visible(ts(1'000)));
}

TEST(SimClock, IsVisible_PastKnowledgeTs_ReturnsTrue) {
  SimClock clock;
  clock.advance_to(ts(1'000));
  EXPECT_TRUE(clock.is_visible(ts(999)));
}

// ============================================================================
//  Epoch boundary
// ============================================================================

TEST(SimClock, IsVisible_EpochRecord_AtDefaultClock_ReturnsTrue) {
  // A record whose knowledge_ts is the epoch (0 ns) is visible when the clock
  // is also at epoch — the very first state before any advance.
  const SimClock clock;
  EXPECT_TRUE(clock.is_visible(Timestamp::epoch()));
}

TEST(SimClock, AdvanceTo_FromEpoch_ToEpoch_Allowed) {
  // Advancing epoch→epoch is the degenerate same-timestamp case.
  SimClock clock;
  clock.advance_to(Timestamp::epoch());
  EXPECT_EQ(clock.now(), Timestamp::epoch());
}

// ============================================================================
//  Restatement window — the core point-in-time look-ahead defence
//
//  Two records share the same event_ts (e.g. 09:30:00) but carry different
//  knowledge_ts:
//    original:    knowledge_ts = t1  (the value known at release time)
//    restatement: knowledge_ts = t2  (t2 > t1; revised figure filed later)
//
//  With the clock at t1:
//    - original is visible   (knowledge_ts == now)       → caller reads v1
//    - restatement invisible (knowledge_ts > now)        → cannot peek at v2
//  After advancing to t2:
//    - both are visible                                  → caller reads v2
//      (the "pick latest visible" selection is caller logic; SimClock only
//       provides the gate — this test demonstrates the gate drives it)
// ============================================================================

TEST(SimClock, Restatement_BeforeKnowledgeTime_OriginalVisibleRestatedInvisible) {
  struct Record {
    Timestamp knowledge_ts;
    double value{0.0};
  };

  constexpr Timestamp kT1 = Timestamp::from_unix_nanos(1'000'000); // original knowledge time
  constexpr Timestamp kT2 = Timestamp::from_unix_nanos(2'000'000); // restatement knowledge time

  constexpr Record original{kT1, 42.0};
  constexpr Record restatement{kT2, 43.5};

  SimClock clock;
  clock.advance_to(kT1);

  // At t1: original is knowable, restatement is a future revision — invisible.
  EXPECT_TRUE(clock.is_visible(original.knowledge_ts));
  EXPECT_FALSE(clock.is_visible(restatement.knowledge_ts));

  // Simulate caller picking the latest visible value (the gate drives the choice).
  const double value_at_t1 =
      clock.is_visible(restatement.knowledge_ts) ? restatement.value : original.value;
  EXPECT_DOUBLE_EQ(value_at_t1, 42.0);
}

TEST(SimClock, Restatement_AfterKnowledgeTime_BothVisible_CallerPicksLatest) {
  struct Record {
    Timestamp knowledge_ts;
    double value{0.0};
  };

  constexpr Timestamp kT2 = Timestamp::from_unix_nanos(2'000'000);

  constexpr Record original{Timestamp::from_unix_nanos(1'000'000), 42.0};
  constexpr Record restatement{kT2, 43.5};

  SimClock clock;
  clock.advance_to(kT2);

  // After the restatement's knowledge time both records are visible.
  EXPECT_TRUE(clock.is_visible(original.knowledge_ts));
  EXPECT_TRUE(clock.is_visible(restatement.knowledge_ts));

  // Caller picks the restatement (latest knowable) — gate has admitted both.
  const double value_at_t2 =
      clock.is_visible(restatement.knowledge_ts) ? restatement.value : original.value;
  EXPECT_DOUBLE_EQ(value_at_t2, 43.5);
}

TEST(SimClock, Restatement_VisibilityFlipsAtKnowledgeBoundary) {
  // Directly pin the boundary: invisible one nanosecond before t2, visible at t2.
  constexpr Timestamp kT2 = Timestamp::from_unix_nanos(2'000'000);

  SimClock clock;
  clock.advance_to(Timestamp::from_unix_nanos(1'999'999)); // one ns before restatement
  EXPECT_FALSE(clock.is_visible(kT2));

  clock.advance_to(kT2); // exactly at the boundary
  EXPECT_TRUE(clock.is_visible(kT2));
}

} // namespace

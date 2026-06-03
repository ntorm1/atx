#include <gtest/gtest.h>

#include "atx/engine/fwd.hpp"

// Phase-1 scaffold smoke test: proves the atx-engine test target compiles,
// links GoogleTest, and can include the engine forward-declaration header.
// Real behaviour is covered by the per-unit *_test.cpp suites (P1-1..P1-6).
TEST(EngineScaffold, TestTargetLinksAndRuns) {
  // Touch a forward-declared spine type so the fwd header is genuinely used
  // (a null pointer needs only the declaration, not the definition).
  atx::engine::SimClock *clock = nullptr;
  EXPECT_EQ(clock, nullptr);
}

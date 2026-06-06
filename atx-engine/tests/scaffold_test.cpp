#include <gtest/gtest.h>

#include <type_traits> // std::is_same_v

#include "atx/core/domain/symbol.hpp" // atx::core::domain::Symbol (named directly in static_assert)
#include "atx/engine/fwd.hpp"
#include "atx/engine/loop/types.hpp"

// Engine scaffold smoke test: proves the atx-engine test target compiles,
// links GoogleTest, and can include the engine forward-declaration headers.
// Real behaviour is covered by the per-unit *_test.cpp suites (P1-1..P1-6,
// P2-1..P2-8). This file grows one section per phase marker commit.

// =====================================================================
//  Phase-1 — event spine
// =====================================================================

TEST(EngineScaffold, TestTargetLinksAndRuns) {
  // Touch a forward-declared spine type so the fwd header is genuinely used
  // (a null pointer needs only the declaration, not the definition).
  atx::engine::SimClock *clock = nullptr;
  EXPECT_EQ(clock, nullptr);
}

// =====================================================================
//  Phase-2 — backtest loop scaffold (P2-0)
// =====================================================================

// Verify that InstrumentId resolves to the expected atx-core type at
// compile time. This static_assert is the machine-checked contract that
// ties the engine alias to the atx-core symbol interning layer.
static_assert(std::is_same_v<atx::engine::InstrumentId, atx::core::domain::Symbol>,
              "InstrumentId must alias atx::core::domain::Symbol");

TEST(Scaffold, Phase2TypesAliasResolves) {
  // Value-initialise an InstrumentId; Symbol{} has id == 0 (the zero-value
  // convention for an un-interned symbol — see symbol.hpp design contract).
  const atx::engine::InstrumentId id{};
  EXPECT_EQ(id.id, 0U);
}

// =====================================================================
//  Phase-3 — alpha-expression DSL scaffold (P3-0)
// =====================================================================

#include "atx/engine/alpha/fwd.hpp"

TEST(EngineScaffold, Phase3AlphaFwdLinks) {
  // Touch a forward-declared alpha spine type so the fwd header is genuinely
  // used (a null pointer needs only the declaration, not the definition).
  atx::engine::alpha::Engine *eng = nullptr;
  EXPECT_EQ(eng, nullptr);
}

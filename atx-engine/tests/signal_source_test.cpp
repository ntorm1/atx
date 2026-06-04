// atx::engine::loop — ISignalSource seam tests (P2-3).
//
// Covers the plan's Tests list for the GREEN deliverable (ScriptedSignalSource):
//   * replays a baked schedule deterministically (two identical runs → identical
//     outputs, value-for-value);
//   * each evaluate() result length == the configured universe size;
//   * NaN passthrough (a NaN baked into a schedule vector comes back NaN);
//   * max_lookback() reports the configured N;
//   * exhausted schedule → Err (expected failure, not abort);
//   * boundaries: empty / all-NaN signal vector; single-instrument universe.
//
// A real RollingPanel<Cap>::view() is passed as the panel argument to prove the
// SignalView/PanelView signature integrates, even though ScriptedSignalSource is
// panel-independent by design (it replays a canned schedule).
//
// VmSignalSource is NOT exercised here: its declaration + impl are compile-
// guarded behind ATX_ENGINE_HAS_ALPHA_VM (undefined until Phase 3 lands), so
// there is no symbol to test today. The seam (ISignalSource + SignalView) is the
// downstream contract; VmSignalSource's green-gate is a Deferred residual.
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include "atx/core/error.hpp" // Result, ErrorCode
#include "atx/core/types.hpp" // usize, f64

#include "atx/engine/loop/panel_types.hpp"   // PanelView
#include "atx/engine/loop/rolling_panel.hpp" // RollingPanel<Cap>
#include "atx/engine/loop/signal_source.hpp" // ISignalSource, SignalView, ScriptedSignalSource
#include "atx/engine/loop/types.hpp"         // InstrumentId

namespace {

using atx::core::ErrorCode;
using atx::engine::InstrumentId;
using atx::engine::PanelView;
using atx::engine::RollingPanel;
using atx::engine::ScriptedSignalSource;
using atx::engine::SignalView;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }

// A trivial empty panel of a fixed universe size, so the evaluate() signature is
// exercised against a real PanelView (Scripted ignores it by design).
template <atx::usize NInst> [[nodiscard]] PanelView empty_panel_view() {
  static std::array<InstrumentId, NInst> uni = [] {
    std::array<InstrumentId, NInst> u{};
    for (atx::usize i = 0; i < NInst; ++i) {
      u[i] = inst(static_cast<atx::u32>(i + 1));
    }
    return u;
  }();
  static RollingPanel<8> panel{std::span<const InstrumentId>{uni}, /*max_lookback=*/4};
  return panel.view();
}

// =====================================================================
//  Static contract — the seam types are the downstream contract.
// =====================================================================

// SignalView is a trivially-copyable non-owning value (passed by value, like
// PanelView). A heavy / non-copyable SignalView would break P2-4's consumer.
static_assert(std::is_trivially_copyable_v<SignalView>, "SignalView is a non-owning value");

// ScriptedSignalSource is a concrete ISignalSource.
static_assert(std::is_base_of_v<atx::engine::ISignalSource, ScriptedSignalSource>,
              "ScriptedSignalSource implements the ISignalSource seam");

// =====================================================================
//  Deterministic replay — same construction + call sequence → same output.
// =====================================================================

TEST(ScriptedSignalSource, ReplaySameSchedule_TwoRuns_IdenticalOutputs) {
  constexpr atx::usize kUni = 3;
  const std::vector<std::vector<atx::f64>> schedule{
      {1.0, 2.0, 3.0},
      {-1.0, 0.0, 0.5},
      {10.0, 20.0, 30.0},
  };

  ScriptedSignalSource a{schedule, kUni, /*max_lookback=*/5};
  ScriptedSignalSource b{schedule, kUni, /*max_lookback=*/5};

  const PanelView panel = empty_panel_view<kUni>();
  for (const std::vector<atx::f64> &expected : schedule) {
    const atx::core::Result<SignalView> ra = a.evaluate(panel);
    const atx::core::Result<SignalView> rb = b.evaluate(panel);
    ASSERT_TRUE(ra.has_value());
    ASSERT_TRUE(rb.has_value());
    ASSERT_EQ(ra->values.size(), kUni);
    ASSERT_EQ(rb->values.size(), kUni);
    for (atx::usize i = 0; i < kUni; ++i) {
      EXPECT_DOUBLE_EQ(ra->values[i], expected[i]);
      EXPECT_DOUBLE_EQ(rb->values[i], ra->values[i]);
    }
  }
}

// =====================================================================
//  Signal length == universe size, every step.
// =====================================================================

TEST(ScriptedSignalSource, Evaluate_EachResult_LengthEqualsUniverse) {
  constexpr atx::usize kUni = 4;
  const std::vector<std::vector<atx::f64>> schedule{
      {1.0, 2.0, 3.0, 4.0},
      {5.0, 6.0, 7.0, 8.0},
  };
  ScriptedSignalSource src{schedule, kUni, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<kUni>();
  for ([[maybe_unused]] const std::vector<atx::f64> &expected : schedule) {
    const atx::core::Result<SignalView> r = src.evaluate(panel);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->values.size(), kUni);
  }
}

// =====================================================================
//  NaN passthrough — a NaN baked into a schedule vector reads back NaN.
// =====================================================================

TEST(ScriptedSignalSource, Evaluate_NaNEntry_PassesThroughUnchanged) {
  constexpr atx::usize kUni = 3;
  const std::vector<std::vector<atx::f64>> schedule{
      {1.0, kNaN, 3.0},
  };
  ScriptedSignalSource src{schedule, kUni, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<kUni>();
  const atx::core::Result<SignalView> r = src.evaluate(panel);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->values.size(), kUni);
  EXPECT_DOUBLE_EQ(r->values[0], 1.0);
  EXPECT_TRUE(std::isnan(r->values[1])); // "no opinion" sentinel survives.
  EXPECT_DOUBLE_EQ(r->values[2], 3.0);
}

// =====================================================================
//  max_lookback() reports the configured N.
// =====================================================================

TEST(ScriptedSignalSource, MaxLookback_ReturnsConfiguredN) {
  const std::vector<std::vector<atx::f64>> schedule{{0.0}};
  const ScriptedSignalSource src{schedule, /*universe_size=*/1, /*max_lookback=*/7};
  EXPECT_EQ(src.max_lookback(), 7U);
}

// =====================================================================
//  Exhausted schedule → Err (expected failure, never abort).
// =====================================================================

TEST(ScriptedSignalSource, Evaluate_PastEndOfSchedule_ReturnsErr) {
  constexpr atx::usize kUni = 2;
  const std::vector<std::vector<atx::f64>> schedule{
      {1.0, 2.0},
  };
  ScriptedSignalSource src{schedule, kUni, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<kUni>();
  const atx::core::Result<SignalView> ok = src.evaluate(panel);
  EXPECT_TRUE(ok.has_value());

  // Second call: the one-entry schedule is exhausted → Err, no abort.
  const atx::core::Result<SignalView> err = src.evaluate(panel);
  ASSERT_FALSE(err.has_value());
  EXPECT_EQ(err.error().code(), ErrorCode::OutOfRange);
}

// =====================================================================
//  Borrow lifetime — the returned span aliases the source's own storage and is
//  overwritten by the next evaluate() (documented non-owning contract).
// =====================================================================

TEST(ScriptedSignalSource, Evaluate_ConsecutiveCalls_AdvanceCursor) {
  constexpr atx::usize kUni = 2;
  const std::vector<std::vector<atx::f64>> schedule{
      {1.0, 1.0},
      {2.0, 2.0},
  };
  ScriptedSignalSource src{schedule, kUni, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<kUni>();
  const atx::core::Result<SignalView> r0 = src.evaluate(panel);
  ASSERT_TRUE(r0.has_value());
  // Snapshot the first value BEFORE the next call (the span itself is reused).
  const atx::f64 first = r0->values[0];

  const atx::core::Result<SignalView> r1 = src.evaluate(panel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_DOUBLE_EQ(first, 1.0);
  EXPECT_DOUBLE_EQ(r1->values[0], 2.0); // cursor advanced to the next vector.
}

// =====================================================================
//  Boundaries.
// =====================================================================

TEST(ScriptedSignalSource, Evaluate_AllNaNVector_AllNoOpinion) {
  constexpr atx::usize kUni = 3;
  const std::vector<std::vector<atx::f64>> schedule{
      {kNaN, kNaN, kNaN},
  };
  ScriptedSignalSource src{schedule, kUni, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<kUni>();
  const atx::core::Result<SignalView> r = src.evaluate(panel);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->values.size(), kUni);
  for (atx::usize i = 0; i < kUni; ++i) {
    EXPECT_TRUE(std::isnan(r->values[i]));
  }
}

TEST(ScriptedSignalSource, Evaluate_EmptyUniverse_EmptySignal) {
  // A zero-instrument universe: one scheduled (empty) vector → an empty signal.
  const std::vector<std::vector<atx::f64>> schedule{
      {},
  };
  ScriptedSignalSource src{schedule, /*universe_size=*/0, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<1>(); // panel ignored by Scripted.
  const atx::core::Result<SignalView> r = src.evaluate(panel);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->values.size(), 0U);
}

TEST(ScriptedSignalSource, Evaluate_SingleInstrument_OneValuePerStep) {
  const std::vector<std::vector<atx::f64>> schedule{
      {0.25},
      {-0.75},
  };
  ScriptedSignalSource src{schedule, /*universe_size=*/1, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<1>();
  const atx::core::Result<SignalView> r0 = src.evaluate(panel);
  ASSERT_TRUE(r0.has_value());
  ASSERT_EQ(r0->values.size(), 1U);
  EXPECT_DOUBLE_EQ(r0->values[0], 0.25);

  const atx::core::Result<SignalView> r1 = src.evaluate(panel);
  ASSERT_TRUE(r1.has_value());
  EXPECT_DOUBLE_EQ(r1->values[0], -0.75);
}

TEST(ScriptedSignalSource, Evaluate_EmptySchedule_FirstCallIsErr) {
  // No scheduled vectors at all: the very first evaluate() is already exhausted.
  const std::vector<std::vector<atx::f64>> schedule{};
  ScriptedSignalSource src{schedule, /*universe_size=*/2, /*max_lookback=*/1};

  const PanelView panel = empty_panel_view<2>();
  const atx::core::Result<SignalView> r = src.evaluate(panel);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

// =====================================================================
//  Polymorphic use — the loop holds an ISignalSource&, not a concrete type.
// =====================================================================

TEST(ScriptedSignalSource, ThroughBaseInterface_EvaluateAndLookback) {
  constexpr atx::usize kUni = 2;
  const std::vector<std::vector<atx::f64>> schedule{{1.0, 2.0}};
  ScriptedSignalSource concrete{schedule, kUni, /*max_lookback=*/3};
  atx::engine::ISignalSource &seam = concrete;

  EXPECT_EQ(seam.max_lookback(), 3U);
  const PanelView panel = empty_panel_view<kUni>();
  const atx::core::Result<SignalView> r = seam.evaluate(panel);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->values.size(), kUni);
  EXPECT_DOUBLE_EQ(r->values[0], 1.0);
  EXPECT_DOUBLE_EQ(r->values[1], 2.0);
}

} // namespace

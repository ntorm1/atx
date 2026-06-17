// atx::engine::alpha — reference oracle unit tests (P3-5).
//
// End-to-end fixtures: parse_program -> analyze -> compile -> evaluate_reference,
// compared against hand-computed expected values. The oracle is the differential
// correctness reference for the fast VM (P3-6), so these pin the numeric
// contract (NaN propagation, min_periods, CsRank tie-break, std ddof).
//
// Covers the plan's list:
//   * element-wise: close/open - 1 on a small panel -> hand values;
//   * rank(close) over a date -> known percentile ranks; all-equal tie-break;
//   * ts_mean(close,2) and delay(close,1) -> NaN warm-up then trailing values;
//   * NaN-in/NaN-out propagation; out-of-universe -> NaN in a rank;
//   * (close > open) ? 1 : 0 -> per-cell mask select;
//   * boundary: 1x1 panel; all-NaN date (rank -> all NaN); empty-universe day.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atxtest_alpha_oracle_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// Process-lifetime Library (the Ast borrows OpSig pointers from it).
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Compile a DSL program string to a Program (asserts each stage succeeds).
[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto an = analyze(ast.value());
  EXPECT_TRUE(an.has_value()) << (an ? "" : an.error().message());
  auto prog = compile(ast.value(), an.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return std::move(prog).value();
}

// Build a Panel; field_data is one column per name, each dates*instruments.
[[nodiscard]] Panel panel_ok(atx::usize dates, atx::usize instruments,
                             std::vector<std::string> names,
                             std::vector<std::vector<atx::f64>> data,
                             std::vector<std::uint8_t> uni = {}) {
  auto res = Panel::create(dates, instruments, std::move(names), std::move(data), std::move(uni));
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

// Run a single-root program and return its alpha values (date-major).
[[nodiscard]] std::vector<atx::f64> eval_single(std::string_view src, const Panel &p) {
  const Program prog = compile_ok(src);
  auto res = evaluate_reference(prog, p);
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  const SignalSet ss = std::move(res).value();
  EXPECT_GE(ss.alphas.size(), 1U);
  return ss.alphas.empty() ? std::vector<atx::f64>{} : ss.alphas.front().values;
}

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

void expect_nan(atx::f64 v) { EXPECT_TRUE(std::isnan(v)) << "expected NaN, got " << v; }

// ---- element-wise -----------------------------------------------------------

TEST(AlphaOracle_Elementwise, CloseOverOpenMinusOne_HandComputed) {
  // 2x2: close d0=[10,20] d1=[15,40]; open d0=[5,10] d1=[10,20].
  const Panel p =
      panel_ok(2, 2, {"close", "open"}, {{10.0, 20.0, 15.0, 40.0}, {5.0, 10.0, 10.0, 20.0}});
  const auto v = eval_single("a = close / open - 1", p);
  ASSERT_EQ(v.size(), 4U);
  EXPECT_DOUBLE_EQ(v[0], 10.0 / 5.0 - 1.0);  // 1.0
  EXPECT_DOUBLE_EQ(v[1], 20.0 / 10.0 - 1.0); // 1.0
  EXPECT_DOUBLE_EQ(v[2], 15.0 / 10.0 - 1.0); // 0.5
  EXPECT_DOUBLE_EQ(v[3], 40.0 / 20.0 - 1.0); // 1.0
}

TEST(AlphaOracle_Elementwise, NaNSourceCell_PropagatesToResult) {
  const Panel p = panel_ok(1, 3, {"close", "open"}, {{10.0, kNaN, 30.0}, {5.0, 5.0, 5.0}});
  const auto v = eval_single("a = close + open", p);
  ASSERT_EQ(v.size(), 3U);
  EXPECT_DOUBLE_EQ(v[0], 15.0);
  expect_nan(v[1]); // NaN + 5 -> NaN
  EXPECT_DOUBLE_EQ(v[2], 35.0);
}

TEST(AlphaOracle_Select, GreaterThanMask_SelectsPerCell) {
  // (close > open) ? 1 : 0
  const Panel p = panel_ok(1, 3, {"close", "open"}, {{10.0, 5.0, 7.0}, {5.0, 5.0, 9.0}});
  const auto v = eval_single("a = (close > open) ? 1 : 0", p);
  ASSERT_EQ(v.size(), 3U);
  EXPECT_DOUBLE_EQ(v[0], 1.0); // 10>5
  EXPECT_DOUBLE_EQ(v[1], 0.0); // 5>5 false
  EXPECT_DOUBLE_EQ(v[2], 0.0); // 7>9 false
}

// ---- cross-sectional rank ---------------------------------------------------

TEST(AlphaOracle_Rank, DistinctValues_PercentileRanks) {
  // One date, 4 instruments: close = [30,10,40,20].
  // Sorted ascending: 10(0),20(1),30(2),40(3); pct = idx/(n-1)=idx/3.
  const Panel p = panel_ok(1, 4, {"close"}, {{30.0, 10.0, 40.0, 20.0}});
  const auto v = eval_single("a = rank(close)", p);
  ASSERT_EQ(v.size(), 4U);
  EXPECT_DOUBLE_EQ(v[0], 2.0 / 3.0); // 30 -> rank 2
  EXPECT_DOUBLE_EQ(v[1], 0.0 / 3.0); // 10 -> rank 0
  EXPECT_DOUBLE_EQ(v[2], 3.0 / 3.0); // 40 -> rank 3
  EXPECT_DOUBLE_EQ(v[3], 1.0 / 3.0); // 20 -> rank 1
}

TEST(AlphaOracle_Rank, AllEqual_DeterministicOrdinalTieBreak) {
  // All equal -> ordinal tie-break by instrument index -> 0/3,1/3,2/3,3/3.
  const Panel p = panel_ok(1, 4, {"close"}, {{5.0, 5.0, 5.0, 5.0}});
  const auto v = eval_single("a = rank(close)", p);
  ASSERT_EQ(v.size(), 4U);
  EXPECT_DOUBLE_EQ(v[0], 0.0);
  EXPECT_DOUBLE_EQ(v[1], 1.0 / 3.0);
  EXPECT_DOUBLE_EQ(v[2], 2.0 / 3.0);
  EXPECT_DOUBLE_EQ(v[3], 1.0);
}

TEST(AlphaOracle_Rank, OutOfUniverseInstrument_IsNaNAndExcluded) {
  // 1 date, 3 instruments; inst1 masked out. Valid set = {inst0=30, inst2=10}.
  // ranks over valid: 10->0/1=0, 30->1/1=1. inst1 -> NaN.
  const Panel p =
      panel_ok(1, 3, {"close"}, {{30.0, 99.0, 10.0}}, std::vector<std::uint8_t>{1, 0, 1});
  const auto v = eval_single("a = rank(close)", p);
  ASSERT_EQ(v.size(), 3U);
  EXPECT_DOUBLE_EQ(v[0], 1.0); // 30 is the larger of the valid pair
  expect_nan(v[1]);            // masked out
  EXPECT_DOUBLE_EQ(v[2], 0.0); // 10 is the smaller
}

TEST(AlphaOracle_Rank, AllNaNDate_AllNaN) {
  const Panel p = panel_ok(1, 3, {"close"}, {{kNaN, kNaN, kNaN}});
  const auto v = eval_single("a = rank(close)", p);
  ASSERT_EQ(v.size(), 3U);
  for (const atx::f64 c : v) {
    expect_nan(c);
  }
}

TEST(AlphaOracle_Rank, EmptyUniverseDay_AllNaN) {
  const Panel p = panel_ok(1, 3, {"close"}, {{1.0, 2.0, 3.0}}, std::vector<std::uint8_t>{0, 0, 0});
  const auto v = eval_single("a = rank(close)", p);
  ASSERT_EQ(v.size(), 3U);
  for (const atx::f64 c : v) {
    expect_nan(c);
  }
}

// ---- time-series ------------------------------------------------------------

TEST(AlphaOracle_TsMean, Window2_WarmupNaNThenTrailing) {
  // 3 dates x 1 inst: close = [2,4,10] (date-major).
  // ts_mean(close,2): d0 NaN (short window), d1=(2+4)/2=3, d2=(4+10)/2=7.
  const Panel p = panel_ok(3, 1, {"close"}, {{2.0, 4.0, 10.0}});
  const auto v = eval_single("a = ts_mean(close, 2)", p);
  ASSERT_EQ(v.size(), 3U);
  expect_nan(v[0]);
  EXPECT_DOUBLE_EQ(v[1], 3.0);
  EXPECT_DOUBLE_EQ(v[2], 7.0);
}

TEST(AlphaOracle_Delay, Lag1_WarmupNaNThenShifted) {
  // delay(close,1): d0 NaN, d1=close[d0], d2=close[d1].
  const Panel p = panel_ok(3, 1, {"close"}, {{2.0, 4.0, 10.0}});
  const auto v = eval_single("a = delay(close, 1)", p);
  ASSERT_EQ(v.size(), 3U);
  expect_nan(v[0]);
  EXPECT_DOUBLE_EQ(v[1], 2.0);
  EXPECT_DOUBLE_EQ(v[2], 4.0);
}

TEST(AlphaOracle_TsMean, NaNInWindow_ProducesNaN) {
  // close = [2, NaN, 10]; ts_mean(.,2): d1 window {2,NaN}->NaN, d2 {NaN,10}->NaN.
  const Panel p = panel_ok(3, 1, {"close"}, {{2.0, kNaN, 10.0}});
  const auto v = eval_single("a = ts_mean(close, 2)", p);
  ASSERT_EQ(v.size(), 3U);
  expect_nan(v[0]);
  expect_nan(v[1]);
  expect_nan(v[2]);
}

TEST(AlphaOracle_TsDelta, Window1_DiffOfAdjacent) {
  const Panel p = panel_ok(3, 1, {"close"}, {{2.0, 5.0, 9.0}});
  const auto v = eval_single("a = delta(close, 1)", p);
  ASSERT_EQ(v.size(), 3U);
  expect_nan(v[0]);
  EXPECT_DOUBLE_EQ(v[1], 3.0); // 5-2
  EXPECT_DOUBLE_EQ(v[2], 4.0); // 9-5
}

TEST(AlphaOracle_TsSum, Window2_TrailingSum) {
  const Panel p = panel_ok(3, 1, {"close"}, {{1.0, 2.0, 3.0}});
  const auto v = eval_single("a = ts_sum(close, 2)", p);
  ASSERT_EQ(v.size(), 3U);
  expect_nan(v[0]);
  EXPECT_DOUBLE_EQ(v[1], 3.0);
  EXPECT_DOUBLE_EQ(v[2], 5.0);
}

TEST(AlphaOracle_TsStd, Window3_SampleDdof1) {
  // close = [2,4,6]; ts_std(.,3) at d2 = sample std of {2,4,6} = 2.0.
  const Panel p = panel_ok(3, 1, {"close"}, {{2.0, 4.0, 6.0}});
  const auto v = eval_single("a = ts_std(close, 3)", p);
  ASSERT_EQ(v.size(), 3U);
  expect_nan(v[0]);
  expect_nan(v[1]);
  EXPECT_DOUBLE_EQ(v[2], 2.0); // sqrt(((2-4)^2+(4-4)^2+(6-4)^2)/2) = sqrt(4)
}

// ---- boundary: 1x1 ----------------------------------------------------------

TEST(AlphaOracle_Boundary, OneByOnePanel_ElementwiseScalar) {
  const Panel p = panel_ok(1, 1, {"close"}, {{42.0}});
  const auto v = eval_single("a = close + 1", p);
  ASSERT_EQ(v.size(), 1U);
  EXPECT_DOUBLE_EQ(v[0], 43.0);
}

// ---- multiple roots ---------------------------------------------------------

TEST(AlphaOracle_MultiRoot, TwoBindings_BothAlphasPopulated) {
  const Panel p = panel_ok(1, 2, {"close", "open"}, {{10.0, 20.0}, {5.0, 4.0}});
  const Program prog = compile_ok("a = close - open\nb = close + open");
  auto res = evaluate_reference(prog, p);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  const SignalSet ss = std::move(res).value();
  ASSERT_EQ(ss.alphas.size(), 2U);
  // Roots are indexed by StoreAlpha output index in declaration order.
  EXPECT_EQ(ss.alphas[0].name, "a");
  EXPECT_EQ(ss.alphas[1].name, "b");
  EXPECT_DOUBLE_EQ(ss.alphas[0].values[0], 5.0);  // 10-5
  EXPECT_DOUBLE_EQ(ss.alphas[0].values[1], 16.0); // 20-4
  EXPECT_DOUBLE_EQ(ss.alphas[1].values[0], 15.0); // 10+5
  EXPECT_DOUBLE_EQ(ss.alphas[1].values[1], 24.0); // 20+4
}

// ---- missing field ----------------------------------------------------------

TEST(AlphaOracle_Validate, MissingField_ReturnsErr) {
  const Panel p = panel_ok(1, 1, {"close"}, {{1.0}});
  const Program prog = compile_ok("a = volume + 1"); // volume not in Panel
  auto res = evaluate_reference(prog, p);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::NotFound);
}


}  // namespace atxtest_alpha_oracle_test

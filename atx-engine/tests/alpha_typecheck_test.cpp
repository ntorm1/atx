// atx::engine::alpha — semantic analysis unit tests (P3-3).
//
// Covers the plan's test list verbatim:
//   * shape: rank(close)→V; close+open→P; ts_mean(close,5)→P; close→P; 5→Scalar;
//   * dtype: close>open→Mask; (close>open)?close:open→F64 (cond Mask);
//     indneutralize(close, IndClass.sector)→ok (Group 2nd arg); arithmetic on a
//     mask→error; indneutralize(close, open) (2nd not Group)→error; SELECT with
//     non-mask cond→error;
//   * lookback: ts_mean(delta(close,5),10)→14; ts_sum(ts_mean(x,3),4)→5;
//     delay(close,5)→5; ts_mean(close,10)→9; correlation(close,volume,6)→5;
//     close+open→0;
//   * window rails: ts_mean(close,0)→err; ts_mean(close,-3)→err;
//     ts_mean(close,close) (non-literal window)→err;
//   * boundary: 2*3+1→Scalar lookback 0; deeply nested ts;
//   * shape mismatch: rank(5) / ts_mean(3,2) → scalar primary operand → error.
//
// Naming: Subject_Condition_ExpectedResult.

#include <string_view>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace {

using atx::core::ErrorCode;
using atx::engine::alpha::Analysis;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Ast;
using atx::engine::alpha::DType;
using atx::engine::alpha::Library;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Shape;
using atx::engine::alpha::TypeInfo;

// ---- helpers ----------------------------------------------------------------

// A process-lifetime Library so any `const OpSig*` a parsed Ast borrows stays
// valid for the duration of every test (the Library must outlive the Ast).
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Parse a bare expression then analyze it. ASSERTs both succeed and returns the
// TypeInfo of the (single anonymous) root.
[[nodiscard]] TypeInfo analyze_root(std::string_view src) {
  auto parsed = parse_expr(src, shared_lib());
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return TypeInfo{};
  }
  const Ast &ast = *parsed;
  auto res = analyze(ast);
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  if (!res) {
    return TypeInfo{};
  }
  const Analysis &a = *res;
  return a.info(ast.roots().front().root);
}

// Parse + analyze; expect analysis to fail with the given code.
void expect_analyze_error(std::string_view src, ErrorCode code = ErrorCode::InvalidArgument) {
  auto parsed = parse_expr(src, shared_lib());
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  auto res = analyze(*parsed);
  ASSERT_FALSE(res.has_value()) << "expected analysis error for: " << src;
  EXPECT_EQ(res.error().code(), code);
}

// ---- shape ------------------------------------------------------------------

TEST(AlphaTypecheck_Shape, RankOfField_IsCrossSection) {
  EXPECT_EQ(analyze_root("rank(close)").shape, Shape::CrossSection);
}

TEST(AlphaTypecheck_Shape, FieldPlusField_IsPanel) {
  EXPECT_EQ(analyze_root("close + open").shape, Shape::Panel);
}

TEST(AlphaTypecheck_Shape, TsMean_IsPanel) {
  EXPECT_EQ(analyze_root("ts_mean(close, 5)").shape, Shape::Panel);
}

TEST(AlphaTypecheck_Shape, BareField_IsPanel) {
  EXPECT_EQ(analyze_root("close").shape, Shape::Panel);
}

TEST(AlphaTypecheck_Shape, Literal_IsScalar) { EXPECT_EQ(analyze_root("5").shape, Shape::Scalar); }

TEST(AlphaTypecheck_Shape, SelectWithPanelCondScalarBranches_IsPanel) {
  // (close > open) ? 1 : 0 — branches are scalar, but the per-cell select runs
  // over the panel mask, so the result must widen to Panel (cond widens too).
  EXPECT_EQ(analyze_root("(close > open) ? 1 : 0").shape, Shape::Panel);
}

// ---- dtype ------------------------------------------------------------------

TEST(AlphaTypecheck_DType, Compare_ProducesMask) {
  EXPECT_EQ(analyze_root("close > open").dtype, DType::Mask);
}

TEST(AlphaTypecheck_DType, FieldLeaf_IsF64) { EXPECT_EQ(analyze_root("close").dtype, DType::F64); }

TEST(AlphaTypecheck_DType, IndClassField_IsGroup) {
  EXPECT_EQ(analyze_root("IndClass.sector").dtype, DType::Group);
}

TEST(AlphaTypecheck_DType, NotOfMask_IsMask) {
  EXPECT_EQ(analyze_root("!(close > open)").dtype, DType::Mask);
}

TEST(AlphaTypecheck_DType, SelectWithMaskCond_IsF64) {
  // (close > open) ? close : open — cond is a Mask, result is F64.
  EXPECT_EQ(analyze_root("(close > open) ? close : open").dtype, DType::F64);
}

TEST(AlphaTypecheck_DType, Logical_RequiresMaskOperands) {
  // (close>open) && (open>0) — both sides are masks → Mask result.
  EXPECT_EQ(analyze_root("(close > open) && (open > 0)").dtype, DType::Mask);
}

TEST(AlphaTypecheck_DType, IndNeutralizeWithGroup_Ok) {
  // 2nd arg is a Group classifier → accepted.
  const TypeInfo t = analyze_root("indneutralize(close, IndClass.sector)");
  EXPECT_EQ(t.shape, Shape::CrossSection);
  EXPECT_EQ(t.dtype, DType::F64);
}

TEST(AlphaTypecheck_DType, ArithmeticOnMask_IsError) {
  // (close > open) + open — adding a mask is a type error.
  expect_analyze_error("(close > open) + open");
}

TEST(AlphaTypecheck_DType, IndNeutralizeWithNonGroup_IsError) {
  // 2nd arg `open` is F64, not a Group classifier → error.
  expect_analyze_error("indneutralize(close, open)");
}

TEST(AlphaTypecheck_DType, SelectWithNonMaskCond_IsError) {
  // cond `close` is F64, not a Mask → error.
  expect_analyze_error("close ? close : open");
}

TEST(AlphaTypecheck_DType, NotOfNonMask_IsError) {
  // !close — `close` is F64; Not requires a Mask operand.
  expect_analyze_error("!close");
}

TEST(AlphaTypecheck_DType, NegOfMask_IsError) {
  // -(close > open) — Neg requires F64, not Mask.
  expect_analyze_error("-(close > open)");
}

TEST(AlphaTypecheck_DType, CompareOfMask_IsError) {
  // (close > open) > open — comparison requires F64 operands.
  expect_analyze_error("(close > open) > open");
}

// ---- lookback ---------------------------------------------------------------

TEST(AlphaTypecheck_Lookback, TsMeanOfDelta_Is14) {
  // ts_mean(delta(close,5),10): delta = 5+0; ts_mean = (10-1)+5 = 14.
  const TypeInfo t = analyze_root("ts_mean(delta(close, 5), 10)");
  EXPECT_EQ(t.lookback, 14U);
}

TEST(AlphaTypecheck_Lookback, TsSumOfTsMean_Is5) {
  // ts_sum(ts_mean(x,3),4): ts_mean = (3-1)+0 = 2; ts_sum = (4-1)+2 = 5.
  const TypeInfo t = analyze_root("ts_sum(ts_mean(close, 3), 4)");
  EXPECT_EQ(t.lookback, 5U);
}

TEST(AlphaTypecheck_Lookback, Delay_IsShiftAmount) {
  // delay(close,5): shift family → 5 + 0 = 5.
  EXPECT_EQ(analyze_root("delay(close, 5)").lookback, 5U);
}

TEST(AlphaTypecheck_Lookback, TsMean_IsWindowMinusOne) {
  // ts_mean(close,10): rolling → (10-1)+0 = 9.
  EXPECT_EQ(analyze_root("ts_mean(close, 10)").lookback, 9U);
}

TEST(AlphaTypecheck_Lookback, Correlation_IsWindowMinusOne) {
  // correlation(close,volume,6): rolling, arity-3 window is arg c → (6-1)+0 = 5.
  EXPECT_EQ(analyze_root("correlation(close, volume, 6)").lookback, 5U);
}

TEST(AlphaTypecheck_Lookback, ElementWise_IsZero) {
  EXPECT_EQ(analyze_root("close + open").lookback, 0U);
}

TEST(AlphaTypecheck_Lookback, CrossSectional_AddsZero) {
  // rank(ts_mean(close,10)) → cross-section adds 0 → still 9.
  EXPECT_EQ(analyze_root("rank(ts_mean(close, 10))").lookback, 9U);
}

TEST(AlphaTypecheck_Lookback, RequiredLookback_IsMaxOverRoots) {
  EXPECT_EQ(analyze_root("ts_mean(delta(close, 5), 10)").lookback, 14U);
}

// ---- window rails -----------------------------------------------------------

TEST(AlphaTypecheck_Window, ZeroWindow_IsError) { expect_analyze_error("ts_mean(close, 0)"); }

TEST(AlphaTypecheck_Window, NegativeWindow_IsError) {
  // -3 const-folds to literal -3 → non-positive window.
  expect_analyze_error("ts_mean(close, -3)");
}

TEST(AlphaTypecheck_Window, NonLiteralWindow_IsError) {
  // window must be a compile-time constant; `close` is a field.
  expect_analyze_error("ts_mean(close, close)");
}

TEST(AlphaTypecheck_Window, FractionalWindow_IsFloored) {
  // P3b-4 lock #3: a fractional positive window is FLOORED (was: rejected). The
  // 101-alphas paper mines fractional constants; the canonical convention is
  // floor(d). ts_mean(close, 8.7) → window 8 → rolling lookback (8-1)+0 = 7,
  // bit-identical to ts_mean(close, 8).
  EXPECT_EQ(analyze_root("ts_mean(close, 8.7)").lookback,
            analyze_root("ts_mean(close, 8)").lookback);
  EXPECT_EQ(analyze_root("ts_mean(close, 8.7)").lookback, 7U);
}

TEST(AlphaTypecheck_Window, SubOneWindow_FloorsToZero_IsError) {
  // 0.5 floors to 0 — a zero window has nothing to roll over → still rejected.
  expect_analyze_error("ts_mean(close, 0.5)");
}

// ---- shape mismatch ---------------------------------------------------------

TEST(AlphaTypecheck_Shape, RankOfScalar_IsError) {
  // rank's primary operand is a pure scalar → reject.
  expect_analyze_error("rank(5)");
}

TEST(AlphaTypecheck_Shape, TsMeanOfScalar_IsError) {
  // ts_mean's primary operand is a pure scalar → reject.
  expect_analyze_error("ts_mean(3, 2)");
}

// ---- boundary ---------------------------------------------------------------

TEST(AlphaTypecheck_Boundary, PureScalarExpr_OkScalarZeroLookback) {
  // 2*3+1 folds to a literal scalar; analysis succeeds with zero lookback.
  const TypeInfo t = analyze_root("2 * 3 + 1");
  EXPECT_EQ(t.shape, Shape::Scalar);
  EXPECT_EQ(t.dtype, DType::F64);
  EXPECT_EQ(t.lookback, 0U);
}

TEST(AlphaTypecheck_Boundary, DeeplyNestedTs_AccumulatesLookback) {
  // ts_sum(ts_mean(delta(close,5),10),4): 14 + (4-1) = 17.
  EXPECT_EQ(analyze_root("ts_sum(ts_mean(delta(close, 5), 10), 4)").lookback, 17U);
}

// ---- required_lookback over a multi-root program ----------------------------

TEST(AlphaTypecheck_Required, MaxOverAllRoots) {
  using atx::engine::alpha::parse_program;
  auto parsed = parse_program("a = ts_mean(close, 10)\nb = delay(open, 3)", shared_lib());
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  auto res = analyze(*parsed);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  // max(9, 3) = 9.
  EXPECT_EQ(res->required_lookback(), 9U);
}

} // namespace

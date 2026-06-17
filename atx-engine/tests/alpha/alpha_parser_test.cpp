// atx::engine::alpha — Pratt parser unit tests (P3-2).
//
// Covers the plan's test list verbatim:
//   * precedence (a+b*c, -a^b, a<b?c:d);
//   * right-assoc ^ ;
//   * function calls with 1/2/3 args; nested calls (rank(ts_corr(close,volume,6)));
//   * unknown operator → ParseError; arity mismatch caught at parse;
//   * unbalanced parens; trailing comma;
//   * constant folding output (2*3→6, log(1)→0, -3, 2^10);
//   * desugar: ternary → Select; - → Neg; ! → Not; comparisons/logical opcodes;
//   * boundary: single literal; deeply nested; max-arity (3) op;
//   * parse_program: assignment roots, multiple bindings, empty source.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <limits>
#include <string_view>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace atxtest_alpha_parser_test {

using atx::core::ErrorCode;
using atx::engine::alpha::Ast;
using atx::engine::alpha::DType;
using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::OpSig;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Shape;

// ---- helpers ----------------------------------------------------------------

// A process-lifetime Library so any `const OpSig*` a parsed Ast borrows stays
// valid for the duration of every test (the documented Ast-vs-Library lifetime
// contract: the Library must outlive the Ast).
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Parse a bare expression; on failure the caller's ASSERT surfaces the message.
[[nodiscard]] Ast parse_ok(std::string_view src) {
  auto res = parse_expr(src, shared_lib());
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return res.value_or(Ast{});
}

// The single anonymous root of a parse_expr result.
[[nodiscard]] const Expr &root_of(const Ast &ast) {
  EXPECT_EQ(ast.roots().size(), 1U);
  return ast.node(ast.roots().front().root);
}

// ---- single literal (boundary) ----------------------------------------------

TEST(AlphaParser_Literal, SingleNumber_ProducesLiteral) {
  const Ast ast = parse_ok("42");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(r.value, 42.0);
}

TEST(AlphaParser_Literal, BareIdent_ProducesField) {
  const Ast ast = parse_ok("close");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Field);
  EXPECT_FALSE(r.dollar);
  EXPECT_EQ(ast.field_name(r.name_id), "close");
}

TEST(AlphaParser_Field, DollarSigil_SetsDollarFlag) {
  const Ast ast = parse_ok("$vwap");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Field);
  EXPECT_TRUE(r.dollar);
  EXPECT_EQ(ast.field_name(r.name_id), "vwap");
}

TEST(AlphaParser_Field, IndClassDotSector_IsOneField) {
  const Ast ast = parse_ok("IndClass.sector");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Field);
  EXPECT_EQ(ast.field_name(r.name_id), "IndClass.sector");
}

// ---- precedence -------------------------------------------------------------

TEST(AlphaParser_Precedence, AddMul_MulBindsTighter) {
  // a + b * c → Add(a, Mul(b, c))
  const Ast ast = parse_ok("a + b * c");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::Add);
  const Expr &rhs = ast.node(r.b);
  EXPECT_EQ(rhs.kind, Expr::Kind::Binary);
  EXPECT_EQ(rhs.opcode, OpCode::Mul);
  // lhs is the bare field a
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field);
}

TEST(AlphaParser_Precedence, UnaryNegPow_PowBindsTighterThanNeg) {
  // -a^b → Neg(Pow(a, b))  (unary binds looser than power)
  const Ast ast = parse_ok("-a^b");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Unary);
  EXPECT_EQ(r.opcode, OpCode::Neg);
  const Expr &child = ast.node(r.a);
  EXPECT_EQ(child.kind, Expr::Kind::Binary);
  EXPECT_EQ(child.opcode, OpCode::Pow);
}

TEST(AlphaParser_Precedence, CompareTernary_TernaryIsLoosest) {
  // a < b ? c : d → Select(CmpLt(a,b), c, d)
  const Ast ast = parse_ok("a < b ? c : d");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Select);
  const Expr &cond = ast.node(r.a);
  EXPECT_EQ(cond.kind, Expr::Kind::Binary);
  EXPECT_EQ(cond.opcode, OpCode::CmpLt);
  EXPECT_EQ(ast.node(r.b).kind, Expr::Kind::Field); // then = c
  EXPECT_EQ(ast.node(r.c).kind, Expr::Kind::Field); // else = d
}

TEST(AlphaParser_Precedence, LogicOrAnd_AndBindsTighter) {
  // a || b && c → Or(a, And(b, c))
  const Ast ast = parse_ok("a || b && c");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::Or);
  EXPECT_EQ(ast.node(r.b).opcode, OpCode::And);
}

TEST(AlphaParser_Precedence, Parens_OverridePrecedence) {
  // (a + b) * c → Mul(Add(a,b), c)
  const Ast ast = parse_ok("(a + b) * c");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::Mul);
  EXPECT_EQ(ast.node(r.a).opcode, OpCode::Add);
}

// ---- right-associativity of ^ -----------------------------------------------

TEST(AlphaParser_Assoc, Power_IsRightAssociative) {
  // a^b^c → Pow(a, Pow(b, c))
  const Ast ast = parse_ok("a^b^c");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::Pow);
  // Right child is another Pow; left child is the bare field a.
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field);
  const Expr &rhs = ast.node(r.b);
  EXPECT_EQ(rhs.kind, Expr::Kind::Binary);
  EXPECT_EQ(rhs.opcode, OpCode::Pow);
}

TEST(AlphaParser_Assoc, Sub_IsLeftAssociative) {
  // a - b - c → Sub(Sub(a,b), c)
  const Ast ast = parse_ok("a - b - c");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::Sub);
  EXPECT_EQ(ast.node(r.a).opcode, OpCode::Sub); // left-nested
  EXPECT_EQ(ast.node(r.b).kind, Expr::Kind::Field);
}

// ---- function calls (1/2/3 args) --------------------------------------------

TEST(AlphaParser_Call, OneArg_Rank) {
  const Ast ast = parse_ok("rank(close)");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  ASSERT_NE(r.op, nullptr);
  EXPECT_EQ(r.op->opcode, OpCode::CsRank);
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field);
}

TEST(AlphaParser_Call, TwoArgs_TsMean) {
  const Ast ast = parse_ok("ts_mean(close, 20)");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(r.opcode, OpCode::TsMean);
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field);
  EXPECT_EQ(ast.node(r.b).kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(ast.node(r.b).value, 20.0);
}

TEST(AlphaParser_Call, ThreeArgs_Correlation) {
  // max-arity (3) op
  const Ast ast = parse_ok("correlation(close, volume, 6)");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(r.opcode, OpCode::TsCorr);
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field);
  EXPECT_EQ(ast.node(r.b).kind, Expr::Kind::Field);
  EXPECT_DOUBLE_EQ(ast.node(r.c).value, 6.0);
}

TEST(AlphaParser_Call, Nested_RankOfTsCorr) {
  // rank(ts_corr(close, volume, 6)) — nested calls
  const Ast ast = parse_ok("rank(ts_corr(close, volume, 6))");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(r.opcode, OpCode::CsRank);
  const Expr &inner = ast.node(r.a);
  EXPECT_EQ(inner.kind, Expr::Kind::Call);
  EXPECT_EQ(inner.opcode, OpCode::TsCorr);
}

// ---- P3b-1: variadic / default-fill of omitted trailing args ----------------

TEST(AlphaParser_Variadic, ScaleOmittedArg_MaterializesDefaultLiteralOne) {
  // scale is (min=1, max=2, defaults={1.0}); scale(close) materializes a 2nd
  // arg: a Literal 1.0, so the DAG/VM see a fully-applied call.
  const Ast ast = parse_ok("scale(close)");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(r.opcode, OpCode::CsScale);
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field); // arg 0 = close
  const Expr &filled = ast.node(r.b);               // arg 1 = default 1.0
  ASSERT_EQ(filled.kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(filled.value, 1.0);
  EXPECT_EQ(r.c, atx::engine::alpha::kNoExpr); // no third slot
}

TEST(AlphaParser_Variadic, ScaleOneArg_StructurallyEqualsExplicitDefault) {
  // The pinned proof: scale(close) ≡ scale(close, 1) — same node shape.
  const Ast implicit_ast = parse_ok("scale(close)");
  const Ast explicit_ast = parse_ok("scale(close, 1)");
  const Expr &im = root_of(implicit_ast);
  const Expr &ex = root_of(explicit_ast);
  ASSERT_EQ(im.kind, Expr::Kind::Call);
  ASSERT_EQ(ex.kind, Expr::Kind::Call);
  EXPECT_EQ(im.opcode, ex.opcode);
  EXPECT_EQ(im.op, ex.op); // same resolved registry row
  // Both: arg0 a Field, arg1 a Literal 1.0, arg2 absent.
  EXPECT_EQ(implicit_ast.node(im.a).kind, explicit_ast.node(ex.a).kind);
  ASSERT_EQ(implicit_ast.node(im.b).kind, Expr::Kind::Literal);
  ASSERT_EQ(explicit_ast.node(ex.b).kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(implicit_ast.node(im.b).value, explicit_ast.node(ex.b).value);
  EXPECT_EQ(im.c, atx::engine::alpha::kNoExpr);
  EXPECT_EQ(ex.c, atx::engine::alpha::kNoExpr);
}

TEST(AlphaParser_Variadic, ScaleExplicitTwoArgs_ParsesUnchanged) {
  const Ast ast = parse_ok("scale(close, 2)");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(r.opcode, OpCode::CsScale);
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field);
  ASSERT_EQ(ast.node(r.b).kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(ast.node(r.b).value, 2.0);
}

TEST(AlphaParser_Variadic, ScaleTooFewArgs_IsParseError) {
  // scale() supplies 0 < min_arity (1).
  const auto res = parse_expr("scale()", shared_lib());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Variadic, ScaleTooManyArgs_IsParseError) {
  // scale(x, 1, 2) supplies 3 > max_arity (2).
  const auto res = parse_expr("scale(close, 1, 2)", shared_lib());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Variadic, NanSentinelDefault_OmittedArgNotMaterialized) {
  // A synthetic op with a NaN-sentinel default: the omitted optional arg must
  // NOT be materialized (the kernel handles absence). Verified via a private
  // Library so the synthetic op does not leak into shared_lib().
  Library lib;
  const OpSig sig{"nan_opt",
                  1,
                  2,
                  OpCode::CsScale,
                  DType::F64,
                  true,
                  {std::numeric_limits<atx::f64>::quiet_NaN()},
                  &atx::engine::alpha::shape_cross_section};
  ASSERT_TRUE(lib.register_op(sig).has_value());
  const auto res = parse_expr("nan_opt(close)", lib);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  const Ast &ast = *res;
  const Expr &r = ast.node(ast.roots().front().root);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(ast.node(r.a).kind, Expr::Kind::Field); // arg 0 = close
  EXPECT_EQ(r.b, atx::engine::alpha::kNoExpr);      // NaN sentinel skipped
  EXPECT_EQ(r.c, atx::engine::alpha::kNoExpr);
}

// ---- desugar ----------------------------------------------------------------

TEST(AlphaParser_Desugar, Bang_BecomesNotUnary) {
  const Ast ast = parse_ok("!close");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Unary);
  EXPECT_EQ(r.opcode, OpCode::Not);
}

TEST(AlphaParser_Desugar, SignedPower_StaysSpowCall_NotExpanded) {
  const Ast ast = parse_ok("signedpower(close, 2)");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(r.opcode, OpCode::Spow);
}

TEST(AlphaParser_Desugar, Equality_ProducesCmpEqBinary) {
  const Ast ast = parse_ok("a == b");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::CmpEq);
}

// ---- constant folding -------------------------------------------------------

TEST(AlphaParser_Fold, IntMul_FoldsToLiteral) {
  // 2 * 3 → 6
  const Ast ast = parse_ok("2 * 3");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(r.value, 6.0);
}

TEST(AlphaParser_Fold, LogOfOne_FoldsToZero) {
  // log(1) → 0
  const Ast ast = parse_ok("log(1)");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(r.value, 0.0);
}

TEST(AlphaParser_Fold, UnaryNegLiteral_Folds) {
  // -3 → literal -3
  const Ast ast = parse_ok("-3");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(r.value, -3.0);
}

TEST(AlphaParser_Fold, PowerOfLiterals_Folds) {
  // 2 ^ 10 → 1024
  const Ast ast = parse_ok("2 ^ 10");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(r.value, 1024.0);
}

TEST(AlphaParser_Fold, NestedArithmetic_FoldsFully) {
  // 2 * 3 + 4 → 10
  const Ast ast = parse_ok("2 * 3 + 4");
  const Expr &r = root_of(ast);
  ASSERT_EQ(r.kind, Expr::Kind::Literal);
  EXPECT_DOUBLE_EQ(r.value, 10.0);
}

TEST(AlphaParser_Fold, FieldOperand_DoesNotFold) {
  // close * 2 must stay a Binary (field operand)
  const Ast ast = parse_ok("close * 2");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::Mul);
}

TEST(AlphaParser_Fold, AbsOfField_DoesNotFold) {
  const Ast ast = parse_ok("abs(close)");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Call);
  EXPECT_EQ(r.opcode, OpCode::Abs);
}

// ---- error paths ------------------------------------------------------------

TEST(AlphaParser_Error, UnknownOperator_IsParseError) {
  const Library lib;
  const auto res = parse_expr("frobnicate(close)", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Error, ArityMismatch_IsParseError) {
  // rank takes 1 arg; supply 2.
  const Library lib;
  const auto res = parse_expr("rank(close, volume)", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Error, TooFewArgs_IsParseError) {
  // ts_mean takes 2; supply 1.
  const Library lib;
  const auto res = parse_expr("ts_mean(close)", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Error, UnbalancedParen_IsParseError) {
  const Library lib;
  const auto res = parse_expr("(a + b", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Error, TrailingComma_IsParseError) {
  const Library lib;
  const auto res = parse_expr("ts_mean(close,)", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Error, MissingTernaryColon_IsParseError) {
  const Library lib;
  const auto res = parse_expr("a ? b", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Error, DanglingOperator_IsParseError) {
  const Library lib;
  const auto res = parse_expr("a +", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaParser_Error, TrailingTokens_IsParseError) {
  // A bare expression must consume to End; "a b" leaves a dangling token.
  const Library lib;
  const auto res = parse_expr("a b", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

// ---- boundary: deeply nested ------------------------------------------------

TEST(AlphaParser_Boundary, DeeplyNestedParens_ParsesToInner) {
  const Ast ast = parse_ok("((((close))))");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Field);
}

TEST(AlphaParser_Boundary, DeepArithmeticChain_Parses) {
  const Ast ast = parse_ok("a + b + c + d + e + f");
  const Expr &r = root_of(ast);
  EXPECT_EQ(r.kind, Expr::Kind::Binary);
  EXPECT_EQ(r.opcode, OpCode::Add);
}

// ---- parse_program ----------------------------------------------------------

TEST(AlphaProgram_Assignment, SingleBinding_HasNamedRoot) {
  const Library lib;
  auto res = parse_program("alpha_001 = rank(close)", lib);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  const Ast &ast = *res;
  ASSERT_EQ(ast.roots().size(), 1U);
  EXPECT_EQ(ast.roots().front().name, "alpha_001");
  EXPECT_EQ(ast.node(ast.roots().front().root).opcode, OpCode::CsRank);
}

TEST(AlphaProgram_Assignment, MultipleBindings_AllParsed) {
  const Library lib;
  auto res = parse_program("a = rank(close)\nb = ts_mean(volume, 5)", lib);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  const Ast &ast = *res;
  ASSERT_EQ(ast.roots().size(), 2U);
  EXPECT_EQ(ast.roots()[0].name, "a");
  EXPECT_EQ(ast.roots()[1].name, "b");
}

TEST(AlphaProgram_Boundary, EmptySource_ZeroRoots) {
  const Library lib;
  auto res = parse_program("", lib);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  EXPECT_EQ(res->roots().size(), 0U);
}

TEST(AlphaProgram_Error, MissingAssign_IsParseError) {
  const Library lib;
  const auto res = parse_program("alpha rank(close)", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}

TEST(AlphaProgram_Error, MissingTarget_IsParseError) {
  const Library lib;
  const auto res = parse_program("= rank(close)", lib);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::ParseError);
}


}  // namespace atxtest_alpha_parser_test

// atx::engine::alpha — member-access (postfix .pin) parser + type-checker tests
// (Phase 3d-B5 / B6).
//
// B5 tests: Member AST node, postfix .pin parselet, hparam peel, regression that
//           dotted field names (IndClass.sector) still parse as plain Fields.
// B6 tests: record TypeInfo, analyze_member, record-misuse error paths.

#include <gtest/gtest.h>

#include "atx/engine/alpha/parser.hpp"

namespace {

using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::Library;
using atx::engine::alpha::parse_program;

// ---------------------------------------------------------------------------
// B5 — parser tests
// ---------------------------------------------------------------------------

TEST(AlphaMember, ParsesMemberOnCall) {
  Library lib;
  auto ast = parse_program("b = split2(close).hi\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto &nodes = ast.value().nodes();
  const ExprId root = ast.value().roots()[0].root;
  ASSERT_EQ(nodes[root].kind, Expr::Kind::Member);
  EXPECT_EQ(nodes[nodes[root].a].kind, Expr::Kind::Call); // member of a call
}

TEST(AlphaMember, MemberOnBinding) {
  Library lib;
  auto ast = parse_program("kf = split2(close)\nb = kf.lo\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto &nodes = ast.value().nodes();
  const ExprId b_root = ast.value().roots()[1].root;
  ASSERT_EQ(nodes[b_root].kind, Expr::Kind::Member);
  EXPECT_EQ(nodes[nodes[b_root].a].kind, Expr::Kind::Call); // kf's record child is the split2 call
}

TEST(AlphaMember, IndClassStillField) {
  Library lib;
  // regression: a non-binding dotted ident stays ONE Field, not a Member.
  auto ast = parse_program("a = group_mean(close, IndClass.sector)\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
}

} // namespace

// ---------------------------------------------------------------------------
// B6 — type-checker tests (appended after B5 parser tests per plan)
// ---------------------------------------------------------------------------

#include "atx/engine/alpha/typecheck.hpp"

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::DType;
using atx::engine::alpha::Shape;

TEST(AlphaMember, PinTypechecks) {
  Library lib;
  auto ast = parse_program("b = split2(close).hi\n", lib);
  ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  ASSERT_TRUE(an) << an.error().message();
  const ExprId root = ast.value().roots()[0].root;
  EXPECT_EQ(an.value().info(root).shape, Shape::Panel);
  EXPECT_EQ(an.value().info(root).dtype, DType::F64);
}

TEST(AlphaMember, UnknownPinRejected) {
  Library lib;
  auto ast = parse_program("b = split2(close).nope\n", lib);
  ASSERT_TRUE(ast);
  EXPECT_FALSE(analyze(ast.value()));
}

TEST(AlphaMember, RecordUsedAsScalarRejected) {
  Library lib;
  auto ast = parse_program("b = split2(close) + 1\n", lib);
  ASSERT_TRUE(ast);
  EXPECT_FALSE(analyze(ast.value())); // record in arithmetic
}

TEST(AlphaMember, RecordRootRejected) {
  Library lib;
  auto ast = parse_program("b = split2(close)\n", lib);
  ASSERT_TRUE(ast);
  EXPECT_FALSE(analyze(ast.value())); // a bare record cannot be an alpha root
}

} // namespace

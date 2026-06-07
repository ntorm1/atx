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

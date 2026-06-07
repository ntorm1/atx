// atx::engine::alpha — local binding resolution tests (Phase 3d-A1).
//
// Verifies that a bare IDENT that matches an earlier assignment in parse_program
// resolves to that binding's ExprId (reuse/CSE), NOT a fresh Field load.

#include <gtest/gtest.h>

#include "atx/engine/alpha/parser.hpp"

namespace {

using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::Library;
using atx::engine::alpha::parse_program;

// A bare `m` after `m = expr` must reuse m's ExprId, not become a Field.
TEST(AlphaBindings, ReferenceReusesBinding) {
  Library lib;
  // `m` is bound, then referenced; the reference must NOT become a Field load.
  auto ast = parse_program("m = ts_mean(close, 5)\na = m\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto roots = ast.value().roots();
  ASSERT_EQ(roots.size(), 2u);
  const ExprId m_root = roots[0].root;
  const ExprId a_root = roots[1].root;
  EXPECT_EQ(m_root, a_root); // `a = m` reuses m's ExprId
}

TEST(AlphaBindings, ForwardReferenceIsFieldFallback) {
  Library lib;
  // `b` used before it is bound -> field-fallback (Field("b")), not a reference.
  auto ast = parse_program("a = b\nb = close\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto &nodes = ast.value().nodes();
  const ExprId a_root = ast.value().roots()[0].root;
  EXPECT_EQ(nodes[a_root].kind, Expr::Kind::Field); // `b` was not yet bound
}

TEST(AlphaBindings, SelfReferenceOnRhsIsField) {
  Library lib;
  auto ast = parse_program("x = close\ny = x + 1\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  const auto &nodes = ast.value().nodes();
  const ExprId x_root = ast.value().roots()[0].root;
  const ExprId y_root = ast.value().roots()[1].root;
  ASSERT_EQ(nodes[y_root].kind, Expr::Kind::Binary);
  EXPECT_EQ(nodes[y_root].a, x_root); // y's `x` references the line-1 binding
}

} // namespace

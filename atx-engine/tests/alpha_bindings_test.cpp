// atx::engine::alpha — local binding resolution tests (Phase 3d-A1).
//
// Verifies that a bare IDENT that matches an earlier assignment in parse_program
// resolves to that binding's ExprId (reuse/CSE), NOT a fresh Field load.

#include <gtest/gtest.h>

#include "atx/engine/alpha/parser.hpp"

namespace {

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

} // namespace

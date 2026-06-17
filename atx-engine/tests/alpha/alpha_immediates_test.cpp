// atx::engine::alpha — guard test for imm[2] / hparams / n_out plumbing (P3d-B1).
//
// This test verifies that a Const node still evaluates correctly after the type
// change from `f64 imm` to `std::array<f64,2> imm` (Const uses imm[0]).
// It must pass BOTH before and after the refactor — proving the change is
// behaviour-preserving. Named AlphaImmediates so it is picked up by the
// `ctest -R Alpha` filter.

#include <gtest/gtest.h>

#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_immediates_test {

using namespace atx::engine::alpha;

TEST(AlphaImmediates, ConstStillWorksWithImmArray) {
  Library lib;

  // Parse, analyze and compile "a = 3.5 + close"
  auto ast = parse_program("a = 3.5 + close\n", lib);
  ASSERT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());

  auto an = analyze(ast.value());
  ASSERT_TRUE(an.has_value()) << (an ? "" : an.error().message());

  auto prog = compile(ast.value(), an.value());
  ASSERT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());

  // Build a 1-date x 1-instrument panel with close=2.0 using the same
  // Panel::create API used by alpha_vm_test.cpp.
  // field_names, field_data both have one entry; universe is empty (all-valid).
  auto panel = Panel::create(
      /*dates=*/1,
      /*instruments=*/1,
      /*field_names=*/{"close"},
      /*field_data=*/{std::vector<atx::f64>{2.0}},
      /*universe=*/{});
  ASSERT_TRUE(panel.has_value()) << (panel ? "" : panel.error().message());

  Engine eng(panel.value());
  auto out = eng.evaluate(prog.value());
  ASSERT_TRUE(out.has_value()) << (out ? "" : out.error().message());

  // 3.5 + 2.0 = 5.5 (one alpha, one cell)
  ASSERT_EQ(out.value().alphas.size(), 1u);
  ASSERT_EQ(out.value().alphas[0].values.size(), 1u);
  EXPECT_DOUBLE_EQ(out.value().alphas[0].values[0], 5.5);
}


}  // namespace atxtest_alpha_immediates_test

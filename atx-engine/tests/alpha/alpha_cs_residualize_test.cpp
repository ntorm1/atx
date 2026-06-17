// atx::engine::alpha — cs_residualize differential + boundary-pin tests (S3.1).
//
// cs_residualize(x, g[, z]) is the general per-date cross-sectional
// regression-residual neutralizer: regress x on group dummies (+ an optional
// continuous style covariate z) over the valid set and emit the residual.
//
// Two load-bearing guarantees this suite proves:
//   1. BOUNDARY PIN — with no style covariate, cs_residualize(x, g) reduces to
//      the existing per-group demean (indneutralize / cs_group_demean) BIT-FOR-
//      BIT. This is the non-regression anchor on the proven demean kernel.
//   2. GENERAL FWL — with a covariate z, the residual is the Frisch-Waugh-Lovell
//      partial-out: within-group-demean x and z, remove the OLS slope of
//      demeaned-x on demeaned-z. VM == oracle to the last bit.
// Plus: NaN-z / out-of-universe exclusion, the degenerate (constant-z) reduction
// back to demean, and singleton/boundary panels.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_cs_residualize_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments,
                               std::vector<std::vector<atx::f64>> cols,
                               std::vector<std::uint8_t> universe = {}) {
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  auto p =
      Panel::create(dates, instruments, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

[[nodiscard]] std::vector<std::vector<atx::f64>> random_cols(atx::usize cells, std::uint64_t seed,
                                                             int num_sectors = 3) {
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{1.0, 100.0};
  std::uniform_real_distribution<atx::f64> vol{0.0, 1.0e6};
  std::uniform_int_distribution<int> sector{0, num_sectors - 1};
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = price(rng);
    cols[1][i] = price(rng);
    cols[2][i] = price(rng);
    cols[3][i] = price(rng);
    cols[4][i] = vol(rng);
    cols[5][i] = static_cast<atx::f64>(sector(rng));
  }
  return cols;
}

void expect_vm_matches_oracle(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto vm = engine.evaluate(prog);
  ASSERT_TRUE(vm.has_value()) << "VM: " << (vm ? "" : vm.error().message());
  auto ref = evaluate_reference(prog, panel);
  ASSERT_TRUE(ref.has_value()) << "oracle: " << (ref ? "" : ref.error().message());
  const SignalSet &v = vm.value();
  const SignalSet &r = ref.value();
  ASSERT_EQ(v.alphas.size(), r.alphas.size());
  for (atx::usize a = 0; a < v.alphas.size(); ++a) {
    ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
    for (atx::usize i = 0; i < v.alphas[a].values.size(); ++i) {
      EXPECT_TRUE(same_cell(v.alphas[a].values[i], r.alphas[a].values[i]))
          << "expr '" << expr << "' alpha " << a << " cell " << i
          << ": VM=" << v.alphas[a].values[i] << " oracle=" << r.alphas[a].values[i];
    }
  }
}

[[nodiscard]] std::vector<atx::f64> vm_values(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  if (!out.has_value() || out.value().alphas.empty()) {
    return {};
  }
  return out.value().alphas[0].values;
}

// ===========================================================================
//  BOUNDARY PIN — cs_residualize(x, g) == indneutralize(x, g) bit-for-bit.
// ===========================================================================

TEST(CsResidualize_BoundaryPin, NoStyle_EqualsIndneutralizeBitForBit) {
  const atx::usize dates = 9;
  const atx::usize instruments = 11;
  const Panel panel =
      make_panel(dates, instruments, random_cols(dates * instruments, 0xE51DULL, 4));
  const std::vector<atx::f64> resid = vm_values("cs_residualize(close, IndClass.sector)", panel);
  const std::vector<atx::f64> demean = vm_values("indneutralize(close, IndClass.sector)", panel);
  ASSERT_EQ(resid.size(), demean.size());
  for (atx::usize i = 0; i < resid.size(); ++i) {
    EXPECT_TRUE(same_cell(resid[i], demean[i]))
        << "cell " << i << ": residualize=" << resid[i] << " demean=" << demean[i];
  }
  // And the VM path agrees with the oracle for the new op.
  expect_vm_matches_oracle("cs_residualize(close, IndClass.sector)", panel);
}

// ===========================================================================
//  GENERAL FWL — with a continuous style covariate z.
// ===========================================================================

// Hand-computed single-group FWL. One date, 3 instruments, one sector (0).
// x = {1,2,6}, z = {1,2,3}. Within-group means: mean_x=3, mean_z=2.
// x~ = {-2,-1,3}, z~ = {-1,0,1}. beta = Sxz/Szz = 5/2 = 2.5.
// r = x~ - 2.5*z~ = {0.5, -1.0, 0.5}.
TEST(CsResidualize_FWL, SingleGroupCovariate_HandComputedResidual) {
  const atx::usize dates = 1;
  const atx::usize instruments = 3;
  auto cols = random_cols(dates * instruments, 0x100ULL);
  cols[0] = {1.0, 2.0, 6.0}; // close = x
  cols[1] = {1.0, 2.0, 3.0}; // open  = z (style covariate)
  cols[5] = {0.0, 0.0, 0.0}; // single sector
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> r = vm_values("cs_residualize(close, IndClass.sector, open)", panel);
  ASSERT_EQ(r.size(), instruments);
  EXPECT_DOUBLE_EQ(r[0], 0.5);
  EXPECT_DOUBLE_EQ(r[1], -1.0);
  EXPECT_DOUBLE_EQ(r[2], 0.5);
  // Residual is orthogonal to the covariate within the group: sum to ~0.
  EXPECT_NEAR(r[0] + r[1] + r[2], 0.0, 1e-12);
  expect_vm_matches_oracle("cs_residualize(close, IndClass.sector, open)", panel);
}

TEST(CsResidualize_FWL, RandomPanelWithCovariate_MatchesOracle) {
  const atx::usize dates = 11;
  const atx::usize instruments = 9;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0xC0DEULL));
  expect_vm_matches_oracle("cs_residualize(close, IndClass.sector, open)", panel);
  // Composed inside a larger DAG.
  expect_vm_matches_oracle("rank(cs_residualize(close, IndClass.sector, volume))", panel);
}

// A covariate that is constant within every group has zero within-group
// variance, so beta is 0 and the residual collapses back to the demean.
TEST(CsResidualize_FWL, ConstantCovariatePerGroup_ReducesToDemean) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  auto cols = random_cols(dates * instruments, 0x200ULL);
  cols[0] = {10.0, 20.0, 100.0, 140.0};
  cols[1] = {7.0, 7.0, 3.0, 3.0};     // open = z constant within each sector
  cols[5] = {0.0, 0.0, 1.0, 1.0};     // two sectors
  const Panel panel = make_panel(dates, instruments, std::move(cols));

  const std::vector<atx::f64> r = vm_values("cs_residualize(close, IndClass.sector, open)", panel);
  const std::vector<atx::f64> d = vm_values("indneutralize(close, IndClass.sector)", panel);
  ASSERT_EQ(r.size(), d.size());
  for (atx::usize i = 0; i < r.size(); ++i) {
    EXPECT_DOUBLE_EQ(r[i], d[i]) << "cell " << i;
  }
}

// ===========================================================================
//  NaN / universe — a cell missing x, group, or z is excluded (NaN out).
// ===========================================================================

TEST(CsResidualize_Nan, OutOfUniverseAndNaNCovariate_ExcludedNaN) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(dates * instruments, 0x300ULL);
  cols[0] = {10.0, 20.0, 30.0, 40.0, 50.0};
  cols[1] = {1.0, 2.0, 3.0, 4.0, 5.0};
  cols[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
  std::vector<std::uint8_t> universe(instruments, std::uint8_t{1});
  universe[2] = 0; // instrument 2 out of universe -> NaN x AND z
  const Panel panel = make_panel(dates, instruments, std::move(cols), std::move(universe));

  const std::vector<atx::f64> r = vm_values("cs_residualize(close, IndClass.sector, open)", panel);
  ASSERT_EQ(r.size(), instruments);
  EXPECT_TRUE(std::isnan(r[2]));
  expect_vm_matches_oracle("cs_residualize(close, IndClass.sector, open)", panel);
}

// ===========================================================================
//  Boundaries.
// ===========================================================================

TEST(CsResidualize_Boundary, SingletonAndOneByOne_MatchesOracle) {
  const Panel p1 = make_panel(3, 1, random_cols(3, 0x400ULL));
  expect_vm_matches_oracle("cs_residualize(close, IndClass.sector)", p1);
  expect_vm_matches_oracle("cs_residualize(close, IndClass.sector, open)", p1);
  const Panel p2 = make_panel(1, 1, random_cols(1, 0x401ULL));
  expect_vm_matches_oracle("cs_residualize(close, IndClass.sector, open)", p2);
}


}  // namespace atxtest_alpha_cs_residualize_test

// atx::engine::alpha — multi-output (Split2 + Pin) VM/oracle tests (P3d-B9).
//
// Verifies that:
//   * Split2 + Pin round-trips through the full compile -> VM -> oracle pipeline.
//   * VM == oracle bit-for-bit on both outputs (differential).
//   * The two outputs are logically independent: lo == -hi by the Split2 contract
//     (hi=x, lo=-x), proving that distinct Pin projections project distinct slots.
//
// Uses the same Panel::create / make_panel helpers as alpha_vm_test.cpp.

#include <cmath>
#include <cstdint>
#include <string>
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
#include "atx/engine/alpha/vm.hpp"

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// Two cells agree iff both NaN, or exactly equal (covers ±inf, ±0).
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Build a minimal Panel with one "close" field over dates x instruments.
// `data` must have exactly dates*instruments entries (date-major order).
[[nodiscard]] Panel make_close_panel(atx::usize dates, atx::usize instruments,
                                     std::vector<atx::f64> data) {
  const atx::usize cells = dates * instruments;
  EXPECT_EQ(data.size(), cells);
  std::vector<std::vector<atx::f64>> cols{std::move(data)};
  auto p = Panel::create(dates, instruments, {"close"}, std::move(cols), /*universe=*/{});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  // All-valid universe (empty == all-in-universe).
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// =============================================================================
//  Split2ValuesAndDifferential
//
//  2 dates x 2 instruments; close = {1.0, -2.0, 3.0, -4.0} (date-major).
//  split2(close).hi == close; split2(close).lo == -close.
//  Assertions:
//    1. VM succeeds and oracle succeeds.
//    2. VM == oracle on both outputs (bit-exact differential).
//    3. lo_vm == -hi_vm for every cell (proves distinct pin projection).
// =============================================================================

TEST(AlphaMultiOutput, Split2ValuesAndDifferential) {
  const atx::usize dates = 2;
  const atx::usize instruments = 2;
  // date-major: [d=0,j=0]=1, [d=0,j=1]=-2, [d=1,j=0]=3, [d=1,j=1]=-4
  std::vector<atx::f64> close_data = {1.0, -2.0, 3.0, -4.0};

  Panel panel = make_close_panel(dates, instruments, close_data);

  Library lib;
  auto ast = parse_program("a = split2(close).hi\nb = split2(close).lo\n", lib);
  ASSERT_TRUE(ast) << ast.error().message();
  auto an = analyze(ast.value());
  ASSERT_TRUE(an) << an.error().message();
  auto prog = compile(ast.value(), an.value());
  ASSERT_TRUE(prog) << prog.error().message();

  Engine eng(panel);
  auto vm_result = eng.evaluate(prog.value());
  ASSERT_TRUE(vm_result) << vm_result.error().message();

  auto orc_result = evaluate_reference(prog.value(), panel);
  ASSERT_TRUE(orc_result) << orc_result.error().message();

  const SignalSet &vm = vm_result.value();
  const SignalSet &orc = orc_result.value();

  ASSERT_EQ(vm.alphas.size(), 2U);
  ASSERT_EQ(orc.alphas.size(), 2U);
  ASSERT_EQ(vm.dates, dates);
  ASSERT_EQ(vm.instruments, instruments);

  const auto &hi_vm = vm.alphas[0].values; // .hi == close
  const auto &lo_vm = vm.alphas[1].values; // .lo == -close
  const auto &hi_or = orc.alphas[0].values;
  const auto &lo_or = orc.alphas[1].values;

  ASSERT_EQ(hi_vm.size(), dates * instruments);
  ASSERT_EQ(lo_vm.size(), dates * instruments);

  for (atx::usize i = 0; i < hi_vm.size(); ++i) {
    // VM == oracle for hi
    EXPECT_TRUE(same_cell(hi_vm[i], hi_or[i]))
        << "hi mismatch at i=" << i << ": vm=" << hi_vm[i] << " oracle=" << hi_or[i];
    // VM == oracle for lo
    EXPECT_TRUE(same_cell(lo_vm[i], lo_or[i]))
        << "lo mismatch at i=" << i << ": vm=" << lo_vm[i] << " oracle=" << lo_or[i];
    // lo == -hi (proves the two Pin projections read distinct output slots)
    EXPECT_EQ(lo_vm[i], -hi_vm[i])
        << "lo != -hi at i=" << i << ": lo=" << lo_vm[i] << " hi=" << hi_vm[i];
  }
}

// =============================================================================
//  Warm second evaluate() reuses the pool without asserting or corrupting data.
// =============================================================================

TEST(AlphaMultiOutput, WarmEvaluateProducesSameResult) {
  const atx::usize dates = 3;
  const atx::usize instruments = 2;
  std::vector<atx::f64> close_data = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0};

  Panel panel = make_close_panel(dates, instruments, close_data);

  Library lib;
  auto ast = parse_program("a = split2(close).hi\nb = split2(close).lo\n", lib);
  ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  ASSERT_TRUE(an);
  auto prog = compile(ast.value(), an.value());
  ASSERT_TRUE(prog);

  Engine eng(panel);
  auto first = eng.evaluate(prog.value());
  ASSERT_TRUE(first);
  auto second = eng.evaluate(prog.value());
  ASSERT_TRUE(second);

  // Both calls must produce identical values.
  const auto &f0 = first.value().alphas[0].values;
  const auto &s0 = second.value().alphas[0].values;
  const auto &f1 = first.value().alphas[1].values;
  const auto &s1 = second.value().alphas[1].values;
  ASSERT_EQ(f0.size(), s0.size());
  for (atx::usize i = 0; i < f0.size(); ++i) {
    EXPECT_TRUE(same_cell(f0[i], s0[i])) << "warm hi mismatch at i=" << i;
    EXPECT_TRUE(same_cell(f1[i], s1[i])) << "warm lo mismatch at i=" << i;
  }
}

} // namespace

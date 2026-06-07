// atx::engine::alpha — P3d-C5: VM+oracle differential for kalman_level + ou_filter.
//
// Two evidence layers per filter op:
//   1. HAND-MATH: known-value trace for a tiny 2-date, 1-instrument panel.
//      Each expected value is derived analytically in the comment; any kernel
//      regression produces a mismatch against the closed-form result.
//   2. DIFFERENTIAL: vm.hpp Engine::evaluate vs evaluate_reference (the oracle)
//      must agree bit-for-bit for every cell. The differential is the primary
//      correctness gate.
//
// Panel layout: date-major (date*instruments + instrument), 1 instrument, 2 dates.
// kalman_level(close, Q=0.1, R=1.0):
//   t=0: seed -> x=2.0, P=R=1.0; out=2.0
//   t=1: predict P=1.0+0.1=1.1; K=1.1/2.1; x=2+K*(4-2); out=x
//
// ou_filter(close, theta=0.6931471805599453, mu=10.0):
//   phi = exp(-theta) = 0.5 (exactly, since theta == ln(2) to full precision)
//   t=0: seed -> xhat=2.0; out=2.0
//   t=1: xhat = 10 + phi*(2-10) = 10 - 4 = 6.0; out=6.0

#include <cmath>
#include <string_view>
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

// Build a 2-date, 1-instrument panel with a single "close" field.
// data is {val_at_t0, val_at_t1} (date-major order).
[[nodiscard]] Panel make_close_panel(double t0, double t1) {
  auto res = Panel::create(/*dates=*/2, /*instruments=*/1, {"close"}, {{t0, t1}}, /*universe=*/{});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  return std::move(res).value();
}

// Compile src through the full pipeline; ASSERT each stage succeeds.
[[nodiscard]] Program compile_ok(std::string_view src, const Library &lib) {
  auto ast = parse_program(src, lib);
  EXPECT_TRUE(ast) << (ast ? "" : ast.error().message());
  auto an = analyze(ast.value());
  EXPECT_TRUE(an) << (an ? "" : an.error().message());
  auto prog = compile(ast.value(), an.value());
  EXPECT_TRUE(prog) << (prog ? "" : prog.error().message());
  return std::move(prog).value();
}

// ===========================================================================
//  KalmanLevel differential + hand-math
// ===========================================================================

TEST(AlphaKalmanLevel, VmMatchesOracleAndHandMath) {
  Library lib;
  const Program prog = compile_ok("a = kalman_level(close, 0.1, 1.0)\n", lib);
  const Panel panel = make_close_panel(2.0, 4.0);

  Engine eng(panel);
  auto vm_res = eng.evaluate(prog);
  ASSERT_TRUE(vm_res) << vm_res.error().message();
  const SignalSet &vm = vm_res.value();

  auto orc_res = evaluate_reference(prog, panel);
  ASSERT_TRUE(orc_res) << orc_res.error().message();
  const SignalSet &orc = orc_res.value();

  // Hand-math:
  // t=0: seed with z=2.0 -> x=2.0, P=R=1.0; out=2.0
  EXPECT_DOUBLE_EQ(vm.alphas[0].values[0], 2.0);
  // t=1: predict P=1.0+Q=1.0+0.1=1.1; K=1.1/(1.1+R)=1.1/2.1;
  //       x = 2.0 + K*(4.0-2.0) = 2.0 + (1.1/2.1)*2.0
  const double K = 1.1 / 2.1;
  const double expected_t1 = 2.0 + K * 2.0;
  EXPECT_DOUBLE_EQ(vm.alphas[0].values[1], expected_t1);

  // Bit-exact differential: VM == oracle for every cell.
  ASSERT_EQ(vm.alphas[0].values.size(), orc.alphas[0].values.size());
  for (atx::usize i = 0; i < vm.alphas[0].values.size(); ++i) {
    EXPECT_EQ(vm.alphas[0].values[i], orc.alphas[0].values[i])
        << "cell " << i << " vm=" << vm.alphas[0].values[i] << " orc=" << orc.alphas[0].values[i];
  }
}

// ===========================================================================
//  OuFilter differential + hand-math
// ===========================================================================

TEST(AlphaOuFilter, VmMatchesOracleAndHandMath) {
  Library lib;
  // Use a literal string with 17 significant figures so the parser round-trips
  // the double for ln(2) exactly — std::to_string gives only 6 decimal places
  // which would truncate theta and shift phi away from 0.5.
  // phi = exp(-0.6931471805599453) = 0.5 exactly in IEEE 754 double.
  const Program prog = compile_ok("a = ou_filter(close, 0.6931471805599453, 10.0)\n", lib);
  // close = [2.0, 99.0]: t=0 seeds at 2.0; t=1 pulls toward mu=10 with phi=0.5:
  //   xhat = 10 + 0.5*(2-10) = 6.0
  // NOTE: x[t=1]=99.0 is INTENTIONALLY ignored after seeding — the OU pull is
  // observation-free per spec §4.3 (xhat = mu + phi*(xhat-mu), no x[t] term), so
  // the 99.0 not affecting the output is by design, not a kernel bug.
  const Panel panel = make_close_panel(2.0, 99.0);

  Engine eng(panel);
  auto vm_res = eng.evaluate(prog);
  ASSERT_TRUE(vm_res) << vm_res.error().message();
  const SignalSet &vm = vm_res.value();

  auto orc_res = evaluate_reference(prog, panel);
  ASSERT_TRUE(orc_res) << orc_res.error().message();
  const SignalSet &orc = orc_res.value();

  // Hand-math using the same phi the kernel computes:
  EXPECT_DOUBLE_EQ(vm.alphas[0].values[0], 2.0); // seed
  const double phi = std::exp(-0.6931471805599453);
  const double expected_t1 = 10.0 + phi * (2.0 - 10.0); // = 6.0
  EXPECT_DOUBLE_EQ(vm.alphas[0].values[1], expected_t1);

  // Bit-exact differential: VM == oracle for every cell.
  ASSERT_EQ(vm.alphas[0].values.size(), orc.alphas[0].values.size());
  for (atx::usize i = 0; i < vm.alphas[0].values.size(); ++i) {
    EXPECT_EQ(vm.alphas[0].values[i], orc.alphas[0].values[i])
        << "cell " << i << " vm=" << vm.alphas[0].values[i] << " orc=" << orc.alphas[0].values[i];
  }
}

// ===========================================================================
//  KalmanReg differential (D3)
// ===========================================================================

TEST(AlphaKalmanReg, VmMatchesOracle) {
  Library lib;
  auto ast = parse_program("al = kalman(close, open, 0.0001, 0.001).alpha\n"
                           "be = kalman(close, open, 0.0001, 0.001).beta\n"
                           "re = kalman(close, open, 0.0001, 0.001).resid\n",
                           lib);
  ASSERT_TRUE(ast);
  auto an = analyze(ast.value());
  ASSERT_TRUE(an);
  auto prog = compile(ast.value(), an.value());
  ASSERT_TRUE(prog);

  // Verify: single shared KalmanReg compute instruction feeds all 3 pins.
  int compute = 0;
  for (const auto &in : prog.value().code) {
    if (in.op == atx::engine::alpha::OpCode::KalmanReg) {
      ++compute;
    }
  }
  EXPECT_EQ(compute, 1);

  // 4 dates x 2 instruments; close and open with varied finite values.
  const atx::usize dates = 4;
  const atx::usize instruments = 2;
  // date-major: [d=0,j=0],[d=0,j=1],[d=1,j=0],[d=1,j=1], ...
  std::vector<atx::f64> close_data = {1.0, 2.0, 3.5, 4.2, 5.1, 6.3, 7.8, 8.4};
  std::vector<atx::f64> open_data = {0.9, 1.8, 3.0, 3.9, 4.7, 5.8, 7.0, 7.9};
  std::vector<std::vector<atx::f64>> cols{close_data, open_data};
  auto panel_res =
      Panel::create(dates, instruments, {"close", "open"}, std::move(cols), /*universe=*/{});
  ASSERT_TRUE(panel_res) << panel_res.error().message();
  const Panel &panel = panel_res.value();

  Engine eng(panel);
  auto vm_res = eng.evaluate(prog.value());
  ASSERT_TRUE(vm_res) << vm_res.error().message();

  auto orc_res = evaluate_reference(prog.value(), panel);
  ASSERT_TRUE(orc_res) << orc_res.error().message();

  ASSERT_EQ(vm_res.value().alphas.size(), 3U);
  ASSERT_EQ(orc_res.value().alphas.size(), 3U);

  for (atx::usize a = 0; a < 3; ++a) {
    const auto &vv = vm_res.value().alphas[a].values;
    const auto &ov = orc_res.value().alphas[a].values;
    ASSERT_EQ(vv.size(), ov.size());
    for (atx::usize i = 0; i < vv.size(); ++i) {
      EXPECT_EQ(vv[i], ov[i]) << "alpha[" << a << "] cell " << i << " vm=" << vv[i]
                              << " orc=" << ov[i];
    }
  }
}

} // namespace

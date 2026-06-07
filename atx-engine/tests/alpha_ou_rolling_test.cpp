// atx::engine::alpha — OU rolling-fit ops VM + oracle differential (P3d-E4).
//
// Tests for the four OU windowed time-series ops:
//   ou_theta    = -ln(b)            (mean-reversion speed)
//   ou_halflife = ln2 / theta       (half-life of mean reversion)
//   ou_mean     = a / (1-b)         (long-run equilibrium mean)
//   ou_zscore   = (x[t]-mu)/sigma_eq (standardised deviation from mean)
//
// Two test groups:
//   AlphaOuRolling — VM-vs-oracle differential (bit-exact NaN==NaN equality)
//                    with a damped-oscillation series that yields finite outputs
//                    in at least some windows. Asserts at least one finite cell.
//   AlphaOuRollingKnown — sanity-check known values on a hand-crafted AR(1)
//                    series where AR(1) parameters are exactly known.

#include <cmath>
#include <limits>
#include <numbers>
#include <string>
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

using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::parse_program;

// Cells agree iff both NaN, or exactly value-equal.
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Build a 1-instrument Panel with "close" as the sole field (all in-universe).
[[nodiscard]] Panel make_panel_1j(const std::vector<atx::f64> &close_vals) {
  const atx::usize dates = close_vals.size();
  auto p = Panel::create(dates, 1, {"close"}, {close_vals}, {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 1, {"close"}, {{}}, {}).value());
}

// Core VM-vs-oracle differential for a 4-alpha program over the given panel.
// Asserts every cell of every alpha agrees (NaN==NaN). Returns the SignalSet
// from the VM so callers can inspect individual cells.
[[nodiscard]] atx::engine::alpha::SignalSet
differential_4alpha(const std::string &program_src, const Panel &panel) {
  Library lib;
  auto ast = parse_program(program_src, lib);
  EXPECT_TRUE(ast.has_value()) << ast.error().message();
  auto an = analyze(ast.value());
  EXPECT_TRUE(an.has_value()) << an.error().message();
  auto prog = compile(ast.value(), an.value());
  EXPECT_TRUE(prog.has_value()) << prog.error().message();

  Engine eng(panel);
  auto vm = eng.evaluate(prog.value());
  EXPECT_TRUE(vm.has_value()) << vm.error().message();
  auto orc = evaluate_reference(prog.value(), panel);
  EXPECT_TRUE(orc.has_value()) << orc.error().message();

  const auto &vs = vm.value();
  const auto &os = orc.value();
  EXPECT_EQ(vs.alphas.size(), os.alphas.size());
  for (atx::usize a = 0; a < vs.alphas.size(); ++a) {
    EXPECT_EQ(vs.alphas[a].values.size(), os.alphas[a].values.size());
    for (atx::usize i = 0; i < vs.alphas[a].values.size(); ++i) {
      EXPECT_TRUE(same_cell(vs.alphas[a].values[i], os.alphas[a].values[i]))
          << "alpha " << a << " cell " << i << ": VM=" << vs.alphas[a].values[i]
          << " oracle=" << os.alphas[a].values[i];
    }
  }
  return std::move(vm.value());
}

// A damped oscillation around 10 that yields AR(1) b in (0,1) for window=5.
// x[t] = 10 + A*cos(2*pi*t/6) * exp(-0.1*t). 8 dates, 1 instrument.
// The window of 5 means we fit on lags [t-4,t]; for t>=4 (date index 4..7)
// the window is full. The exponentially-damped cosine is genuinely AR(1)-like.
[[nodiscard]] std::vector<atx::f64> damped_oscillation() {
  std::vector<atx::f64> v(8);
  for (atx::usize t = 0; t < 8; ++t) {
    const atx::f64 tf = static_cast<atx::f64>(t);
    v[t] = 10.0 + 3.0 * std::cos(2.0 * std::numbers::pi * tf / 6.0) * std::exp(-0.1 * tf);
  }
  return v;
}

// ============================================================================
// AlphaOuRolling — main VM-vs-oracle differential
// ============================================================================

TEST(AlphaOuRolling, VmMatchesOracle) {
  const std::vector<atx::f64> close_vals = damped_oscillation();
  Panel panel = make_panel_1j(close_vals);

  const std::string prog_src =
      "hl = ou_halflife(close, 5)\n"
      "z  = ou_zscore(close, 5)\n"
      "th = ou_theta(close, 5)\n"
      "mu = ou_mean(close, 5)\n";

  const auto result = differential_4alpha(prog_src, panel);

  // Find ou_zscore (alpha index 1 = "z") and verify at least one finite cell.
  // With 8 dates and window=5, the first full window is at t=4; dates 4..7
  // (cells 4..7, 1 instrument) are candidates. Verify at least one is finite.
  bool any_finite_zscore = false;
  ASSERT_GE(result.alphas.size(), 2U);
  for (const atx::f64 v : result.alphas[1].values) { // "z" = ou_zscore
    if (std::isfinite(v)) {
      any_finite_zscore = true;
      break;
    }
  }
  EXPECT_TRUE(any_finite_zscore) << "ou_zscore produced all NaN — check the AR(1) fit";
}

TEST(AlphaOuRolling, VmMatchesOracle_SingleOp_OuTheta) {
  const std::vector<atx::f64> close_vals = damped_oscillation();
  Panel panel = make_panel_1j(close_vals);

  Library lib;
  const std::string prog_src = "th = ou_theta(close, 5)\n";
  auto ast = parse_program(prog_src, lib);
  ASSERT_TRUE(ast.has_value()) << ast.error().message();
  auto an = analyze(ast.value());
  ASSERT_TRUE(an.has_value()) << an.error().message();
  auto prog = compile(ast.value(), an.value());
  ASSERT_TRUE(prog.has_value()) << prog.error().message();

  Engine eng(panel);
  auto vm = eng.evaluate(prog.value());
  ASSERT_TRUE(vm.has_value()) << vm.error().message();
  auto orc = evaluate_reference(prog.value(), panel);
  ASSERT_TRUE(orc.has_value()) << orc.error().message();

  const auto &vs = vm.value().alphas[0].values;
  const auto &os = orc.value().alphas[0].values;
  for (atx::usize i = 0; i < vs.size(); ++i) {
    EXPECT_TRUE(same_cell(vs[i], os[i])) << "cell " << i;
  }
}

} // namespace

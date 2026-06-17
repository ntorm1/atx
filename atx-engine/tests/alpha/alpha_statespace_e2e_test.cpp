// atx::engine::alpha — end-to-end state-space / OU pairs pipeline + causality
// regression (P3d-F1).
//
//   KalmanOuPipeline — the headline pairs program (Chan kalman record -> beta +
//     spread pins -> OU half-life + trading signal). A record value cannot be a
//     terminal alpha output, so the kalman(...) call is inlined into both `.pin`
//     projections; hash-cons CSE merges the two identical calls into ONE Kalman
//     scan that feeds both pins. Asserts that single shared scan, the root count,
//     and VM == oracle bit-for-bit.
//   NoLookAhead — perturbing ONLY the last date's input must not change any
//     EARLIER output cell of a causal op. Checks both a forward-scan recurrence
//     (kalman_level) and a trailing-window fit (ou_zscore): both must be strictly
//     causal.

#include <cmath>
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

namespace atxtest_alpha_statespace_e2e_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;

// Cells agree iff both NaN, or exactly value-equal.
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Deterministic ret/hedge panel: hedge mean-reverts around 1, ret tracks
// ~1.5*hedge plus a small wobble, so the Kalman residual (spread) is itself
// mean-reverting (a good OU input). Values are pure functions of (t, j).
[[nodiscard]] Panel make_pairs_panel(atx::usize dates, atx::usize instruments) {
  std::vector<atx::f64> hedge(dates * instruments);
  std::vector<atx::f64> ret(dates * instruments);
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize j = 0; j < instruments; ++j) {
      const atx::f64 tf = static_cast<atx::f64>(t);
      const atx::f64 jf = static_cast<atx::f64>(j);
      const atx::f64 h = 1.0 + 0.5 * std::sin(0.3 * tf + jf);
      hedge[t * instruments + j] = h;
      ret[t * instruments + j] = 1.5 * h + 0.1 * std::cos(0.2 * tf + jf);
    }
  }
  auto p = Panel::create(dates, instruments, {"ret", "hedge"}, {ret, hedge}, {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 1, {"ret", "hedge"}, {{}, {}}, {}).value());
}

// ============================================================================
// KalmanOuPipeline — the headline pairs-trading program, end to end.
// ============================================================================

TEST(AlphaStatespaceE2E, KalmanOuPipeline) {
  Library lib;
  const std::string src = "beta = kalman(ret, hedge, 0.0001, 0.001).beta\n"
                          "spread = kalman(ret, hedge, 0.0001, 0.001).resid\n"
                          "hl = ou_halflife(spread, 20)\n"
                          "sig = -ou_zscore(spread, 20)\n";
  auto ast = parse_program(src, lib);
  ASSERT_TRUE(ast.has_value()) << ast.error().message();
  auto an = analyze(ast.value());
  ASSERT_TRUE(an.has_value()) << an.error().message();
  auto prog = compile(ast.value(), an.value());
  ASSERT_TRUE(prog.has_value()) << prog.error().message();

  // beta + spread share ONE Kalman scan (single compute node, two pins).
  atx::usize kalman_scans = 0;
  for (const auto &in : prog.value().code) {
    if (in.op == OpCode::KalmanReg) {
      ++kalman_scans;
    }
  }
  EXPECT_EQ(kalman_scans, 1U);
  // 4 roots: beta, spread, hl, sig (the kalman record is projected to pins, so
  // it is never itself a terminal alpha output).
  EXPECT_EQ(prog.value().roots.size(), 4U);

  Panel panel = make_pairs_panel(24, 3);
  Engine eng(panel);
  auto vm = eng.evaluate(prog.value());
  ASSERT_TRUE(vm.has_value()) << vm.error().message();
  auto orc = evaluate_reference(prog.value(), panel);
  ASSERT_TRUE(orc.has_value()) << orc.error().message();

  const auto &vs = vm.value();
  const auto &os = orc.value();
  ASSERT_EQ(vs.alphas.size(), os.alphas.size());
  for (atx::usize a = 0; a < vs.alphas.size(); ++a) {
    ASSERT_EQ(vs.alphas[a].values.size(), os.alphas[a].values.size());
    for (atx::usize i = 0; i < vs.alphas[a].values.size(); ++i) {
      EXPECT_TRUE(same_cell(vs.alphas[a].values[i], os.alphas[a].values[i]))
          << "alpha " << a << " cell " << i;
    }
  }
}

// ============================================================================
// NoLookAhead — causal-by-construction regression.
// ============================================================================

// Evaluate `src` over a single-field "close" panel; return alpha[0] values.
[[nodiscard]] std::vector<atx::f64> eval_close_alpha0(const std::string &src,
                                                      const std::vector<atx::f64> &close,
                                                      atx::usize dates, atx::usize instruments) {
  Library lib;
  auto ast = parse_program(src, lib);
  EXPECT_TRUE(ast.has_value()) << ast.error().message();
  auto an = analyze(ast.value());
  EXPECT_TRUE(an.has_value()) << an.error().message();
  auto prog = compile(ast.value(), an.value());
  EXPECT_TRUE(prog.has_value()) << prog.error().message();
  auto panel = Panel::create(dates, instruments, {"close"}, {close}, {});
  EXPECT_TRUE(panel.has_value()) << (panel ? "" : panel.error().message());
  Engine eng(panel.value());
  auto vm = eng.evaluate(prog.value());
  EXPECT_TRUE(vm.has_value()) << vm.error().message();
  return vm.value().alphas[0].values;
}

TEST(AlphaStatespaceE2E, NoLookAhead) {
  const atx::usize dates = 10;
  const atx::usize instruments = 2;
  const atx::usize cells = dates * instruments;

  // Base "close": deterministic, mildly oscillating series.
  std::vector<atx::f64> base(cells);
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize j = 0; j < instruments; ++j) {
      const atx::f64 tf = static_cast<atx::f64>(t);
      base[t * instruments + j] = 10.0 + std::sin(0.4 * tf + static_cast<atx::f64>(j));
    }
  }
  // Perturbed copy: differs ONLY at the last date (all instruments).
  std::vector<atx::f64> bumped = base;
  for (atx::usize j = 0; j < instruments; ++j) {
    bumped[(dates - 1) * instruments + j] += 5.0;
  }

  // A forward-scan recurrence AND a trailing-window fit — both must be causal.
  const std::vector<std::string> programs = {
      "a = kalman_level(close, 0.01, 0.1)\n",
      "a = ou_zscore(close, 5)\n",
  };
  for (const std::string &src : programs) {
    const auto out_base = eval_close_alpha0(src, base, dates, instruments);
    const auto out_bumped = eval_close_alpha0(src, bumped, dates, instruments);
    ASSERT_EQ(out_base.size(), cells);
    ASSERT_EQ(out_bumped.size(), cells);
    // Every cell STRICTLY BEFORE the last date must be bit-identical (no leak).
    for (atx::usize t = 0; t + 1 < dates; ++t) {
      for (atx::usize j = 0; j < instruments; ++j) {
        const atx::usize i = t * instruments + j;
        EXPECT_TRUE(same_cell(out_base[i], out_bumped[i]))
            << "src=" << src << " look-ahead leak at t=" << t << " j=" << j;
      }
    }
  }
}


}  // namespace atxtest_alpha_statespace_e2e_test

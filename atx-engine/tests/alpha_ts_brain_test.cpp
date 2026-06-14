// atx::engine::alpha — BRAIN-superset time-series ops (S3.2).
//
// Four new rolling operators, each with an oracle twin (oracle == VM bit-for-bit)
// plus a correctness pin (a differential alone only proves the two paths AGREE,
// not that they are right — so every op also carries a known-value/property test):
//   * ts_regression(y, x, d) — rolling OLS slope (beta) of y on x.
//   * ts_decay_exp(x, d, f)  — exponential-weight decay (f^k, newest heaviest).
//   * ts_moment(x, d, k)     — k-th central moment (generalizes skew/kurt).
//   * ts_entropy(x, d, b)    — rolling Shannon entropy over b equal-width buckets.
// Plus the lookahead rail (warm-up cells are NaN; a value never reads past
// [t-d+1, t]) and the any-NaN/short-window -> NaN policy.
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

namespace {

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

[[nodiscard]] bool compiles(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  if (!ast.has_value()) {
    return false;
  }
  auto ana = analyze(ast.value());
  return ana.has_value();
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

[[nodiscard]] std::vector<std::vector<atx::f64>> random_cols(atx::usize cells, std::uint64_t seed) {
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{1.0, 100.0};
  std::uniform_real_distribution<atx::f64> vol{0.0, 1.0e6};
  std::uniform_int_distribution<int> sector{0, 2};
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
          << "expr '" << expr << "' cell " << i << ": VM=" << v.alphas[a].values[i]
          << " oracle=" << r.alphas[a].values[i];
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
//  Differential — every new op on a seeded-random multi-date panel.
// ===========================================================================

TEST(AlphaTsBrain_Differential, EveryNewOp_RandomPanel_MatchesOracle) {
  const atx::usize dates = 14;
  const atx::usize instruments = 7;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0xB3A1FULL));
  for (const std::string_view e : {
           std::string_view{"ts_regression(close, open, 5)"},
           std::string_view{"ts_decay_exp(close, 6, 0.5)"},
           std::string_view{"ts_moment(close, 5, 3)"},
           std::string_view{"ts_entropy(close, 8, 4)"},
           std::string_view{"ts_regression(close, volume, 4)"},
           std::string_view{"rank(ts_decay_exp(close, 4, 0.7))"},
       }) {
    expect_vm_matches_oracle(e, panel);
  }
}

// ===========================================================================
//  ts_regression — rolling OLS slope (beta) of y on x.
// ===========================================================================

// close = 2*open exactly -> the rolling regression of close on open has slope 2
// (zero residual) for every full window; warm-up cells (t < d-1) are NaN.
TEST(AlphaTsBrain_Regression, ExactLinear_SlopeIsTwo) {
  const atx::usize dates = 6;
  const atx::usize instruments = 2;
  auto cols = random_cols(dates * instruments, 0x1001ULL);
  for (atx::usize i = 0; i < dates * instruments; ++i) {
    cols[0][i] = 2.0 * cols[1][i]; // close = 2*open (exact linear relation)
  }
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> v = vm_values("ts_regression(close, open, 3)", panel);
  ASSERT_EQ(v.size(), dates * instruments);
  // Window d=3 -> first full window at t=2; t in {0,1} are warm-up NaN.
  for (atx::usize j = 0; j < instruments; ++j) {
    EXPECT_TRUE(std::isnan(v[0 * instruments + j]));
    EXPECT_TRUE(std::isnan(v[1 * instruments + j]));
    for (atx::usize t = 2; t < dates; ++t) {
      EXPECT_NEAR(v[t * instruments + j], 2.0, 1e-9) << "t=" << t << " j=" << j;
    }
  }
}

// ===========================================================================
//  ts_decay_exp — exponential decay weights f^k, newest heaviest.
// ===========================================================================

TEST(AlphaTsBrain_DecayExp, HandComputedWeights) {
  const atx::usize dates = 3;
  const atx::usize instruments = 1;
  auto cols = random_cols(dates, 0x2002ULL);
  cols[0] = {4.0, 2.0, 8.0}; // chronological close
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> v = vm_values("ts_decay_exp(close, 3, 0.5)", panel);
  ASSERT_EQ(v.size(), dates);
  EXPECT_TRUE(std::isnan(v[0])); // warm-up
  EXPECT_TRUE(std::isnan(v[1])); // warm-up
  // t=2 window {4,2,8}: newest=8 (f^0=1), 2 (f^1=0.5), 4 (f^2=0.25). sum=1.75.
  const atx::f64 want = (1.0 * 8.0 + 0.5 * 2.0 + 0.25 * 4.0) / (1.0 + 0.5 + 0.25);
  EXPECT_DOUBLE_EQ(v[2], want);
}

// ===========================================================================
//  ts_moment — k-th central moment. k=2 is the POPULATION variance.
// ===========================================================================

TEST(AlphaTsBrain_Moment, SecondMoment_IsPopulationVariance) {
  const atx::usize dates = 3;
  const atx::usize instruments = 1;
  auto cols = random_cols(dates, 0x3003ULL);
  cols[0] = {4.0, 2.0, 8.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> v = vm_values("ts_moment(close, 3, 2)", panel);
  ASSERT_EQ(v.size(), dates);
  const atx::f64 m = (4.0 + 2.0 + 8.0) / 3.0;
  const atx::f64 popvar =
      ((4.0 - m) * (4.0 - m) + (2.0 - m) * (2.0 - m) + (8.0 - m) * (8.0 - m)) / 3.0;
  EXPECT_DOUBLE_EQ(v[2], popvar);
  // First central moment (k=1) is identically 0 over any full window.
  const std::vector<atx::f64> v1 = vm_values("ts_moment(close, 3, 1)", panel);
  EXPECT_NEAR(v1[2], 0.0, 1e-12);
}

// ===========================================================================
//  ts_entropy — Shannon entropy over equal-width buckets.
// ===========================================================================

// A flat window has zero dispersion -> all mass in one bucket -> entropy 0.
TEST(AlphaTsBrain_Entropy, FlatWindow_ZeroEntropy) {
  const atx::usize dates = 4;
  const atx::usize instruments = 1;
  auto cols = random_cols(dates, 0x4004ULL);
  cols[0] = {5.0, 5.0, 5.0, 5.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> v = vm_values("ts_entropy(close, 4, 4)", panel);
  ASSERT_EQ(v.size(), dates);
  EXPECT_DOUBLE_EQ(v[3], 0.0);
}

// d distinct, evenly-spread values with b==d buckets -> one count per bucket ->
// uniform distribution -> entropy ln(d).
TEST(AlphaTsBrain_Entropy, UniformDistinct_LogD) {
  const atx::usize dates = 4;
  const atx::usize instruments = 1;
  auto cols = random_cols(dates, 0x4005ULL);
  cols[0] = {0.0, 1.0, 2.0, 3.0}; // evenly spaced over [0,3]
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> v = vm_values("ts_entropy(close, 4, 4)", panel);
  ASSERT_EQ(v.size(), dates);
  EXPECT_NEAR(v[3], std::log(4.0), 1e-12);
}

// ===========================================================================
//  Lookahead rail / NaN policy — a NaN inside the window blanks the cell.
// ===========================================================================

TEST(AlphaTsBrain_Rail, NaNInWindow_AndWarmup_AreNaN) {
  const atx::usize dates = 5;
  const atx::usize instruments = 1;
  auto cols = random_cols(dates, 0x5005ULL);
  std::vector<std::uint8_t> universe(dates, std::uint8_t{1});
  universe[1] = 0; // date-1 out of universe -> NaN in any window covering it
  const Panel panel = make_panel(dates, instruments, std::move(cols), std::move(universe));
  for (const std::string_view e :
       {std::string_view{"ts_decay_exp(close, 3, 0.5)"}, std::string_view{"ts_moment(close, 3, 2)"},
        std::string_view{"ts_entropy(close, 3, 3)"}}) {
    const std::vector<atx::f64> v = vm_values(e, panel);
    EXPECT_TRUE(std::isnan(v[0])) << e; // warm-up
    EXPECT_TRUE(std::isnan(v[1])) << e; // out-of-universe
    EXPECT_TRUE(std::isnan(v[2])) << e; // window [0,1,2] contains the NaN at 1
    EXPECT_TRUE(std::isnan(v[3])) << e; // window [1,2,3] contains the NaN at 1
    expect_vm_matches_oracle(e, panel);
  }
}

// ===========================================================================
//  Hparam validation — out-of-range immediates are rejected by analyze.
// ===========================================================================

TEST(AlphaTsBrain_Hparam, OutOfRange_Rejected) {
  EXPECT_TRUE(compiles("ts_decay_exp(close, 4, 0.5)"));
  EXPECT_TRUE(compiles("ts_moment(close, 4, 2)"));
  EXPECT_TRUE(compiles("ts_entropy(close, 4, 3)"));
  EXPECT_FALSE(compiles("ts_decay_exp(close, 4, -0.5)")); // f must be > 0
  EXPECT_FALSE(compiles("ts_moment(close, 4, 0)"));       // k must be >= 1
  EXPECT_FALSE(compiles("ts_entropy(close, 4, 0)"));      // buckets must be >= 1
}

} // namespace

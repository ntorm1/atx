// atx::engine::alpha — cross-sectional gap-fill ops (S3.3): quantile / reverse /
// vec_sum / vec_avg. Each is proved (a) VM == oracle bit-for-bit on random
// panels, and (b) against a hand-computed reference or an algebraic identity
// with an already-shipped op (reverse vs Neg, vec_avg vs normalize).
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

namespace atxtest_alpha_cs_gapfill_test {

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
//  reverse(x) — routes to the Neg opcode: -x, the rank-reversal idiom.
// ===========================================================================

TEST(Reverse, Field_EqualsNegationBitForBit) {
  const atx::usize dates = 7;
  const atx::usize instruments = 8;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0x5E1AULL));
  const std::vector<atx::f64> rev = vm_values("reverse(close)", panel);
  const std::vector<atx::f64> neg = vm_values("-close", panel);
  ASSERT_EQ(rev.size(), neg.size());
  for (atx::usize i = 0; i < rev.size(); ++i) {
    EXPECT_TRUE(same_cell(rev[i], neg[i])) << "cell " << i;
  }
  expect_vm_matches_oracle("reverse(close)", panel);
}

// Rank reversal: for a distinct-valued cross-section, rank(reverse(x)) is the
// mirror of rank(x), so the two ordinal percentiles sum to 1 per valid cell.
TEST(Reverse, RankReversal_PercentilesSumToOne) {
  const atx::usize dates = 1;
  const atx::usize instruments = 6;
  auto cols = random_cols(instruments, 0x5E1BULL);
  cols[0] = {10.0, 50.0, 20.0, 90.0, 30.0, 70.0}; // distinct
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> a = vm_values("rank(close)", panel);
  const std::vector<atx::f64> b = vm_values("rank(reverse(close))", panel);
  ASSERT_EQ(a.size(), b.size());
  for (atx::usize i = 0; i < a.size(); ++i) {
    EXPECT_NEAR(a[i] + b[i], 1.0, 1e-12) << "cell " << i;
  }
}

// ===========================================================================
//  quantile(x[, n]) — bucket the valid set into n discrete quantile levels.
// ===========================================================================

// 5 distinct sorted values, n=5: percentiles {0,.25,.5,.75,1} -> buckets
// {0,1,2,3,4} -> values {0,.25,.5,.75,1}.
TEST(Quantile, FiveDistinctFiveBuckets_HandComputed) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(instruments, 0x9001ULL);
  cols[0] = {10.0, 20.0, 30.0, 40.0, 50.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> q = vm_values("quantile(close, 5)", panel);
  ASSERT_EQ(q.size(), instruments);
  EXPECT_DOUBLE_EQ(q[0], 0.0);
  EXPECT_DOUBLE_EQ(q[1], 0.25);
  EXPECT_DOUBLE_EQ(q[2], 0.5);
  EXPECT_DOUBLE_EQ(q[3], 0.75);
  EXPECT_DOUBLE_EQ(q[4], 1.0);
  expect_vm_matches_oracle("quantile(close, 5)", panel);
}

// n=2 (median split): percentiles {0,.25,.5,.75,1} -> buckets {0,0,1,1,1}.
TEST(Quantile, TwoBuckets_MedianSplit) {
  const atx::usize dates = 1;
  const atx::usize instruments = 5;
  auto cols = random_cols(instruments, 0x9002ULL);
  cols[0] = {10.0, 20.0, 30.0, 40.0, 50.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> q = vm_values("quantile(close, 2)", panel);
  ASSERT_EQ(q.size(), instruments);
  EXPECT_DOUBLE_EQ(q[0], 0.0);
  EXPECT_DOUBLE_EQ(q[1], 0.0);
  EXPECT_DOUBLE_EQ(q[2], 1.0);
  EXPECT_DOUBLE_EQ(q[3], 1.0);
  EXPECT_DOUBLE_EQ(q[4], 1.0);
}

// The default window is 5 (registry default), so quantile(x) == quantile(x, 5).
TEST(Quantile, DefaultWindow_EqualsFive) {
  const atx::usize dates = 4;
  const atx::usize instruments = 9;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0x9003ULL));
  const std::vector<atx::f64> a = vm_values("quantile(close)", panel);
  const std::vector<atx::f64> b = vm_values("quantile(close, 5)", panel);
  ASSERT_EQ(a.size(), b.size());
  for (atx::usize i = 0; i < a.size(); ++i) {
    EXPECT_TRUE(same_cell(a[i], b[i])) << "cell " << i;
  }
}

// A singleton valid set ranks to p == 0.5 -> bucket floor(0.5*5)=2 -> 0.5.
TEST(Quantile, SingletonValidSet_CentreBucket) {
  const atx::usize dates = 1;
  const atx::usize instruments = 1;
  auto cols = random_cols(instruments, 0x9004ULL);
  cols[0] = {42.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> q = vm_values("quantile(close, 5)", panel);
  ASSERT_EQ(q.size(), 1U);
  EXPECT_DOUBLE_EQ(q[0], 0.5);
}

// A degenerate window n < 2 has no defined spacing -> NaN for every valid cell.
TEST(Quantile, DegenerateWindow_AllNaN) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  const Panel panel = make_panel(dates, instruments, random_cols(instruments, 0x9005ULL));
  const std::vector<atx::f64> q = vm_values("quantile(close, 1)", panel);
  ASSERT_EQ(q.size(), instruments);
  for (atx::usize i = 0; i < q.size(); ++i) {
    EXPECT_TRUE(std::isnan(q[i])) << "cell " << i;
  }
  expect_vm_matches_oracle("quantile(close, 1)", panel);
}

TEST(Quantile, RandomAndComposed_MatchesOracle) {
  const atx::usize dates = 13;
  const atx::usize instruments = 17;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0x9006ULL));
  expect_vm_matches_oracle("quantile(close, 10)", panel);
  expect_vm_matches_oracle("rank(quantile(close, 4))", panel);
  expect_vm_matches_oracle("quantile(close, 7) - quantile(open, 7)", panel);
}

// ===========================================================================
//  vec_sum(x) / vec_avg(x) — reduce over the valid set, broadcast the scalar.
// ===========================================================================

TEST(VecReduce, AllValid_SumAndAvgBroadcast) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  auto cols = random_cols(instruments, 0xA001ULL);
  cols[0] = {1.0, 2.0, 3.0, 4.0};
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> s = vm_values("vec_sum(close)", panel);
  const std::vector<atx::f64> a = vm_values("vec_avg(close)", panel);
  ASSERT_EQ(s.size(), instruments);
  for (atx::usize i = 0; i < instruments; ++i) {
    EXPECT_DOUBLE_EQ(s[i], 10.0) << "cell " << i;
    EXPECT_DOUBLE_EQ(a[i], 2.5) << "cell " << i;
  }
}

// vec_avg is the cross-sectional mean, so x - vec_avg(x) is the demean — which
// `normalize` already computes. The identity holds bit-for-bit.
TEST(VecReduce, AvgMinusFromField_EqualsNormalize) {
  const atx::usize dates = 6;
  const atx::usize instruments = 11;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0xA002ULL));
  const std::vector<atx::f64> lhs = vm_values("close - vec_avg(close)", panel);
  const std::vector<atx::f64> rhs = vm_values("normalize(close)", panel);
  ASSERT_EQ(lhs.size(), rhs.size());
  for (atx::usize i = 0; i < lhs.size(); ++i) {
    EXPECT_TRUE(same_cell(lhs[i], rhs[i])) << "cell " << i;
  }
}

// Out-of-universe cells are excluded from the reduction AND stay NaN.
TEST(VecReduce, OutOfUniverse_ExcludedAndNaN) {
  const atx::usize dates = 1;
  const atx::usize instruments = 4;
  auto cols = random_cols(instruments, 0xA003ULL);
  cols[0] = {1.0, 2.0, 3.0, 4.0};
  std::vector<std::uint8_t> universe(instruments, std::uint8_t{1});
  universe[3] = 0; // drop the 4.0 -> sum over {1,2,3}
  const Panel panel = make_panel(dates, instruments, std::move(cols), std::move(universe));
  const std::vector<atx::f64> s = vm_values("vec_sum(close)", panel);
  ASSERT_EQ(s.size(), instruments);
  EXPECT_DOUBLE_EQ(s[0], 6.0);
  EXPECT_DOUBLE_EQ(s[1], 6.0);
  EXPECT_DOUBLE_EQ(s[2], 6.0);
  EXPECT_TRUE(std::isnan(s[3]));
  expect_vm_matches_oracle("vec_sum(close)", panel);
  expect_vm_matches_oracle("vec_avg(close)", panel);
}

TEST(VecReduce, RandomAndComposed_MatchesOracle) {
  const atx::usize dates = 9;
  const atx::usize instruments = 13;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0xA004ULL));
  expect_vm_matches_oracle("vec_sum(close)", panel);
  expect_vm_matches_oracle("vec_avg(close)", panel);
  expect_vm_matches_oracle("rank(close / vec_avg(volume))", panel);
}


}  // namespace atxtest_alpha_cs_gapfill_test

// Golden digest over the EXISTING DSL op set (platform-v7 W2 / A7 guard).
//
// W2 adds new opcodes (appended after OpCode::Free) and new kernels. This suite
// proves every pre-W2 opcode still produces the same bits: one expression per
// registered named op plus every infix / prefix / ternary opcode, evaluated on a
// fixed-seed panel with NaN holes, a forward-filled field and universe gaps,
// folded into FNV-1a digests (NaN canonicalized) for three paths:
//   * the production VM under AuditExact (the default, publication path),
//   * the production VM under ResearchFast (online / sliding lanes),
//   * the reference oracle.
//
// CAPTURE PROTOCOL (no binary may run in the implementing lane): this file uses
// ONLY pre-W2 operators, so it compiles unchanged on the W2 base commit. Root
// builds atx-engine-alpha-tests at the commit that introduced this file (base +
// this test only), runs --gtest_filter=AlphaLitGolden_*, reads the printed
// `[lit-golden]` digests and pins them below; the W2 head must print the SAME
// three values. Until pinned (all zero) the test fails and prints the digests.
//
// Naming: Subject_Condition_ExpectedResult.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
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

namespace atx_test_alpha_lit_golden {

using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::Program;

// PIN: captured at the W2 base + this test (see CAPTURE PROTOCOL). 0 = unpinned.
constexpr std::uint64_t kGoldenVmAuditExact = 0x0ULL;
constexpr std::uint64_t kGoldenVmResearchFast = 0x0ULL;
constexpr std::uint64_t kGoldenOracle = 0x0ULL;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

const Library &lib() {
  static const Library l;
  return l;
}

Program compile_ok(std::string_view src) {
  auto ast = atx::engine::alpha::parse_expr(src, lib());
  EXPECT_TRUE(ast.has_value()) << src << ": " << (ast ? "" : ast.error().message());
  if (!ast) {
    return Program{};
  }
  auto ana = atx::engine::alpha::analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << src << ": " << (ana ? "" : ana.error().message());
  if (!ana) {
    return Program{};
  }
  auto prog = atx::engine::alpha::compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << src;
  return prog.value_or(Program{});
}

// 64 dates x 12 instruments, seed-fixed. THIS FIXTURE IS FROZEN once pinned.
Panel golden_panel() {
  constexpr atx::usize kD = 64;
  constexpr atx::usize kI = 12;
  const std::vector<std::string> names = {"open",    "high",    "low",
                                          "close",   "volume",  "vwap",
                                          "returns", "IndClass.sector", "fund"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(kD * kI));
  std::vector<std::uint8_t> uni(kD * kI, 1);
  std::mt19937_64 rng{0x6011D3A7ULL};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::uniform_real_distribution<atx::f64> u{0.0, 1.0};
  std::vector<atx::f64> px(kI);
  for (atx::usize j = 0; j < kI; ++j) {
    px[j] = 20.0 + 3.0 * static_cast<atx::f64>(j);
  }
  for (atx::usize t = 0; t < kD; ++t) {
    for (atx::usize j = 0; j < kI; ++j) {
      const atx::usize i = t * kI + j;
      const atx::f64 prev = px[j];
      px[j] = std::max(1.0, px[j] * (1.0 + 0.02 * nd(rng)));
      const atx::f64 o = prev * (1.0 + 0.005 * nd(rng));
      const atx::f64 c = px[j];
      const atx::f64 h = std::max(o, c) * (1.0 + 0.01 * u(rng));
      const atx::f64 l = std::min(o, c) * (1.0 - 0.01 * u(rng));
      cols[0][i] = o;
      cols[1][i] = h;
      cols[2][i] = l;
      cols[3][i] = c;
      cols[4][i] = 1.0e5 * (0.5 + u(rng));
      cols[5][i] = (h + l + c) / 3.0;
      cols[6][i] = c / prev - 1.0;
      cols[7][i] = static_cast<atx::f64>(j % 3);
      cols[8][i] = 0.1 * static_cast<atx::f64>(1 + (j % 4)) + 0.05 * static_cast<atx::f64>(t / 8);
      if (u(rng) < 0.02) {
        cols[6][i] = kNaN; // returns holes
      }
      if (u(rng) < 0.01) {
        cols[3][i] = kNaN; // close holes
      }
    }
  }
  for (atx::usize t = 20; t < 30; ++t) {
    uni[t * kI + 2] = 0; // instrument 2 leaves for 10 dates
  }
  for (atx::usize t = 0; t < 9; ++t) {
    uni[t * kI + 5] = 0; // instrument 5 enters late
  }
  auto p = Panel::create(kD, kI, names, std::move(cols), std::move(uni));
  EXPECT_TRUE(p.has_value());
  return p.value();
}

std::uint64_t fnv_cells(std::uint64_t h, const std::vector<atx::f64> &v) {
  for (const atx::f64 x : v) {
    std::uint64_t bits = 0;
    if (std::isnan(x)) {
      bits = 0x7ff8000000000000ULL;
    } else {
      std::memcpy(&bits, &x, sizeof(bits));
    }
    for (int k = 0; k < 8; ++k) {
      h ^= (bits >> (8 * k)) & 0xFFULL;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

// One expression per pre-W2 named operator (registry order) + every infix /
// prefix / ternary opcode. Every one compiles on the W2 base.
const std::vector<std::string_view> &existing_ops() {
  static const std::vector<std::string_view> k = {
      "abs(close - open)", "sign(close - open)", "log(volume)", "sigmoid(returns * 10)",
      "tanh(returns * 10)", "power(close, 0.5)", "signedpower(returns, 2)", "min(open, close)",
      "max(open, close)", "rank(close)", "zscore(volume)", "scale(returns, 2)",
      "normalize(close)", "winsorize(returns, 2)", "indneutralize(close, IndClass.sector)",
      "group_neutralize(returns, IndClass.sector)", "group_rank(volume, IndClass.sector)",
      "group_zscore(close, IndClass.sector)", "group_count(close, IndClass.sector)",
      "group_mean(close, IndClass.sector)", "group_scale(returns, IndClass.sector)",
      "cs_residualize(close, IndClass.sector)", "cs_residualize(close, IndClass.sector, volume)",
      "quantile(close, 4)", "reverse(close)", "vec_sum(returns)", "vec_avg(returns)",
      "delay(close, 3)", "delta(close, 3)", "ts_sum(returns, 5)", "ts_mean(close, 5)",
      "stddev(returns, 6)", "ts_std(close, 6)", "ts_var(returns, 6)", "ts_min(low, 4)",
      "ts_max(high, 4)", "ts_argmin(close, 5)", "ts_argmax(close, 5)", "ts_rank(close, 7)",
      "correlation(close, volume, 8)", "covariance(close, volume, 8)", "product(1 + returns, 4)",
      "decay_linear(close, 5)", "ema(close, 6)", "wma(close, 6)", "skew(returns, 8)",
      "kurt(returns, 9)", "med(close, 5)", "mad(close, 5)", "slope(close, 6)",
      "rsquare(close, 6)", "resid(close, 6)", "ts_zscore(fund, 5)", "ts_backfill(returns, 5)",
      "ts_av_diff(close, 5)", "ts_quantile(close, 7)", "ts_scale(close, 7)",
      "ts_count_nans(returns, 6)", "ts_skew(close, 8)", "ts_kurt(close, 9)",
      "ts_corr(close, open, 6)", "ts_regression(close, vwap, 7)", "ts_decay_exp(close, 6, 0.7)",
      "ts_moment(returns, 6, 3)", "ts_entropy(returns, 10, 4)",
      "trade_when(volume > ts_mean(volume, 5), rank(returns), returns < -0.02)",
      "hump(rank(close), 0.05)", "kalman_level(close, 0.01, 1.0)", "ou_filter(close, 0.2, 50)",
      "split2(close).lo", "kalman(close, vwap, 0.5, 1.0).beta",
      "kalman(close, vwap, 0.5, 1.0).resid", "ou_theta(close, 10)", "ou_halflife(close, 10)",
      "ou_mean(close, 10)", "ou_zscore(close, 10)",
      // infix / prefix / ternary opcodes (Add Sub Mul Div Pow Neg Cmp* And Or Not Select)
      "(close > open) ? close : open", "((close < open) && (volume >= 100000)) ? 1 : 0",
      "((close <= open) || !(volume != 100000)) ? high : low", "(close == open) ? 1 : 0",
      "close / open - 1", "-close + high * low", "close ^ 2", "close ^ 3"};
  return k;
}

struct Digests {
  std::uint64_t vm_ae{1469598103934665603ULL};
  std::uint64_t vm_rf{1469598103934665603ULL};
  std::uint64_t oracle{1469598103934665603ULL};
};

Digests run_battery() {
  const Panel panel = golden_panel();
  Digests d;
  for (const std::string_view e : existing_ops()) {
    const Program prog = compile_ok(e);
    Engine ae{panel};
    auto a = ae.evaluate(prog);
    EXPECT_TRUE(a.has_value()) << e;
    Engine rf{panel};
    rf.set_eval_mode(EvalMode::ResearchFast);
    auto r = rf.evaluate(prog);
    EXPECT_TRUE(r.has_value()) << e;
    auto o = atx::engine::alpha::evaluate_reference(prog, panel);
    EXPECT_TRUE(o.has_value()) << e;
    if (!a || !r || !o) {
      continue;
    }
    d.vm_ae = fnv_cells(d.vm_ae, a.value().alphas.front().values);
    d.vm_rf = fnv_cells(d.vm_rf, r.value().alphas.front().values);
    d.oracle = fnv_cells(d.oracle, o.value().alphas.front().values);
  }
  return d;
}

TEST(AlphaLitGolden_ExistingOps, EveryPreW2OpcodeDigestIsPinned) {
  const Digests d = run_battery();
  std::printf("[lit-golden] vm_audit_exact=0x%016llx vm_research_fast=0x%016llx "
              "oracle=0x%016llx\n",
              static_cast<unsigned long long>(d.vm_ae), static_cast<unsigned long long>(d.vm_rf),
              static_cast<unsigned long long>(d.oracle));
  if (kGoldenVmAuditExact == 0 && kGoldenVmResearchFast == 0 && kGoldenOracle == 0) {
    ADD_FAILURE() << "golden digests unpinned: capture on the W2 base + this test, then pin";
    return;
  }
  EXPECT_EQ(d.vm_ae, kGoldenVmAuditExact);
  EXPECT_EQ(d.vm_rf, kGoldenVmResearchFast);
  EXPECT_EQ(d.oracle, kGoldenOracle);
}

// Each battery row is a distinct expression and exercises the op it names.
TEST(AlphaLitGolden_ExistingOps, BatteryCoversEveryPreW2NamedOp) {
  const std::vector<std::string_view> names = {
      "abs", "sign", "log", "sigmoid", "tanh", "power", "signedpower", "min", "max", "rank",
      "zscore", "scale", "normalize", "winsorize", "indneutralize", "group_neutralize",
      "group_rank", "group_zscore", "group_count", "group_mean", "group_scale", "cs_residualize",
      "quantile", "reverse", "vec_sum", "vec_avg", "delay", "delta", "ts_sum", "ts_mean",
      "stddev", "ts_std", "ts_var", "ts_min", "ts_max", "ts_argmin", "ts_argmax", "ts_rank",
      "correlation", "covariance", "product", "decay_linear", "ema", "wma", "skew", "kurt",
      "med", "mad", "slope", "rsquare", "resid", "ts_zscore", "ts_backfill", "ts_av_diff",
      "ts_quantile", "ts_scale", "ts_count_nans", "ts_skew", "ts_kurt", "ts_corr",
      "ts_regression", "ts_decay_exp", "ts_moment", "ts_entropy", "trade_when", "hump",
      "kalman_level", "ou_filter", "split2", "kalman", "ou_theta", "ou_halflife", "ou_mean",
      "ou_zscore"};
  for (const std::string_view n : names) {
    ASSERT_NE(lib().find(n), nullptr) << n;
    const std::string call = std::string{n} + "(";
    const bool used = std::any_of(existing_ops().begin(), existing_ops().end(),
                                  [&call](std::string_view e) {
                                    return e.find(call) != std::string_view::npos;
                                  });
    EXPECT_TRUE(used) << "no golden row exercises " << n;
  }
}

} // namespace atx_test_alpha_lit_golden

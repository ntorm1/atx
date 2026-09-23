// atx::engine::alpha — sliding-window order statistics (Lane 1 / ts_order_stat.hpp).
//
// ts_rank / med / ts_quantile are routed through the O(log d) order-statistic
// sweep on the DEFAULT AuditExact path, so the contract is BIT-EXACT agreement
// with the batch per-cell kernel (ts_value_at) and with the oracle — in both
// eval modes — including NaN runs, warm-up, ties, +/-0.0 and +/-inf, and on both
// window structures (sorted array for small d, Fenwick for large d).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
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
#include "atx/engine/alpha/ts_ops.hpp"
#include "atx/engine/alpha/ts_order_stat.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_test_l1_kernels_ts_order_stat {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
namespace ordstat = atx::engine::alpha::ordstat;
namespace det = atx::engine::alpha::detail;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();

[[nodiscard]] bool same_bits(atx::f64 a, atx::f64 b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  atx::u64 ua = 0;
  atx::u64 ub = 0;
  std::memcpy(&ua, &a, sizeof(a));
  std::memcpy(&ub, &b, sizeof(b));
  return ua == ub;
}

// A column with NaN runs, heavy ties (small integer grid), both signed zeros and
// both infinities.
[[nodiscard]] std::vector<atx::f64> nasty_col(atx::usize n, atx::u32 seed) {
  std::mt19937_64 rng{seed};
  std::uniform_int_distribution<int> pick{0, 29};
  std::uniform_int_distribution<int> grid{-4, 4};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::vector<atx::f64> x(n);
  atx::usize nan_run = 0;
  for (atx::usize i = 0; i < n; ++i) {
    if (nan_run > 0) {
      x[i] = kNaN;
      --nan_run;
      continue;
    }
    const int p = pick(rng);
    if (p == 0) {
      nan_run = static_cast<atx::usize>(grid(rng) + 5); // a NaN run of 1..9
      x[i] = kNaN;
    } else if (p == 1) {
      x[i] = -0.0;
    } else if (p == 2) {
      x[i] = 0.0;
    } else if (p == 3) {
      x[i] = (grid(rng) > 0) ? kInf : -kInf;
    } else if (p < 16) {
      x[i] = static_cast<atx::f64>(grid(rng)); // ties
    } else {
      x[i] = nd(rng);
    }
  }
  return x;
}

[[nodiscard]] std::vector<atx::f64> batch_col(OpCode op, const std::vector<atx::f64> &x,
                                              atx::usize d) {
  std::vector<atx::f64> out(x.size());
  std::vector<atx::f64> buf(d + 1);
  for (atx::usize t = 0; t < x.size(); ++t) {
    out[t] = det::ts_value_at(op, x, t, 0, d, 1, buf, 0.0);
  }
  return out;
}

class TsOrderStat_Kernel : public ::testing::TestWithParam<atx::usize> {};

TEST_P(TsOrderStat_Kernel, SweepBitExactVsBatchOnNastyColumn) {
  const atx::usize d = GetParam();
  for (const OpCode op : {OpCode::TsRank, OpCode::TsMed, OpCode::TsQuantile}) {
    for (atx::u32 seed = 1; seed <= 4; ++seed) {
      const std::vector<atx::f64> x = nasty_col(900, seed * 31U + static_cast<atx::u32>(d));
      const std::vector<atx::f64> want = batch_col(op, x, d);
      std::vector<atx::f64> got(x.size(), 123.0);
      ordstat::sweep_strided(op, x, got, x.size(), 0, d, 1);
      for (atx::usize t = 0; t < x.size(); ++t) {
        ASSERT_TRUE(same_bits(got[t], want[t]))
            << "op=" << static_cast<int>(op) << " d=" << d << " seed=" << seed << " t=" << t
            << " got=" << got[t] << " want=" << want[t];
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Windows, TsOrderStat_Kernel,
                         ::testing::Values(0U, 1U, 2U, 3U, 5U, 20U, 60U, 160U, 161U, 250U));

TEST(TsOrderStat_Kernel2, SignedZeroMedianFallsBackToBatchBits) {
  // Window of 3 holding {-0, +0, +0}: the median's sign is whatever the batch
  // sort leaves in the middle slot — the sweep must reproduce it exactly.
  const std::vector<atx::f64> x{-0.0, 0.0, 0.0, -0.0, -0.0, 0.0, 1.0, -0.0, 0.0};
  for (const atx::usize d : {2U, 3U, 4U}) {
    const std::vector<atx::f64> want = batch_col(OpCode::TsMed, x, d);
    std::vector<atx::f64> got(x.size());
    ordstat::sweep_strided(OpCode::TsMed, x, got, x.size(), 0, d, 1);
    for (atx::usize t = 0; t < x.size(); ++t) {
      ASSERT_TRUE(same_bits(got[t], want[t])) << "d=" << d << " t=" << t;
    }
  }
}

TEST(TsOrderStat_Kernel2, StridedColumnsMatchPerColumnBatch) {
  constexpr atx::usize kDates = 300;
  constexpr atx::usize kInst = 7;
  std::vector<atx::f64> panel(kDates * kInst);
  for (atx::usize j = 0; j < kInst; ++j) {
    const std::vector<atx::f64> c = nasty_col(kDates, 900U + static_cast<atx::u32>(j));
    for (atx::usize t = 0; t < kDates; ++t) {
      panel[t * kInst + j] = c[t];
    }
  }
  for (const atx::usize d : {10U, 200U}) {
    std::vector<atx::f64> got(panel.size(), 0.0);
    std::vector<atx::f64> buf(d);
    for (atx::usize j = 0; j < kInst; ++j) {
      ordstat::sweep_strided(OpCode::TsRank, panel, got, kDates, j, d, kInst);
    }
    for (atx::usize j = 0; j < kInst; ++j) {
      for (atx::usize t = 0; t < kDates; ++t) {
        const atx::f64 want = det::ts_value_at(OpCode::TsRank, panel, t, j, d, kInst, buf, 0.0);
        ASSERT_TRUE(same_bits(got[t * kInst + j], want)) << "d=" << d << " j=" << j << " t=" << t;
      }
    }
  }
}

TEST(TsOrderStat_Kernel2, RoutedAlwaysThroughOnlineDispatch) {
  EXPECT_TRUE(det::ts_is_online_op(OpCode::TsRank));
  EXPECT_TRUE(det::ts_is_online_op(OpCode::TsMed));
  EXPECT_TRUE(det::ts_is_online_op(OpCode::TsQuantile));
  EXPECT_FALSE(det::ts_is_online_op(OpCode::TsMad)); // not bit-exactly slidable
}

// ---- Engine-level: VM (both modes) == oracle, bit-for-bit -----------------

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
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

TEST(TsOrderStat_Engine, VmMatchesOracleBitExactInBothModes) {
  constexpr atx::usize kDates = 400;
  constexpr atx::usize kInst = 9;
  std::vector<atx::f64> close(kDates * kInst);
  for (atx::usize j = 0; j < kInst; ++j) {
    const std::vector<atx::f64> c = nasty_col(kDates, 4000U + static_cast<atx::u32>(j));
    for (atx::usize t = 0; t < kDates; ++t) {
      close[t * kInst + j] = c[t];
    }
  }
  auto panel = Panel::create(kDates, kInst, {"close"}, {close}, {});
  ASSERT_TRUE(panel.has_value());
  for (const char *expr : {"ts_rank(close, 7)", "med(close, 8)", "ts_quantile(close, 21)",
                           "ts_rank(close, 200)", "med(close, 181)", "ts_rank(close, 1)"}) {
    const Program prog = compile_ok(expr);
    auto want = evaluate_reference(prog, panel.value());
    ASSERT_TRUE(want.has_value()) << expr;
    for (const EvalMode mode : {EvalMode::AuditExact, EvalMode::ResearchFast}) {
      Engine eng{panel.value()};
      eng.set_eval_mode(mode);
      auto got = eng.evaluate(prog);
      ASSERT_TRUE(got.has_value()) << expr;
      const std::vector<atx::f64> &g = got.value().alphas[0].values;
      const std::vector<atx::f64> &w = want.value().alphas[0].values;
      ASSERT_EQ(g.size(), w.size());
      for (atx::usize i = 0; i < g.size(); ++i) {
        ASSERT_TRUE(same_bits(g[i], w[i])) << expr << " mode=" << static_cast<int>(mode)
                                           << " cell=" << i << " got=" << g[i] << " want=" << w[i];
      }
    }
  }
}

} // namespace atx_test_l1_kernels_ts_order_stat

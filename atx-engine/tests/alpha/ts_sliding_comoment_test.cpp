// atx::engine::alpha — O(1) sliding window kernels (Lane 1 / ts_sliding.hpp).
//
// CoMoment (corr / cov / regression), LinDecay (decay_linear / wma) and TimeReg
// (slope / rsquare / resid) ship on the ResearchFast tier: they must agree with
// an ACCURATE reference (compensated mean + two-pass centred sums) within
// atol = rtol = 1e-9 over NaN runs, warm-up and d in {2,5,20,60,250}, including
// an adversarial large-offset series where naive running sums would cancel. The
// NaN pattern must match the batch kernels exactly, and AuditExact must stay
// byte-identical to the oracle (these ops are only routed under ResearchFast).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstring>
#include <limits>
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
#include "atx/engine/alpha/ts_ops.hpp"
#include "atx/engine/alpha/ts_sliding.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_test_l1_kernels_ts_sliding {

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
namespace sliding = atx::engine::alpha::sliding;
namespace det = atx::engine::alpha::detail;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();
constexpr atx::f64 kTol = 1e-9;

[[nodiscard]] bool close_cell(atx::f64 got, atx::f64 want, atx::f64 atol, atx::f64 rtol) noexcept {
  if (std::isnan(got) || std::isnan(want)) {
    return std::isnan(got) && std::isnan(want);
  }
  if (std::isinf(got) || std::isinf(want)) {
    return got == want;
  }
  return std::fabs(got - want) <= atol + rtol * std::fabs(want);
}

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

// Neumaier-compensated sum: an accurate mean for the reference.
[[nodiscard]] atx::f64 comp_sum(const std::vector<atx::f64> &v) {
  atx::f64 s = 0.0;
  atx::f64 c = 0.0;
  for (const atx::f64 x : v) {
    const atx::f64 t = s + x;
    c += (std::fabs(s) >= std::fabs(x)) ? (s - t) + x : (x - t) + s;
    s = t;
  }
  return s + c;
}

// Accurate pair reference over the trailing window (NaN on warm-up / any
// non-finite pair, matching the batch gate's outcome).
[[nodiscard]] atx::f64 ref_pair(OpCode op, const std::vector<atx::f64> &x,
                                const std::vector<atx::f64> &y, atx::usize t, atx::usize d) {
  if (d < 2 || t + 1 < d) {
    return kNaN;
  }
  std::vector<atx::f64> a;
  std::vector<atx::f64> b;
  for (atx::usize k = t + 1 - d; k <= t; ++k) {
    if (!std::isfinite(x[k]) || !std::isfinite(y[k])) {
      return kNaN;
    }
    a.push_back(x[k]);
    b.push_back(y[k]);
  }
  const atx::f64 nf = static_cast<atx::f64>(d);
  const atx::f64 ma = comp_sum(a) / nf;
  const atx::f64 mb = comp_sum(b) / nf;
  std::vector<atx::f64> pab;
  std::vector<atx::f64> paa;
  std::vector<atx::f64> pbb;
  for (atx::usize i = 0; i < d; ++i) {
    pab.push_back((a[i] - ma) * (b[i] - mb));
    paa.push_back((a[i] - ma) * (a[i] - ma));
    pbb.push_back((b[i] - mb) * (b[i] - mb));
  }
  const atx::f64 sab = comp_sum(pab);
  const atx::f64 saa = comp_sum(paa);
  const atx::f64 sbb = comp_sum(pbb);
  switch (op) {
  case OpCode::TsCov:
    return sab / (nf - 1.0);
  case OpCode::TsCorr:
    return (saa == 0.0 || sbb == 0.0) ? kNaN : sab / std::sqrt(saa * sbb);
  case OpCode::TsRegression:
    return sbb == 0.0 ? kNaN : sab / sbb;
  default:
    return kNaN;
  }
}

// Random walk around `level` with NaN runs and occasional +/-inf.
[[nodiscard]] std::vector<atx::f64> walk(atx::usize n, atx::u32 seed, atx::f64 level,
                                         atx::f64 step, bool holes) {
  std::mt19937_64 rng{seed};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::uniform_int_distribution<int> pick{0, 199};
  std::vector<atx::f64> v(n);
  atx::f64 cur = level;
  atx::usize run = 0;
  for (atx::usize i = 0; i < n; ++i) {
    cur += step * nd(rng);
    v[i] = cur;
    if (!holes) {
      continue;
    }
    if (run > 0) {
      v[i] = kNaN;
      --run;
      continue;
    }
    const int p = pick(rng);
    if (p == 0) {
      run = 6;
      v[i] = kNaN;
    } else if (p == 1) {
      v[i] = kInf;
    }
  }
  return v;
}

class TsSlidingCoMoment_Kernel : public ::testing::TestWithParam<atx::usize> {};

TEST_P(TsSlidingCoMoment_Kernel, MatchesAccurateReferenceWithHoles) {
  const atx::usize d = GetParam();
  constexpr atx::usize kDates = 1500;
  const std::vector<atx::f64> x = walk(kDates, 11U + static_cast<atx::u32>(d), 50.0, 1.0, true);
  const std::vector<atx::f64> y = walk(kDates, 77U + static_cast<atx::u32>(d), 20.0, 0.5, true);
  for (const OpCode op : {OpCode::TsCorr, OpCode::TsCov, OpCode::TsRegression}) {
    std::vector<atx::f64> got(kDates, 0.0);
    sliding::sweep_comoment(op, x, y, got, kDates, 1, d, 0, 1);
    for (atx::usize t = 0; t < kDates; ++t) {
      const atx::f64 want = ref_pair(op, x, y, t, d);
      ASSERT_TRUE(close_cell(got[t], want, kTol, kTol))
          << "op=" << static_cast<int>(op) << " d=" << d << " t=" << t << " got=" << got[t]
          << " want=" << want;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Windows, TsSlidingCoMoment_Kernel,
                         ::testing::Values(2U, 5U, 20U, 60U, 250U));

TEST(TsSlidingCoMoment_Adversarial, LargeOffsetSeriesDoesNotCancel) {
  // Volume-like level 1e8 with unit noise, price-like 1e4: raw Σx² would lose
  // ~16 digits; the shifted sums must stay within 1e-9 of the accurate value.
  constexpr atx::usize kDates = 3000;
  constexpr atx::usize kD = 20;
  std::mt19937_64 rng{2024};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::vector<atx::f64> x(kDates);
  std::vector<atx::f64> y(kDates);
  for (atx::usize t = 0; t < kDates; ++t) {
    const atx::f64 z = nd(rng);
    x[t] = 1.0e8 + z;
    y[t] = 1.0e4 + 0.3 * z + nd(rng) * 0.01;
  }
  for (const OpCode op : {OpCode::TsCorr, OpCode::TsCov, OpCode::TsRegression}) {
    std::vector<atx::f64> got(kDates);
    sliding::sweep_comoment(op, x, y, got, kDates, 1, kD, 0, 1);
    for (atx::usize t = kD - 1; t < kDates; ++t) {
      const atx::f64 want = ref_pair(op, x, y, t, kD);
      ASSERT_TRUE(close_cell(got[t], want, kTol, kTol))
          << "op=" << static_cast<int>(op) << " t=" << t << " got=" << got[t] << " want=" << want;
    }
  }
}

TEST(TsSlidingCoMoment_Adversarial, ExactConstantWindowIsNaNLikeBatch) {
  const std::vector<atx::f64> x{3.0, 3.0, 3.0, 3.0, 4.0, 5.0, 3.0};
  const std::vector<atx::f64> y{1.0, 2.0, 5.0, 7.0, 7.0, 7.0, 7.0};
  std::vector<atx::f64> got(x.size());
  std::vector<atx::f64> bx(4);
  std::vector<atx::f64> by(4);
  sliding::sweep_comoment(OpCode::TsCorr, x, y, got, x.size(), 1, 4, 0, 1);
  for (atx::usize t = 0; t < x.size(); ++t) {
    const atx::f64 want = det::ts_pair_at(OpCode::TsCorr, x, y, t, 0, 4, 1, bx, by);
    ASSERT_TRUE(close_cell(got[t], want, kTol, kTol)) << "t=" << t;
  }
  EXPECT_TRUE(std::isnan(got[3])); // x constant in [0,3]
  EXPECT_TRUE(std::isnan(got[6])); // y constant in [3,6]
}

TEST(TsSlidingCoMoment_Panel, MultiColumnSweepEqualsPerColumnSweep) {
  constexpr atx::usize kDates = 400;
  constexpr atx::usize kInst = 13;
  std::vector<atx::f64> x(kDates * kInst);
  std::vector<atx::f64> y(kDates * kInst);
  for (atx::usize j = 0; j < kInst; ++j) {
    const auto cx = walk(kDates, 500U + static_cast<atx::u32>(j), 10.0, 1.0, true);
    const auto cy = walk(kDates, 900U + static_cast<atx::u32>(j), 30.0, 1.0, true);
    for (atx::usize t = 0; t < kDates; ++t) {
      x[t * kInst + j] = cx[t];
      y[t * kInst + j] = cy[t];
    }
  }
  std::vector<atx::f64> whole(x.size());
  sliding::sweep_comoment(OpCode::TsCorr, x, y, whole, kDates, kInst, 20, 0, kInst);
  std::vector<atx::f64> cols(x.size());
  for (atx::usize j = 0; j < kInst; ++j) {
    sliding::sweep_comoment(OpCode::TsCorr, x, y, cols, kDates, kInst, 20, j, j + 1);
  }
  for (atx::usize i = 0; i < x.size(); ++i) {
    ASSERT_TRUE(same_bits(whole[i], cols[i])) << "cell=" << i;
  }
}

// ---- unary sliding ops: decay_linear / wma / slope / rsquare / resid ------

[[nodiscard]] atx::f64 ref_unary(OpCode op, const std::vector<atx::f64> &x, atx::usize t,
                                 atx::usize d) {
  if (d == 0 || t + 1 < d) {
    return kNaN;
  }
  std::vector<atx::f64> w;
  bool pinf = false;
  bool ninf = false;
  for (atx::usize k = t + 1 - d; k <= t; ++k) {
    if (std::isnan(x[k])) {
      return kNaN;
    }
    pinf = pinf || x[k] == kInf;
    ninf = ninf || x[k] == -kInf;
    w.push_back(x[k]);
  }
  const atx::f64 nf = static_cast<atx::f64>(d);
  if (op == OpCode::TsDecayLinear || op == OpCode::TsWma) {
    if (pinf || ninf) {
      return (pinf && ninf) ? kNaN : (pinf ? kInf : -kInf);
    }
    std::vector<atx::f64> terms;
    for (atx::usize i = 0; i < d; ++i) {
      terms.push_back(static_cast<atx::f64>(i + 1) * w[i]);
    }
    return comp_sum(terms) / (nf * (nf + 1.0) / 2.0);
  }
  if (pinf || ninf || d < 2) {
    return kNaN;
  }
  std::vector<atx::f64> t_axis(d);
  for (atx::usize i = 0; i < d; ++i) {
    t_axis[i] = static_cast<atx::f64>(i);
  }
  const atx::f64 my = comp_sum(w) / nf;
  const atx::f64 mt = (nf - 1.0) / 2.0;
  std::vector<atx::f64> pty;
  std::vector<atx::f64> ptt;
  std::vector<atx::f64> pyy;
  for (atx::usize i = 0; i < d; ++i) {
    pty.push_back((t_axis[i] - mt) * (w[i] - my));
    ptt.push_back((t_axis[i] - mt) * (t_axis[i] - mt));
    pyy.push_back((w[i] - my) * (w[i] - my));
  }
  const atx::f64 slope = comp_sum(pty) / comp_sum(ptt);
  if (op == OpCode::TsSlope) {
    return slope;
  }
  if (op == OpCode::TsResid) {
    return w[d - 1] - (my + slope * ((nf - 1.0) - mt));
  }
  const atx::f64 syy = comp_sum(pyy);
  if (syy == 0.0) {
    return kNaN;
  }
  const atx::f64 sty = comp_sum(pty);
  return sty * sty / (comp_sum(ptt) * syy);
}

class TsSlidingUnary_Kernel : public ::testing::TestWithParam<atx::usize> {};

TEST_P(TsSlidingUnary_Kernel, MatchesAccurateReferenceWithHoles) {
  const atx::usize d = GetParam();
  constexpr atx::usize kDates = 1200;
  std::vector<atx::f64> x = walk(kDates, 3U + static_cast<atx::u32>(d), 1.0e4, 3.0, true);
  x[700] = -kInf; // pair with a nearby +inf somewhere -> NaN windows for decay
  for (const OpCode op : {OpCode::TsDecayLinear, OpCode::TsWma, OpCode::TsSlope,
                          OpCode::TsRsquare, OpCode::TsResid}) {
    std::vector<atx::f64> got(kDates, 0.0);
    sliding::sweep_unary(op, x, got, kDates, 1, d, 0, 1);
    for (atx::usize t = 0; t < kDates; ++t) {
      const atx::f64 want = ref_unary(op, x, t, d);
      // r² of a near-flat trend is a ratio of small numbers: absolute tolerance.
      ASSERT_TRUE(close_cell(got[t], want, kTol, kTol))
          << "op=" << static_cast<int>(op) << " d=" << d << " t=" << t << " got=" << got[t]
          << " want=" << want;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Windows, TsSlidingUnary_Kernel,
                         ::testing::Values(1U, 2U, 5U, 20U, 60U, 250U));

// ---- Engine-level: routed under ResearchFast only -------------------------

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

TEST(TsSlidingUnary_Engine, ResearchFastWithinToleranceAuditExactByteIdentical) {
  constexpr atx::usize kDates = 500;
  constexpr atx::usize kInst = 11;
  std::vector<atx::f64> close(kDates * kInst);
  for (atx::usize j = 0; j < kInst; ++j) {
    const auto c = walk(kDates, 70U + static_cast<atx::u32>(j), 100.0, 1.0, true);
    for (atx::usize t = 0; t < kDates; ++t) {
      close[t * kInst + j] = c[t];
    }
  }
  auto panel = Panel::create(kDates, kInst, {"close"}, {close}, {});
  ASSERT_TRUE(panel.has_value());
  for (const char *expr : {"decay_linear(close, 10)", "wma(close, 20)", "slope(close, 15)",
                           "rsquare(close, 30)", "resid(close, 12)"}) {
    const Program prog = compile_ok(expr);
    auto oracle = evaluate_reference(prog, panel.value());
    ASSERT_TRUE(oracle.has_value());
    const std::vector<atx::f64> &w = oracle.value().alphas[0].values;
    Engine audit{panel.value()};
    auto a = audit.evaluate(prog);
    ASSERT_TRUE(a.has_value());
    Engine fast{panel.value()};
    fast.set_eval_mode(EvalMode::ResearchFast);
    auto f = fast.evaluate(prog);
    ASSERT_TRUE(f.has_value());
    for (atx::usize i = 0; i < w.size(); ++i) {
      ASSERT_TRUE(same_bits(a.value().alphas[0].values[i], w[i])) << expr << " cell=" << i;
      ASSERT_TRUE(close_cell(f.value().alphas[0].values[i], w[i], kTol, kTol))
          << expr << " cell=" << i << " fast=" << f.value().alphas[0].values[i]
          << " oracle=" << w[i];
    }
  }
  EXPECT_TRUE(det::ts_is_online_variance_op(OpCode::TsDecayLinear));
  EXPECT_FALSE(det::ts_is_online_variance_op(OpCode::TsCorr)); // pair ops need the VM's y
}

} // namespace atx_test_l1_kernels_ts_sliding

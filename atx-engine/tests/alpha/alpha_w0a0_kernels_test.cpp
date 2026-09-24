// W0-A0 — alpha kernel correctness (A-01, A-02, A-03, A-09, A-13).
//
// Each suite proves one acceptance item of plan §7 W0-A0 with measured values,
// on the production Engine (vm.hpp), the reference oracle (oracle.hpp) and, where
// the op streams, the StreamingEngine:
//   * AlphaCsRankTies_*              — average-rank ties; OrdinalV1 legacy.
//   * AlphaHumpWarmup_*              — hump seeds from NaN, emits NaN on NaN, cap.
//   * AlphaTypecheckScalarLiteral_*  — scalar slots require a finite Literal.
//   * AlphaFlatWindow_*              — relative flat-window guard, VM == oracle.
//   * AlphaAuditExactParity_*        — AuditExact ts_sum/ts_mean oracle-exact;
//                                      ResearchFast Neumaier; full differential.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/cs_ops.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/state_ops.hpp"
#include "atx/engine/alpha/streaming_engine.hpp"
#include "atx/engine/alpha/ts_ops.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_test_w0_a0_kernels {

using atx::f64;
using atx::usize;
using atx::engine::alpha::CrossSection;
using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::FlatGuard;
using atx::engine::alpha::HumpNaN;
using atx::engine::alpha::KernelPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::Program;
using atx::engine::alpha::RankTies;
using atx::engine::alpha::StreamingEngine;
using atx::engine::alpha::TsSumPath;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kInf = std::numeric_limits<f64>::infinity();

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

// Bit-exact cell equality (both NaN counts as equal; -0.0 != +0.0).
bool same_bits(f64 a, f64 b) {
  if (std::isnan(a) || std::isnan(b)) {
    return std::isnan(a) && std::isnan(b);
  }
  std::uint64_t ua = 0;
  std::uint64_t ub = 0;
  std::memcpy(&ua, &a, sizeof(ua));
  std::memcpy(&ub, &b, sizeof(ub));
  return ua == ub;
}

usize count_mismatch(const std::vector<f64> &a, const std::vector<f64> &b) {
  EXPECT_EQ(a.size(), b.size());
  usize n = 0;
  for (usize i = 0; i < std::min(a.size(), b.size()); ++i) {
    n += same_bits(a[i], b[i]) ? 0U : 1U;
  }
  return n;
}

// A raw column-major-by-field fixture that builds both a Panel and per-date
// streaming CrossSections.
struct Fixture {
  usize dates{0};
  usize inst{0};
  std::vector<std::string> names;
  std::vector<std::vector<f64>> cols;
  std::vector<std::uint8_t> uni; // empty == all in-universe

  [[nodiscard]] Panel panel() const {
    auto p = Panel::create(dates, inst, names, cols, uni);
    EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
    return p.value();
  }
  // Panel over dates [from, dates).
  [[nodiscard]] Panel suffix(usize from) const {
    std::vector<std::vector<f64>> c;
    for (const auto &col : cols) {
      c.emplace_back(col.begin() + static_cast<std::ptrdiff_t>(from * inst), col.end());
    }
    std::vector<std::uint8_t> u;
    if (!uni.empty()) {
      u.assign(uni.begin() + static_cast<std::ptrdiff_t>(from * inst), uni.end());
    }
    auto p = Panel::create(dates - from, inst, names, std::move(c), std::move(u));
    EXPECT_TRUE(p.has_value());
    return p.value();
  }
};

std::vector<f64> vm_eval(std::string_view expr, const Panel &panel,
                         EvalMode mode = EvalMode::AuditExact, KernelPolicy policy = {}) {
  const Program prog = compile_ok(expr);
  Engine eng{panel};
  eng.set_eval_mode(mode);
  eng.set_kernel_policy(policy);
  auto out = eng.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << expr << ": " << (out ? "" : out.error().message());
  return out.has_value() ? out.value().alphas.front().values : std::vector<f64>{};
}

std::vector<f64> oracle_eval(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  auto out = atx::engine::alpha::evaluate_reference(prog, panel);
  EXPECT_TRUE(out.has_value()) << expr << ": " << (out ? "" : out.error().message());
  return out.has_value() ? out.value().alphas.front().values : std::vector<f64>{};
}

// Stream every date of `f` through a StreamingEngine; returns the date-major
// output of root 0.
std::vector<f64> stream_eval(std::string_view expr, const Fixture &f, EvalMode mode) {
  const Program prog = compile_ok(expr);
  auto se = StreamingEngine::create(prog, static_cast<atx::u32>(f.inst), mode);
  EXPECT_TRUE(se.has_value()) << expr << ": " << (se ? "" : se.error().message());
  std::vector<f64> out(f.dates * f.inst, kNaN);
  if (!se) {
    return out;
  }
  for (usize t = 0; t < f.dates; ++t) {
    CrossSection cs;
    for (const std::string &name : prog.fields) {
      usize k = 0;
      while (k < f.names.size() && f.names[k] != name) {
        ++k;
      }
      EXPECT_LT(k, f.names.size()) << name;
      cs.fields.emplace_back(f.cols[k].data() + t * f.inst, f.inst);
    }
    if (!f.uni.empty()) {
      cs.universe = std::span<const std::uint8_t>{f.uni.data() + t * f.inst, f.inst};
    }
    auto r = se->step(cs);
    EXPECT_TRUE(r.has_value());
    const std::span<const f64> row = se->output(0);
    std::copy(row.begin(), row.end(), out.begin() + static_cast<std::ptrdiff_t>(t * f.inst));
  }
  return out;
}

// One-field fixture (plus a sector column) from explicit date-major values.
Fixture one_field(usize dates, usize inst, std::vector<f64> x, std::string name = "close") {
  Fixture f;
  f.dates = dates;
  f.inst = inst;
  f.names = {std::move(name), "IndClass.sector"};
  std::vector<f64> sec(dates * inst);
  for (usize i = 0; i < sec.size(); ++i) {
    sec[i] = static_cast<f64>((i % inst) % 2);
  }
  f.cols = {std::move(x), std::move(sec)};
  return f;
}

// Shared mixed fixture: rounded prices (ties), a tied tier column, a
// forward-filled fundamental, universe gaps. Deterministic.
Fixture mixed_fixture(usize dates, usize inst, std::uint64_t seed) {
  Fixture f;
  f.dates = dates;
  f.inst = inst;
  f.names = {"close", "open", "volume", "IndClass.sector", "fund", "tier"};
  f.cols.assign(f.names.size(), std::vector<f64>(dates * inst));
  f.uni.assign(dates * inst, 1);
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<f64> u{0.0, 1.0};
  std::vector<f64> px(inst);
  for (usize j = 0; j < inst; ++j) {
    px[j] = 10.0 + static_cast<f64>(j);
  }
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < inst; ++j) {
      const usize c = t * inst + j;
      px[j] *= 1.0 + 0.03 * (u(rng) - 0.5);
      f.cols[0][c] = std::round(px[j] * 10.0) / 10.0;
      f.cols[1][c] = std::round(px[j] * (1.0 + 0.02 * (u(rng) - 0.5)) * 10.0) / 10.0;
      f.cols[2][c] = 1.0e6 * (1.0 + u(rng));
      f.cols[3][c] = static_cast<f64>(j % 3);
      f.cols[4][c] = 0.1 * static_cast<f64>(1 + (j % 4)) + 0.25 * static_cast<f64>(t / 7);
      f.cols[5][c] = static_cast<f64>(j / 3);
      if (u(rng) < 0.04) {
        f.uni[c] = 0;
      }
    }
  }
  for (usize t = dates / 3; t < dates / 3 + 9 && t < dates; ++t) {
    f.uni[t * inst + 1] = 0; // a 9-date universe exit (beyond the hump cap)
  }
  return f;
}

void expect_vm_oracle_bitexact(std::string_view expr, const Panel &panel) {
  const std::vector<f64> v = vm_eval(expr, panel);
  const std::vector<f64> r = oracle_eval(expr, panel);
  ASSERT_EQ(v.size(), r.size()) << expr;
  EXPECT_EQ(count_mismatch(v, r), 0U) << expr;
}

// ===========================================================================
//  A-01 — AlphaCsRankTies_*
// ===========================================================================

TEST(AlphaCsRankTies_Row, AllTiedRowRanksHalfForEveryName) {
  for (const usize n : {usize{2}, usize{5}, usize{6}, usize{7}}) {
    const Fixture f = one_field(1, n, std::vector<f64>(n, 7.0));
    const Panel p = f.panel();
    const std::vector<f64> v = vm_eval("rank(close)", p);
    const std::vector<f64> r = oracle_eval("rank(close)", p);
    ASSERT_EQ(v.size(), n);
    for (usize j = 0; j < n; ++j) {
      EXPECT_EQ(v[j], 0.5) << "n=" << n << " j=" << j;
      EXPECT_EQ(r[j], 0.5) << "n=" << n << " j=" << j;
    }
    // Legacy OrdinalV1 keeps the index tie-break: 0 .. 1 in instrument order.
    const std::vector<f64> legacy =
        vm_eval("rank(close)", p, EvalMode::AuditExact, KernelPolicy::legacy_v1());
    EXPECT_EQ(legacy.front(), 0.0);
    EXPECT_EQ(legacy.back(), 1.0);
  }
}

TEST(AlphaCsRankTies_Row, PartialTiesTakeTheAverageOrdinalPosition) {
  // {3, 1, 3, 2, 3, NaN}: sorted 1(0) 2(1) 3 3 3 (positions 2,3,4 -> 3). n = 5.
  const Fixture f = one_field(1, 6, {3.0, 1.0, 3.0, 2.0, 3.0, kNaN});
  const Panel p = f.panel();
  const std::vector<f64> v = vm_eval("rank(close)", p);
  const std::vector<f64> r = oracle_eval("rank(close)", p);
  const std::vector<f64> want = {0.75, 0.0, 0.75, 0.25, 0.75, kNaN};
  for (usize j = 0; j < 6; ++j) {
    EXPECT_TRUE(same_bits(v[j], want[j])) << j << " vm=" << v[j];
    EXPECT_TRUE(same_bits(r[j], want[j])) << j << " oracle=" << r[j];
  }
}

TEST(AlphaCsRankTies_Row, SignedZerosTie) {
  const Fixture f = one_field(1, 3, {-0.0, 0.0, 1.0});
  const Panel p = f.panel();
  const std::vector<f64> v = vm_eval("rank(close)", p);
  EXPECT_EQ(v[0], 0.25);
  EXPECT_EQ(v[1], 0.25);
  EXPECT_EQ(v[2], 1.0);
  expect_vm_oracle_bitexact("rank(close)", p);
}

TEST(AlphaCsRankTies_Group, GroupRankAndQuantileShareTiedValues) {
  // 6 names, sector = j % 2, all values equal within a sector -> 0.5 each;
  // quantile of an all-tied row puts every name in ONE bucket.
  const Fixture f = one_field(1, 6, {4.0, 9.0, 4.0, 9.0, 4.0, 9.0});
  const Panel p = f.panel();
  const std::vector<f64> g = vm_eval("group_rank(close, IndClass.sector)", p);
  for (usize j = 0; j < 6; ++j) {
    EXPECT_EQ(g[j], 0.5) << j;
  }
  const Fixture tied = one_field(1, 6, std::vector<f64>(6, 2.0));
  const std::vector<f64> q = vm_eval("quantile(close, 4)", tied.panel());
  for (usize j = 0; j < 6; ++j) {
    EXPECT_EQ(q[j], q[0]) << j; // p = 0.5 -> bucket 2 of 4 -> 2/3
    EXPECT_DOUBLE_EQ(q[j], 2.0 / 3.0);
  }
  expect_vm_oracle_bitexact("group_rank(close, IndClass.sector)", p);
  expect_vm_oracle_bitexact("quantile(close, 4)", tied.panel());
}

TEST(AlphaCsRankTies_Radix, WideTiedRowsMatchOracleAboveRadixThreshold) {
  // 200 names (> kCsRadixMinRow = 96) with values in {0,1,2}: heavy ties on the
  // radix argsort path; plus an all-tied date.
  constexpr usize kD = 4;
  constexpr usize kI = 200;
  static_assert(kI > atx::engine::alpha::detail::kCsRadixMinRow);
  std::vector<f64> x(kD * kI);
  std::mt19937_64 rng{0xA01ULL};
  for (usize i = 0; i < x.size(); ++i) {
    x[i] = static_cast<f64>(rng() % 3);
  }
  for (usize j = 0; j < kI; ++j) {
    x[3 * kI + j] = 5.0; // date 3 all tied
  }
  const Fixture f = one_field(kD, kI, x);
  const Panel p = f.panel();
  for (const std::string_view e :
       {"rank(close)", "group_rank(close, IndClass.sector)", "quantile(close, 5)"}) {
    expect_vm_oracle_bitexact(e, p);
  }
  const std::vector<f64> v = vm_eval("rank(close)", p);
  for (usize j = 0; j < kI; ++j) {
    EXPECT_EQ(v[3 * kI + j], 0.5) << j;
  }
}

TEST(AlphaCsRankTies_Differential, TieHeavyExpressionsMatchOracleAndStream) {
  const Fixture f = mixed_fixture(40, 12, 0xA01A01ULL);
  const Panel p = f.panel();
  for (const std::string_view e :
       {"rank(sign(close - open))", "rank(tier)", "group_rank(sign(close - open), IndClass.sector)",
        "quantile(tier, 3)", "rank(group_count(close, IndClass.sector))", "rank(close)"}) {
    expect_vm_oracle_bitexact(e, p);
    EXPECT_EQ(count_mismatch(stream_eval(e, f, EvalMode::AuditExact), vm_eval(e, p)), 0U) << e;
  }
}

// ===========================================================================
//  A-02 — AlphaHumpWarmup_*
// ===========================================================================

TEST(AlphaHumpWarmup_TsMean, FiniteFromT4) {
  constexpr usize kD = 20;
  constexpr usize kI = 3;
  std::vector<f64> x(kD * kI);
  for (usize t = 0; t < kD; ++t) {
    for (usize j = 0; j < kI; ++j) {
      x[t * kI + j] = 1.0 + 0.3 * static_cast<f64>(t) + static_cast<f64>(j);
    }
  }
  const Fixture f = one_field(kD, kI, x);
  const Panel p = f.panel();
  const std::string_view e = "hump(ts_mean(close, 5), 0.1)";
  const std::vector<f64> v = vm_eval(e, p);
  const std::vector<f64> r = oracle_eval(e, p);
  const std::vector<f64> s = stream_eval(e, f, EvalMode::AuditExact);
  usize finite = 0;
  for (usize t = 0; t < kD; ++t) {
    for (usize j = 0; j < kI; ++j) {
      const usize c = t * kI + j;
      if (t < 4) {
        EXPECT_TRUE(std::isnan(v[c])) << "t=" << t;
      } else {
        EXPECT_TRUE(std::isfinite(v[c])) << "t=" << t << " j=" << j;
        finite += std::isfinite(v[c]) ? 1U : 0U;
      }
    }
  }
  EXPECT_EQ(finite, (kD - 4) * kI);
  EXPECT_NEAR(v[4 * kI + 0], 1.6, 1e-12); // seeded with ts_mean at t=4 (mean of t=0..4)
  EXPECT_EQ(count_mismatch(v, r), 0U);
  EXPECT_EQ(count_mismatch(v, s), 0U);
  // The pre-W0 rule (StickyV1) never recovers from the NaN warm-up.
  KernelPolicy legacy{};
  legacy.hump = HumpNaN::StickyV1;
  const std::vector<f64> old = vm_eval(e, p, EvalMode::AuditExact, legacy);
  const auto old_finite = std::count_if(old.begin(), old.end(), [](f64 z) { return std::isfinite(z); });
  EXPECT_EQ(old_finite, 0);
  std::printf("[w0a0] hump(ts_mean(x,5),0.1): finite cells new=%zu old=%td of %zu\n", finite,
              old_finite, kD * kI);
}

TEST(AlphaHumpWarmup_Nan, NanInputEmitsNanAndCapBoundsStaleness) {
  // thr 0.1. Column 0: gap of exactly kHumpMaxStaleDates NaNs -> prior held.
  // Column 1: gap of cap+1 NaNs -> prior dropped, re-seeds on return.
  static_assert(atx::engine::alpha::detail::kHumpMaxStaleDates == 5);
  constexpr usize kD = 10;
  constexpr usize kI = 2;
  std::vector<f64> x = {
      1.00, 1.00, //
      1.05, 1.05, //
      kNaN, kNaN, //
      kNaN, kNaN, //
      kNaN, kNaN, //
      kNaN, kNaN, //
      kNaN, kNaN, //
      1.08, kNaN, //
      1.30, 1.08, //
      1.35, 1.12, //
  };
  const Fixture f = one_field(kD, kI, x);
  const Panel p = f.panel();
  const std::string_view e = "hump(close, 0.1)";
  const std::vector<f64> v = vm_eval(e, p);
  const std::vector<f64> want = {
      1.00, 1.00, //
      1.00, 1.00, // |0.05| <= 0.1 holds
      kNaN, kNaN, // NaN in -> NaN out
      kNaN, kNaN, //
      kNaN, kNaN, //
      kNaN, kNaN, //
      kNaN, kNaN, //
      1.00, kNaN, // col 0: 5 NaNs, prior 1.00 kept, |1.08-1| <= 0.1 holds
      1.30, 1.08, // col 1: 6 NaNs dropped the prior -> re-seeds with 1.08
      1.30, 1.08, //
  };
  for (usize c = 0; c < want.size(); ++c) {
    EXPECT_TRUE(same_bits(v[c], want[c])) << "cell " << c << " got " << v[c];
  }
  EXPECT_EQ(count_mismatch(v, oracle_eval(e, p)), 0U);
  EXPECT_EQ(count_mismatch(v, stream_eval(e, f, EvalMode::AuditExact)), 0U);
}

TEST(AlphaHumpWarmup_Differential, UniverseGapsMatchOracleAndStream) {
  const Fixture f = mixed_fixture(60, 10, 0xA02A02ULL);
  const Panel p = f.panel();
  for (const std::string_view e :
       {"hump(ts_mean(close, 5), 0.1)", "hump(close, 0.5)", "hump(rank(close), 0.05)",
        "hump(close)"}) {
    expect_vm_oracle_bitexact(e, p);
    EXPECT_EQ(count_mismatch(stream_eval(e, f, EvalMode::AuditExact), vm_eval(e, p)), 0U) << e;
  }
}

// ===========================================================================
//  A-03 — AlphaTypecheckScalarLiteral_*
// ===========================================================================

atx::core::Status analyze_src(std::string_view src) {
  auto ast = atx::engine::alpha::parse_expr(src, lib());
  EXPECT_TRUE(ast.has_value()) << src;
  if (!ast) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "parse");
  }
  auto ana = atx::engine::alpha::analyze(ast.value());
  if (!ana) {
    return atx::core::Err(ana.error());
  }
  return atx::core::Ok();
}

TEST(AlphaTypecheckScalarLiteral_Analyze, WinsorizeWithPanelScalarIsErr) {
  const atx::core::Status s = analyze_src("winsorize(open, close)");
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_NE(s.error().message().find("literal"), std::string::npos) << s.error().message();
}

TEST(AlphaTypecheckScalarLiteral_Analyze, EveryScalarSlotRejectsNonLiteral) {
  for (const std::string_view src :
       {"scale(close, volume)", "quantile(close, volume)", "hump(close, open)",
        "winsorize(close, rank(volume))", "scale(close, 1 + volume * 0)",
        "hump(close, ts_mean(close, 3))", "winsorize(close, 1 / 0)", "scale(close, 0 / 0)"}) {
    EXPECT_FALSE(analyze_src(src).has_value()) << src;
  }
}

TEST(AlphaTypecheckScalarLiteral_Analyze, LiteralsAndDefaultsAccepted) {
  for (const std::string_view src :
       {"scale(close, 2)", "scale(close, -1)", "scale(close)", "winsorize(close)",
        "winsorize(close, 2.5)", "quantile(close, 5)", "quantile(close)", "hump(close, 0.1)",
        "hump(close)", "scale(close, 2 * 3)"}) {
    EXPECT_TRUE(analyze_src(src).has_value()) << src;
  }
}

TEST(AlphaTypecheckScalarLiteral_Analyze, SlotPredicateCoversExactlyTheFourOps) {
  using atx::engine::alpha::OpCode;
  using atx::engine::alpha::detail::has_scalar_literal_slot;
  EXPECT_TRUE(has_scalar_literal_slot(OpCode::CsScale));
  EXPECT_TRUE(has_scalar_literal_slot(OpCode::CsWinsorize));
  EXPECT_TRUE(has_scalar_literal_slot(OpCode::CsQuantile));
  EXPECT_TRUE(has_scalar_literal_slot(OpCode::Hump));
  EXPECT_FALSE(has_scalar_literal_slot(OpCode::CsRank));
  EXPECT_FALSE(has_scalar_literal_slot(OpCode::TsMean)); // windows have their own rail
  EXPECT_FALSE(has_scalar_literal_slot(OpCode::Spow));
}

// ===========================================================================
//  A-09 — AlphaFlatWindow_*
// ===========================================================================

TEST(AlphaFlatWindow_Zscore, ConstantPointOneWindowIsNaNInVmAndOracle) {
  constexpr usize kD = 40;
  constexpr usize kI = 3;
  const Fixture f = one_field(kD, kI, std::vector<f64>(kD * kI, 0.1), "fund");
  const Panel p = f.panel();
  usize legacy_finite_total = 0;
  f64 legacy_max_abs = 0.0;
  for (const int d : {3, 5, 7, 10, 20}) {
    const std::string e = "ts_zscore(fund, " + std::to_string(d) + ")";
    const std::vector<f64> v = vm_eval(e, p);
    const std::vector<f64> r = oracle_eval(e, p);
    for (usize c = 0; c < v.size(); ++c) {
      EXPECT_TRUE(std::isnan(v[c])) << e << " cell " << c << " vm=" << v[c];
      EXPECT_TRUE(std::isnan(r[c])) << e << " cell " << c << " oracle=" << r[c];
    }
    KernelPolicy legacy{};
    legacy.flat = FlatGuard::NoneV1;
    const std::vector<f64> old = vm_eval(e, p, EvalMode::AuditExact, legacy);
    usize fin = 0;
    for (const f64 z : old) {
      if (std::isfinite(z)) {
        ++fin;
        legacy_max_abs = std::max(legacy_max_abs, std::fabs(z));
      }
    }
    legacy_finite_total += fin;
    std::printf("[w0a0] %s on constant 0.1: legacy finite cells=%zu\n", e.c_str(), fin);
  }
  std::printf("[w0a0] legacy ts_zscore noise: %zu finite cells, max |z| = %.6f\n",
              legacy_finite_total, legacy_max_abs);
  EXPECT_GT(legacy_finite_total, 0U); // the defect existed: noise, not NaN
  EXPECT_GT(legacy_max_abs, 0.5);
}

TEST(AlphaFlatWindow_Family, FlatWindowsGuardStdCorrSlopeSkewRegression) {
  // fund: constant per instrument (forward-filled); close varies.
  constexpr usize kD = 30;
  constexpr usize kI = 4;
  Fixture f;
  f.dates = kD;
  f.inst = kI;
  f.names = {"fund", "close"};
  f.cols.assign(2, std::vector<f64>(kD * kI));
  for (usize t = 0; t < kD; ++t) {
    for (usize j = 0; j < kI; ++j) {
      f.cols[0][t * kI + j] = 0.1 * static_cast<f64>(j + 1) + 0.7; // flat, inexact binary
      f.cols[1][t * kI + j] = 10.0 + std::sin(0.3 * static_cast<f64>(t + 5 * j));
    }
  }
  const Panel p = f.panel();
  const auto all = [](const std::vector<f64> &v, auto pred) {
    for (usize c = 4 * 4; c < v.size(); ++c) { // every cell with a full window (d <= 5)
      if (!pred(v[c])) {
        return false;
      }
    }
    return true;
  };
  EXPECT_TRUE(all(vm_eval("ts_std(fund, 5)", p), [](f64 z) { return z == 0.0; }));
  EXPECT_TRUE(all(vm_eval("ts_var(fund, 5)", p), [](f64 z) { return z == 0.0; }));
  EXPECT_TRUE(all(vm_eval("correlation(fund, close, 5)", p), [](f64 z) { return std::isnan(z); }));
  EXPECT_TRUE(all(vm_eval("slope(fund, 5)", p), [](f64 z) { return z == 0.0; }));
  EXPECT_TRUE(all(vm_eval("resid(fund, 5)", p), [](f64 z) { return z == 0.0; }));
  EXPECT_TRUE(all(vm_eval("rsquare(fund, 5)", p), [](f64 z) { return std::isnan(z); }));
  EXPECT_TRUE(all(vm_eval("skew(fund, 5)", p), [](f64 z) { return std::isnan(z); }));
  EXPECT_TRUE(all(vm_eval("kurt(fund, 5)", p), [](f64 z) { return std::isnan(z); }));
  EXPECT_TRUE(
      all(vm_eval("ts_regression(close, fund, 5)", p), [](f64 z) { return std::isnan(z); }));
  for (const std::string_view e :
       {"ts_std(fund, 5)", "ts_var(fund, 5)", "ts_zscore(fund, 5)", "correlation(fund, close, 5)",
        "correlation(close, fund, 5)", "slope(fund, 5)", "resid(fund, 5)", "rsquare(fund, 5)",
        "skew(fund, 5)", "kurt(fund, 5)", "ts_regression(close, fund, 5)",
        "ts_regression(fund, close, 5)", "covariance(fund, close, 5)"}) {
    expect_vm_oracle_bitexact(e, p);
    EXPECT_EQ(count_mismatch(stream_eval(e, f, EvalMode::AuditExact), vm_eval(e, p)), 0U) << e;
  }
  // Measured pre-W0 noise on the same windows (reported, not asserted exact).
  KernelPolicy legacy{};
  legacy.flat = FlatGuard::NoneV1;
  const std::vector<f64> old_corr = vm_eval("correlation(fund, close, 5)", p,
                                            EvalMode::AuditExact, legacy);
  const std::vector<f64> old_slope = vm_eval("slope(fund, 5)", p, EvalMode::AuditExact, legacy);
  usize corr_fin = 0;
  f64 slope_max = 0.0;
  for (usize c = 16; c < old_corr.size(); ++c) {
    corr_fin += std::isfinite(old_corr[c]) ? 1U : 0U;
    slope_max = std::max(slope_max, std::fabs(old_slope[c]));
  }
  std::printf("[w0a0] legacy flat corr finite cells=%zu, max |slope| = %.3e\n", corr_fin,
              slope_max);
}

TEST(AlphaFlatWindow_Guard, NonFlatWindowsAreBitIdenticalToLegacy) {
  // Relative dispersion ~1e-6 (far above the 1e-10 guard): the guard must not
  // move a single bit of any guarded op.
  constexpr usize kD = 50;
  constexpr usize kI = 3;
  std::vector<f64> x(kD * kI);
  for (usize t = 0; t < kD; ++t) {
    for (usize j = 0; j < kI; ++j) {
      x[t * kI + j] = 100.0 + 1e-4 * std::sin(0.7 * static_cast<f64>(t) + static_cast<f64>(j));
    }
  }
  Fixture f = one_field(kD, kI, x);
  f.names[1] = "open";
  for (usize i = 0; i < f.cols[1].size(); ++i) {
    f.cols[1][i] = 5.0 + std::cos(0.2 * static_cast<f64>(i));
  }
  const Panel p = f.panel();
  KernelPolicy legacy{};
  legacy.flat = FlatGuard::NoneV1;
  for (const std::string_view e :
       {"ts_std(close, 7)", "ts_zscore(close, 7)", "skew(close, 7)", "kurt(close, 7)",
        "correlation(close, open, 7)", "slope(close, 7)", "rsquare(close, 7)", "resid(close, 7)",
        "ts_regression(open, close, 7)"}) {
    EXPECT_EQ(count_mismatch(vm_eval(e, p), vm_eval(e, p, EvalMode::AuditExact, legacy)), 0U)
        << e;
  }
}

TEST(AlphaFlatWindow_Guard, PredicateBoundary) {
  using atx::engine::alpha::detail::tsv_is_flat;
  using atx::engine::alpha::detail::window_is_flat;
  // mean 1, population std exactly at / above the 1e-10 relative tolerance.
  EXPECT_TRUE(tsv_is_flat(0.0, 0.0, 5));  // exact all-zero window
  EXPECT_TRUE(tsv_is_flat(0.0, 0.1, 5));  // exact constant
  EXPECT_TRUE(tsv_is_flat(5e-21, 1.0, 5)); // std 1e-10.5 < 1e-10
  EXPECT_FALSE(tsv_is_flat(5e-18, 1.0, 5)); // std ~1e-9
  EXPECT_FALSE(tsv_is_flat(kNaN, 1.0, 5));
  for (const f64 ss : {0.0, 5e-21, 5e-20, 5e-19, 5e-18, 1.0}) {
    EXPECT_EQ(tsv_is_flat(ss, 1.0, 5), window_is_flat(ss, 1.0, 5)) << ss; // VM == oracle rule
  }
}

// ===========================================================================
//  A-13 — AlphaAuditExactParity_*
// ===========================================================================

// High-magnitude drifting columns with NaN and ±inf holes (running-sum stress).
Fixture sum_fixture(usize dates, usize inst, std::uint64_t seed) {
  Fixture f;
  f.dates = dates;
  f.inst = inst;
  f.names = {"close", "volume"};
  f.cols.assign(2, std::vector<f64>(dates * inst));
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<f64> u{0.0, 1.0};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < inst; ++j) {
      const usize c = t * inst + j;
      f.cols[0][c] = 1.0e8 * static_cast<f64>(j + 1) + 1.0e3 * u(rng) + 0.001 * static_cast<f64>(t);
      f.cols[1][c] = (u(rng) - 0.5) * std::pow(10.0, static_cast<f64>(t % 9));
      if (u(rng) < 0.01) {
        f.cols[0][c] = kNaN;
      }
      if (u(rng) < 0.005) {
        f.cols[1][c] = (t % 2 == 0) ? kInf : -kInf;
      }
    }
  }
  return f;
}

TEST(AlphaAuditExactParity_Sum, TsSumTsMeanBitExactVsOracle) {
  const Fixture f = sum_fixture(300, 6, 0xA13A13ULL);
  const Panel p = f.panel();
  KernelPolicy legacy{};
  legacy.ts_sum = TsSumPath::OnlineV1;
  usize legacy_mismatch = 0;
  usize cells = 0;
  for (const std::string_view fld : {"close", "volume"}) {
    for (const int d : {1, 2, 5, 20, 63, 250}) {
      for (const std::string_view op : {"ts_sum", "ts_mean"}) {
        const std::string e =
            std::string{op} + "(" + std::string{fld} + ", " + std::to_string(d) + ")";
        const std::vector<f64> v = vm_eval(e, p);
        const std::vector<f64> r = oracle_eval(e, p);
        EXPECT_EQ(count_mismatch(v, r), 0U) << e; // AuditExact: oracle-exact
        EXPECT_EQ(count_mismatch(stream_eval(e, f, EvalMode::AuditExact), v), 0U) << e;
        legacy_mismatch += count_mismatch(vm_eval(e, p, EvalMode::AuditExact, legacy), r);
        cells += v.size();
      }
    }
  }
  std::printf("[w0a0] AuditExact ts_sum/ts_mean: new mismatches=0, legacy online mismatches=%zu "
              "of %zu cells\n",
              legacy_mismatch, cells);
  EXPECT_GT(legacy_mismatch, 0U); // the pre-W0 online path was NOT oracle-exact
}

TEST(AlphaAuditExactParity_Sum, OutputIndependentOfPanelStart) {
  const Fixture f = sum_fixture(200, 4, 0x57A27ULL);
  const Panel full = f.panel();
  constexpr usize kFrom = 37;
  const Panel tail = f.suffix(kFrom);
  KernelPolicy legacy{};
  legacy.ts_sum = TsSumPath::OnlineV1;
  usize legacy_diff = 0;
  usize compared = 0;
  for (const auto &[e, d] : {std::pair<std::string_view, usize>{"ts_sum(close, 20)", 20},
                             std::pair<std::string_view, usize>{"ts_mean(volume, 10)", 10}}) {
    const std::vector<f64> a = vm_eval(e, full);
    const std::vector<f64> b = vm_eval(e, tail);
    const std::vector<f64> la = vm_eval(e, full, EvalMode::AuditExact, legacy);
    const std::vector<f64> lb = vm_eval(e, tail, EvalMode::AuditExact, legacy);
    // Every date whose window lies wholly inside the shorter panel.
    for (usize c = (d - 1) * f.inst; c < b.size(); ++c) {
      EXPECT_TRUE(same_bits(a[kFrom * f.inst + c], b[c])) << e << " cell " << c;
      legacy_diff += same_bits(la[kFrom * f.inst + c], lb[c]) ? 0U : 1U;
      ++compared;
    }
  }
  std::printf("[w0a0] panel-start dependence over %zu cells: new=0, legacy=%zu\n", compared,
              legacy_diff);
  EXPECT_GT(legacy_diff, 0U);
}

// A per-window compensated (Neumaier) sum — an accurate reference (error ~eps·|s|)
// against which both online slides are measured. NaN on a short/missing window.
f64 ref_window_sum(const std::vector<f64> &col, usize t, usize j, usize d, usize inst) {
  if (t + 1 < d) {
    return kNaN;
  }
  f64 s = 0.0;
  f64 c = 0.0;
  for (usize k = t + 1 - d; k <= t; ++k) {
    const f64 v = col[k * inst + j];
    if (!std::isfinite(v)) {
      return kNaN;
    }
    const f64 tt = s + v;
    c += (std::fabs(s) >= std::fabs(v)) ? (s - tt) + v : (v - tt) + s;
    s = tt;
  }
  return s + c;
}

TEST(AlphaAuditExactParity_ResearchFast, NeumaierSlideTighterThanUncompensated) {
  constexpr usize kD = 400;
  constexpr usize kI = 6;
  const Fixture f = sum_fixture(kD, kI, 0xF457ULL);
  const Panel p = f.panel();
  KernelPolicy legacy{};
  legacy.ts_sum = TsSumPath::OnlineV1;
  struct Case {
    std::string_view expr;
    usize field;
    usize d;
    bool mean;
  };
  f64 err_new = 0.0;
  f64 err_old = 0.0;
  for (const Case &k : {Case{"ts_sum(close, 63)", 0, 63, false},
                        Case{"ts_mean(close, 250)", 0, 250, true},
                        Case{"ts_sum(volume, 20)", 1, 20, false}}) {
    const std::vector<f64> v = vm_eval(k.expr, p, EvalMode::ResearchFast);
    const std::vector<f64> o = vm_eval(k.expr, p, EvalMode::ResearchFast, legacy);
    const std::vector<f64> r = oracle_eval(k.expr, p);
    EXPECT_EQ(count_mismatch(stream_eval(k.expr, f, EvalMode::ResearchFast), v), 0U) << k.expr;
    for (usize t = 0; t < kD; ++t) {
      for (usize j = 0; j < kI; ++j) {
        const usize c = t * kI + j;
        f64 ref = ref_window_sum(f.cols[k.field], t, j, k.d, kI);
        EXPECT_EQ(std::isnan(v[c]), std::isnan(r[c])) << k.expr << " " << c; // same NaN gate
        if (std::isnan(ref)) {
          EXPECT_TRUE(std::isnan(v[c])) << k.expr << " " << c;
          continue;
        }
        ref = k.mean ? ref / static_cast<f64>(k.d) : ref;
        const f64 scale = std::max(1.0, std::fabs(ref));
        err_new = std::max(err_new, std::fabs(v[c] - ref) / scale);
        err_old = std::max(err_old, std::fabs(o[c] - ref) / scale);
      }
    }
  }
  std::printf("[w0a0] ResearchFast max rel err vs compensated reference: neumaier=%.3e "
              "uncompensated=%.3e\n",
              err_new, err_old);
  EXPECT_LE(err_new, err_old);
  EXPECT_LT(err_new, 1e-12);
}

// The full VM↔oracle differential over the W0-A0 surface on a mixed fixture
// (ties, universe gaps, forward-filled blocks), AuditExact, bit-for-bit.
TEST(AlphaAuditExactParity_Differential, W0SurfaceBitExact) {
  const Fixture f = mixed_fixture(70, 14, 0xD1FFULL);
  const Panel p = f.panel();
  for (const std::string_view e :
       {"rank(close)", "rank(sign(close - open))", "group_rank(tier, IndClass.sector)",
        "quantile(close, 4)", "quantile(tier, 3)", "scale(close, 2)", "winsorize(close, 1.5)",
        "hump(ts_mean(close, 5), 0.1)", "hump(close, 0.2)", "ts_sum(close, 5)",
        "ts_mean(volume, 10)", "ts_sum(fund, 7)", "ts_zscore(fund, 5)", "ts_std(fund, 7)",
        "correlation(fund, close, 6)", "slope(fund, 5)", "resid(fund, 5)", "rsquare(fund, 5)",
        "skew(fund, 6)", "kurt(fund, 6)", "ts_regression(close, fund, 5)",
        "ts_zscore(close, 10)", "correlation(close, volume, 8)"}) {
    expect_vm_oracle_bitexact(e, p);
  }
}

} // namespace atx_test_w0_a0_kernels

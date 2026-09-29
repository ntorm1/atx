// Platform-v7 W2 DSL ops for the literature families (A7): oracle <-> VM <->
// streaming differentials, analytic pins, typecheck refusals, parser / unparse
// round trips and the registry / opcode-id freeze.
//
//   * Differential: every W2 op (and nestings with packs, group builders and
//     pre-W2 ops) on a seed-fixed panel with NaN holes, a flat field, NaN group
//     labels and universe gaps — VM == oracle cell-for-cell (NaN == NaN), the
//     fused / subtree-cached / node-subset / global-DAG paths == plain evaluate,
//     and StreamingEngine warm + step == batch bit-for-bit in both EvalModes.
//   * Analytic: hand-computed windows for each rule in lit_ops.hpp, exact-linear
//     regressions (residual 0, slopes recovered), bucket == quantile * (n-1).
//   * Refusals: Group-typed arguments where a numeric vector is required and
//     vice versa, records outside a regressor slot, count / window rails.
//   * The five literature-family DSL strings of the W2 report compile and agree
//     VM == oracle (they are NOT added to any library).
//
// Naming: Subject_Condition_ExpectedResult.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "atx/engine/alpha/fusion.hpp"
#include "atx/engine/alpha/lit_ops.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streaming_engine.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/unparse.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/global_dag_eval.hpp"

namespace atx_test_alpha_lit_ops {

namespace alpha = atx::engine::alpha;
namespace par = atx::engine::parallel;
using alpha::CrossSection;
using alpha::Engine;
using alpha::EvalMode;
using alpha::Library;
using alpha::OpCode;
using alpha::Panel;
using alpha::Program;
using alpha::SignalSet;
using alpha::StreamingEngine;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

const Library &lib() {
  static const Library l;
  return l;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
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

[[nodiscard]] Program compile_src(std::string_view src) {
  auto ast = alpha::parse_program(src, lib());
  EXPECT_TRUE(ast.has_value()) << src << ": " << (ast ? "" : ast.error().message());
  if (!ast) {
    return Program{};
  }
  auto ana = alpha::analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << src << ": " << (ana ? "" : ana.error().message());
  if (!ana) {
    return Program{};
  }
  auto prog = alpha::compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << src;
  return prog.value_or(Program{});
}

[[nodiscard]] Program compile_expr(std::string_view expr) {
  return compile_src(std::string{"a = "} + std::string{expr});
}

// True when `src` is refused by the parser or the typechecker.
[[nodiscard]] bool refused(std::string_view src) {
  auto ast = alpha::parse_expr(src, lib());
  if (!ast) {
    return true;
  }
  return !alpha::analyze(ast.value()).has_value();
}

// ---- fixture ----------------------------------------------------------------

struct Fixture {
  atx::usize dates{0};
  atx::usize inst{0};
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  std::vector<std::uint8_t> universe;
};

[[nodiscard]] const std::vector<std::string> &field_names() {
  static const std::vector<std::string> k = {
      "open",   "high",   "low",    "close",   "volume",          "returns",  "ff_mkt",
      "ff_smb", "ff_hml", "size",   "cop_at",  "droe",            "tobin_q",  "ia",
      "ni_q",   "flat",   "grp_ff12", "grp_neg", "IndClass.sector", "grp_frac"};
  return k;
}

// Price walks, factor-like broadcast series (a NaN day), cross-sectional
// characteristics with holes, a quarterly forward-filled ni_q, a flat field,
// integer / negative / fractional group labels, and a moving universe.
[[nodiscard]] Fixture make_fixture(atx::usize dates, atx::usize inst, std::uint64_t seed) {
  Fixture f;
  f.dates = dates;
  f.inst = inst;
  f.names = field_names();
  const atx::usize cells = dates * inst;
  f.cols.assign(f.names.size(), std::vector<atx::f64>(cells, 0.0));
  f.universe.assign(cells, 1);
  std::mt19937_64 rng{seed};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::uniform_real_distribution<atx::f64> u{0.0, 1.0};
  std::vector<atx::f64> px(inst, 40.0);
  std::vector<atx::f64> ni(inst, 1.0);
  for (atx::usize t = 0; t < dates; ++t) {
    const atx::f64 mkt = (t == 17) ? kNaN : 0.01 * nd(rng);
    const atx::f64 smb = 0.005 * nd(rng);
    const atx::f64 hml = 0.005 * nd(rng);
    for (atx::usize j = 0; j < inst; ++j) {
      const atx::usize i = t * inst + j;
      const atx::f64 prev = px[j];
      px[j] = std::max(1.0, px[j] * (1.0 + 0.02 * nd(rng)));
      const atx::f64 c = px[j];
      const atx::f64 o = prev * (1.0 + 0.005 * nd(rng));
      f.cols[0][i] = o;
      f.cols[1][i] = std::max(o, c) * (1.0 + 0.01 * u(rng));
      f.cols[2][i] = std::min(o, c) * (1.0 - 0.01 * u(rng));
      f.cols[3][i] = (u(rng) < 0.01) ? kNaN : c;
      f.cols[4][i] = 1.0e5 * (0.5 + u(rng));
      f.cols[5][i] = (u(rng) < 0.02) ? kNaN : c / prev - 1.0;
      f.cols[6][i] = mkt;
      f.cols[7][i] = smb;
      f.cols[8][i] = hml;
      f.cols[9][i] = (u(rng) < 0.02) ? kNaN : 10.0 + 3.0 * nd(rng);
      f.cols[10][i] = 0.1 + 0.05 * nd(rng);
      f.cols[11][i] = (u(rng) < 0.03) ? kNaN : 0.02 * nd(rng);
      f.cols[12][i] = std::exp(0.3 * nd(rng));
      f.cols[13][i] = 0.08 + 0.03 * nd(rng);
      if (t % 21 == 0) {
        ni[j] += (u(rng) < 0.6) ? 0.1 : -0.1; // a quarterly report
      }
      f.cols[14][i] = ni[j];
      f.cols[15][i] = 5.0;
      f.cols[16][i] = (j == 4 && t % 7 == 0) ? kNaN : static_cast<atx::f64>((j * 7) % 12);
      f.cols[17][i] = -1.0;
      f.cols[18][i] = static_cast<atx::f64>(j % 3);
      f.cols[19][i] = 0.5;
      if (u(rng) < 0.02) {
        f.universe[i] = 0;
      }
    }
  }
  return f;
}

[[nodiscard]] Panel prefix_panel(const Fixture &f, atx::usize upto) {
  const atx::usize cells = upto * f.inst;
  std::vector<std::vector<atx::f64>> cols;
  for (const std::vector<atx::f64> &c : f.cols) {
    cols.emplace_back(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(cells));
  }
  std::vector<std::uint8_t> uni(f.universe.begin(),
                                f.universe.begin() + static_cast<std::ptrdiff_t>(cells));
  auto p = Panel::create(upto, f.inst, f.names, std::move(cols), std::move(uni));
  EXPECT_TRUE(p.has_value());
  return p.value();
}

[[nodiscard]] CrossSection row_of(const Fixture &f, const Program &prog, atx::usize t) {
  CrossSection cs;
  for (const std::string &name : prog.fields) {
    atx::usize k = 0;
    while (k < f.names.size() && f.names[k] != name) {
      ++k;
    }
    EXPECT_LT(k, f.names.size()) << name;
    cs.fields.emplace_back(f.cols[k].data() + t * f.inst, f.inst);
  }
  cs.universe = std::span<const std::uint8_t>{f.universe.data() + t * f.inst, f.inst};
  return cs;
}

// A one-instrument panel of named series (no universe gaps).
[[nodiscard]] Panel series_panel(const std::vector<std::string> &names,
                                 std::vector<std::vector<atx::f64>> cols) {
  const atx::usize dates = cols.front().size();
  auto p = Panel::create(dates, 1, names, std::move(cols), {});
  EXPECT_TRUE(p.has_value());
  return p.value();
}

[[nodiscard]] std::vector<atx::f64> vm_values(std::string_view expr, const Panel &panel) {
  const Program prog = compile_expr(expr);
  Engine eng{panel};
  auto out = eng.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << expr << ": " << (out ? "" : out.error().message());
  return out.has_value() ? out.value().alphas.front().values : std::vector<atx::f64>{};
}

void expect_signals_equal(const SignalSet &got, const SignalSet &want, std::string_view what) {
  ASSERT_EQ(got.alphas.size(), want.alphas.size()) << what;
  for (atx::usize a = 0; a < got.alphas.size(); ++a) {
    ASSERT_EQ(got.alphas[a].values.size(), want.alphas[a].values.size()) << what;
    for (atx::usize i = 0; i < got.alphas[a].values.size(); ++i) {
      ASSERT_TRUE(same_bits(got.alphas[a].values[i], want.alphas[a].values[i]))
          << what << " root " << want.alphas[a].name << " cell " << i
          << ": got=" << got.alphas[a].values[i] << " want=" << want.alphas[a].values[i];
    }
  }
}

// ---- the differential battery -------------------------------------------------

[[nodiscard]] const std::vector<std::string_view> &battery() {
  static const std::vector<std::string_view> k = {
      "ts_topk_mean(returns, 5, 2)",
      "ts_topk_mean(close, 7, 7)",
      "ts_topk_mean(close, 1, 1)",
      "ts_count_increases(delta(close, 1), 6)",
      "ts_count_increases(returns, 10)",
      "ts_count_increases((ni_q != delay(ni_q, 1)) ? ((ni_q > delay(ni_q, 21)) ? 1 : -1) : 0, "
      "40)",
      "ts_sum_mp(returns, 6, 3)",
      "ts_mean_mp(returns, 6, 1)",
      "ts_std_mp(returns, 8, 4)",
      "ts_zscore_mp(close, 8, 3)",
      "ts_min_mp(returns, 5, 2)",
      "ts_max_mp(returns, 5, 2)",
      "decay_linear_mp(returns, 7, 3)",
      "ts_corr_mp(returns, ff_mkt, 10, 5)",
      "ts_std_mp(flat, 5, 2)",
      "ts_zscore_mp(flat, 5, 2)",
      "ts_resid_on(returns, ff_mkt, 12)",
      "ts_resid_on(returns, ff_mkt, ff_smb, 12)",
      "ts_resid_on(returns, ff_mkt, ff_smb, ff_hml, 15)",
      "ts_beta_on(returns, ff_mkt, 12)",
      "ts_beta_on(returns, ff_mkt, ff_smb, ff_hml, 15)",
      "ts_resid_on(close, flat, 8)",
      "ts_resid_on(flat, close, 8)",
      "ts_resid_on(close, pack2(open, high), 9)",
      "cs_resid_on(close, size)",
      "cs_resid_on(close, size, cop_at)",
      "cs_resid_on(returns, size, cop_at, droe)",
      "cs_resid_on(returns, size, cop_at, droe, tobin_q)",
      "cs_resid_on(close, pack2(size, cop_at), pack2(droe, tobin_q))",
      "cs_resid_on(close, flat)",
      "bucket(size, 4)",
      "bucket(close, 13)",
      "group_rank(close, bucket(size, 5))",
      "group_neutralize(returns, group_cross(IndClass.sector, bucket(size, 3)))",
      "group_cross(IndClass.sector, grp_ff12)",
      "group_cross(grp_neg, IndClass.sector)",
      "group_cross(IndClass.sector, grp_frac)",
      "group_mean(close, group_cross(IndClass.sector, grp_ff12))",
      "(ts_resid_on(returns, ff_mkt, ff_smb, 12) > 0) ? 1 : -1",
      "rank(ts_topk_mean(returns, 5, 2)) + cs_resid_on(close, size, cop_at, droe)",
      "pack2(close, open).p1",
      "pack3(close, open, high).p2 - ts_sum_mp(delta(open, 2), 4, 2)",
  };
  return k;
}

// One multi-root program exercising CSE of a shared pack (refcount 2 block).
[[nodiscard]] std::string_view shared_pack_program() {
  return "r = ts_resid_on(returns, ff_mkt, ff_smb, 12)\n"
         "b = ts_beta_on(returns, ff_mkt, ff_smb, 12)\n"
         "c = cs_resid_on(close, size, cop_at, droe, tobin_q)\n"
         "g = group_rank(ts_topk_mean(returns, 6, 3), group_cross(IndClass.sector, "
         "bucket(size, 3)))\n"
         "m = ts_zscore_mp(close, 9, 4) + decay_linear_mp(returns, 5, 2)\n";
}

TEST(AlphaLitOps_Differential, EveryNewOpVmMatchesOracleCellForCell) {
  const Fixture f = make_fixture(90, 13, 0x11705EEDULL);
  const Panel panel = prefix_panel(f, f.dates);
  for (const std::string_view expr : battery()) {
    const Program prog = compile_expr(expr);
    ASSERT_FALSE(prog.roots.empty()) << expr;
    Engine eng{panel};
    auto vm = eng.evaluate(prog);
    ASSERT_TRUE(vm.has_value()) << expr << ": " << vm.error().message();
    auto ref = alpha::evaluate_reference(prog, panel);
    ASSERT_TRUE(ref.has_value()) << expr << ": " << ref.error().message();
    const std::vector<atx::f64> &v = vm->alphas.front().values;
    const std::vector<atx::f64> &r = ref->alphas.front().values;
    ASSERT_EQ(v.size(), r.size());
    atx::usize finite = 0;
    for (atx::usize i = 0; i < v.size(); ++i) {
      ASSERT_TRUE(same_cell(v[i], r[i]))
          << expr << " cell " << i << ": VM=" << v[i] << " oracle=" << r[i];
      finite += std::isfinite(v[i]) ? 1U : 0U;
    }
    const bool all_nan_by_design = expr == "ts_resid_on(close, flat, 8)" ||
                                   expr == "cs_resid_on(close, flat)" ||
                                   expr == "ts_zscore_mp(flat, 5, 2)" ||
                                   expr == "group_cross(grp_neg, IndClass.sector)" ||
                                   expr == "group_cross(IndClass.sector, grp_frac)";
    if (all_nan_by_design) {
      EXPECT_EQ(finite, 0U) << expr;
    } else {
      EXPECT_GT(finite, 0U) << expr << " produced no finite cell";
    }
  }
}

TEST(AlphaLitOps_Differential, SharedPackProgramMatchesOracleAndEveryVmPath) {
  const Fixture f = make_fixture(80, 11, 0x5A4EDBADULL);
  const Panel panel = prefix_panel(f, f.dates);
  const Program prog = compile_src(shared_pack_program());
  ASSERT_EQ(prog.roots.size(), 5U);
  Engine eng{panel};
  auto plain = eng.evaluate(prog);
  ASSERT_TRUE(plain.has_value()) << plain.error().message();
  auto ref = alpha::evaluate_reference(prog, panel);
  ASSERT_TRUE(ref.has_value()) << ref.error().message();
  for (atx::usize a = 0; a < ref->alphas.size(); ++a) {
    for (atx::usize i = 0; i < ref->alphas[a].values.size(); ++i) {
      ASSERT_TRUE(same_cell(plain->alphas[a].values[i], ref->alphas[a].values[i]))
          << ref->alphas[a].name << " cell " << i;
    }
  }
  auto fp = alpha::fuse(prog);
  ASSERT_TRUE(fp.has_value());
  Engine fused_eng{panel};
  auto fused = fused_eng.evaluate(fp.value());
  ASSERT_TRUE(fused.has_value()) << fused.error().message();
  expect_signals_equal(fused.value(), plain.value(), "fused");
  alpha::SubtreeCache cache{atx::usize{1} << 26};
  for (int pass = 0; pass < 2; ++pass) { // cold publish, then warm hits
    Engine cached_eng{panel};
    auto cached = cached_eng.evaluate(prog, &cache);
    ASSERT_TRUE(cached.has_value()) << cached.error().message();
    expect_signals_equal(cached.value(), plain.value(), "subtree-cached");
  }
  par::DetPool pool{3};
  par::GlobalDagOptions opt;
  opt.min_chunk_cells = 16; // force many range chunks per node
  auto gdag = par::global_dag_evaluate(prog, panel, pool, nullptr, nullptr, opt);
  ASSERT_TRUE(gdag.has_value()) << gdag.error().message();
  expect_signals_equal(gdag.value(), plain.value(), "global-DAG");
}

void expect_stream_equals_batch(EvalMode mode) {
  constexpr atx::usize kDates = 70;
  constexpr atx::usize kInst = 9;
  constexpr atx::usize kStream = 22;
  const Fixture f = make_fixture(kDates, kInst, 0x57EA11ULL);
  std::string src{shared_pack_program()};
  for (atx::usize k = 0; k < battery().size(); ++k) {
    src += "x" + std::to_string(k) + " = " + std::string{battery()[k]} + "\n";
  }
  const Program prog = compile_src(src);
  ASSERT_FALSE(prog.roots.empty());
  const Panel full = prefix_panel(f, kDates);
  Engine eng{full};
  eng.set_eval_mode(mode);
  auto batch = eng.evaluate(prog);
  ASSERT_TRUE(batch.has_value()) << batch.error().message();
  auto se = StreamingEngine::create(prog, static_cast<atx::u32>(kInst), mode);
  ASSERT_TRUE(se.has_value()) << se.error().message();
  ASSERT_TRUE(se->warm(prefix_panel(f, kDates - kStream)).has_value());
  for (atx::usize t = kDates - kStream; t < kDates; ++t) {
    auto r = se->step(row_of(f, prog, t));
    ASSERT_TRUE(r.has_value()) << r.error().message();
    for (atx::usize a = 0; a < prog.roots.size(); ++a) {
      const std::span<const atx::f64> got = se->output(a);
      const std::vector<atx::f64> &want = batch->alphas[a].values;
      for (atx::usize j = 0; j < kInst; ++j) {
        ASSERT_TRUE(same_bits(got[j], want[t * kInst + j]))
            << prog.roots[a].name << " t=" << t << " j=" << j << " got=" << got[j]
            << " want=" << want[t * kInst + j];
      }
    }
  }
}

TEST(AlphaLitOps_Streaming, WarmThenStepMatchesBatchBitsAuditExact) {
  expect_stream_equals_batch(EvalMode::AuditExact);
}

TEST(AlphaLitOps_Streaming, WarmThenStepMatchesBatchBitsResearchFast) {
  expect_stream_equals_batch(EvalMode::ResearchFast);
}

// ---- analytic pins ------------------------------------------------------------

TEST(AlphaLitOps_TopkMean, KnownWindowsAndNaNPolicy) {
  const Panel p = series_panel({"x"}, {{1.0, 5.0, 3.0, 9.0, 7.0, 2.0}});
  const std::vector<atx::f64> got = vm_values("ts_topk_mean(x, 4, 2)", p);
  ASSERT_EQ(got.size(), 6U);
  EXPECT_TRUE(std::isnan(got[2])); // short window
  EXPECT_DOUBLE_EQ(got[3], 7.0);   // {1,5,3,9} -> (5+9)/2
  EXPECT_DOUBLE_EQ(got[4], 8.0);   // {5,3,9,7} -> (7+9)/2
  EXPECT_DOUBLE_EQ(got[5], 8.0);   // {3,9,7,2}
  EXPECT_DOUBLE_EQ(vm_values("ts_topk_mean(x, 4, 4)", p)[5], 5.25);
  const Panel h = series_panel({"x"}, {{1.0, kNaN, 3.0, 9.0, 7.0, 2.0}});
  const std::vector<atx::f64> holed = vm_values("ts_topk_mean(x, 4, 2)", h);
  EXPECT_TRUE(std::isnan(holed[3]) && std::isnan(holed[4])); // NaN inside the window
  EXPECT_DOUBLE_EQ(holed[5], 8.0);
}

TEST(AlphaLitOps_CountIncreases, ZerosSkipDecreaseOrNaNEndsTheRun) {
  const Panel p = series_panel({"x"}, {{1.0, -1.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0}});
  EXPECT_DOUBLE_EQ(vm_values("ts_count_increases(x, 8)", p)[7], 3.0);
  const std::vector<atx::f64> w3 = vm_values("ts_count_increases(x, 3)", p);
  EXPECT_DOUBLE_EQ(w3[7], 1.0); // {0, 0, 1}
  EXPECT_DOUBLE_EQ(w3[6], 1.0); // {1, 0, 0}: x[t] == 0 is no event, the run holds 1
  EXPECT_DOUBLE_EQ(w3[3], 1.0); // {-1, 1, 0}
  EXPECT_TRUE(std::isnan(w3[1])); // short window
  const Panel h = series_panel({"x"}, {{1.0, 1.0, kNaN, 1.0, 1.0}});
  const std::vector<atx::f64> holed = vm_values("ts_count_increases(x, 4)", h);
  EXPECT_TRUE(std::isnan(holed[2])); // x[t] missing
  EXPECT_DOUBLE_EQ(holed[4], 2.0); // the NaN at t=2 ends the run
}

TEST(AlphaLitOps_MinPeriods, PartialWindowsAndHolesFollowPandasRules) {
  const Panel p = series_panel({"x", "y"}, {{kNaN, 2.0, 4.0, kNaN, 8.0, 16.0, 32.0},
                                            {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.5}});
  const std::vector<atx::f64> s = vm_values("ts_sum_mp(x, 3, 2)", p);
  EXPECT_TRUE(std::isnan(s[0]) && std::isnan(s[1]));
  EXPECT_DOUBLE_EQ(s[2], 6.0);
  EXPECT_DOUBLE_EQ(s[3], 6.0);
  EXPECT_DOUBLE_EQ(s[4], 12.0);
  EXPECT_DOUBLE_EQ(s[6], 56.0);
  EXPECT_DOUBLE_EQ(vm_values("ts_mean_mp(x, 3, 2)", p)[2], 3.0);
  const std::vector<atx::f64> mn = vm_values("ts_min_mp(x, 3, 1)", p);
  EXPECT_TRUE(std::isnan(mn[0]));
  EXPECT_DOUBLE_EQ(mn[1], 2.0);
  EXPECT_DOUBLE_EQ(mn[6], 8.0);
  EXPECT_DOUBLE_EQ(vm_values("ts_max_mp(x, 3, 1)", p)[6], 32.0);
  const std::vector<atx::f64> dl = vm_values("decay_linear_mp(x, 3, 2)", p);
  EXPECT_DOUBLE_EQ(dl[2], (2.0 * 2.0 + 3.0 * 4.0) / 5.0); // ages 1, 0 weigh 2, 3
  EXPECT_DOUBLE_EQ(dl[4], (1.0 * 4.0 + 3.0 * 8.0) / 4.0); // ages 2, 0 weigh 1, 3
  const std::vector<atx::f64> sd = vm_values("ts_std_mp(x, 3, 1)", p);
  EXPECT_TRUE(std::isnan(sd[1])); // one cell: a sample std needs two
  EXPECT_DOUBLE_EQ(sd[2], std::sqrt(2.0));
  const std::vector<atx::f64> z = vm_values("ts_zscore_mp(x, 3, 2)", p);
  EXPECT_DOUBLE_EQ(z[2], 1.0 / std::sqrt(2.0));
  EXPECT_TRUE(std::isnan(z[3])); // x[t] missing
  const atx::f64 c = vm_values("ts_corr_mp(x, y, 3, 3)", p)[6];
  const std::array<atx::f64, 3> a{8.0, 16.0, 32.0};
  const std::array<atx::f64, 3> b{5.0, 6.0, 7.5};
  const atx::f64 ma = (a[0] + a[1] + a[2]) / 3.0;
  const atx::f64 mb = (b[0] + b[1] + b[2]) / 3.0;
  atx::f64 sab = 0.0;
  atx::f64 saa = 0.0;
  atx::f64 sbb = 0.0;
  for (atx::usize i = 0; i < 3; ++i) {
    sab += (a[i] - ma) * (b[i] - mb);
    saa += (a[i] - ma) * (a[i] - ma);
    sbb += (b[i] - mb) * (b[i] - mb);
  }
  EXPECT_NEAR(c, sab / std::sqrt(saa * sbb), 1e-12);
  EXPECT_TRUE(std::isnan(vm_values("ts_corr_mp(x, y, 3, 3)", p)[5])); // only 2 pairs
}

TEST(AlphaLitOps_MinPeriods, FullValidWindowAgreesWithTheFullWindowOps) {
  Fixture f = make_fixture(60, 7, 0xF011ULL);
  f.universe.assign(f.universe.size(), 1); // open / volume carry no NaN: every window valid
  const Panel panel = prefix_panel(f, f.dates);
  const std::vector<std::pair<std::string_view, std::string_view>> exact = {
      {"ts_sum_mp(open, 6, 6)", "ts_sum(open, 6)"},
      {"ts_mean_mp(open, 6, 6)", "ts_mean(open, 6)"},
      {"ts_min_mp(open, 6, 6)", "ts_min(open, 6)"},
      {"ts_max_mp(open, 6, 6)", "ts_max(open, 6)"}};
  for (const auto &[mp, full] : exact) {
    const std::vector<atx::f64> a = vm_values(mp, panel);
    const std::vector<atx::f64> b = vm_values(full, panel);
    for (atx::usize i = 0; i < a.size(); ++i) {
      ASSERT_TRUE(same_cell(a[i], b[i])) << mp << " vs " << full << " cell " << i;
    }
  }
  const std::vector<std::pair<std::string_view, std::string_view>> close_to = {
      {"ts_std_mp(open, 6, 6)", "stddev(open, 6)"},
      {"ts_zscore_mp(open, 6, 6)", "ts_zscore(open, 6)"},
      {"decay_linear_mp(open, 6, 6)", "decay_linear(open, 6)"},
      {"ts_corr_mp(open, volume, 6, 6)", "correlation(open, volume, 6)"}};
  for (const auto &[mp, full] : close_to) {
    const std::vector<atx::f64> a = vm_values(mp, panel);
    const std::vector<atx::f64> b = vm_values(full, panel);
    for (atx::usize i = 0; i < a.size(); ++i) {
      ASSERT_EQ(std::isnan(a[i]), std::isnan(b[i])) << mp << " cell " << i;
      if (!std::isnan(a[i])) {
        EXPECT_NEAR(a[i], b[i], 1e-9 * (1.0 + std::fabs(b[i]))) << mp << " cell " << i;
      }
    }
  }
}

// y = 1 + 2 x1 - 3 x2 + 0.5 x3 exactly: every residual ~ 0, slopes recovered.
TEST(AlphaLitOps_TsRegression, ExactLinearSeriesHasZeroResidualAndRecoveredSlopes) {
  std::mt19937_64 rng{0xB37AULL};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  constexpr atx::usize kD = 40;
  std::vector<std::vector<atx::f64>> cols(4, std::vector<atx::f64>(kD));
  for (atx::usize t = 0; t < kD; ++t) {
    cols[1][t] = nd(rng);
    cols[2][t] = nd(rng);
    cols[3][t] = nd(rng);
    cols[0][t] = 1.0 + 2.0 * cols[1][t] - 3.0 * cols[2][t] + 0.5 * cols[3][t];
  }
  const Panel p = series_panel({"y", "x1", "x2", "x3"}, cols);
  const std::vector<atx::f64> r = vm_values("ts_resid_on(y, x1, x2, x3, 20)", p);
  const std::vector<atx::f64> b1 = vm_values("ts_beta_on(y, x1, x2, x3, 20)", p);
  const std::vector<atx::f64> b2 = vm_values("ts_beta_on(y, x2, x1, x3, 20)", p);
  for (atx::usize t = 0; t < kD; ++t) {
    if (t + 1 < 20) {
      EXPECT_TRUE(std::isnan(r[t]) && std::isnan(b1[t])) << t;
      continue;
    }
    EXPECT_NEAR(r[t], 0.0, 1e-9) << t;
    EXPECT_NEAR(b1[t], 2.0, 1e-9) << t;
    EXPECT_NEAR(b2[t], -3.0, 1e-9) << t;
  }
  // One regressor: the slope equals the covariance ratio; a flat y is exactly 0.
  const std::vector<atx::f64> one = vm_values("ts_beta_on(x1, x2, 10)", p);
  EXPECT_TRUE(std::isfinite(one[kD - 1]));
  const Panel flat = series_panel({"y", "x"}, {std::vector<atx::f64>(12, 3.0),
                                                {1, 4, 2, 8, 5, 7, 3, 9, 6, 2, 5, 1}});
  EXPECT_EQ(vm_values("ts_resid_on(y, x, 6)", flat)[11], 0.0);
  EXPECT_EQ(vm_values("ts_beta_on(y, x, 6)", flat)[11], 0.0);
  EXPECT_TRUE(std::isnan(vm_values("ts_resid_on(x, y, 6)", flat)[11])); // flat regressor
}

// Collinear regressors (x2 = 2 x1) make the design singular -> NaN.
TEST(AlphaLitOps_TsRegression, CollinearDesignIsNaN) {
  const std::vector<atx::f64> x1{1, 4, 2, 8, 5, 7, 3, 9, 6, 2};
  std::vector<atx::f64> x2(x1.size());
  std::vector<atx::f64> y(x1.size());
  for (atx::usize t = 0; t < x1.size(); ++t) {
    x2[t] = 2.0 * x1[t];
    y[t] = static_cast<atx::f64>(t * t % 7);
  }
  const Panel p = series_panel({"y", "x1", "x2"}, {y, x1, x2});
  EXPECT_TRUE(std::isnan(vm_values("ts_resid_on(y, x1, x2, 8)", p)[9]));
}

TEST(AlphaLitOps_CsRegression, ResidualIsOrthogonalToEveryCovariate) {
  const Fixture f = make_fixture(12, 40, 0xC5C5ULL);
  const Panel panel = prefix_panel(f, f.dates);
  const std::vector<atx::f64> r = vm_values("cs_resid_on(returns, size, cop_at, droe, tobin_q)",
                                            panel);
  const std::array<atx::usize, 4> cov{9, 10, 11, 12}; // size, cop_at, droe, tobin_q
  for (atx::usize t = 0; t < f.dates; ++t) {
    atx::f64 s0 = 0.0;
    std::array<atx::f64, 4> sc{};
    atx::usize n = 0;
    for (atx::usize j = 0; j < f.inst; ++j) {
      const atx::f64 v = r[t * f.inst + j];
      if (std::isnan(v)) {
        continue;
      }
      ++n;
      s0 += v;
      for (atx::usize c = 0; c < cov.size(); ++c) {
        sc[c] += v * f.cols[cov[c]][t * f.inst + j];
      }
    }
    ASSERT_GE(n, 6U) << t;
    EXPECT_NEAR(s0, 0.0, 1e-12) << t;
    for (atx::usize c = 0; c < cov.size(); ++c) {
      EXPECT_NEAR(sc[c], 0.0, 1e-10) << t << " covariate " << c;
    }
  }
}

TEST(AlphaLitOps_Bucket, EqualsQuantileScaledAndTiesShareABucket) {
  for (const atx::usize inst : {atx::usize{13}, atx::usize{128}}) { // comparison + radix sorts
    const Fixture f = make_fixture(20, inst, 0xB0C4ULL + inst);
    const Panel panel = prefix_panel(f, f.dates);
    const std::vector<atx::f64> b = vm_values("bucket(size, 5)", panel);
    const std::vector<atx::f64> q = vm_values("quantile(size, 5)", panel);
    for (atx::usize i = 0; i < b.size(); ++i) {
      ASSERT_TRUE(same_cell(q[i], b[i] / 4.0)) << "cell " << i;
      if (!std::isnan(b[i])) {
        EXPECT_TRUE(b[i] >= 0.0 && b[i] <= 4.0 && b[i] == std::floor(b[i])) << b[i];
      }
    }
  }
  const Panel ties = [] {
    std::vector<std::vector<atx::f64>> cols{{1.0, 1.0, 1.0, 2.0, 3.0, kNaN}};
    auto p = Panel::create(1, 6, {"x"}, std::move(cols), {});
    EXPECT_TRUE(p.has_value());
    return p.value();
  }();
  const std::vector<atx::f64> tb = vm_values("bucket(x, 2)", ties);
  EXPECT_EQ(tb[0], tb[1]);
  EXPECT_EQ(tb[1], tb[2]);
  EXPECT_TRUE(std::isnan(tb[5])); // NaN -> no group
}

TEST(AlphaLitOps_GroupCross, ProductLabelsAndGroupOpsOverThem) {
  const Fixture f = make_fixture(30, 13, 0x6C05ULL);
  const Panel panel = prefix_panel(f, f.dates);
  const std::vector<atx::f64> g = vm_values("group_cross(IndClass.sector, grp_ff12)", panel);
  for (atx::usize t = 0; t < f.dates; ++t) {
    for (atx::usize j = 0; j < f.inst; ++j) {
      const atx::usize i = t * f.inst + j;
      const atx::f64 ff = f.cols[16][i];
      if (f.universe[i] == 0 || std::isnan(ff)) {
        EXPECT_TRUE(std::isnan(g[i])) << i;
        continue;
      }
      EXPECT_EQ(g[i], static_cast<atx::f64>(j % 3) * 67108864.0 + ff) << i;
    }
  }
  EXPECT_TRUE(std::isnan(alpha::detail::lit_group_cross(-1.0, 2.0)));
  EXPECT_TRUE(std::isnan(alpha::detail::lit_group_cross(1.0, 0.5)));
  EXPECT_TRUE(std::isnan(alpha::detail::lit_group_cross(67108864.0, 0.0)));
  EXPECT_EQ(alpha::detail::lit_group_cross(67108863.0, 67108863.0),
            67108863.0 * 67108864.0 + 67108863.0);
}

// ---- typecheck refusals --------------------------------------------------------

TEST(AlphaLitOps_Typecheck, GroupWhereVectorRequiredIsRefused) {
  for (const std::string_view src :
       {"bucket(IndClass.sector, 5)", "ts_topk_mean(IndClass.sector, 5, 2)",
        "ts_resid_on(close, IndClass.sector, 10)", "ts_resid_on(IndClass.sector, close, 10)",
        "ts_beta_on(close, open, grp_ff12, 10)", "cs_resid_on(close, grp_ff12)",
        "cs_resid_on(grp_ff12, close)", "ts_corr_mp(close, IndClass.sector, 10, 5)",
        "ts_mean_mp(IndClass.sector, 5, 2)", "ts_count_increases(grp_ff12, 5)",
        "pack2(close, IndClass.sector).p0", "rank(bucket(close, 5))",
        "bucket(close, 5) + 1", "ts_mean(bucket(close, 5), 5)",
        "ts_topk_mean(group_cross(IndClass.sector, grp_ff12), 5, 2)"}) {
    EXPECT_TRUE(refused(src)) << src;
  }
}

TEST(AlphaLitOps_Typecheck, VectorWhereGroupRequiredIsRefused) {
  for (const std::string_view src :
       {"group_cross(close, IndClass.sector)", "group_cross(IndClass.sector, rank(close))",
        "group_cross(bucket(close, 3), ts_mean(close, 5))", "group_rank(close, rank(close))",
        "group_neutralize(close, ts_topk_mean(close, 5, 2))",
        "group_mean(close, cs_resid_on(close, open))"}) {
    EXPECT_TRUE(refused(src)) << src;
  }
}

TEST(AlphaLitOps_Typecheck, RecordsAndCountRailsAreRefused) {
  for (const std::string_view src :
       {"pack2(close, open)", "rank(pack2(close, open))", "ts_mean(pack3(close, open, high), 5)",
        "ts_resid_on(close, pack2(close, pack2(open, high)), 10)",
        "ts_resid_on(close, open, high, low, volume, 10)",
        "cs_resid_on(close, pack3(open, high, low), pack2(volume, size))",
        "ts_corr_mp(close, pack2(open, high), 10, 5)", "ts_topk_mean(close, 5, 6)",
        "ts_topk_mean(close, 5, 0)", "ts_topk_mean(close, 5, 2.5)", "ts_mean_mp(close, 5, 6)",
        "ts_mean_mp(close, 5, 0)", "bucket(close, 1)", "bucket(close, 2.5)",
        "ts_resid_on(close, open, high, low, 4)", "ts_resid_on(close, open, volume)",
        "ts_topk_mean(close, volume, 2)", "group_cross(IndClass.sector)"}) {
    EXPECT_TRUE(refused(src)) << src;
  }
}

TEST(AlphaLitOps_Typecheck, WellTypedNestingsAreAcceptedWithLookback) {
  for (const std::string_view src :
       {"group_rank(close, bucket(close, 5))",
        "group_neutralize(close, group_cross(IndClass.sector, bucket(size, 3)))",
        "ts_resid_on(close, pack3(open, high, low), 10)", "cs_resid_on(close, open, high, low, volume)"}) {
    EXPECT_FALSE(refused(src)) << src;
  }
  const auto lookback = [](std::string_view src) -> int {
    auto ast = alpha::parse_expr(src, lib());
    if (!ast) {
      return -1;
    }
    auto ana = alpha::analyze(ast.value());
    return ana ? static_cast<int>(ana->required_lookback()) : -1;
  };
  EXPECT_EQ(lookback("ts_resid_on(close, open, high, 20)"), 19);
  EXPECT_EQ(lookback("ts_mean_mp(close, 20, 5)"), 19);
  EXPECT_EQ(lookback("ts_topk_mean(delay(close, 3), 10, 2)"), 12);
  EXPECT_EQ(lookback("bucket(ts_count_increases(close, 7), 3)"), 6);
  EXPECT_EQ(lookback("ts_corr_mp(close, delay(open, 4), 8, 3)"), 11);
  EXPECT_EQ(lookback("cs_resid_on(delay(close, 2), open, high, low, delay(volume, 5))"), 5);
}

// ---- parser / unparse / linearizer -------------------------------------------

TEST(AlphaLitOps_Parser, SurplusRegressorsFoldIntoPacksAndRoundTrip) {
  const std::vector<std::pair<std::string_view, std::string_view>> cases = {
      {"ts_resid_on(close, open, 10)", "ts_resid_on(close, open, 10)"},
      {"ts_resid_on(close, open, high, 10)", "ts_resid_on(close, pack2(open, high), 10)"},
      {"ts_beta_on(close, open, high, low, 10)",
       "ts_beta_on(close, pack3(open, high, low), 10)"},
      {"cs_resid_on(close, open, high)", "cs_resid_on(close, open, high)"},
      {"cs_resid_on(close, open, high, low)", "cs_resid_on(close, pack2(open, high), low)"},
      {"cs_resid_on(close, open, high, low, volume)",
       "cs_resid_on(close, pack3(open, high, low), volume)"}};
  for (const auto &[src, want] : cases) {
    auto ast = alpha::parse_expr(src, lib());
    ASSERT_TRUE(ast.has_value()) << src;
    const std::string once = alpha::unparse(ast.value());
    EXPECT_EQ(once, want) << src;
    auto again = alpha::parse_expr(once, lib());
    ASSERT_TRUE(again.has_value()) << once;
    EXPECT_EQ(alpha::unparse(again.value()), once);
    EXPECT_EQ(again->nodes().size(), ast->nodes().size()) << src;
  }
}

TEST(AlphaLitOps_Linearizer, PackConsumersCarryTheirBlockWidthsInParam) {
  const auto param_of = [](std::string_view expr, OpCode op) -> atx::u32 {
    const Program prog = compile_expr(expr);
    for (const alpha::Instr &in : prog.code) {
      if (in.op == op) {
        return in.param;
      }
    }
    ADD_FAILURE() << "no instruction for " << expr;
    return 0xFFFFFFFFU;
  };
  EXPECT_EQ(param_of("ts_resid_on(close, open, 10)", OpCode::TsResidOn),
            alpha::detail::lit_reg_param(1, 0));
  EXPECT_EQ(param_of("ts_resid_on(close, open, high, low, 10)", OpCode::TsResidOn),
            alpha::detail::lit_reg_param(3, 0));
  EXPECT_EQ(param_of("cs_resid_on(close, open)", OpCode::CsResidOn),
            alpha::detail::lit_reg_param(1, 0));
  EXPECT_EQ(param_of("cs_resid_on(close, open, high)", OpCode::CsResidOn),
            alpha::detail::lit_reg_param(1, 1));
  EXPECT_EQ(param_of("cs_resid_on(close, open, high, low, volume)", OpCode::CsResidOn),
            alpha::detail::lit_reg_param(3, 1));
  const Program pk = compile_expr("ts_resid_on(close, open, high, 10)");
  bool saw_pack = false;
  for (const alpha::Instr &in : pk.code) {
    if (in.op == OpCode::ArgPack) {
      saw_pack = true;
      EXPECT_EQ(static_cast<unsigned>(in.n_out), 2U);
    }
  }
  EXPECT_TRUE(saw_pack);
}

// ---- registry ------------------------------------------------------------------

TEST(AlphaLitOps_Registry, NewOpsResolveOutsideTheFactoryBuiltinTable) {
  const std::vector<std::pair<std::string_view, OpCode>> rows = {
      {"pack2", OpCode::ArgPack},
      {"pack3", OpCode::ArgPack},
      {"ts_topk_mean", OpCode::TsTopkMean},
      {"bucket", OpCode::CsBucket},
      {"group_cross", OpCode::GroupCross},
      {"ts_resid_on", OpCode::TsResidOn},
      {"ts_beta_on", OpCode::TsBetaOn},
      {"cs_resid_on", OpCode::CsResidOn},
      {"ts_count_increases", OpCode::TsCountIncreases},
      {"ts_sum_mp", OpCode::TsSumMp},
      {"ts_mean_mp", OpCode::TsMeanMp},
      {"ts_std_mp", OpCode::TsStdMp},
      {"ts_zscore_mp", OpCode::TsZscoreMp},
      {"ts_min_mp", OpCode::TsMinMp},
      {"ts_max_mp", OpCode::TsMaxMp},
      {"decay_linear_mp", OpCode::TsDecayLinearMp},
      {"ts_corr_mp", OpCode::TsCorrMp}};
  const std::span<const alpha::OpSig> builtins = alpha::detail::builtin_ops();
  EXPECT_EQ(builtins.size(), 74U); // the factory op-swap / wrapper table is unchanged
  for (const auto &[name, op] : rows) {
    const alpha::OpSig *sig = lib().find(name);
    ASSERT_NE(sig, nullptr) << name;
    EXPECT_EQ(sig->opcode, op) << name;
    EXPECT_TRUE(alpha::detail::is_lit_op(op)) << name;
    for (const alpha::OpSig &b : builtins) {
      EXPECT_NE(b.name, name) << name << " leaked into builtin_ops()";
    }
  }
  EXPECT_EQ(lib().find("bucket")->out_dtype, alpha::DType::Group);
  EXPECT_EQ(lib().find("group_cross")->out_dtype, alpha::DType::Group);
  EXPECT_EQ(static_cast<int>(OpCode::Free), 88);
  EXPECT_EQ(static_cast<int>(OpCode::ArgPack), 89);
  EXPECT_EQ(static_cast<int>(OpCode::TsTopkMean), 90);
  EXPECT_EQ(static_cast<int>(OpCode::CsBucket), 91);
  EXPECT_EQ(static_cast<int>(OpCode::GroupCross), 92);
  EXPECT_EQ(static_cast<int>(OpCode::TsResidOn), 93);
  EXPECT_EQ(static_cast<int>(OpCode::TsBetaOn), 94);
  EXPECT_EQ(static_cast<int>(OpCode::CsResidOn), 95);
  EXPECT_EQ(static_cast<int>(OpCode::TsCountIncreases), 96);
  EXPECT_EQ(static_cast<int>(OpCode::TsSumMp), 97);
  EXPECT_EQ(static_cast<int>(OpCode::TsCorrMp), 104);
}

// ---- the literature-family DSL strings of the W2 report -------------------------

[[nodiscard]] const std::vector<std::string_view> &family_dsls() {
  static const std::vector<std::string_view> k = {
      // MAX5-SMAX (Bali-Cakici-Whitelaw 2011 MAX5; Asness-Frazzini-Gormsen-Pedersen 2020 SMAX)
      "rank(-1 * ts_topk_mean(close / delay(close, 1) - 1, 21, 5) / stddev(close / delay(close, "
      "1) - 1, 21))",
      // BAC: low correlation to the market within volatility quintiles (AFGP 2020)
      "group_rank(-1 * correlation(close / delay(close, 1) - 1, ff_mkt, 252), "
      "bucket(stddev(close / delay(close, 1) - 1, 252), 5))",
      // FF3 residual momentum (Blitz-Huij-Martens 2011): t-12..t-2 residual sum / residual vol
      "rank(ts_sum(delay(ts_resid_on(close / delay(close, 1) - 1, ff_mkt, ff_smb, ff_hml, 252), "
      "21), 231) / stddev(delay(ts_resid_on(close / delay(close, 1) - 1, ff_mkt, ff_smb, ff_hml, "
      "252), 21), 231))",
      // nincr (Barth-Elliott-Finn 1999; Green-Hand-Zhang 2017): consecutive YoY report increases
      "rank((ts_count_increases((ni_q != delay(ni_q, 1)) ? ((ni_q > delay(ni_q, 252)) ? 1 : -1) "
      ": 0, 546) > 8) ? 8 : ts_count_increases((ni_q != delay(ni_q, 1)) ? ((ni_q > delay(ni_q, "
      "252)) ? 1 : -1) : 0, 546))",
      // q5 expected growth (Hou-Mo-Xue-Zhang 2021): FWL Fama-MacBeth slopes of realized 1y
      // investment growth on lagged log q / Cop / dROE, smoothed, applied to today's values
      "rank(ts_mean_mp(vec_sum(cs_resid_on(delay(log(tobin_q), 252) + 0 * (ia - delay(ia, 252)), "
      "delay(cop_at, 252), delay(droe, 252)) * (ia - delay(ia, 252))) / "
      "vec_sum(power(cs_resid_on(delay(log(tobin_q), 252) + 0 * (ia - delay(ia, 252)), "
      "delay(cop_at, 252), delay(droe, 252)), 2)), 252, 63) * log(tobin_q) + "
      "ts_mean_mp(vec_sum(cs_resid_on(delay(cop_at, 252) + 0 * (ia - delay(ia, 252)), "
      "delay(log(tobin_q), 252), delay(droe, 252)) * (ia - delay(ia, 252))) / "
      "vec_sum(power(cs_resid_on(delay(cop_at, 252) + 0 * (ia - delay(ia, 252)), "
      "delay(log(tobin_q), 252), delay(droe, 252)), 2)), 252, 63) * cop_at + "
      "ts_mean_mp(vec_sum(cs_resid_on(delay(droe, 252) + 0 * (ia - delay(ia, 252)), "
      "delay(log(tobin_q), 252), delay(cop_at, 252)) * (ia - delay(ia, 252))) / "
      "vec_sum(power(cs_resid_on(delay(droe, 252) + 0 * (ia - delay(ia, 252)), "
      "delay(log(tobin_q), 252), delay(cop_at, 252)), 2)), 252, 63) * droe)"};
  return k;
}

TEST(AlphaLitOps_Families, ReportDslStringsCompileAndMatchTheOracle) {
  const Fixture f = make_fixture(840, 6, 0xFA111E5ULL); // > the nincr lookback (797)
  const Panel panel = prefix_panel(f, f.dates);
  for (const std::string_view dsl : family_dsls()) {
    EXPECT_LE(dsl.size(), 4096U) << dsl; // the IC runner's DSL byte limit
    const Program prog = compile_expr(dsl);
    ASSERT_FALSE(prog.roots.empty()) << dsl;
    Engine eng{panel};
    auto vm = eng.evaluate(prog);
    ASSERT_TRUE(vm.has_value()) << vm.error().message();
    auto ref = alpha::evaluate_reference(prog, panel);
    ASSERT_TRUE(ref.has_value()) << ref.error().message();
    for (atx::usize i = 0; i < vm->alphas.front().values.size(); ++i) {
      ASSERT_TRUE(same_cell(vm->alphas.front().values[i], ref->alphas.front().values[i]))
          << dsl << " cell " << i;
    }
  }
}

} // namespace atx_test_alpha_lit_ops

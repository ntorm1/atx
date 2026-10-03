// Platform-v8 lane YOPS formulaic DSL ops: group_sum, group_delay and the as-of
// rank family (asof_rank_ts_rank / ts_min / decay_linear / correlation /
// covariance) — the constructs of Kakushadze's "101 Formulaic Alphas" the op
// catalogue lacked.
//
//   * Differential: every new op (and nestings with house ops) on a seed-fixed
//     panel with splits in the rebase factor, halted sessions, tied levels, NaN
//     labels, a reclassification and universe gaps — VM == oracle cell for cell,
//     the fused / subtree-cached / global-DAG paths == plain evaluate, and
//     StreamingEngine warm + step == batch bit for bit in both EvalModes.
//   * Analytic: closed-form group sums (group sizes 1 and all, NaN members, a
//     group with no valid member, NaN labels), group_delay's shifted labels, the
//     rebase re-ranking a past session, the halted-session factor borrow, lag and
//     warm-up NaNs, hand-computed as-of covariance / correlation.
//   * Identity: with a unit factor every as-of op IS the house composition
//     <outer>(rank(x), d) delayed by j, bit for bit — under the default policy,
//     the legacy policy and a Cs eligibility mask.
//   * Refusals and lookbacks; the registry / opcode-id freeze (the factory's
//     builtin and literature tables are unchanged); the ten frozen formulaic DSL
//     strings of task-YOPS-report.md (byte-pinned by SHA-256, VM == oracle).
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

#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/fusion.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streaming_engine.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/global_dag_eval.hpp"

namespace atx_test_alpha_formulaic_ops {

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

enum Col : atx::usize {
  kX,
  kY,
  kW,
  kOne,
  kVolume,
  kFlat,
  kGrpInd,
  kGrpOne,
  kGrpEach,
  kGrpNan,
  kClose,
  kRawClose,
  kOpenAdj,
  kHighAdj,
  kLowAdj,
  kGrpFf49,
};

[[nodiscard]] const std::vector<std::string> &field_names() {
  static const std::vector<std::string> k = {
      "x",     "y",        "w",        "one",      "volume",  "flat",
      "grp_ind", "grp_one", "grp_each", "grp_nan", "close",   "raw_close",
      "open_adj", "high_adj", "low_adj", "grp_ff49"};
  return k;
}

struct Fixture {
  atx::usize dates{0};
  atx::usize inst{0};
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  std::vector<std::uint8_t> universe;
};

// House-basis price walks with sessions without a bar, a per-line rebase factor
// w = raw_close / close that steps on splits (x2 / x0.5) and is NaN on those
// sessions, integer-level x (cross-sectional ties), a noise y, industry labels
// with a reclassification and NaN labels, size-1 / size-all / all-NaN groupings
// and a moving universe.
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
  std::vector<atx::f64> factor(inst, 1.0);
  for (atx::usize j = 0; j < inst; ++j) {
    factor[j] = std::exp(u(rng) - 0.5); // a per-line anchor
  }
  for (atx::usize t = 0; t < dates; ++t) {
    for (atx::usize j = 0; j < inst; ++j) {
      const atx::usize i = t * inst + j;
      const atx::f64 prev = px[j];
      px[j] = std::max(1.0, px[j] * (1.0 + 0.02 * nd(rng)));
      if (u(rng) < 0.03) {
        factor[j] *= (u(rng) < 0.5) ? 2.0 : 0.5; // a split moves the rebase factor
      }
      const bool hole = u(rng) < 0.02; // a session without a bar
      const atx::f64 o = prev * (1.0 + 0.005 * nd(rng));
      f.cols[kClose][i] = hole ? kNaN : px[j];
      f.cols[kRawClose][i] = hole ? kNaN : px[j] * factor[j];
      f.cols[kW][i] = hole ? kNaN : factor[j];
      f.cols[kOpenAdj][i] = hole ? kNaN : o;
      f.cols[kHighAdj][i] = hole ? kNaN : std::max(o, px[j]) * (1.0 + 0.01 * u(rng));
      f.cols[kLowAdj][i] = hole ? kNaN : std::min(o, px[j]) * (1.0 - 0.01 * u(rng));
      f.cols[kX][i] = (u(rng) < 0.03) ? kNaN : std::round(px[j] / 4.0);
      f.cols[kY][i] = (u(rng) < 0.03) ? kNaN : nd(rng);
      f.cols[kOne][i] = 1.0;
      f.cols[kVolume][i] = 1.0e5 * (0.5 + u(rng));
      f.cols[kFlat][i] = 5.0;
      const atx::usize ind = (t >= dates / 2 && j % 4 == 0) ? (j * 7 + 1) % 5 : (j * 7) % 5;
      f.cols[kGrpInd][i] = (j == 3 && t % 9 == 0) ? kNaN : static_cast<atx::f64>(ind);
      f.cols[kGrpFf49][i] = f.cols[kGrpInd][i];
      f.cols[kGrpOne][i] = 1.0;
      f.cols[kGrpEach][i] = static_cast<atx::f64>(j);
      f.cols[kGrpNan][i] = kNaN;
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

// A small hand-written panel: `cols[k]` is field `names[k]`, date-major.
[[nodiscard]] Panel small_panel(atx::usize dates, atx::usize inst,
                                const std::vector<std::string> &names,
                                std::vector<std::vector<atx::f64>> cols) {
  auto p = Panel::create(dates, inst, names, std::move(cols), {});
  EXPECT_TRUE(p.has_value());
  return p.value();
}

// The VM output of `expr` on `panel`, after checking it equals the oracle's.
[[nodiscard]] std::vector<atx::f64> checked_values(std::string_view expr, const Panel &panel) {
  const Program prog = compile_expr(expr);
  Engine eng{panel};
  auto vm = eng.evaluate(prog);
  EXPECT_TRUE(vm.has_value()) << expr << ": " << (vm ? "" : vm.error().message());
  auto ref = alpha::evaluate_reference(prog, panel);
  EXPECT_TRUE(ref.has_value()) << expr << ": " << (ref ? "" : ref.error().message());
  if (!vm || !ref) {
    return {};
  }
  const std::vector<atx::f64> &v = vm->alphas.front().values;
  const std::vector<atx::f64> &r = ref->alphas.front().values;
  EXPECT_EQ(v.size(), r.size()) << expr;
  for (atx::usize i = 0; i < v.size() && i < r.size(); ++i) {
    EXPECT_TRUE(same_cell(v[i], r[i])) << expr << " cell " << i << ": VM=" << v[i]
                                       << " oracle=" << r[i];
  }
  return v;
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
      "group_sum(x, grp_ind)",
      "group_sum(y, grp_ind)",
      "group_sum(x, grp_one)",
      "group_sum(x, grp_each)",
      "group_sum(x, grp_nan)",
      "group_sum(x * volume, group_delay(grp_ind, 3))",
      "group_delay(grp_ind, 1)",
      "indneutralize(y, group_delay(grp_ind, 4))",
      "asof_rank_ts_rank(x, w, 5, 0)",
      "asof_rank_ts_rank(x, w, 1, 0)",
      "asof_rank_ts_rank(y, w, 4, 3)",
      "asof_rank_ts_min(x, w, 2, 4)",
      "asof_rank_ts_min(close, w, 1, 7)",
      "asof_rank_decay_linear(delta(close, 3), w, 6, 0)",
      "asof_rank_decay_linear(x, w, 3, 2)",
      "asof_rank_correlation(close, w, rank(volume), 5, 0)",
      "asof_rank_correlation(x, w, y, 3, 2)",
      "asof_rank_covariance(close, w, rank(volume), 5, 1)",
      "asof_rank_covariance(x, one, y, 4, 0)",
      "asof_rank_correlation(x, w, y, 1, 0)",
      "asof_rank_correlation(close, w, flat, 4, 0)",
      "asof_rank_ts_rank(x, w, 6, 200)",
      "rank(asof_rank_covariance(high_adj, (raw_close / close), rank(volume), 5, 0)) + "
      "group_sum(x, grp_ind)",
  };
  return k;
}

[[nodiscard]] bool all_nan_by_design(std::string_view expr) noexcept {
  return expr == "group_sum(x, grp_nan)" ||                       // no labelled member
         expr == "asof_rank_correlation(x, w, y, 1, 0)" ||        // a 1-session correlation
         expr == "asof_rank_correlation(close, w, flat, 4, 0)" || // a flat second series
         expr == "asof_rank_ts_rank(x, w, 6, 200)";               // window beyond the panel
}

TEST(AlphaFormulaicOps_Differential, EveryNewOpVmMatchesOracleCellForCell) {
  const Fixture f = make_fixture(90, 13, 0x7095EEDULL);
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
    if (all_nan_by_design(expr)) {
      EXPECT_EQ(finite, 0U) << expr;
    } else {
      EXPECT_GT(finite, 0U) << expr << " produced no finite cell";
    }
  }
}

// One multi-root program: CSE of shared as-of nodes, lags 0 / 1 of the same op
// (distinct imm[1] — the subtree-cache key must keep them apart), group_sum over
// a delayed classifier.
[[nodiscard]] std::string_view shared_program() {
  return "a = asof_rank_correlation(close, w, rank(volume), 5, 0)\n"
         "b = asof_rank_correlation(close, w, rank(volume), 5, 1) - "
         "asof_rank_correlation(close, w, rank(volume), 5, 0)\n"
         "c = group_sum(x * volume, group_delay(grp_ind, 2)) / "
         "group_sum(volume + 0 * x, group_delay(grp_ind, 2))\n"
         "d = asof_rank_ts_min(x, w, 1, 3) + asof_rank_decay_linear(x, w, 4, 1)\n"
         "e = asof_rank_ts_rank(low_adj, (raw_close / close), 9, 0)\n"
         "g = asof_rank_covariance(high_adj, w, rank(volume), 5, 2)\n";
}

TEST(AlphaFormulaicOps_Differential, SharedProgramMatchesOracleAndEveryVmPath) {
  const Fixture f = make_fixture(80, 11, 0x5A4EDF0ULL);
  const Panel panel = prefix_panel(f, f.dates);
  const Program prog = compile_src(shared_program());
  ASSERT_EQ(prog.roots.size(), 6U);
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
  opt.min_chunk_cells = 16; // force many date / instrument chunks per node
  auto gdag = par::global_dag_evaluate(prog, panel, pool, nullptr, nullptr, opt);
  ASSERT_TRUE(gdag.has_value()) << gdag.error().message();
  expect_signals_equal(gdag.value(), plain.value(), "global-DAG");
}

void expect_stream_equals_batch(EvalMode mode) {
  constexpr atx::usize kDates = 70;
  constexpr atx::usize kInst = 9;
  constexpr atx::usize kStream = 22;
  const Fixture f = make_fixture(kDates, kInst, 0x57EA0F5ULL);
  std::string src{shared_program()};
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

TEST(AlphaFormulaicOps_Streaming, WarmThenStepMatchesBatchBitsAuditExact) {
  expect_stream_equals_batch(EvalMode::AuditExact);
}

TEST(AlphaFormulaicOps_Streaming, WarmThenStepMatchesBatchBitsResearchFast) {
  expect_stream_equals_batch(EvalMode::ResearchFast);
}

// The as-of family runs the batch per-cell kernels in every mode.
TEST(AlphaFormulaicOps_Modes, ResearchFastEqualsAuditExactForTheAsofFamily) {
  const Fixture f = make_fixture(60, 10, 0x40DE5ULL);
  const Panel panel = prefix_panel(f, f.dates);
  const Program prog = compile_src(shared_program());
  Engine audit{panel};
  auto a = audit.evaluate(prog);
  ASSERT_TRUE(a.has_value()) << a.error().message();
  Engine fast{panel};
  fast.set_eval_mode(EvalMode::ResearchFast);
  auto b = fast.evaluate(prog);
  ASSERT_TRUE(b.has_value()) << b.error().message();
  expect_signals_equal(b.value(), a.value(), "ResearchFast");
}

// ---- group_sum -------------------------------------------------------------------

TEST(AlphaFormulaicOps_GroupSum, ClosedFormSumsNaNMembersAndEmptyGroups) {
  // Date 0: groups {1: x 1, 2, NaN} {2: 4, 8}, a NaN label. Date 1: group 9 has no
  // valid member, group 7 sums 5 + 1, group 3 sums 1 + 1.
  const Panel p = small_panel(2, 6, {"x", "grp_g", "grp_one", "grp_each"},
                              {{1.0, 2.0, kNaN, 4.0, 8.0, 16.0, kNaN, kNaN, 5.0, 1.0, 1.0, 1.0},
                               {1.0, 1.0, 1.0, 2.0, 2.0, kNaN, 9.0, 9.0, 7.0, 7.0, 3.0, 3.0},
                               std::vector<atx::f64>(12, 1.0),
                               {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0}});
  const std::vector<atx::f64> s = checked_values("group_sum(x, grp_g)", p);
  const std::array<atx::f64, 12> want{3.0, 3.0, kNaN, 12.0, 12.0, kNaN,
                                      kNaN, kNaN, 6.0, 6.0, 2.0, 2.0};
  ASSERT_EQ(s.size(), want.size());
  for (atx::usize i = 0; i < want.size(); ++i) {
    EXPECT_TRUE(same_cell(s[i], want[i])) << "cell " << i << ": " << s[i];
  }
  // Group size 1: the value itself; group size all: the row total over valid cells.
  const std::vector<atx::f64> each = checked_values("group_sum(x, grp_each)", p);
  const std::vector<atx::f64> all = checked_values("group_sum(x, grp_one)", p);
  const std::array<atx::f64, 12> xs{1.0,  2.0,  kNaN, 4.0, 8.0, 16.0,
                                    kNaN, kNaN, 5.0,  1.0, 1.0, 1.0};
  const std::array<atx::f64, 12> want_all{31.0, 31.0, kNaN, 31.0, 31.0, 31.0,
                                          kNaN, kNaN, 8.0,  8.0,  8.0,  8.0};
  ASSERT_EQ(each.size(), xs.size());
  ASSERT_EQ(all.size(), xs.size());
  for (atx::usize i = 0; i < xs.size(); ++i) {
    EXPECT_TRUE(same_cell(each[i], xs[i])) << "size 1, cell " << i;
    EXPECT_TRUE(same_cell(all[i], want_all[i])) << "size all, cell " << i;
  }
  // group_count(x, g) (the existing arity-2 op) is the group size the sum runs over.
  const std::vector<atx::f64> n = checked_values("group_count(x, grp_g)", p);
  EXPECT_EQ(n[0], 2.0);
  EXPECT_EQ(n[3], 2.0);
  EXPECT_EQ(n[8], 2.0);
}

TEST(AlphaFormulaicOps_GroupSum, StressGroupSizesOneAndAllOverManyNames) {
  constexpr atx::usize kInst = 300;
  std::vector<std::vector<atx::f64>> cols(3, std::vector<atx::f64>(kInst));
  for (atx::usize j = 0; j < kInst; ++j) {
    cols[0][j] = static_cast<atx::f64>(j + 1); // integers: every sum is exact
    cols[1][j] = 1.0;
    cols[2][j] = static_cast<atx::f64>(j);
  }
  const Panel p = small_panel(1, kInst, {"x", "grp_one", "grp_each"}, cols);
  const std::vector<atx::f64> all = checked_values("group_sum(x, grp_one)", p);
  const std::vector<atx::f64> each = checked_values("group_sum(x, grp_each)", p);
  ASSERT_EQ(all.size(), kInst);
  for (atx::usize j = 0; j < kInst; ++j) {
    EXPECT_EQ(all[j], 45150.0) << j; // 300 * 301 / 2
    EXPECT_EQ(each[j], static_cast<atx::f64>(j + 1)) << j;
  }
}

TEST(AlphaFormulaicOps_GroupSum, EqualsGroupMeanTimesCountOnTheFixture) {
  const Fixture f = make_fixture(40, 17, 0x6505ULL);
  const Panel panel = prefix_panel(f, f.dates);
  const std::vector<atx::f64> s = checked_values("group_sum(y, grp_ind)", panel);
  const std::vector<atx::f64> mc =
      checked_values("group_mean(y, grp_ind) * group_count(y, grp_ind)", panel);
  ASSERT_EQ(s.size(), mc.size());
  atx::usize finite = 0;
  for (atx::usize i = 0; i < s.size(); ++i) {
    ASSERT_EQ(std::isnan(s[i]), std::isnan(mc[i])) << i;
    if (!std::isnan(s[i])) {
      EXPECT_NEAR(s[i], mc[i], 1e-12 * (1.0 + std::fabs(s[i]))) << i;
      ++finite;
    }
  }
  EXPECT_GT(finite, 0U);
}

// ---- group_delay ------------------------------------------------------------------

TEST(AlphaFormulaicOps_GroupDelay, ShiftsLabelsAndFeedsGroupOps) {
  // 5 dates x 4 names; name 0 is reclassified 1 -> 2 at date 2.
  const Panel p = small_panel(5, 4, {"x", "grp_g"},
                              {{1.0, 2.0, 3.0, 4.0, 1.0, 2.0, 3.0, 4.0, 1.0, 2.0, 3.0, 4.0,
                                1.0, 2.0, 3.0, 4.0, 10.0, 20.0, 30.0, 40.0},
                               {1.0, 1.0, kNaN, 3.0, 1.0, 1.0, 3.0, 3.0, 2.0, 1.0, 3.0, 3.0,
                                2.0, 1.0, 3.0, 3.0, 2.0, 1.0, 1.0, 3.0}});
  const std::vector<atx::f64> g = checked_values("group_delay(grp_g, 2)", p);
  ASSERT_EQ(g.size(), 20U);
  for (atx::usize i = 0; i < 8; ++i) {
    EXPECT_TRUE(std::isnan(g[i])) << i; // the first two dates have no label 2 sessions ago
  }
  const std::array<atx::f64, 12> want{1.0, 1.0, kNaN, 3.0, 1.0, 1.0, 3.0, 3.0,
                                      2.0, 1.0, 3.0, 3.0};
  for (atx::usize i = 0; i < want.size(); ++i) {
    EXPECT_TRUE(same_cell(g[8 + i], want[i])) << "cell " << 8 + i;
  }
  // Date 4 groups by date 2's labels {2, 1, 3, 3}: sums 10, 20, 70, 70.
  const std::vector<atx::f64> s = checked_values("group_sum(x, group_delay(grp_g, 2))", p);
  EXPECT_EQ(s[16], 10.0);
  EXPECT_EQ(s[17], 20.0);
  EXPECT_EQ(s[18], 70.0);
  EXPECT_EQ(s[19], 70.0);
  // With today's labels {2, 1, 1, 3} the sums differ: 10, 50, 50, 40.
  const std::vector<atx::f64> today = checked_values("group_sum(x, grp_g)", p);
  EXPECT_EQ(today[17], 50.0);
  EXPECT_EQ(today[19], 40.0);
}

// ---- the as-of rank family: analytic pins --------------------------------------------

// Names A, B, C, D; B's factor doubles at date 2 (a reverse split on the house
// basis: its as-of price is 16 from then on, for every past session) and B has no
// bar at date 3; D has no factor at all.
[[nodiscard]] Panel rebase_panel() {
  return small_panel(4, 4, {"x", "w"},
                     {{10.0, 8.0, 12.0, 9.0, 10.0, 8.0, 12.0, 9.0, 10.0, 8.0, 12.0, 9.0, 10.0,
                       kNaN, 12.0, 9.0},
                      {1.0, 1.0, 1.0, kNaN, 1.0, 1.0, 1.0, kNaN, 1.0, 2.0, 1.0, kNaN, 1.0,
                       kNaN, 1.0, kNaN}});
}

void expect_row(const std::vector<atx::f64> &v, atx::usize t, std::array<atx::f64, 4> want,
                std::string_view what) {
  ASSERT_GE(v.size(), (t + 1) * 4) << what;
  for (atx::usize j = 0; j < 4; ++j) {
    EXPECT_TRUE(same_cell(v[t * 4 + j], want[j]))
        << what << " t=" << t << " j=" << j << ": " << v[t * 4 + j] << " want " << want[j];
  }
}

TEST(AlphaFormulaicOps_AsofRank, RebaseReRanksEveryPastSession) {
  const Panel p = rebase_panel();
  const std::array<atx::f64, 4> nan4{kNaN, kNaN, kNaN, kNaN};
  // As of date 2, B's price is 16 in sessions 0..2: ranks A 0, B 1, C 0.5.
  const std::vector<atx::f64> m3 = checked_values("asof_rank_ts_min(x, w, 3, 0)", p);
  expect_row(m3, 0, nan4, "warm-up");
  expect_row(m3, 1, nan4, "warm-up");
  expect_row(m3, 2, {0.0, 1.0, 0.5, kNaN}, "ts_min d=3 t=2");
  // Date 3: B has no bar (its window holds a NaN rank); A and C rank among two.
  expect_row(m3, 3, {0.0, kNaN, 0.5, kNaN}, "ts_min d=3 t=3");
  // The point-in-time reading ranks B lowest in sessions 0 and 1.
  const std::vector<atx::f64> pit = checked_values("ts_min(rank(x * w), 3)", p);
  expect_row(pit, 2, {0.0, 0.0, 0.5, kNaN}, "point-in-time");
  // d = 1 is the as-of rank at lag j; date 3 borrows B's factor from date 2.
  const std::vector<atx::f64> l1 = checked_values("asof_rank_ts_min(x, w, 1, 1)", p);
  expect_row(l1, 0, nan4, "lag warm-up");
  expect_row(l1, 1, {0.5, 0.0, 1.0, kNaN}, "lag 1 as of date 1 (B unsplit)");
  expect_row(l1, 3, {0.0, 1.0, 0.5, kNaN}, "lag 1 as of date 3 (borrowed factor)");
  const std::vector<atx::f64> l3 = checked_values("asof_rank_ts_min(x, w, 1, 3)", p);
  expect_row(l3, 2, nan4, "lag 3 warm-up");
  expect_row(l3, 3, {0.0, 1.0, 0.5, kNaN}, "session 0 as of date 3");
  // decay_linear over sessions 1..2 as of date 3; ts_rank of a constant window.
  expect_row(checked_values("asof_rank_decay_linear(x, w, 2, 1)", p), 3,
             {0.0, 1.0, 0.5, kNaN}, "decay_linear");
  expect_row(checked_values("asof_rank_ts_rank(x, w, 3, 0)", p), 2, {0.5, 0.5, 0.5, kNaN},
             "ts_rank");
}

TEST(AlphaFormulaicOps_AsofRank, HandComputedCovarianceAndCorrelation) {
  // x: A 1,3,2  B 2,1,3  C 3,2,1. C's factor falls to 0.1 at date 2, so as of date 2
  // C ranks last in every session and A's ranks are [0.5, 1, 0.5].
  const std::vector<std::string> names{"x", "w", "one", "y"};
  const Panel p = small_panel(3, 3, names,
                              {{1.0, 2.0, 3.0, 3.0, 1.0, 2.0, 2.0, 3.0, 1.0},
                               {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.1},
                               std::vector<atx::f64>(9, 1.0),
                               {1.0, 5.0, 2.0, 2.0, kNaN, 2.0, 4.0, 1.0, 2.0}});
  const std::vector<atx::f64> cov = checked_values("asof_rank_covariance(x, w, y, 3, 0)", p);
  EXPECT_NEAR(cov[6], -1.0 / 12.0, 1e-15);
  EXPECT_TRUE(std::isnan(cov[7])); // B: a NaN in its y window
  EXPECT_EQ(cov[8], 0.0);          // C: a flat rank window has zero covariance
  const std::vector<atx::f64> unit = checked_values("asof_rank_covariance(x, one, y, 3, 0)", p);
  EXPECT_NEAR(unit[6], 0.25, 1e-15); // A's unadjusted ranks [0, 1, 0.5]
  const std::vector<atx::f64> corr = checked_values("asof_rank_correlation(x, w, y, 3, 0)", p);
  EXPECT_NEAR(corr[6], -1.0 / (2.0 * std::sqrt(7.0)), 1e-12);
  EXPECT_TRUE(std::isnan(corr[7]));
  EXPECT_TRUE(std::isnan(corr[8])); // flat ranks: correlation undefined
  for (atx::usize i = 0; i < 6; ++i) {
    EXPECT_TRUE(std::isnan(cov[i]) && std::isnan(corr[i])) << "warm-up cell " << i;
  }
}

// ---- identity: a unit factor reduces every as-of op to the house composition -------

[[nodiscard]] const std::vector<std::pair<std::string_view, std::string_view>> &unit_pairs() {
  static const std::vector<std::pair<std::string_view, std::string_view>> k = {
      {"asof_rank_ts_rank(close, one, 7, 0)", "ts_rank(rank(close), 7)"},
      {"asof_rank_ts_rank(x, one, 7, 3)", "delay(ts_rank(rank(x), 7), 3)"},
      {"asof_rank_ts_min(x, one, 4, 0)", "ts_min(rank(x), 4)"},
      {"asof_rank_ts_min(y, one, 1, 4)", "delay(rank(y), 4)"},
      {"asof_rank_decay_linear(y, one, 5, 0)", "decay_linear(rank(y), 5)"},
      {"asof_rank_decay_linear(x, one, 5, 2)", "delay(decay_linear(rank(x), 5), 2)"},
      {"asof_rank_correlation(close, one, rank(volume), 6, 0)",
       "correlation(rank(close), rank(volume), 6)"},
      {"asof_rank_correlation(x, one, y, 6, 2)", "delay(correlation(rank(x), y, 6), 2)"},
      {"asof_rank_covariance(x, one, y, 5, 0)", "covariance(rank(x), y, 5)"},
      {"asof_rank_covariance(close, one, rank(volume), 4, 1)",
       "delay(covariance(rank(close), rank(volume), 4), 1)"}};
  return k;
}

void expect_unit_identity(const Panel &panel, const alpha::KernelPolicy &policy,
                          const std::vector<std::uint8_t> &mask, std::string_view what) {
  for (const auto &[asof, house] : unit_pairs()) {
    const Program pa = compile_expr(asof);
    const Program ph = compile_expr(house);
    Engine ea{panel};
    Engine eh{panel};
    ea.set_kernel_policy(policy);
    eh.set_kernel_policy(policy);
    ASSERT_TRUE(ea.set_cross_section_mask(mask).has_value());
    ASSERT_TRUE(eh.set_cross_section_mask(mask).has_value());
    auto a = ea.evaluate(pa);
    auto h = eh.evaluate(ph);
    ASSERT_TRUE(a.has_value() && h.has_value()) << asof;
    const std::vector<atx::f64> &av = a->alphas.front().values;
    const std::vector<atx::f64> &hv = h->alphas.front().values;
    ASSERT_EQ(av.size(), hv.size());
    atx::usize finite = 0;
    for (atx::usize i = 0; i < av.size(); ++i) {
      ASSERT_TRUE(same_bits(av[i], hv[i]))
          << what << ": " << asof << " vs " << house << " cell " << i << ": " << av[i]
          << " vs " << hv[i];
      finite += std::isfinite(av[i]) ? 1U : 0U;
    }
    EXPECT_GT(finite, 0U) << what << ": " << asof;
  }
}

TEST(AlphaFormulaicOps_Identity, UnitFactorEqualsHouseCompositionBitForBit) {
  const Fixture f = make_fixture(70, 15, 0x1D3ULL);
  const Panel panel = prefix_panel(f, f.dates);
  expect_unit_identity(panel, alpha::KernelPolicy{}, {}, "default policy");
  expect_unit_identity(panel, alpha::KernelPolicy::legacy_v1(), {}, "legacy policy");
  std::vector<std::uint8_t> mask(f.dates * f.inst, 1);
  std::mt19937_64 rng{0xA5CULL};
  for (std::uint8_t &m : mask) {
    m = (rng() % 7 == 0) ? std::uint8_t{0} : std::uint8_t{1};
  }
  expect_unit_identity(panel, alpha::KernelPolicy{}, mask, "Cs eligibility mask");
}

// ---- typecheck: refusals and lookbacks ------------------------------------------------

TEST(AlphaFormulaicOps_Typecheck, IllTypedCallsAreRefused) {
  for (const std::string_view src :
       {"group_sum(close, close)", "group_sum(grp_ind, grp_ind)", "group_sum(close, 1)",
        "group_sum(close)", "group_delay(close, 2)", "group_delay(grp_ind, 0)",
        "group_delay(grp_ind, close)", "delay(grp_ind, 2)", "rank(group_delay(grp_ind, 2))",
        "group_delay(grp_ind, 2) + 1", "asof_rank_ts_rank(grp_ind, w, 5, 0)",
        "asof_rank_ts_rank(close, grp_ind, 5, 0)",
        "asof_rank_correlation(close, w, grp_ind, 5, 0)", "asof_rank_ts_rank(close, 1, 5, 0)",
        "asof_rank_ts_rank(1, w, 5, 0)", "asof_rank_ts_rank(close, w, 0, 0)",
        "asof_rank_ts_rank(close, w, 2.5, 0)", "asof_rank_ts_rank(close, w, 5, -1)",
        "asof_rank_ts_rank(close, w, 5, 1.5)", "asof_rank_ts_rank(close, w, close, 0)",
        "asof_rank_ts_rank(close, w, 5, close)", "asof_rank_ts_rank(close, w, 5)",
        "asof_rank_correlation(close, w, 5, 0)",
        "asof_rank_ts_min(close, w, rank(volume), 5, 0)",
        "asof_rank_ts_rank(close, w, 65536, 0)",
        "asof_rank_ts_rank(delay(close, 5), w, 65535, 1)",
        "asof_rank_ts_rank(close, pack2(w, one), 5, 0)"}) {
    EXPECT_TRUE(refused(src)) << src;
  }
}

TEST(AlphaFormulaicOps_Typecheck, WellTypedCallsCarryTheirLookback) {
  const auto lookback = [](std::string_view src) -> int {
    auto ast = alpha::parse_expr(src, lib());
    if (!ast) {
      return -1;
    }
    auto ana = alpha::analyze(ast.value());
    return ana ? static_cast<int>(ana->required_lookback()) : -1;
  };
  EXPECT_EQ(lookback("group_sum(close, grp_ind)"), 0);
  EXPECT_EQ(lookback("group_sum(delay(close, 3), group_delay(grp_ind, 5))"), 5);
  EXPECT_EQ(lookback("group_delay(grp_ind, 4)"), 4);
  EXPECT_EQ(lookback("asof_rank_ts_rank(close, w, 5, 0)"), 4);
  EXPECT_EQ(lookback("asof_rank_ts_min(close, w, 2, 4)"), 5);
  EXPECT_EQ(lookback("asof_rank_ts_min(close, w, 1, 0)"), 0);
  EXPECT_EQ(lookback("asof_rank_correlation(delay(close, 3), w, rank(volume), 10, 2)"), 14);
  EXPECT_EQ(lookback("asof_rank_ts_rank(close, ts_backfill(w, 5), 3, 1)"), 7);
  EXPECT_EQ(lookback("asof_rank_ts_rank(close, w, 65535, 0)"), 65534);
}

// ---- registry ---------------------------------------------------------------------------

TEST(AlphaFormulaicOps_Registry, NewOpsResolveOutsideTheFactoryTables) {
  struct Row {
    std::string_view name;
    OpCode op;
    alpha::DType dtype;
    atx::u8 arity;
    atx::u8 hparams;
  };
  const std::array<Row, 7> rows{{
      {"group_sum", OpCode::CsSumG, alpha::DType::F64, 2, 0},
      {"group_delay", OpCode::GroupDelay, alpha::DType::Group, 2, 0},
      {"asof_rank_ts_rank", OpCode::AsofRankTsRank, alpha::DType::F64, 4, 2},
      {"asof_rank_ts_min", OpCode::AsofRankTsMin, alpha::DType::F64, 4, 2},
      {"asof_rank_decay_linear", OpCode::AsofRankDecayLinear, alpha::DType::F64, 4, 2},
      {"asof_rank_correlation", OpCode::AsofRankCorr, alpha::DType::F64, 5, 2},
      {"asof_rank_covariance", OpCode::AsofRankCov, alpha::DType::F64, 5, 2},
  }};
  const std::span<const alpha::OpSig> builtins = alpha::detail::builtin_ops();
  const std::span<const alpha::OpSig> lit = alpha::detail::literature_ops();
  EXPECT_EQ(builtins.size(), 74U); // the factory op-swap / wrapper table is unchanged
  EXPECT_EQ(lit.size(), 17U);      // OpCatalogCfg::literature_ops' table is unchanged
  EXPECT_EQ(alpha::detail::formulaic_ops().size(), rows.size());
  for (const Row &r : rows) {
    const alpha::OpSig *sig = lib().find(r.name);
    ASSERT_NE(sig, nullptr) << r.name;
    EXPECT_EQ(sig->opcode, r.op) << r.name;
    EXPECT_EQ(sig->out_dtype, r.dtype) << r.name;
    EXPECT_EQ(sig->min_arity, r.arity) << r.name;
    EXPECT_EQ(sig->max_arity, r.arity) << r.name;
    EXPECT_EQ(sig->n_hparams, r.hparams) << r.name;
    EXPECT_TRUE(alpha::detail::is_formulaic_op(r.op)) << r.name;
    EXPECT_FALSE(alpha::detail::is_lit_op(r.op)) << r.name;
    for (const alpha::OpSig &b : builtins) {
      EXPECT_NE(b.name, r.name) << r.name << " leaked into builtin_ops()";
    }
    for (const alpha::OpSig &b : lit) {
      EXPECT_NE(b.name, r.name) << r.name << " leaked into literature_ops()";
    }
  }
  // group_count keeps its arity-2 registration (no group_count(g) overload).
  ASSERT_NE(lib().find("group_count"), nullptr);
  EXPECT_EQ(lib().find("group_count")->min_arity, atx::u8{2});
  EXPECT_EQ(lib().find("group_count")->max_arity, atx::u8{2});
  // Opcode ids: every earlier id is untouched, the new ones follow TsCorrMp.
  EXPECT_EQ(static_cast<int>(OpCode::Free), 88);
  EXPECT_EQ(static_cast<int>(OpCode::TsCorrMp), 104);
  EXPECT_EQ(static_cast<int>(OpCode::CsSumG), 105);
  EXPECT_EQ(static_cast<int>(OpCode::GroupDelay), 106);
  EXPECT_EQ(static_cast<int>(OpCode::AsofRankTsRank), 107);
  EXPECT_EQ(static_cast<int>(OpCode::AsofRankTsMin), 108);
  EXPECT_EQ(static_cast<int>(OpCode::AsofRankDecayLinear), 109);
  EXPECT_EQ(static_cast<int>(OpCode::AsofRankCorr), 110);
  EXPECT_EQ(static_cast<int>(OpCode::AsofRankCov), 111);
  // Range execution axes.
  EXPECT_EQ(Engine::chunk_axis(OpCode::CsSumG), alpha::ChunkAxis::Dates);
  EXPECT_EQ(Engine::chunk_axis(OpCode::AsofRankCorr), alpha::ChunkAxis::Dates);
  EXPECT_EQ(Engine::chunk_axis(OpCode::GroupDelay), alpha::ChunkAxis::Instruments);
}

// ---- the frozen formulaic DSL strings (task-YOPS-report.md section 4) -------------------

struct Frozen {
  int alpha;
  std::string_view sha256;
  std::string_view dsl;
};

// clang-format off
[[nodiscard]] const std::vector<Frozen> &frozen() {
  static const std::vector<Frozen> k = {
      {1,
       "cd6d1589a3756d3326ef8fffee724af2ae9c43b00dda1f3767702054edb748ee",
       "(rank((((ts_max(((((close / delay(close, 1)) - 1) < 0) ? -1 : close), 5) * (raw_close / "
       "close)) > ts_max(((((close / delay(close, 1)) - 1) < 0) ? stddev(((close / delay(close, "
       "1)) - 1), 20) : -1), 5)) ? ts_argmax(((((close / delay(close, 1)) - 1) < 0) ? -1 : close"
       "), 5) : ((ts_max(((((close / delay(close, 1)) - 1) < 0) ? stddev(((close / delay(close, "
       "1)) - 1), 20) : -1), 5) > (ts_max(((((close / delay(close, 1)) - 1) < 0) ? -1 : close), "
       "5) * (raw_close / close))) ? ts_argmax(((((close / delay(close, 1)) - 1) < 0) ? stddev(("
       "(close / delay(close, 1)) - 1), 20) : -1), 5) : min(ts_argmax(((((close / delay(close, 1"
       ")) - 1) < 0) ? stddev(((close / delay(close, 1)) - 1), 20) : -1), 5), ts_argmax(((((clos"
       "e / delay(close, 1)) - 1) < 0) ? -1 : close), 5))))) - 0.5)"},
      {3,
       "c8016b408c87f2c9add9b834968ef7f13e77ba040e5989402a6f89ae334f96d6",
       "(-1 * asof_rank_correlation(open_adj, (raw_close / close), rank(volume), 10, 0))"},
      {4,
       "1a4f1e40c0ee80ea68ba2e9d63a598907cfea0ab367a90f0f7c2d2aae7d1a151",
       "(-1 * asof_rank_ts_rank(low_adj, (raw_close / close), 9, 0))"},
      {13,
       "036fdbcf75b39e15169897c8f40831c72f7f15ef1339d40edef247095a1bec8f",
       "(-1 * rank(asof_rank_covariance(close, (raw_close / close), rank(volume), 5, 0)))"},
      {15,
       "74dc73d5e6f31136798ba6b1498343f06705dddda63cd073086c392f843001b8",
       "(-1 * ((rank(asof_rank_correlation(high_adj, (raw_close / close), rank(volume), 3, 2)) +"
       " rank(asof_rank_correlation(high_adj, (raw_close / close), rank(volume), 3, 1))) + rank("
       "asof_rank_correlation(high_adj, (raw_close / close), rank(volume), 3, 0))))"},
      {16,
       "10e331f2a68f806c5fa0408f6793b17f61d34e9d115354ca5af7ad05c827d687",
       "(-1 * rank(asof_rank_covariance(high_adj, (raw_close / close), rank(volume), 5, 0)))"},
      {29,
       "a3b9ef0a636e1e748de2915ab2491850d4aea4c4057f1bb058ad2630d0da4b6c",
       "(min(min(min(min(product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close "
       "- 1), 5)), (raw_close / close), 2, 4), 1))))), 1), product(rank(rank(scale(log(ts_sum(as"
       "of_rank_ts_min((-1 * delta((close - 1), 5)), (raw_close / close), 2, 3), 1))))), 1)), pr"
       "oduct(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), (raw_clo"
       "se / close), 2, 2), 1))))), 1)), product(rank(rank(scale(log(ts_sum(asof_rank_ts_min((-1"
       " * delta((close - 1), 5)), (raw_close / close), 2, 1), 1))))), 1)), product(rank(rank(sc"
       "ale(log(ts_sum(asof_rank_ts_min((-1 * delta((close - 1), 5)), (raw_close / close), 2, 0)"
       ", 1))))), 1)) + ts_rank(delay((-1 * ((close / delay(close, 1)) - 1)), 6), 5))"},
      {31,
       "d844365bb80a187e3f0eeeee86f43a9a6fe0b53316e4fc462a224b2eea46375b",
       "((rank(rank(rank((-1 * asof_rank_decay_linear(delta(close, 10), (raw_close / close), 10,"
       " 0))))) + rank((-1 * (delta(close, 3) * (raw_close / close))))) + sign(scale(correlation"
       "(ts_mean((raw_close * volume), 20), low_adj, 12))))"},
      {80,
       "307be4989b6f7e616e612b43f91971cbd3f7100d41f604054cb213fff3b402e3",
       "(-1 * power(rank(sign((indneutralize((((open_adj * 0.868128) + (high_adj * (1 - 0.868128"
       "))) * ts_backfill((raw_close / close), 5)), grp_ff49) - indneutralize((delay(((open_adj "
       "* 0.868128) + (high_adj * (1 - 0.868128))), 4) * ts_backfill((raw_close / close), 5)), g"
       "roup_delay(grp_ff49, 4))))), ts_rank(correlation(high_adj, ts_mean((raw_close * volume),"
       " 10), 5), 5)))"},
      {88,
       "0e4bd47e30e5acbf882519b680cf1353c27e1ed744c5aa24eb396df04a59da55",
       "min(rank((((((((((1 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 7) + asof_ran"
       "k_ts_min(low_adj, (raw_close / close), 1, 7)) - (asof_rank_ts_min(high_adj, (raw_close /"
       " close), 1, 7) + asof_rank_ts_min(close, (raw_close / close), 1, 7)))) + (2 * ((asof_ran"
       "k_ts_min(open_adj, (raw_close / close), 1, 6) + asof_rank_ts_min(low_adj, (raw_close / c"
       "lose), 1, 6)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 6) + asof_rank_ts_mi"
       "n(close, (raw_close / close), 1, 6))))) + (3 * ((asof_rank_ts_min(open_adj, (raw_close /"
       " close), 1, 5) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 5)) - (asof_rank_ts_m"
       "in(high_adj, (raw_close / close), 1, 5) + asof_rank_ts_min(close, (raw_close / close), 1"
       ", 5))))) + (4 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 4) + asof_rank_ts_m"
       "in(low_adj, (raw_close / close), 1, 4)) - (asof_rank_ts_min(high_adj, (raw_close / close"
       "), 1, 4) + asof_rank_ts_min(close, (raw_close / close), 1, 4))))) + (5 * ((asof_rank_ts_"
       "min(open_adj, (raw_close / close), 1, 3) + asof_rank_ts_min(low_adj, (raw_close / close)"
       ", 1, 3)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 3) + asof_rank_ts_min(clo"
       "se, (raw_close / close), 1, 3))))) + (6 * ((asof_rank_ts_min(open_adj, (raw_close / clos"
       "e), 1, 2) + asof_rank_ts_min(low_adj, (raw_close / close), 1, 2)) - (asof_rank_ts_min(hi"
       "gh_adj, (raw_close / close), 1, 2) + asof_rank_ts_min(close, (raw_close / close), 1, 2))"
       "))) + (7 * ((asof_rank_ts_min(open_adj, (raw_close / close), 1, 1) + asof_rank_ts_min(lo"
       "w_adj, (raw_close / close), 1, 1)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1,"
       " 1) + asof_rank_ts_min(close, (raw_close / close), 1, 1))))) + (8 * ((asof_rank_ts_min(o"
       "pen_adj, (raw_close / close), 1, 0) + asof_rank_ts_min(low_adj, (raw_close / close), 1, "
       "0)) - (asof_rank_ts_min(high_adj, (raw_close / close), 1, 0) + asof_rank_ts_min(close, ("
       "raw_close / close), 1, 0))))) / 36)), ts_rank(decay_linear(correlation(ts_rank(close, 8)"
       ", ts_rank(ts_mean((raw_close * volume), 60), 20), 8), 6), 2))"},
  };
  return k;
}
// clang-format on

TEST(AlphaFormulaicOps_Frozen, ReportStringsArePinnedCompileAndMatchTheOracle) {
  const Fixture f = make_fixture(130, 8, 0xF0E2E4ULL); // > #88's 91-bar lookback
  const Panel panel = prefix_panel(f, f.dates);
  ASSERT_EQ(frozen().size(), 10U);
  for (const Frozen &fz : frozen()) {
    EXPECT_LE(fz.dsl.size(), 4096U) << "#" << fz.alpha; // the IC runner's DSL byte limit
    auto sha = atx::core::sha256_hex(fz.dsl);
    ASSERT_TRUE(sha.has_value());
    EXPECT_EQ(sha.value(), fz.sha256) << "#" << fz.alpha << " drifted from its frozen bytes";
    const Program prog = compile_expr(fz.dsl);
    ASSERT_FALSE(prog.roots.empty()) << "#" << fz.alpha;
    Engine eng{panel};
    auto vm = eng.evaluate(prog);
    ASSERT_TRUE(vm.has_value()) << "#" << fz.alpha << ": " << vm.error().message();
    auto ref = alpha::evaluate_reference(prog, panel);
    ASSERT_TRUE(ref.has_value()) << "#" << fz.alpha << ": " << ref.error().message();
    atx::usize finite = 0;
    for (atx::usize i = 0; i < vm->alphas.front().values.size(); ++i) {
      const atx::f64 v = vm->alphas.front().values[i];
      ASSERT_TRUE(same_cell(v, ref->alphas.front().values[i])) << "#" << fz.alpha << " cell " << i;
      finite += std::isfinite(v) ? 1U : 0U;
    }
    EXPECT_GT(finite, 0U) << "#" << fz.alpha;
  }
}

} // namespace atx_test_alpha_formulaic_ops

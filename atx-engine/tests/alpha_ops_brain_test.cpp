// atx::engine::alpha — P3b-2 BRAIN-superset op battery.
//
// The 13 consultant-staple operators added in P3b-2:
//   * element-wise (P->P):       sigmoid, tanh
//   * cross-sectional (P->V):    normalize, winsorize, group_count, group_mean,
//                                group_scale
//   * time-series rolling (P->P): ts_zscore, ts_backfill, ts_av_diff,
//                                ts_quantile, ts_scale, ts_count_nans
//
// Two layers of evidence per op:
//   1. DIFFERENTIAL (the main gate): for every op, compile a tiny program using
//      it and evaluate with BOTH the fast VM (vm.hpp::Engine) and the reference
//      oracle (oracle.hpp::evaluate_reference) on a fixed synthetic panel with
//      delisted/NaN cells, then assert cell-by-cell bit-identity (NaN==NaN). The
//      VM kernels and the independent oracle kernels are written separately but
//      MUST agree to the bit — any divergence is a real reduction-order/policy
//      bug, not a tolerance question.
//   2. KNOWN-VALUE + BOUNDARY: hand-computed expectations on small panels
//      (activation values, demeaned means, winsorize clamp bound, the P3b-1
//      default winsorize(x) == winsorize(x,4), median/min-max windows, group
//      broadcasts), plus boundary panels (all-NaN, 1-instrument, single-member
//      group, d=1, d>history) and the ts_backfill truncation-invariance rail.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <limits>
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

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// A process-lifetime Library: the Ast borrows OpSig pointers from it, so it must
// outlive every parse result.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN or exactly value-equal (covers +-inf, +-0). The
// VM reproduces the oracle EXACTLY, so equality is the right bar.
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// ---------------------------------------------------------------------------
//  Pipeline helper — parse_program -> analyze -> compile, asserting each stage.
// ---------------------------------------------------------------------------
[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] SignalSet eval_fast(const Program &prog, const Panel &panel) {
  Engine engine{panel};
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  return out.value_or(SignalSet{});
}

[[nodiscard]] SignalSet eval_oracle(const Program &prog, const Panel &panel) {
  auto out = evaluate_reference(prog, panel);
  EXPECT_TRUE(out.has_value()) << "oracle: " << (out ? "" : out.error().message());
  return out.value_or(SignalSet{});
}

// Build a Panel directly from columns (date-major). `names`/`cols` parallel; the
// universe defaults to all-in unless supplied.
[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments,
                               const std::vector<std::string> &names,
                               const std::vector<std::vector<atx::f64>> &cols,
                               std::vector<std::uint8_t> universe = {}) {
  if (universe.empty()) {
    universe.assign(dates * instruments, std::uint8_t{1});
  }
  auto p = Panel::create(dates, instruments, names, cols, universe);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// ===========================================================================
//  Synthetic differential panel — date x instrument block with delisted + NaN
//  cells, plus an IndClass.sector group classifier. Deterministic (no RNG):
//  values are a smooth analytic function so windows/cross-sections are varied.
// ===========================================================================

struct DiffPanel {
  atx::usize dates{};
  atx::usize instruments{};
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  std::vector<std::uint8_t> universe;
};

[[nodiscard]] DiffPanel make_diff_panel(atx::usize dates, atx::usize instruments,
                                        int num_sectors = 3) {
  const atx::usize cells = dates * instruments;
  DiffPanel pd;
  pd.dates = dates;
  pd.instruments = instruments;
  pd.names = {"close", "volume", "IndClass.sector"};
  pd.cols.assign(pd.names.size(), std::vector<atx::f64>(cells));
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      const atx::usize i = d * instruments + j;
      const auto df = static_cast<atx::f64>(d);
      const auto jf = static_cast<atx::f64>(j);
      pd.cols[0][i] = 50.0 + 10.0 * std::sin(0.3 * df + 0.7 * jf) + 0.5 * df; // close
      pd.cols[1][i] = 1.0e4 + 500.0 * jf + 200.0 * df;                        // volume
      pd.cols[2][i] = static_cast<atx::f64>((j + 2 * d) % static_cast<atx::usize>(num_sectors));
    }
  }
  // All-in-universe, then delist a couple of instruments mid-sample.
  pd.universe.assign(cells, std::uint8_t{1});
  if (instruments >= 3 && dates >= 4) {
    const atx::usize delist_date = dates * 2 / 3;
    for (atx::usize d = delist_date; d < dates; ++d) {
      pd.universe[d * instruments + 1] = 0;
      pd.universe[d * instruments + (instruments - 1)] = 0;
    }
  }
  // Scatter NaN gaps into close + volume (the rolling/cross-sectional NaN paths).
  if (cells >= 8) {
    pd.cols[0][cells / 5] = kNaN;
    pd.cols[1][cells / 3] = kNaN;
    pd.cols[0][cells * 4 / 5] = kNaN;
  }
  return pd;
}

[[nodiscard]] Panel panel_from(const DiffPanel &pd) {
  return make_panel(pd.dates, pd.instruments, pd.names, pd.cols, pd.universe);
}

// Run one alpha source through BOTH engines on the differential panel and assert
// cell-by-cell bit-identity. Returns the fast result for further inspection
// (callers may ignore it — not [[nodiscard]], since most only want the assert).
SignalSet assert_differential(std::string_view src, const DiffPanel &pd) {
  const Panel panel = panel_from(pd);
  const Program prog = compile_ok(src);
  const SignalSet fast = eval_fast(prog, panel);
  const SignalSet oracle = eval_oracle(prog, panel);
  EXPECT_EQ(fast.alphas.size(), oracle.alphas.size());
  atx::usize divergences = 0;
  for (atx::usize a = 0; a < fast.alphas.size(); ++a) {
    EXPECT_EQ(fast.alphas[a].values.size(), oracle.alphas[a].values.size());
    for (atx::usize i = 0; i < fast.alphas[a].values.size(); ++i) {
      const atx::f64 fc = fast.alphas[a].values[i];
      const atx::f64 oc = oracle.alphas[a].values[i];
      if (!same_cell(fc, oc)) {
        ++divergences;
        EXPECT_TRUE(same_cell(fc, oc)) << "alpha '" << fast.alphas[a].name << "' cell " << i
                                       << ": FAST=" << fc << " ORACLE=" << oc;
      }
    }
  }
  EXPECT_EQ(divergences, 0U) << "FAST==ORACLE differential diverged for src: " << src;
  return fast;
}

// ===========================================================================
//  1. Differential gate — every new op, FAST == ORACLE, bit-identical.
// ===========================================================================

TEST(AlphaBrain_Differential, AllThirteenOps_FastEqualsOracle_BitIdentical) {
  const DiffPanel pd = make_diff_panel(18, 9);
  // One alpha per new op (windows < dates so the rolling reach is real).
  assert_differential("a = sigmoid(close - 50)\n", pd);
  assert_differential("a = tanh(close - 50)\n", pd);
  assert_differential("a = normalize(close)\n", pd);
  assert_differential("a = winsorize(close, 2)\n", pd);
  assert_differential("a = winsorize(close)\n", pd); // P3b-1 default std=4
  assert_differential("a = group_count(close, IndClass.sector)\n", pd);
  assert_differential("a = group_mean(close, IndClass.sector)\n", pd);
  assert_differential("a = group_scale(close - 50, IndClass.sector)\n", pd);
  assert_differential("a = ts_zscore(close, 5)\n", pd);
  assert_differential("a = ts_backfill(close, 4)\n", pd);
  assert_differential("a = ts_av_diff(close, 6)\n", pd);
  assert_differential("a = ts_quantile(close, 5)\n", pd);
  assert_differential("a = ts_scale(close, 5)\n", pd);
  assert_differential("a = ts_count_nans(close, 4)\n", pd);
}

TEST(AlphaBrain_Differential, NestedAndCompositePrograms_FastEqualsOracle) {
  const DiffPanel pd = make_diff_panel(20, 10);
  // Nesting across families: rolling -> cross-sectional -> element-wise, and a
  // group aggregate fed by a rolling op.
  assert_differential("a = sigmoid(ts_zscore(close, 5))\n"
                      "b = normalize(ts_av_diff(close, 4))\n"
                      "c = tanh(group_scale(close - group_mean(close, IndClass.sector), "
                      "IndClass.sector))\n"
                      "e = winsorize(ts_scale(volume, 6), 3) + ts_quantile(close, 5)\n"
                      "f = ts_backfill(close, 3) - group_count(close, IndClass.sector)\n",
                      pd);
}

// ===========================================================================
//  2. Known-value — element-wise activations.
// ===========================================================================

TEST(AlphaBrain_Sigmoid, KnownInputs_MatchClosedForm) {
  // close column chosen so close-50 hits {0, large+, large-}.
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{50.0, 90.0, 10.0, kNaN}};
  const Panel panel = make_panel(1, 4, names, cols);
  const Program prog = compile_ok("a = sigmoid(close - 50)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 0.5);   // sigmoid(0)
  EXPECT_GT(v[1], 0.999);        // sigmoid(+40) ~ 1
  EXPECT_LT(v[2], 0.0001);       // sigmoid(-40) ~ 0
  EXPECT_TRUE(std::isnan(v[3])); // NaN -> NaN
}

TEST(AlphaBrain_Tanh, KnownInputs_MatchClosedForm) {
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{50.0, 60.0, kNaN}};
  const Panel panel = make_panel(1, 3, names, cols);
  const Program prog = compile_ok("a = tanh(close - 50)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 0.0);   // tanh(0)
  EXPECT_GT(v[1], 0.999);        // tanh(10) ~ 1
  EXPECT_TRUE(std::isnan(v[2])); // NaN -> NaN
}

// ===========================================================================
//  3. Known-value — cross-sectional.
// ===========================================================================

TEST(AlphaBrain_Normalize, ValidSetMeanIsZero) {
  // One date, 5 instruments; normalize ⇒ the valid-set mean is ~0.
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{1.0, 2.0, 3.0, 4.0, 5.0}};
  const Panel panel = make_panel(1, 5, names, cols);
  const Program prog = compile_ok("a = normalize(close)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  atx::f64 sum = 0.0;
  for (const atx::f64 c : v) {
    sum += c;
  }
  EXPECT_NEAR(sum, 0.0, 1e-12);
  EXPECT_DOUBLE_EQ(v[0], 1.0 - 3.0); // x - mean(=3)
}

TEST(AlphaBrain_Winsorize, NoValidCellBeyondBound_AndDefaultIsFour) {
  // One date, 7 instruments with an outlier. winsorize(x,1) must clamp every
  // cell into [mean - σ, mean + σ]; winsorize(x) ≡ winsorize(x,4) (the default).
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 100.0}};
  const Panel panel = make_panel(1, 7, names, cols);

  const Program p1 = compile_ok("a = winsorize(close, 1)\n");
  const SignalSet r1 = eval_fast(p1, panel);
  const std::vector<atx::f64> &v = r1.alphas.at(0).values;

  // Recompute the valid-set mean/sample-std to derive the bound.
  const atx::f64 mean = (10 + 11 + 12 + 13 + 14 + 15 + 100) / 7.0;
  atx::f64 ss = 0.0;
  for (const atx::f64 c : cols[0]) {
    ss += (c - mean) * (c - mean);
  }
  const atx::f64 sd = std::sqrt(ss / 6.0);
  const atx::f64 hi = mean + 1.0 * sd;
  const atx::f64 lo = mean - 1.0 * sd;
  for (const atx::f64 c : v) {
    EXPECT_LE(c, hi + 1e-9);
    EXPECT_GE(c, lo - 1e-9);
  }
  EXPECT_DOUBLE_EQ(v[6], hi); // the outlier clamps to the upper bound

  // winsorize(x) default-fills std=4 (P3b-1) ⇒ identical to winsorize(x,4).
  const SignalSet rdef = eval_fast(compile_ok("a = winsorize(close)\n"), panel);
  const SignalSet r4 = eval_fast(compile_ok("a = winsorize(close, 4)\n"), panel);
  for (atx::usize i = 0; i < 7; ++i) {
    EXPECT_TRUE(same_cell(rdef.alphas[0].values[i], r4.alphas[0].values[i]));
  }
}

TEST(AlphaBrain_Group, CountMeanScale_PerGroupBroadcast) {
  // 1 date, 6 instruments, 2 sectors {0,1,0,1,0,1}. close = {2,10,4,20,6,30}.
  const std::vector<std::string> names{"close", "IndClass.sector"};
  const std::vector<std::vector<atx::f64>> cols{{2.0, 10.0, 4.0, 20.0, 6.0, 30.0},
                                                {0.0, 1.0, 0.0, 1.0, 0.0, 1.0}};
  const Panel panel = make_panel(1, 6, names, cols);

  // group_count: 3 members in each sector.
  const SignalSet cnt = eval_fast(compile_ok("a = group_count(close, IndClass.sector)\n"), panel);
  for (atx::usize i = 0; i < 6; ++i) {
    EXPECT_DOUBLE_EQ(cnt.alphas[0].values[i], 3.0);
  }
  // group_mean: sector 0 = (2+4+6)/3 = 4; sector 1 = (10+20+30)/3 = 20.
  const SignalSet mean = eval_fast(compile_ok("a = group_mean(close, IndClass.sector)\n"), panel);
  EXPECT_DOUBLE_EQ(mean.alphas[0].values[0], 4.0);
  EXPECT_DOUBLE_EQ(mean.alphas[0].values[1], 20.0);
  EXPECT_DOUBLE_EQ(mean.alphas[0].values[4], 4.0);
  // group_scale: Σ|x| within each group == 1.
  const SignalSet scl = eval_fast(compile_ok("a = group_scale(close, IndClass.sector)\n"), panel);
  atx::f64 l1_s0 = 0.0;
  atx::f64 l1_s1 = 0.0;
  for (atx::usize i = 0; i < 6; ++i) {
    if (i % 2 == 0) {
      l1_s0 += std::fabs(scl.alphas[0].values[i]);
    } else {
      l1_s1 += std::fabs(scl.alphas[0].values[i]);
    }
  }
  EXPECT_NEAR(l1_s0, 1.0, 1e-12);
  EXPECT_NEAR(l1_s1, 1.0, 1e-12);
}

// ===========================================================================
//  4. Known-value — time-series rolling.
// ===========================================================================

TEST(AlphaBrain_TsZscore, LinearRamp_MatchesHandMeanStd) {
  // 1 instrument, close = 0,1,2,3,4. ts_zscore(.,3) at t=4: window {2,3,4}.
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{0.0, 1.0, 2.0, 3.0, 4.0}};
  const Panel panel = make_panel(5, 1, names, cols);
  const SignalSet r = eval_fast(compile_ok("a = ts_zscore(close, 3)\n"), panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_TRUE(std::isnan(v[0])); // window not full
  EXPECT_TRUE(std::isnan(v[1]));
  // window {2,3,4}: mean 3, sample sd 1 -> z(4) = (4-3)/1 = 1.
  EXPECT_DOUBLE_EQ(v[4], 1.0);
  EXPECT_DOUBLE_EQ(v[2], 1.0); // window {0,1,2}: mean 1, sd 1 -> z(2) = (2-1)/1
}

TEST(AlphaBrain_TsBackfill, FillsGapFromLastValid_PastOnly) {
  // 1 instrument: 5, NaN, NaN, 9. backfill(.,3) looks past NaNs to last valid.
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{5.0, kNaN, kNaN, 9.0}};
  const Panel panel = make_panel(4, 1, names, cols);
  const SignalSet r = eval_fast(compile_ok("a = ts_backfill(close, 3)\n"), panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 5.0); // itself
  EXPECT_DOUBLE_EQ(v[1], 5.0); // last valid within [t-2,t] = 5
  EXPECT_DOUBLE_EQ(v[2], 5.0); // window {0,1,2} -> 5 (the only valid)
  EXPECT_DOUBLE_EQ(v[3], 9.0); // current is valid
}

TEST(AlphaBrain_TsBackfill, TruncationInvariant_PastOnly) {
  // Truncating dates > t must leave outputs <= t identical (no look-ahead).
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{5.0, kNaN, 7.0, kNaN, 9.0, 11.0}};
  const Panel full = make_panel(6, 1, names, cols);
  const Program prog = compile_ok("a = ts_backfill(close, 4)\n");
  const SignalSet full_res = eval_fast(prog, full);

  const atx::usize t = 3; // cut after date 3
  const std::vector<std::vector<atx::f64>> tcols{{cols[0].begin(), cols[0].begin() + t + 1}};
  const Panel trunc = make_panel(t + 1, 1, names, tcols);
  const SignalSet trunc_res = eval_fast(prog, trunc);
  for (atx::usize d = 0; d <= t; ++d) {
    EXPECT_TRUE(same_cell(full_res.alphas[0].values[d], trunc_res.alphas[0].values[d]))
        << "look-ahead at date " << d;
  }
}

TEST(AlphaBrain_TsQuantile, KnownWindow_IsMedian) {
  // 1 instrument: 4,1,3,2,5. quantile(.,3) at t=2: median{4,1,3}=3.
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{4.0, 1.0, 3.0, 2.0, 5.0}};
  const Panel panel = make_panel(5, 1, names, cols);
  const SignalSet r = eval_fast(compile_ok("a = ts_quantile(close, 3)\n"), panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[2], 3.0); // median{4,1,3}
  EXPECT_DOUBLE_EQ(v[3], 2.0); // median{1,3,2}
  EXPECT_DOUBLE_EQ(v[4], 3.0); // median{3,2,5}
}

TEST(AlphaBrain_TsScale, KnownWindow_InUnitInterval) {
  // 1 instrument: 2,4,6,8. scale(.,3) at t=2: (6-2)/(6-2)=1; t=3:(8-4)/(8-4)=1.
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{2.0, 4.0, 6.0, 8.0}};
  const Panel panel = make_panel(4, 1, names, cols);
  const SignalSet r = eval_fast(compile_ok("a = ts_scale(close, 3)\n"), panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[2], 1.0);
  EXPECT_DOUBLE_EQ(v[3], 1.0);
  for (atx::usize d = 2; d < 4; ++d) {
    EXPECT_GE(v[d], 0.0);
    EXPECT_LE(v[d], 1.0);
  }
}

TEST(AlphaBrain_TsScale, FlatWindow_IsZero) {
  // Flat window (max == min) -> 0 (no /0).
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{7.0, 7.0, 7.0}};
  const Panel panel = make_panel(3, 1, names, cols);
  const SignalSet r = eval_fast(compile_ok("a = ts_scale(close, 3)\n"), panel);
  EXPECT_DOUBLE_EQ(r.alphas.at(0).values[2], 0.0);
}

TEST(AlphaBrain_TsAvDiff, DeviationFromTrailingMean) {
  // 1 instrument: 0,2,4,6. av_diff(.,2) at t=3: 6 - mean{4,6}=6-5=1.
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{0.0, 2.0, 4.0, 6.0}};
  const Panel panel = make_panel(4, 1, names, cols);
  const SignalSet r = eval_fast(compile_ok("a = ts_av_diff(close, 2)\n"), panel);
  EXPECT_DOUBLE_EQ(r.alphas.at(0).values[3], 1.0);
  EXPECT_DOUBLE_EQ(r.alphas.at(0).values[1], 1.0); // 2 - mean{0,2}=1
}

TEST(AlphaBrain_TsCountNans, CountsKnownGap_FullWindowOnly) {
  // 1 instrument: 1, NaN, 3, NaN, NaN. count_nans(.,3).
  const std::vector<std::string> names{"close"};
  const std::vector<std::vector<atx::f64>> cols{{1.0, kNaN, 3.0, kNaN, kNaN}};
  const Panel panel = make_panel(5, 1, names, cols);
  const SignalSet r = eval_fast(compile_ok("a = ts_count_nans(close, 3)\n"), panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_TRUE(std::isnan(v[0])); // incomplete window -> NaN
  EXPECT_TRUE(std::isnan(v[1]));
  EXPECT_DOUBLE_EQ(v[2], 1.0); // window {1,NaN,3} -> 1 NaN
  EXPECT_DOUBLE_EQ(v[3], 2.0); // window {NaN,3,NaN} -> 2 NaN
  EXPECT_DOUBLE_EQ(v[4], 2.0); // window {3,NaN,NaN} -> 2 NaN
}

// ===========================================================================
//  5. Boundaries — all-NaN, 1-instrument, single-member group, d=1, d>history.
//  Each asserted via the differential (FAST==ORACLE) on a tailored panel.
// ===========================================================================

TEST(AlphaBrain_Boundary, AllNaNRowAndWindow_FastEqualsOracle) {
  const std::vector<std::string> names{"close", "IndClass.sector"};
  const std::vector<std::vector<atx::f64>> cols{{kNaN, kNaN, kNaN, kNaN}, {0.0, 0.0, 1.0, 1.0}};
  DiffPanel pd;
  pd.dates = 2;
  pd.instruments = 2;
  pd.names = names;
  pd.cols = cols;
  pd.universe.assign(4, std::uint8_t{1});
  assert_differential("a = normalize(close)\n", pd);
  assert_differential("a = winsorize(close, 2)\n", pd);
  assert_differential("a = ts_zscore(close, 2)\n", pd);
  assert_differential("a = ts_backfill(close, 2)\n", pd);
  assert_differential("a = ts_count_nans(close, 2)\n", pd);
  assert_differential("a = group_mean(close, IndClass.sector)\n", pd);
}

TEST(AlphaBrain_Boundary, OneInstrumentUniverse_FastEqualsOracle) {
  const DiffPanel pd = make_diff_panel(8, 1, /*num_sectors=*/1);
  assert_differential("a = normalize(close)\n", pd);
  assert_differential("a = winsorize(close, 3)\n", pd);
  assert_differential("a = group_scale(close, IndClass.sector)\n", pd);
  assert_differential("a = ts_zscore(close, 3)\n", pd);
  assert_differential("a = ts_scale(close, 3)\n", pd);
}

TEST(AlphaBrain_Boundary, SingleMemberGroups_FastEqualsOracle) {
  // Each instrument its own sector -> group ops degenerate to single members.
  const std::vector<std::string> names{"close", "IndClass.sector"};
  const std::vector<std::vector<atx::f64>> cols{{3.0, -5.0, 7.0, 2.0, -1.0, 4.0},
                                                {0.0, 1.0, 2.0, 0.0, 1.0, 2.0}};
  DiffPanel pd;
  pd.dates = 2;
  pd.instruments = 3;
  pd.names = names;
  pd.cols = cols;
  pd.universe.assign(6, std::uint8_t{1});
  // Differential covers the bit-match; also assert the singleton semantics.
  const SignalSet cnt = assert_differential("a = group_count(close, IndClass.sector)\n", pd);
  for (const atx::f64 c : cnt.alphas[0].values) {
    EXPECT_DOUBLE_EQ(c, 1.0); // every group has exactly one member
  }
  const SignalSet scl = assert_differential("a = group_scale(close, IndClass.sector)\n", pd);
  for (const atx::f64 c : scl.alphas[0].values) {
    EXPECT_DOUBLE_EQ(std::fabs(c), 1.0); // single-member L1 normalize -> ±1
  }
}

TEST(AlphaBrain_Boundary, WindowOne_FastEqualsOracle) {
  const DiffPanel pd = make_diff_panel(6, 4);
  assert_differential("a = ts_zscore(close, 1)\n", pd);     // d<2 -> NaN (sd undefined)
  assert_differential("a = ts_quantile(close, 1)\n", pd);   // median of singleton
  assert_differential("a = ts_scale(close, 1)\n", pd);      // flat -> 0
  assert_differential("a = ts_av_diff(close, 1)\n", pd);    // x - x = 0
  assert_differential("a = ts_backfill(close, 1)\n", pd);   // itself or NaN
  assert_differential("a = ts_count_nans(close, 1)\n", pd); // 0/1 per cell
}

TEST(AlphaBrain_Boundary, WindowExceedsHistory_FastEqualsOracle) {
  // d (=10) > dates (=5): no full window ever exists -> all NaN (except backfill
  // which still finds valid cells, and count_nans which stays NaN until full).
  const DiffPanel pd = make_diff_panel(5, 3);
  assert_differential("a = ts_zscore(close, 10)\n", pd);
  assert_differential("a = ts_av_diff(close, 10)\n", pd);
  assert_differential("a = ts_quantile(close, 10)\n", pd);
  assert_differential("a = ts_scale(close, 10)\n", pd);
  assert_differential("a = ts_backfill(close, 10)\n", pd);
  assert_differential("a = ts_count_nans(close, 10)\n", pd);
}

} // namespace

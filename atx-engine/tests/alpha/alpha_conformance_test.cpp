// atx::engine::alpha — P3b-4 Alpha101 conformance battery + semantic locks.
//
// This suite is the conformance evidence for the alpha-expression DSL against
// the *101 Formulaic Alphas* (Kakushadze 2016, arXiv:1601.00991). It does NOT
// claim bit-equality with an external Python reference (we have none in-process);
// instead it (plan §3.2):
//
//   (a) compiles + evaluates the EXPRESSIBLE subset of the 101 alphas as fixed
//       fixtures over a deterministic synthetic panel — they parse, type-check,
//       compile, and evaluate without error;
//   (b) PINS the disputed semantic conventions (plan §6) with targeted locking
//       tests: indneutralize == per-group demean == group_neutralize;
//       signedpower == sign(x)·|x|^a (NOT x^a); ts window floor(d); min/max
//       element-wise vs ts_min/ts_max time-series; full-window min_periods;
//   (c) for the SIMPLE alphas, asserts HAND-COMPUTED values to a stated
//       tolerance (EXPECT_DOUBLE_EQ where exact; EXPECT_NEAR with a documented
//       epsilon for transcendental/division results);
//   (d) confirms the fast VM == the reference oracle, cell-by-cell, bit-identical
//       (the established differential pattern from alpha_proof_test.cpp);
//   (e) a META-test asserts the out-of-battery list is EXHAUSTIVE: every Alpha#
//       1..101 is either in-battery or listed with a concrete reason (which
//       field/op is unshipped) — no silent omission.
//
// Shipped fields this battery provides: open high low close volume vwap returns
// adv20 IndClass.sector. Unshipped (→ out-of-battery): cap (market cap), adv{d}
// for d != 20, and IndClass.industry / IndClass.subindustry (we materialize only
// the sector classifier here). Every op the 101 reference for d=20-style alphas
// needs IS shipped (rank/ts_rank/correlation/covariance/delta/delay/decay_linear/
// scale/signedpower/indneutralize/ts_min/ts_max/ts_argmin/ts_argmax/sum/product/
// stddev/min/max), so out-of-battery is driven purely by missing FIELDS.
//
// Naming: Subject_Condition_ExpectedResult.

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_conformance_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// A process-lifetime Library: a parsed Ast borrows OpSig pointers from it, so it
// must outlive every parse result. One shared instance keeps tests consistent.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN, or exactly value-equal (covers ±inf, ±0). The
// VM reproduces the oracle EXACTLY by design, so equality is the right bar.
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// ===========================================================================
//  Pipeline helpers — parse_program → analyze → compile, asserting each stage.
// ===========================================================================

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  if (!ast) {
    return Program{};
  }
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  if (!ana) {
    return Program{};
  }
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// True iff `src` fails to compile at the analyze stage (a typecheck rejection).
[[nodiscard]] bool analyze_rejects(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  if (!ast) {
    return true; // a parse rejection counts as a rejection for the rail tests.
  }
  return !analyze(ast.value()).has_value();
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

// Assert the VM and oracle agree cell-by-cell, bit-identical, over a program.
void expect_fast_equals_oracle(const Program &prog, const Panel &panel) {
  const SignalSet fast = eval_fast(prog, panel);
  const SignalSet oracle = eval_oracle(prog, panel);
  ASSERT_EQ(fast.alphas.size(), oracle.alphas.size());
  for (atx::usize a = 0; a < fast.alphas.size(); ++a) {
    ASSERT_EQ(fast.alphas[a].values.size(), oracle.alphas[a].values.size());
    for (atx::usize i = 0; i < fast.alphas[a].values.size(); ++i) {
      const atx::f64 fc = fast.alphas[a].values[i];
      const atx::f64 oc = oracle.alphas[a].values[i];
      EXPECT_TRUE(same_cell(fc, oc)) << "alpha '" << fast.alphas[a].name << "' (idx " << a
                                     << ") cell " << i << ": FAST=" << fc << " ORACLE=" << oc;
    }
  }
}

// ===========================================================================
//  Deterministic synthetic panel.
//
//  A small, fully HAND-KNOWN block (no RNG): values are explicit so the simple
//  alphas can be checked against hand-computed expectations. Enough dates (16)
//  for the window-10 alphas plus warm-up; enough instruments (6) for meaningful
//  cross-sections and >= 2 sectors. Fields, in order:
//    open high low close volume vwap returns adv20 IndClass.sector
//  All cells are in-universe and finite (a separate fixture injects NaN gaps to
//  exercise the full-window min_periods policy).
// ===========================================================================

constexpr atx::usize kDates = 16;
constexpr atx::usize kInstruments = 6;

struct PanelData {
  atx::usize dates{kDates};
  atx::usize instruments{kInstruments};
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols; // one column per field, date-major
  std::vector<std::uint8_t> universe;      // dates*instruments {0,1}
};

[[nodiscard]] PanelData make_panel_data() {
  const atx::usize cells = kDates * kInstruments;
  PanelData pd;
  pd.names = {"open", "high",    "low",   "close",          "volume",
              "vwap", "returns", "adv20", "IndClass.sector"};
  pd.cols.assign(pd.names.size(), std::vector<atx::f64>(cells, 0.0));

  // Deterministic, finite, separable closed forms so hand-computation is exact.
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize n = 0; n < kInstruments; ++n) {
      const atx::usize i = d * kInstruments + n;
      const atx::f64 dd = static_cast<atx::f64>(d);
      const atx::f64 nn = static_cast<atx::f64>(n);
      const atx::f64 base = 100.0 + 10.0 * nn + dd;    // monotone in inst & date
      pd.cols[0][i] = base;                            // open
      pd.cols[1][i] = base + 2.0;                      // high
      pd.cols[2][i] = base - 2.0;                      // low
      pd.cols[3][i] = base + 1.0;                      // close (inside [low, high])
      pd.cols[4][i] = 1000.0 + 100.0 * nn + 10.0 * dd; // volume (positive)
      pd.cols[5][i] = base + 0.5;                      // vwap
      pd.cols[6][i] = 0.01 * (nn - 2.0);               // returns (small, signed)
      pd.cols[7][i] = 5000.0 + 50.0 * nn;              // adv20 (positive)
      pd.cols[8][i] = static_cast<atx::f64>(n % 2);    // IndClass.sector (2 groups)
    }
  }
  pd.universe.assign(cells, std::uint8_t{1});
  return pd;
}

[[nodiscard]] Panel panel_from(const PanelData &pd) {
  auto p = Panel::create(pd.dates, pd.instruments, pd.names, pd.cols, pd.universe);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

[[nodiscard]] Panel make_panel() { return panel_from(make_panel_data()); }

// A panel variant with a scattered NaN gap, to exercise full-window min_periods.
[[nodiscard]] Panel make_panel_with_gap(atx::usize field, atx::usize gap_cell) {
  PanelData pd = make_panel_data();
  pd.cols.at(field).at(gap_cell) = std::numeric_limits<atx::f64>::quiet_NaN();
  return panel_from(pd);
}

// ===========================================================================
//  Battery — the expressible Alpha101 subset as fixed fixtures.
//
//  Each line is verbatim from the paper EXCEPT `scale(x)` → `scale(x, 1)` (the
//  registry's scale is arity-2; a=1 is canonical unit-L1-norm) and the
//  Alpha#101 leading `.001` constant kept exact. Compiled as ONE program; both
//  engines evaluate it and the differential asserts FAST == ORACLE.
// ===========================================================================

[[nodiscard]] std::string_view battery_src() {
  return "a4   = -1 * ts_rank(rank(low), 9)\n"
         "a6   = -1 * correlation(open, volume, 10)\n"
         "a101 = (close - open) / ((high - low) + 0.001)\n"
         "a12  = sign(delta(volume, 1)) * (-1 * delta(close, 1))\n"
         "a23  = (ts_mean(high, 10) < high) ? (-1 * delta(high, 2)) : 0\n"
         "a33  = rank(-1 * (1 - (open / close)))\n"
         "a41  = power(high * low, 0.5) - vwap\n"
         "a53  = -1 * delta((close - low) - (high - close) / (close - low), 9)\n"
         "a54  = -1 * (low - close) * power(open, 5) / ((low - high) * power(close, 5))\n"
         "asp  = signedpower(close - open, 2)\n"
         "asc  = scale(close - ts_mean(close, 10), 1)\n"
         "adl  = decay_linear(rank(close), 4)\n"
         "ain  = indneutralize(close, IndClass.sector)\n"
         "acov = covariance(rank(high), rank(volume), 5)\n";
}

// ---------------------------------------------------------------------------
//  Differential: the whole battery, FAST == ORACLE, bit-identical.
// ---------------------------------------------------------------------------

TEST(AlphaConformance_Battery, ExpressibleSubset_FastEqualsOracle_BitIdentical) {
  const Panel panel = make_panel();
  const Program prog = compile_ok(battery_src());
  ASSERT_EQ(prog.roots.size(), 14U) << "battery must compile to 14 alpha roots";
  expect_fast_equals_oracle(prog, panel);
}

// ---------------------------------------------------------------------------
//  Hand-computed simple alphas. Tolerance: EXPECT_DOUBLE_EQ for exact rational
//  arithmetic; EXPECT_NEAR(1e-12) where a non-trivial division/root is involved.
// ---------------------------------------------------------------------------

TEST(AlphaConformance_HandValue, Alpha101_CloseOpenOverRange_ExactRational) {
  // Alpha#101 = (close - open) / ((high - low) + .001). In this panel
  // close-open = 1, high-low = 4 for every cell, so every value == 1/4.001.
  const Panel panel = make_panel();
  const Program prog = compile_ok("a101 = (close - open) / ((high - low) + 0.001)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 1U);
  const atx::f64 expected = 1.0 / 4.001;
  for (atx::usize i = 0; i < ss.alphas[0].values.size(); ++i) {
    EXPECT_DOUBLE_EQ(ss.alphas[0].values[i], expected) << "cell " << i;
  }
}

TEST(AlphaConformance_HandValue, Alpha41_GeomMeanMinusVwap_ExactClosedForm) {
  // Alpha#41 = (high*low)^0.5 - vwap. high=base+2, low=base-2, vwap=base+0.5,
  // so value = sqrt(base^2 - 4) - (base + 0.5), computed per (date, inst).
  const Panel panel = make_panel();
  const Program prog = compile_ok("a41 = power(high * low, 0.5) - vwap\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 1U);
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize n = 0; n < kInstruments; ++n) {
      const atx::f64 base = 100.0 + 10.0 * static_cast<atx::f64>(n) + static_cast<atx::f64>(d);
      const atx::f64 expected = std::sqrt(base * base - 4.0) - (base + 0.5);
      EXPECT_NEAR(ss.alphas[0].values[d * kInstruments + n], expected, 1e-12)
          << "date " << d << " inst " << n;
    }
  }
}

TEST(AlphaConformance_HandValue, Rank_OrdinalPercentile_MatchesHandRank) {
  // rank(close) — close is strictly increasing in instrument index at every
  // date, so the ordinal percentile is i/(N-1) over N=6 instruments (the oracle
  // documents [0,1] ordinal rank, tie-break by ascending instrument index).
  const Panel panel = make_panel();
  const Program prog = compile_ok("r = rank(close)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 1U);
  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize n = 0; n < kInstruments; ++n) {
      const atx::f64 expected = static_cast<atx::f64>(n) / static_cast<atx::f64>(kInstruments - 1);
      EXPECT_DOUBLE_EQ(ss.alphas[0].values[d * kInstruments + n], expected)
          << "date " << d << " inst " << n;
    }
  }
}

// ===========================================================================
//  SEMANTIC LOCKS (plan §6). Each pins one disputed convention against the
//  AS-BUILT engine and documents the chosen semantics.
// ===========================================================================

// ---- Lock #1: indneutralize(x, g) == per-group demean == group_neutralize ----

TEST(AlphaConformance_Lock_IndNeutralize, EqualsPerGroupDemean_AndGroupNeutralize) {
  const Panel panel = make_panel();
  // indneutralize and group_neutralize must be identical (both per-group demean).
  const Program prog = compile_ok("a = indneutralize(close, IndClass.sector)\n"
                                  "b = group_neutralize(close, IndClass.sector)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 2U);
  for (atx::usize i = 0; i < ss.alphas[0].values.size(); ++i) {
    EXPECT_TRUE(same_cell(ss.alphas[0].values[i], ss.alphas[1].values[i]))
        << "indneutralize != group_neutralize at cell " << i;
  }

  // And both equal the HAND per-group demean: sector = n%2, so even instruments
  // (0,2,4) form group 0, odd (1,3,5) group 1. Within each date, subtract the
  // group mean of `close` from each member.
  for (atx::usize d = 0; d < kDates; ++d) {
    std::array<atx::f64, 2> sum{0.0, 0.0};
    std::array<atx::usize, 2> cnt{0, 0};
    for (atx::usize n = 0; n < kInstruments; ++n) {
      const atx::f64 base = 100.0 + 10.0 * static_cast<atx::f64>(n) + static_cast<atx::f64>(d);
      sum[n % 2] += base + 1.0; // close
      ++cnt[n % 2];
    }
    for (atx::usize n = 0; n < kInstruments; ++n) {
      const atx::f64 base = 100.0 + 10.0 * static_cast<atx::f64>(n) + static_cast<atx::f64>(d);
      const atx::f64 close = base + 1.0;
      const atx::f64 expected = close - sum[n % 2] / static_cast<atx::f64>(cnt[n % 2]);
      EXPECT_DOUBLE_EQ(ss.alphas[0].values[d * kInstruments + n], expected)
          << "date " << d << " inst " << n;
    }
  }
}

// ---- Lock #2: signedpower(x, a) == sign(x)·|x|^a (NOT x^a) ----

TEST(AlphaConformance_Lock_SignedPower, IsSignTimesAbsPow_NotPlainPow) {
  // Build per-instrument constant inputs so we can read fixed hand values:
  //   x = close - 2*open  (mix of negative magnitudes); a = 0.5 and 1/3.
  // The lock: signedpower(-8, 1/3) == -2, signedpower(-4, 0.5) == -2 — i.e. the
  // odd-root / fractional power of a NEGATIVE preserves the sign and uses |x|.
  EXPECT_DOUBLE_EQ(atx::engine::alpha::detail::vm_spow(-8.0, 1.0 / 3.0), -std::pow(8.0, 1.0 / 3.0));
  EXPECT_DOUBLE_EQ(atx::engine::alpha::detail::vm_spow(-4.0, 0.5), -2.0);
  EXPECT_DOUBLE_EQ(atx::engine::alpha::detail::vm_spow(4.0, 0.5), 2.0);
  EXPECT_DOUBLE_EQ(atx::engine::alpha::detail::vm_spow(0.0, 0.5), 0.0);
  // The VM and oracle kernels must agree on the convention.
  EXPECT_DOUBLE_EQ(atx::engine::alpha::detail::vm_spow(-4.0, 0.5),
                   atx::engine::alpha::detail::op_spow(-4.0, 0.5));

  // End-to-end through the pipeline, contrasted with power(x,a) = plain x^a.
  // Make `x` negative by feeding (low - high) = -4 everywhere; a = 2 so
  // signedpower(-4, 2) = sign(-4)*|−4|^2 = -16, while power(-4, 2) = +16.
  const Panel panel = make_panel();
  const Program prog = compile_ok("sp = signedpower(low - high, 2)\n"
                                  "pw = power(low - high, 2)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 2U);
  for (atx::usize i = 0; i < ss.alphas[0].values.size(); ++i) {
    EXPECT_DOUBLE_EQ(ss.alphas[0].values[i], -16.0) << "signedpower cell " << i;
    EXPECT_DOUBLE_EQ(ss.alphas[1].values[i], 16.0) << "power cell " << i;
  }
  expect_fast_equals_oracle(prog, panel);
}

// ---- Lock #3: ts_{O}(x, d) non-integer window → floor(d) ----

TEST(AlphaConformance_Lock_FloorWindow, FractionalWindow_FloorsTo8) {
  // ts_mean(close, 8.7) must compile AND evaluate bit-identically to
  // ts_mean(close, 8) — the window floors, it is not rejected nor rounded.
  const Panel panel = make_panel();
  const Program prog = compile_ok("a = ts_mean(close, 8.7)\n"
                                  "b = ts_mean(close, 8)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 2U);
  for (atx::usize i = 0; i < ss.alphas[0].values.size(); ++i) {
    EXPECT_TRUE(same_cell(ss.alphas[0].values[i], ss.alphas[1].values[i]))
        << "ts_mean(close, 8.7) != ts_mean(close, 8) at cell " << i;
  }
  expect_fast_equals_oracle(prog, panel);
}

TEST(AlphaConformance_Lock_FloorWindow, SubOneWindow_FloorsToZero_Rejected) {
  // 0.5 floors to 0 → a zero window has nothing to roll over → rejected.
  EXPECT_TRUE(analyze_rejects("ts_mean(close, 0.5)"));
}

TEST(AlphaConformance_Lock_FloorWindow, NegativeWindow_Rejected) {
  // -3 floors to -3 (< 1) → rejected (the <=0 rail survives the floor change).
  EXPECT_TRUE(analyze_rejects("ts_mean(close, -3)"));
  // A fractional negative likewise rejects (floor(-0.5) = -1 < 1).
  EXPECT_TRUE(analyze_rejects("ts_mean(close, -0.5)"));
}

TEST(AlphaConformance_Lock_FloorWindow, NonConstWindow_Rejected) {
  // A non-literal window (a field) is still rejected — the constant rail holds.
  EXPECT_TRUE(analyze_rejects("ts_mean(close, close)"));
}

TEST(AlphaConformance_Lock_FloorWindow, ArityThreeCorrelation_FloorsWindow) {
  // The arity-3 window arg (correlation/covariance) floors too: corr(.,.,5.9)
  // == corr(.,.,5).
  const Panel panel = make_panel();
  const Program prog = compile_ok("a = correlation(close, volume, 5.9)\n"
                                  "b = correlation(close, volume, 5)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 2U);
  for (atx::usize i = 0; i < ss.alphas[0].values.size(); ++i) {
    EXPECT_TRUE(same_cell(ss.alphas[0].values[i], ss.alphas[1].values[i]))
        << "correlation window did not floor at cell " << i;
  }
}

// ---- Lock #4: full-window min_periods (any-NaN window → NaN) ----

TEST(AlphaConformance_Lock_MinPeriods, AnyNaNInWindow_ProducesNaN_FullWindowPolicy) {
  // The as-built policy is UNIFORM full-window: a rolling window containing any
  // NaN yields NaN (no partial reduction) for stddev/correlation AND ts_mean/
  // ts_sum. Inject a NaN into `close` at (date 3, inst 0); a window-4 ts_mean
  // ending at dates 3,4,5,6 for inst 0 includes that NaN, so those cells are
  // NaN. The window starting at date 7 (covers dates 4..7) is clear → finite.
  const atx::usize victim_date = 3;
  const atx::usize victim_inst = 0;
  const atx::usize gap_cell = victim_date * kInstruments + victim_inst;
  const Panel panel = make_panel_with_gap(/*field=close*/ 3, gap_cell);
  const Program prog = compile_ok("m = ts_mean(close, 4)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 1U);

  for (atx::usize d = victim_date; d <= victim_date + 3; ++d) {
    const atx::f64 cell = ss.alphas[0].values[d * kInstruments + victim_inst];
    EXPECT_TRUE(std::isnan(cell)) << "date " << d << " inst " << victim_inst
                                  << " must be NaN (window includes the gap)";
  }
  const atx::f64 clear = ss.alphas[0].values[(victim_date + 4) * kInstruments + victim_inst];
  EXPECT_FALSE(std::isnan(clear)) << "date 7 window clears the gap → finite";
  // The VM and oracle must agree on the NaN propagation too.
  expect_fast_equals_oracle(prog, panel);
}

// ---- Lock #5: min/max element-wise (2 panels) vs ts_min/ts_max (panel+window)

TEST(AlphaConformance_Lock_MinMax, ElementWise_VsTimeSeries_Disambiguated) {
  // min(x, y) over two panel args is the per-cell element-wise minimum; the
  // registry resolves it to MinP. ts_min(x, d) over a panel + integer window is
  // the trailing-window minimum (TsMin). Distinct names + shapes disambiguate.
  const Panel panel = make_panel();
  const Program prog = compile_ok("emin = min(low, open)\n"
                                  "emax = max(high, close)\n"
                                  "tmin = ts_min(close, 3)\n"
                                  "tmax = ts_max(close, 3)\n");
  const SignalSet ss = eval_fast(prog, panel);
  ASSERT_EQ(ss.alphas.size(), 4U);

  for (atx::usize d = 0; d < kDates; ++d) {
    for (atx::usize n = 0; n < kInstruments; ++n) {
      const atx::usize i = d * kInstruments + n;
      const atx::f64 base = 100.0 + 10.0 * static_cast<atx::f64>(n) + static_cast<atx::f64>(d);
      const atx::f64 open = base;
      const atx::f64 high = base + 2.0;
      const atx::f64 low = base - 2.0;
      const atx::f64 close = base + 1.0;
      // element-wise: per-cell min/max of the two panels.
      EXPECT_DOUBLE_EQ(ss.alphas[0].values[i], std::min(low, open)) << "emin cell " << i;
      EXPECT_DOUBLE_EQ(ss.alphas[1].values[i], std::max(high, close)) << "emax cell " << i;
      // time-series: close is increasing in date, so ts_min over [d-2,d] is the
      // earliest in-window close and ts_max is `close` itself. Only check once
      // the window is full (d >= 2) to respect full-window min_periods.
      if (d >= 2) {
        const atx::f64 close_dm2 =
            100.0 + 10.0 * static_cast<atx::f64>(n) + static_cast<atx::f64>(d - 2) + 1.0;
        EXPECT_DOUBLE_EQ(ss.alphas[2].values[i], close_dm2) << "tmin cell " << i;
        EXPECT_DOUBLE_EQ(ss.alphas[3].values[i], close) << "tmax cell " << i;
      }
    }
  }
  expect_fast_equals_oracle(prog, panel);
}

// ===========================================================================
//  Exhaustiveness meta-test: every Alpha# 1..101 is classified.
//
//  Membership is FIELD-driven: every operator the 101 reference needs is
//  shipped (see header note), so an alpha is out-of-battery iff it references a
//  field we do not materialize here. Shipped fields: open high low close volume
//  vwap returns adv20 IndClass.sector. Out-of-battery reasons (concrete):
//    * "cap"                — market-cap field unshipped.
//    * "adv{d} d!=20"       — only adv20 provided.
//    * "IndClass.industry"  — only IndClass.sector provided.
//    * "IndClass.subindustry" — only IndClass.sector provided.
//  An alpha needing several lists the first blocking field. The table is the
//  AUTHORITY: the test asserts it covers 1..101 with no gap and no duplicate.
// ===========================================================================

// InBattery == "expressible" (every field the alpha references is shipped); a
// representative 14-alpha subset is actually evaluated as fixtures by the
// differential battery above. OutOfBattery == references at least one unshipped
// field (the `reason` names it).
enum class Membership : std::uint8_t { InBattery, OutOfBattery };

struct AlphaClass {
  int number{};            // 1..101
  Membership membership{}; //
  std::string_view reason; // out-of-battery: the blocking field; else ""
};

// The full 1..101 classification. Reasons cite the unshipped FIELD per the
// canonical Kakushadze (2016) formulas. Operators are ALL shipped, so a row is
// in-battery iff it references only shipped fields; the blocking field named is
// the first unshipped field in that alpha's formula. Alphas using only adv20
// (which IS shipped) and/or IndClass.sector are in-battery.
[[nodiscard]] const std::array<AlphaClass, 101> &alpha_table() {
  static constexpr Membership In = Membership::InBattery;
  static constexpr Membership Out = Membership::OutOfBattery;
  static constexpr std::string_view kCap = "cap (market-cap field unshipped)";
  static constexpr std::string_view kAdv = "adv{d} for d!=20 unshipped (only adv20 provided)";
  static constexpr std::string_view kInd =
      "IndClass.industry unshipped (only IndClass.sector provided)";
  static constexpr std::string_view kSub =
      "IndClass.subindustry unshipped (only IndClass.sector provided)";
  static const std::array<AlphaClass, 101> kTable = {{
      {1, In, ""},  // rank(ts_argmax(signedpower((returns<0?stddev:close),2),5))-0.5
      {2, In, ""},  // -corr(delta(log(volume),2), (close-open)/open, 6)
      {3, In, ""},  // -corr(rank(open), rank(volume), 10)
      {4, In, ""},  // -ts_rank(rank(low), 9)
      {5, In, ""},  // rank(open - sum(vwap,10)/10) * -abs(rank(close-vwap))
      {6, In, ""},  // -corr(open, volume, 10)
      {7, In, ""},  // conditional on adv20<volume; ts ops over adv20 — adv20 shipped
      {8, In, ""},  // -rank(sum(open,5)*sum(returns,5) - delay(...,10))
      {9, In, ""},  // conditional ts_min/ts_max of delta(close,1)
      {10, In, ""}, // rank(conditional ts_min/ts_max delta)
      {11, In, ""}, // (rank(ts_max(vwap-close,3))+rank(ts_min(vwap-close,3)))*rank(delta(volume,3))
      {12, In, ""}, // sign(delta(volume,1)) * -delta(close,1)
      {13, In, ""}, // -rank(covariance(rank(close), rank(volume), 5))
      {14, In, ""}, // -rank(delta(returns,3)) * corr(open,volume,10)
      {15, In, ""}, // -sum(rank(corr(rank(high),rank(volume),3)),3)
      {16, In, ""}, // -rank(covariance(rank(high), rank(volume), 5))
      {17, In, ""}, // ... * rank(ts_rank(volume/adv20, 5)) — adv20 shipped
      {18, In, ""}, // -rank(stddev(abs(close-open),5)+(close-open)+corr(close,open,10))
      {19, In, ""}, // -sign(...)*(1+rank(1+sum(returns,250)))
      {20, In, ""}, // -rank(open-delay(high,1))*rank(open-delay(close,1))*rank(open-delay(low,1))
      {21, In, ""}, // conditional on adv20 vs volume — adv20 shipped
      {22, In, ""}, // -delta(corr(high,volume,5),5)*rank(stddev(close,20))
      {23, In, ""}, // (ts_mean(high,20)<high)? -delta(high,2) : 0
      {24, In, ""}, // conditional on delta(ts_mean(close,100))/delay...
      {25, In, ""}, // rank((-returns)*adv20*vwap*(high-close)) — adv20 shipped
      {26, In, ""}, // -ts_max(corr(ts_rank(volume,5),ts_rank(high,5),5),3)
      {27, In, ""}, // conditional rank(sum(corr(rank(volume),rank(vwap),6),2)/2)
      {28, In, ""}, // scale((corr(adv20,low,5)+(high+low)/2)-close) — adv20 shipped
      {29, In, ""}, // nested min/product/log/rank/scale/ts_min + delay(-returns)
      {30, In, ""}, // (1-rank(sign-sums)) * sum(volume,5)/sum(volume,20)
      {31, In, ""}, // rank decay_linear + rank(-delta(close,3)) + sign(scale(corr(adv20,low,12)))
      {32, In, ""}, // scale(sum(close,7)/7-close) + 20*scale(corr(vwap,delay(close,5),230))
      {33, In, ""}, // rank(-1*(1-(open/close)))
      {34, In, ""}, // rank((1-rank(stddev(returns,2)/stddev(returns,5)))+(1-rank(delta(close,1))))
      {35, In, ""}, // ts_rank(volume,32)*(1-ts_rank(close+high-low,16))*(1-ts_rank(returns,32))
      {36, In, ""}, // multi-term incl. ts_rank(volume,5) and corr; uses adv20 — shipped
      {37, In, ""}, // rank(corr(delay(open-close,1),close,200)) + rank(open-close)
      {38, In, ""}, // -rank(ts_rank(close,10))*rank(close/open)
      {39, In,
       ""}, // -rank(delta(close,7)*(1-rank(decay_linear(volume/adv20,9))))*(1+rank(sum(returns,250)))
      {40, In, ""}, // -rank(stddev(high,10))*corr(high,volume,10)
      {41, In, ""}, // (high*low)^0.5 - vwap
      {42, In, ""}, // rank(vwap-close)/rank(vwap+close)
      {43, In, ""}, // ts_rank(volume/adv20,20)*ts_rank(-delta(close,7),8) — adv20 shipped
      {44, In, ""}, // -corr(high, rank(volume), 5)
      {45, In,
       ""}, // -(rank(sum(delay(close,5),20)/20)*corr(close,volume,2)*rank(corr(sum(close,5),sum(close,20),2)))
      {46, In, ""},    // conditional on delayed close slopes
      {47, In, ""},    // (rank(1/close)*volume/adv20)*... — adv20 shipped
      {48, Out, kSub}, // indneutralize(..., IndClass.subindustry)
      {49, In, ""},    // conditional on delayed close slopes (threshold -0.1)
      {50, In, ""},    // -ts_max(rank(corr(rank(volume),rank(vwap),5)),5)
      {51, In, ""},    // conditional on delayed close slopes (threshold -0.05)
      {52, In,
       ""}, // (-ts_min(low,5)+delay(ts_min(low,5),5))*rank((sum(returns,240)-sum(returns,20))/220)*ts_rank(volume,5)
      {53, In, ""}, // -delta((close-low)-(high-close)/(close-low),9)
      {54, In, ""}, // -(low-close)*open^5 / ((low-high)*close^5)
      {55, In,
       ""}, // -corr(rank((close-ts_min(low,12))/(ts_max(high,12)-ts_min(low,12))), rank(volume),6)
      {56, Out, kCap}, // 0-(1*(rank(sum(returns,10)/sum(sum(returns,2),3))*rank(returns*cap)))
      {57, In, ""},    // 0-(1*((close-vwap)/decay_linear(rank(ts_argmax(close,30)),2)))
      {58, In,
       ""}, // -ts_rank(decay_linear(corr(indneutralize(vwap,IndClass.sector),volume,3.92),7.89),5.51) — sector shipped
      {59, Out, kInd}, // indneutralize(vwap*... , IndClass.industry)
      {60, In,
       ""}, // 0-(1*((2*scale(rank((((close-low)-(high-close))/(high-low))*volume)))-scale(rank(ts_argmax(close,10)))))
      {61, Out, kAdv}, // rank(vwap-ts_min(vwap,16)) < rank(corr(vwap,adv180,18))
      {62, In, ""},    // (rank(corr(vwap,sum(adv20,22),10))<rank(...))*-1 — adv20 shipped
      {63, Out, kInd}, // indneutralize(close, IndClass.industry) decay terms
      {64, Out, kAdv}, // corr(sum(((open*.178)+(low*.822)),13), sum(adv120,13),17)
      {65, Out, kAdv}, // corr(((open*.0073)+(vwap*.9927)), sum(adv60,9),6)
      {66, In,
       ""}, // -(rank(decay_linear(delta(vwap,4),7))+ts_rank(decay_linear((low-vwap)/(open-(high+low)/2),11),7))
      {67, Out,
       kSub}, // -(rank(high-ts_min(high,2))^rank(corr(indneutralize(vwap,IndClass.sector),indneutralize(adv20,IndClass.subindustry),6)))
      {68, Out,
       kAdv}, // ts_rank(corr(rank(high),rank(adv15),9),14) < rank(delta(((close*.518)+(low*.482)),1))
      {69, Out, kInd}, // indneutralize(vwap, IndClass.industry) terms + adv20
      {70, Out, kInd}, // indneutralize(close, IndClass.industry) + adv50
      {71, Out,
       kAdv}, // max(ts_rank(decay_linear(corr(ts_rank(close,3),ts_rank(adv180,12),18),4),16), ...)
      {72, Out,
       kAdv}, // rank(decay_linear(corr((high+low)/2, adv40,9),10)) / rank(decay_linear(corr(ts_rank(vwap,4),ts_rank(volume,19),7),3))
      {73, In,
       ""}, // -max(rank(decay_linear(delta(vwap,5),3)), ts_rank(decay_linear((delta((open*.147+low*.853),2)/(open*.147+low*.853))*-1,3),17))
      {74, Out,
       kAdv}, // rank(corr(close, sum(adv30,37),15)) < rank(corr(rank(high*.026+vwap*.974), rank(volume),11))
      {75, Out, kAdv}, // rank(corr(vwap,volume,4)) < rank(corr(rank(low),rank(adv50),12))
      {76, Out,
       kAdv}, // max(rank(decay_linear(delta(vwap,1),12)), ts_rank(decay_linear(ts_rank(corr(low,adv81,8),20),17),19))
      {77, Out,
       kAdv}, // min(rank(decay_linear((high+low)/2+high-(vwap+high),20)), rank(decay_linear(corr((high+low)/2,adv40,3),6)))
      {78, Out,
       kAdv}, // rank(corr(sum(low*.352+vwap*.648,20), sum(adv40,20),7))^rank(corr(rank(vwap),rank(volume),6))
      {79, Out,
       kAdv}, // rank(delta(indneutralize(close*.607+open*.393,IndClass.sector),1)) < rank(corr(ts_rank(vwap,4),ts_rank(adv150,9),15))
      {80, Out,
       kInd}, // sign(rank(delta(indneutralize(open*.868+high*.132,IndClass.industry),4)))^ts_rank(corr(high,adv10,5),6)*-1
      {81, Out,
       kAdv}, // -(rank(log(product(rank(rank(corr(vwap,sum(adv10,50),8))^4),15))) < rank(corr(rank(vwap),rank(volume),5)))
      {82, In,
       ""}, // -min(rank(decay_linear(delta(open,1),15)), ts_rank(decay_linear(corr(indneutralize(volume,IndClass.sector),open,17),7),13)) — sector shipped
      {83, In,
       ""}, // (rank(delay((high-low)/(sum(close,5)/5),2))*rank(rank(volume)))/(((high-low)/(sum(close,5)/5))/(vwap-close))
      {84, In, ""}, // signedpower(ts_rank(vwap-ts_max(vwap,15),21), delta(close,5))
      {85, Out,
       kAdv}, // rank(corr(high*.876+close*.124, adv30,10))^rank(corr(ts_rank((high+low)/2,4),ts_rank(volume,10),7))
      {86, In,
       ""}, // (ts_rank(corr(close, sum(adv20,15),6),20) < rank(open+close-(vwap+open)))*-1 — adv20 shipped
      {87, Out,
       kAdv}, // -max(rank(decay_linear(delta(close*.37+vwap*.63,2),3)), ts_rank(decay_linear(abs(corr(adv81,close,13)),5),14))
      {88, Out,
       kAdv}, // min(rank(decay_linear(rank(open)+rank(low)-rank(high)-rank(close),8)), ts_rank(decay_linear(corr(ts_rank(close,8),ts_rank(adv60,21),8),7),3))
      {89, Out,
       kInd}, // ts_rank(decay_linear(corr(low,adv10,7),6),4) - ts_rank(decay_linear(delta(indneutralize(vwap,IndClass.industry),3),10),15)
      {90, Out,
       kSub}, // -(rank(close-ts_max(close,5))^ts_rank(corr(indneutralize(adv40,IndClass.subindustry),low,5),3))
      {91, Out,
       kInd}, // -(ts_rank(decay_linear(decay_linear(corr(indneutralize(close,IndClass.industry),volume,10),16),4),5)-rank(decay_linear(corr(vwap,adv30,4),3)))
      {92, Out,
       kAdv}, // min(ts_rank(decay_linear((((high+low)/2+close)<(low+open))?1:0,15),19), ts_rank(decay_linear(corr(rank(low),rank(adv30),8),7),7))
      {93, Out,
       kInd}, // ts_rank(decay_linear(corr(indneutralize(vwap,IndClass.industry),adv81,17),20),8) / rank(decay_linear(delta(close*.524+vwap*.476,3),16))
      {94, Out,
       kAdv}, // -(rank(vwap-ts_min(vwap,12))^ts_rank(corr(ts_rank(vwap,20),ts_rank(adv60,4),18),3))
      {95, Out,
       kAdv}, // rank(open-ts_min(open,12)) < ts_rank(rank(corr(sum((high+low)/2,19),sum(adv40,19),13))^5,12)
      {96, Out,
       kAdv}, // -max(ts_rank(decay_linear(corr(rank(vwap),rank(volume),4),4),8), ts_rank(decay_linear(ts_argmax(corr(ts_rank(close,7),ts_rank(adv60,4),4),13),14),13))
      {97, Out,
       kInd}, // -(rank(decay_linear(delta(indneutralize(low*.721+vwap*.279,IndClass.industry),3),20)) - ts_rank(decay_linear(ts_rank(corr(ts_rank(low,8),ts_rank(adv60,17),5),19),16),7))
      {98, Out,
       kAdv}, // rank(decay_linear(corr(vwap,sum(adv5,26),5),7)) - rank(decay_linear(ts_rank(ts_argmin(corr(rank(open),rank(adv15),21),9),7),8))
      {99, Out,
       kAdv}, // (rank(corr(sum((high+low)/2,20),sum(adv60,20),9)) < rank(corr(low,volume,6)))*-1
      {100, Out,
       kSub}, // 0-(1*((1.5*scale(indneutralize(indneutralize(rank(...),IndClass.subindustry),IndClass.subindustry)))-scale(...)))*(volume/adv20)
      {101, In, ""}, // (close-open)/((high-low)+.001)
  }};
  return kTable;
}

TEST(AlphaConformance_Meta, OutOfBatteryList_IsExhaustive_Covers1To101_NoGaps) {
  const std::array<AlphaClass, 101> &table = alpha_table();
  // Each Alpha# in 1..101 appears EXACTLY once; no gap, no duplicate.
  std::array<int, 102> seen{}; // index by alpha number (1..101)
  for (const AlphaClass &row : table) {
    ASSERT_GE(row.number, 1) << "alpha number below 1";
    ASSERT_LE(row.number, 101) << "alpha number above 101";
    ++seen[static_cast<std::size_t>(row.number)];
    // An out-of-battery row MUST carry a non-empty reason; an in-battery row
    // MUST carry an empty reason. No silent omission, no spurious reason.
    if (row.membership == Membership::OutOfBattery) {
      EXPECT_FALSE(row.reason.empty()) << "alpha #" << row.number << " out-of-battery w/o reason";
    } else {
      EXPECT_TRUE(row.reason.empty()) << "alpha #" << row.number << " in-battery w/ a reason";
    }
  }
  for (int n = 1; n <= 101; ++n) {
    EXPECT_EQ(seen[static_cast<std::size_t>(n)], 1)
        << "Alpha #" << n << " is not classified exactly once (gap or duplicate)";
  }
}

TEST(AlphaConformance_Meta, InBatteryCount_AndOutOfBatteryCount_AreReported) {
  const std::array<AlphaClass, 101> &table = alpha_table();
  int in_count = 0;
  int out_count = 0;
  for (const AlphaClass &row : table) {
    if (row.membership == Membership::InBattery) {
      ++in_count;
    } else {
      ++out_count;
    }
  }
  EXPECT_EQ(in_count + out_count, 101);
  // Pin the exact split so an accidental In<->Out flip (which still totals 101)
  // is caught: 65 expressible, 36 out-of-battery (1 cap + 22 adv{d!=20} + 9
  // IndClass.industry + 4 IndClass.subindustry).
  EXPECT_EQ(in_count, 65);
  EXPECT_EQ(out_count, 36);
}


}  // namespace atxtest_alpha_conformance_test

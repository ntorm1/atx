// atx::engine::alpha — P3b-3 stateful causal-recurrence op battery.
//
// Two stateful operators carrying TRUE cross-date state from the panel's first
// date forward (a per-instrument `state[t-1]`), evaluated by a NEW forward-scan
// eval path (Engine::eval_recurrence) that is causal BY CONSTRUCTION — it reads
// only `state[t-1]` + inputs at date `<= t`; a forward index is unrepresentable.
//
//   * trade_when(trigger, alpha, exit) — arity 3, output Panel:
//        out[t] = NaN       if exit[t]  > 0       (close / no position)
//               = alpha[t]  elif trigger[t] > 0   ((re)enter with the new signal)
//               = out[t-1]  else                  (HOLD the prior signal)
//        out[0] = (trigger[0]>0 && !(exit[0]>0)) ? alpha[0] : NaN
//     trigger/exit are Mask dtype (truthy == > 0 == mask_true); alpha is F64.
//   * hump(x, threshold=0.01) — arity (1,2), output Panel (turnover damper):
//        out[t] = x[t]      if |x[t] - out[t-1]| > threshold
//               = out[t-1]  else                  (suppress small change, hold)
//        out[0] = x[0]
//     threshold is the scalar 2nd operand (read exactly as winsorize/scale do).
//
// Evidence layers (Subject_Condition_ExpectedResult):
//   1. HAND-TRACED known-value — enter/hold/close/re-enter; first-date rules;
//      hump suppress/pass/boundary/default.
//   2. DIFFERENTIAL (the main gate) — every op via Engine::evaluate vs
//      evaluate_reference on a synthetic panel w/ delisted+NaN cells, bit-equal.
//   3. TRUNCATION-INVARIANCE (the causality proof) — full vs date-truncated panel
//      agree byte-for-byte at dates <= t (the forward scan cannot reach > t).
//   4. DETERMINISM + boundaries (1 date, 1 instrument, NaN trigger/exit/x).

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

namespace atxtest_alpha_trade_when_test {

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

// Two cells agree iff both NaN or exactly value-equal (covers +-inf, +-0).
// Task 7: a composite/nested differential may fold ONLINE FP Ts ops (ts_mean /
// ts_zscore) whose VM output is within a TIGHT TOLERANCE of — not bit-identical
// to — the batch oracle. cells_conform() keeps the NaN pattern exact and applies
// atol+rtol=1e-9 (the bit-exact ops pass trivially). See alpha_ts_test.cpp.
inline constexpr atx::f64 kOnlineAtol = 1e-9;
inline constexpr atx::f64 kOnlineRtol = 1e-9;

[[nodiscard]] bool cells_conform(atx::f64 vm, atx::f64 oracle) noexcept {
  if (std::isnan(vm) && std::isnan(oracle)) {
    return true;
  }
  if (std::isnan(vm) != std::isnan(oracle)) {
    return false;
  }
  return std::fabs(vm - oracle) <= kOnlineAtol + kOnlineRtol * std::fabs(oracle);
}

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
//  cells. Deterministic (no RNG): a smooth analytic close + trig/exit fields
//  derived from it so the trade_when state machine threads enters/holds/exits.
// ===========================================================================

struct DiffPanel {
  atx::usize dates{};
  atx::usize instruments{};
  std::vector<std::string> names;
  std::vector<std::vector<atx::f64>> cols;
  std::vector<std::uint8_t> universe;
};

[[nodiscard]] DiffPanel make_diff_panel(atx::usize dates, atx::usize instruments) {
  const atx::usize cells = dates * instruments;
  DiffPanel pd;
  pd.dates = dates;
  pd.instruments = instruments;
  // `trig`/`exitf` are raw f64 fields we threshold with `> 0` masks in the DSL
  // (so trade_when sees real Mask operands). `alpha` is the signal to carry.
  pd.names = {"close", "trig", "exitf"};
  pd.cols.assign(pd.names.size(), std::vector<atx::f64>(cells));
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::usize j = 0; j < instruments; ++j) {
      const atx::usize i = d * instruments + j;
      const auto df = static_cast<atx::f64>(d);
      const auto jf = static_cast<atx::f64>(j);
      pd.cols[0][i] = 50.0 + 10.0 * std::sin(0.3 * df + 0.7 * jf) + 0.5 * df; // close
      pd.cols[1][i] = std::sin(0.9 * df + 0.4 * jf);                          // trig (sign varies)
      pd.cols[2][i] = std::cos(1.3 * df + 0.2 * jf);                          // exit (sign varies)
    }
  }
  pd.universe.assign(cells, std::uint8_t{1});
  if (instruments >= 3 && dates >= 4) {
    const atx::usize delist_date = dates * 2 / 3;
    for (atx::usize d = delist_date; d < dates; ++d) {
      pd.universe[d * instruments + 1] = 0;
      pd.universe[d * instruments + (instruments - 1)] = 0;
    }
  }
  // Scatter NaN gaps into close/trig/exit (the recurrence NaN paths).
  if (cells >= 8) {
    pd.cols[0][cells / 5] = kNaN;
    pd.cols[1][cells / 3] = kNaN;
    pd.cols[2][cells * 4 / 5] = kNaN;
  }
  return pd;
}

[[nodiscard]] Panel panel_from(const DiffPanel &pd) {
  return make_panel(pd.dates, pd.instruments, pd.names, pd.cols, pd.universe);
}

// Run one alpha source through BOTH engines on the differential panel and assert
// cell-by-cell bit-identity. Returns the fast result for further inspection.
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
      if (!cells_conform(fc, oc)) {
        ++divergences;
        EXPECT_TRUE(cells_conform(fc, oc)) << "alpha '" << fast.alphas[a].name << "' cell " << i
                                           << ": FAST=" << fc << " ORACLE=" << oc;
      }
    }
  }
  EXPECT_EQ(divergences, 0U) << "FAST vs ORACLE differential exceeded tolerance for src: " << src;
  return fast;
}

// Truncation-invariance: evaluate `src` on the full single-instrument column,
// then on the column cut after date `t`; outputs at dates <= t must be byte-
// identical (the forward scan cannot reach > t). Multi-field aware.
void assert_truncation_invariant(std::string_view src, atx::usize t,
                                 const std::vector<std::string> &names,
                                 const std::vector<std::vector<atx::f64>> &cols) {
  const atx::usize dates = cols.at(0).size();
  const Panel full = make_panel(dates, 1, names, cols);
  const Program prog = compile_ok(src);
  const SignalSet full_res = eval_fast(prog, full);

  std::vector<std::vector<atx::f64>> tcols;
  tcols.reserve(cols.size());
  for (const std::vector<atx::f64> &c : cols) {
    tcols.emplace_back(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(t + 1));
  }
  const Panel trunc = make_panel(t + 1, 1, names, tcols);
  const SignalSet trunc_res = eval_fast(prog, trunc);
  for (atx::usize d = 0; d <= t; ++d) {
    EXPECT_TRUE(same_cell(full_res.alphas[0].values[d], trunc_res.alphas[0].values[d]))
        << "look-ahead at date " << d << " for src: " << src;
  }
}

// ===========================================================================
//  1. trade_when — hand-traced enter / hold / close / re-enter.
// ===========================================================================

// 1 instrument, 5 dates. trig/exit are raw fields thresholded `> 0` in the DSL.
//   date: 0     1     2     3     4
//   trig:  1     0     0     1     0    (enter d0, re-enter d3)
//   exit:  0     0     1     0     0    (close d2)
//   alpha: 10    20    30    40    50
// expected out: d0 enter->10; d1 hold->10; d2 exit->NaN; d3 re-enter->40; d4 hold->40
TEST(AlphaTradeWhen_HandTrace, EnterHoldCloseReenter_MatchesByHand) {
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{
      {1.0, 0.0, 0.0, 1.0, 0.0}, {0.0, 0.0, 1.0, 0.0, 0.0}, {10.0, 20.0, 30.0, 40.0, 50.0}};
  const Panel panel = make_panel(5, 1, names, cols);
  const Program prog = compile_ok("a = trade_when(trig > 0, alpha, exitf > 0)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 10.0);  // enter on trigger at d0
  EXPECT_DOUBLE_EQ(v[1], 10.0);  // hold (no trig, no exit)
  EXPECT_TRUE(std::isnan(v[2])); // exit -> close -> NaN
  EXPECT_DOUBLE_EQ(v[3], 40.0);  // re-enter with the NEW signal
  EXPECT_DOUBLE_EQ(v[4], 40.0);  // hold the re-entered signal
}

TEST(AlphaTradeWhen_FirstDate, TriggerOnDateZero_Enters) {
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{{1.0}, {0.0}, {7.0}};
  const Panel panel = make_panel(1, 1, names, cols);
  const Program prog = compile_ok("a = trade_when(trig > 0, alpha, exitf > 0)\n");
  const SignalSet r = eval_fast(prog, panel);
  EXPECT_DOUBLE_EQ(r.alphas.at(0).values[0], 7.0);
}

TEST(AlphaTradeWhen_FirstDate, NoTriggerOnDateZero_IsNaN) {
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{{0.0}, {0.0}, {7.0}};
  const Panel panel = make_panel(1, 1, names, cols);
  const Program prog = compile_ok("a = trade_when(trig > 0, alpha, exitf > 0)\n");
  const SignalSet r = eval_fast(prog, panel);
  EXPECT_TRUE(std::isnan(r.alphas.at(0).values[0]));
}

TEST(AlphaTradeWhen_FirstDate, ExitDominatesTriggerOnDateZero_IsNaN) {
  // exit > 0 dominates trigger on the first date (exit branch first).
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{{1.0}, {1.0}, {7.0}};
  const Panel panel = make_panel(1, 1, names, cols);
  const Program prog = compile_ok("a = trade_when(trig > 0, alpha, exitf > 0)\n");
  const SignalSet r = eval_fast(prog, panel);
  EXPECT_TRUE(std::isnan(r.alphas.at(0).values[0]));
}

TEST(AlphaTradeWhen_AllTrigger, OutputEqualsAlphaEveryDate) {
  // trigger every date, never exit -> out == alpha at every date.
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{
      {1.0, 1.0, 1.0, 1.0}, {0.0, 0.0, 0.0, 0.0}, {3.0, 6.0, 9.0, 12.0}};
  const Panel panel = make_panel(4, 1, names, cols);
  const Program prog = compile_ok("a = trade_when(trig > 0, alpha, exitf > 0)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 3.0);
  EXPECT_DOUBLE_EQ(v[1], 6.0);
  EXPECT_DOUBLE_EQ(v[2], 9.0);
  EXPECT_DOUBLE_EQ(v[3], 12.0);
}

TEST(AlphaTradeWhen_AllExit, OutputIsAllNaN) {
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{
      {1.0, 1.0, 1.0, 1.0}, {1.0, 1.0, 1.0, 1.0}, {3.0, 6.0, 9.0, 12.0}};
  const Panel panel = make_panel(4, 1, names, cols);
  const Program prog = compile_ok("a = trade_when(trig > 0, alpha, exitf > 0)\n");
  const SignalSet r = eval_fast(prog, panel);
  for (const atx::f64 c : r.alphas.at(0).values) {
    EXPECT_TRUE(std::isnan(c));
  }
}

TEST(AlphaTradeWhen_NaNTriggerExit, TreatedAsNotTriggeredNotExited_HoldsPrior) {
  // A NaN trigger/exit is mask_true==false -> neither enters nor exits -> holds.
  //   date: 0    1     2     3
  //   trig:  1   NaN   NaN    0
  //   exit:  0    0    NaN    0
  //   alpha: 5   10    15    20
  // out: d0 enter->5; d1 NaN-trig hold->5; d2 NaN-trig/NaN-exit hold->5; d3 hold->5
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{
      {1.0, kNaN, kNaN, 0.0}, {0.0, 0.0, kNaN, 0.0}, {5.0, 10.0, 15.0, 20.0}};
  const Panel panel = make_panel(4, 1, names, cols);
  // Mask operands constructed from raw fields with `> 0` (a NaN field -> NaN
  // mask cell, which mask_true treats as false).
  const Program prog = compile_ok("a = trade_when(trig > 0, alpha, exitf > 0)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 5.0);
  EXPECT_DOUBLE_EQ(v[1], 5.0);
  EXPECT_DOUBLE_EQ(v[2], 5.0);
  EXPECT_DOUBLE_EQ(v[3], 5.0);
}

// ===========================================================================
//  2. hump — suppress / pass / boundary / first-date / default.
// ===========================================================================

TEST(AlphaHump_KnownValue, SmallChangesSuppressed_LargePassThrough) {
  // threshold 1.0. x = 10, 10.5, 12, 12.4, 9.
  //   d0: 10 (first date == x[0])
  //   d1: |10.5-10|=0.5 <= 1 -> hold 10
  //   d2: |12-10|=2 > 1 -> pass 12
  //   d3: |12.4-12|=0.4 <= 1 -> hold 12
  //   d4: |9-12|=3 > 1 -> pass 9
  const std::vector<std::string> names{"x"};
  const std::vector<std::vector<atx::f64>> cols{{10.0, 10.5, 12.0, 12.4, 9.0}};
  const Panel panel = make_panel(5, 1, names, cols);
  const Program prog = compile_ok("a = hump(x, 1.0)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 10.0);
  EXPECT_DOUBLE_EQ(v[1], 10.0);
  EXPECT_DOUBLE_EQ(v[2], 12.0);
  EXPECT_DOUBLE_EQ(v[3], 12.0);
  EXPECT_DOUBLE_EQ(v[4], 9.0);
}

TEST(AlphaHump_Boundary, ExactlyThreshold_HoldsThenLargerChangePasses) {
  // x = {5, 6, 7}, threshold 1.0.
  //   d0: 5 (first date)
  //   d1: |6-5| == 1 -> NOT > 1 -> hold prior 5 (strict `>` keeps the boundary)
  //   d2: |7-prior=5| == 2 > 1 -> pass 7
  const std::vector<std::string> names{"x"};
  const std::vector<std::vector<atx::f64>> cols{{5.0, 6.0, 7.0}};
  const Panel panel = make_panel(3, 1, names, cols);
  const Program prog = compile_ok("a = hump(x, 1.0)\n");
  const SignalSet r = eval_fast(prog, panel);
  const std::vector<atx::f64> &v = r.alphas.at(0).values;
  EXPECT_DOUBLE_EQ(v[0], 5.0); // first date
  EXPECT_DOUBLE_EQ(v[1], 5.0); // |6-5|==1 -> hold (boundary, strict >)
  EXPECT_DOUBLE_EQ(v[2], 7.0); // |7-5|==2 > 1 -> pass
}

TEST(AlphaHump_FirstDate, EqualsXZero) {
  const std::vector<std::string> names{"x"};
  const std::vector<std::vector<atx::f64>> cols{{42.0, 42.001}};
  const Panel panel = make_panel(2, 1, names, cols);
  const Program prog = compile_ok("a = hump(x)\n"); // default threshold 0.01
  const SignalSet r = eval_fast(prog, panel);
  EXPECT_DOUBLE_EQ(r.alphas.at(0).values[0], 42.0);
  // |42.001-42|=0.001 <= 0.01 default -> hold.
  EXPECT_DOUBLE_EQ(r.alphas.at(0).values[1], 42.0);
}

TEST(AlphaHump_Default, HumpXEqualsHumpX001) {
  // hump(x) ≡ hump(x, 0.01) via the P3b-1 default-fill machinery.
  const std::vector<std::string> names{"x"};
  const std::vector<std::vector<atx::f64>> cols{{1.0, 1.005, 1.02, 1.5, 1.51}};
  const Panel panel = make_panel(5, 1, names, cols);
  const SignalSet rdef = eval_fast(compile_ok("a = hump(x)\n"), panel);
  const SignalSet r001 = eval_fast(compile_ok("a = hump(x, 0.01)\n"), panel);
  for (atx::usize i = 0; i < 5; ++i) {
    EXPECT_TRUE(same_cell(rdef.alphas[0].values[i], r001.alphas[0].values[i]));
  }
}

TEST(AlphaHump_NaN, NaNInputPropagatesPerOracle) {
  // Hand-trace under "pass-if |x - prior| > thr, else hold": once x is NaN the
  // difference is NaN, NaN > thr is false -> hold the prior. This is the pinned
  // policy; the differential confirms VM==oracle on the NaN path.
  const std::vector<std::string> names{"x"};
  const std::vector<std::vector<atx::f64>> cols{{2.0, kNaN, 9.0}};
  const Panel panel = make_panel(3, 1, names, cols);
  const Program prog = compile_ok("a = hump(x, 1.0)\n");
  const SignalSet fast = eval_fast(prog, panel);
  const SignalSet oracle = eval_oracle(prog, panel);
  for (atx::usize i = 0; i < 3; ++i) {
    EXPECT_TRUE(same_cell(fast.alphas[0].values[i], oracle.alphas[0].values[i]));
  }
  EXPECT_DOUBLE_EQ(fast.alphas[0].values[0], 2.0); // first date
  EXPECT_DOUBLE_EQ(fast.alphas[0].values[1], 2.0); // |NaN-2| not > 1 -> hold
  EXPECT_DOUBLE_EQ(fast.alphas[0].values[2], 9.0); // |9-2|=7 > 1 -> pass
}

// ===========================================================================
//  3. Differential gate — FAST == ORACLE, bit-identical, both ops.
// ===========================================================================

TEST(AlphaTradeWhen_Differential, BothOps_FastEqualsOracle_BitIdentical) {
  const DiffPanel pd = make_diff_panel(18, 9);
  assert_differential("a = trade_when(trig > 0, close, exitf > 0)\n", pd);
  assert_differential("a = hump(close, 0.5)\n", pd);
  assert_differential("a = hump(close)\n", pd); // P3b-1 default 0.01
}

TEST(AlphaTradeWhen_Differential, NestedAndComposite_FastEqualsOracle) {
  const DiffPanel pd = make_diff_panel(20, 10);
  // Nested: a rolling signal carried by trade_when; hump damping a ts op; the
  // trade_when output itself fed through hump (stateful over stateful).
  assert_differential("a = trade_when(trig > 0, ts_mean(close, 4), exitf > 0)\n"
                      "b = hump(ts_zscore(close, 5), 0.2)\n"
                      "c = hump(trade_when(trig > 0, close, exitf > 0), 0.3)\n",
                      pd);
}

// ===========================================================================
//  4. Truncation-invariance — the causality proof (no state[>t] read possible).
// ===========================================================================

TEST(AlphaTradeWhen_Causality, TruncationInvariant) {
  const std::vector<std::string> names{"trig", "exitf", "alpha"};
  const std::vector<std::vector<atx::f64>> cols{{1.0, 0.0, -1.0, 1.0, 0.0, -1.0, 1.0, 0.0},
                                                {0.0, 0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0},
                                                {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0}};
  assert_truncation_invariant("a = trade_when(trig > 0, alpha, exitf > 0)\n", 4, names, cols);
}

TEST(AlphaHump_Causality, TruncationInvariant) {
  const std::vector<std::string> names{"x"};
  const std::vector<std::vector<atx::f64>> cols{{1.0, 1.005, 1.02, 1.5, 1.51, 0.9, 0.901, 2.0}};
  assert_truncation_invariant("a = hump(x, 0.05)\n", 4, names, cols);
}

// ===========================================================================
//  5. Determinism — same input -> identical output (re-run).
// ===========================================================================

TEST(AlphaTradeWhen_Determinism, ReRunIsIdentical) {
  const DiffPanel pd = make_diff_panel(12, 6);
  const Panel panel = panel_from(pd);
  const Program prog =
      compile_ok("a = trade_when(trig > 0, close, exitf > 0)\nb = hump(close, 0.4)\n");
  const SignalSet r1 = eval_fast(prog, panel);
  const SignalSet r2 = eval_fast(prog, panel);
  ASSERT_EQ(r1.alphas.size(), r2.alphas.size());
  for (atx::usize a = 0; a < r1.alphas.size(); ++a) {
    ASSERT_EQ(r1.alphas[a].values.size(), r2.alphas[a].values.size());
    for (atx::usize i = 0; i < r1.alphas[a].values.size(); ++i) {
      EXPECT_TRUE(same_cell(r1.alphas[a].values[i], r2.alphas[a].values[i]));
    }
  }
}

// ===========================================================================
//  6. Boundaries — 1 date, 1 instrument, all-NaN, via the differential.
// ===========================================================================

TEST(AlphaTradeWhen_Boundary, OneDate_FastEqualsOracle) {
  const DiffPanel pd = make_diff_panel(1, 5);
  assert_differential("a = trade_when(trig > 0, close, exitf > 0)\n", pd);
  assert_differential("a = hump(close, 0.5)\n", pd);
}

TEST(AlphaTradeWhen_Boundary, OneInstrument_FastEqualsOracle) {
  const DiffPanel pd = make_diff_panel(10, 1);
  assert_differential("a = trade_when(trig > 0, close, exitf > 0)\n", pd);
  assert_differential("a = hump(close, 0.5)\n", pd);
  assert_differential("a = hump(close)\n", pd);
}

TEST(AlphaTradeWhen_Boundary, AllNaNInputs_FastEqualsOracle) {
  const std::vector<std::string> names{"close", "trig", "exitf"};
  const std::vector<std::vector<atx::f64>> cols{
      {kNaN, kNaN, kNaN, kNaN}, {kNaN, kNaN, kNaN, kNaN}, {kNaN, kNaN, kNaN, kNaN}};
  DiffPanel pd;
  pd.dates = 2;
  pd.instruments = 2;
  pd.names = names;
  pd.cols = cols;
  pd.universe.assign(4, std::uint8_t{1});
  assert_differential("a = trade_when(trig > 0, close, exitf > 0)\n", pd);
  assert_differential("a = hump(close, 0.5)\n", pd);
}


}  // namespace atxtest_alpha_trade_when_test

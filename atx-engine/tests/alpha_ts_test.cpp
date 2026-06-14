// atx::engine::alpha — time-series VM kernel differential tests (P3-8).
//
// The VM's time-series opcodes (vm.hpp via ts_ops.hpp) are the PRODUCTION path;
// oracle.hpp is the slow, obviously-correct reference. These tests run BOTH on
// identical inputs and assert bit-for-bit agreement (NaN==NaN treated as equal)
// for every Ts* opcode — the differential is exactly what catches a kernel that
// drifts from the pinned semantic contract (summation order, argmax tie-break,
// ema seeding, ddof). Plus:
//   * hand-computed known-value checks (delay/ts_mean warm-up NaN then correct,
//     ts_sum, delta, a known argmax peak);
//   * causality / lookahead: row t depends ONLY on rows <= t (truncate the
//     panel after date t, confirm rows <= t are unchanged — the §3.3 rail);
//   * NaN policy (a NaN in the window -> that and dependent cells NaN; short
//     window -> NaN);
//   * boundaries (d > date count -> all NaN; d=1; nested ts ops).
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

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/ts_ops.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_ts_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// A process-lifetime Library so any borrowed OpSig stays valid across tests.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN, or exactly value-equal (covers ±inf, ±0).
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Compile a bare expression all the way to a Program (parse -> analyze ->
// compile). On any failure the caller's EXPECT surfaces the message.
[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// Build a Panel with the OHLCV fields plus an `IndClass.sector` Group field.
// Field order: close/open/high/low/volume, then IndClass.sector. An empty
// universe means all cells in-universe.
[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments,
                               std::vector<std::vector<atx::f64>> cols,
                               std::vector<std::uint8_t> universe = {}) {
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  auto p =
      Panel::create(dates, instruments, std::move(names), std::move(cols), std::move(universe));
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Fill five OHLCV columns + a sector classifier from a fixed-seed RNG. Prices in
// [1,100], volume positive, sector in {0,1,2}. Deterministic.
[[nodiscard]] std::vector<std::vector<atx::f64>> random_cols(atx::usize cells, std::uint64_t seed) {
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{1.0, 100.0};
  std::uniform_real_distribution<atx::f64> vol{1.0, 1.0e6};
  std::uniform_int_distribution<int> sector{0, 2};
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = price(rng); // close
    cols[1][i] = price(rng); // open
    cols[2][i] = price(rng); // high
    cols[3][i] = price(rng); // low
    cols[4][i] = vol(rng);   // volume
    cols[5][i] = static_cast<atx::f64>(sector(rng));
  }
  return cols;
}

// The core differential assertion: VM == oracle, cell by cell.
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
  ASSERT_EQ(v.dates, r.dates);
  ASSERT_EQ(v.instruments, r.instruments);
  for (atx::usize a = 0; a < v.alphas.size(); ++a) {
    ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
    for (atx::usize i = 0; i < v.alphas[a].values.size(); ++i) {
      const atx::f64 vc = v.alphas[a].values[i];
      const atx::f64 rc = r.alphas[a].values[i];
      EXPECT_TRUE(same_cell(vc, rc)) << "expr '" << expr << "' alpha " << a << " cell " << i
                                     << ": VM=" << vc << " oracle=" << rc;
    }
  }
}

// Run the VM and return the single root's date-major values (for known-value
// asserts). The caller's ASSERT surfaces any evaluate() error via an empty vec.
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
//  Differential — every Ts* opcode on a seeded-random multi-date panel, for a
//  couple of window sizes so warm-up and full windows are both exercised.
// ===========================================================================

TEST(AlphaTs_Differential, EveryTimeSeriesOp_RandomPanel_MatchesOracle) {
  const atx::usize dates = 14;
  const atx::usize instruments = 6;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0x751DEAULL));

  // Unary-series ops parameterized over a window size.
  const std::vector<std::string_view> unary = {
      "delay(close, {d})",        "delta(close, {d})",   "ts_sum(close, {d})",
      "ts_mean(close, {d})",      "stddev(close, {d})",  "ts_var(close, {d})",
      "ts_min(close, {d})",       "ts_max(close, {d})",  "ts_argmin(close, {d})",
      "ts_argmax(close, {d})",    "ts_rank(close, {d})", "product(close, {d})",
      "decay_linear(close, {d})", "ema(close, {d})",     "wma(close, {d})",
      "skew(close, {d})",         "kurt(close, {d})",    "med(close, {d})",
      "mad(close, {d})",          "slope(close, {d})",   "rsquare(close, {d})",
      "resid(close, {d})",
  };
  const std::vector<std::string_view> binary = {
      "correlation(close, volume, {d})",
      "covariance(close, volume, {d})",
  };
  for (const std::string_view dlit : {std::string_view{"3"}, std::string_view{"5"}}) {
    for (const std::string_view tmpl : unary) {
      std::string e{tmpl};
      const auto pos = e.find("{d}");
      e.replace(pos, 3, dlit);
      expect_vm_matches_oracle(e, panel);
    }
    for (const std::string_view tmpl : binary) {
      std::string e{tmpl};
      const auto pos = e.find("{d}");
      e.replace(pos, 3, dlit);
      expect_vm_matches_oracle(e, panel);
    }
  }
}

// ===========================================================================
//  Known-value — hand-computed on a small single-instrument panel.
// ===========================================================================

// A single instrument so the trailing window is the literal close series:
// close = [10, 12, 11, 15, 14] over 5 dates.
[[nodiscard]] Panel single_instrument_panel(std::vector<atx::f64> close) {
  const atx::usize dates = close.size();
  auto cols = random_cols(dates, 0x1ULL);
  cols[0] = std::move(close);
  return make_panel(dates, 1, std::move(cols));
}

// delay(close, 1): row 0 is NaN (no prior), then close shifted by one date.
TEST(AlphaTs_Delay, OneStep_WarmupNaNThenShifted) {
  const Panel panel = single_instrument_panel({10.0, 12.0, 11.0, 15.0, 14.0});
  const std::vector<atx::f64> v = vm_values("delay(close, 1)", panel);
  ASSERT_EQ(v.size(), 5u);
  EXPECT_TRUE(std::isnan(v[0]));
  EXPECT_DOUBLE_EQ(v[1], 10.0);
  EXPECT_DOUBLE_EQ(v[2], 12.0);
  EXPECT_DOUBLE_EQ(v[3], 11.0);
  EXPECT_DOUBLE_EQ(v[4], 15.0);
}

// delta(close, 1): row 0 NaN, then x[t] - x[t-1].
TEST(AlphaTs_Delta, OneStep_FirstDifference) {
  const Panel panel = single_instrument_panel({10.0, 12.0, 11.0, 15.0, 14.0});
  const std::vector<atx::f64> v = vm_values("delta(close, 1)", panel);
  ASSERT_EQ(v.size(), 5u);
  EXPECT_TRUE(std::isnan(v[0]));
  EXPECT_DOUBLE_EQ(v[1], 2.0);
  EXPECT_DOUBLE_EQ(v[2], -1.0);
  EXPECT_DOUBLE_EQ(v[3], 4.0);
  EXPECT_DOUBLE_EQ(v[4], -1.0);
}

// ts_mean(close, 2): row 0 NaN (window incomplete), then trailing 2-mean.
TEST(AlphaTs_Mean, Window2_WarmupNaNThenTrailingMean) {
  const Panel panel = single_instrument_panel({10.0, 12.0, 11.0, 15.0, 14.0});
  const std::vector<atx::f64> v = vm_values("ts_mean(close, 2)", panel);
  ASSERT_EQ(v.size(), 5u);
  EXPECT_TRUE(std::isnan(v[0]));
  EXPECT_DOUBLE_EQ(v[1], 11.0); // (10+12)/2
  EXPECT_DOUBLE_EQ(v[2], 11.5); // (12+11)/2
  EXPECT_DOUBLE_EQ(v[3], 13.0); // (11+15)/2
  EXPECT_DOUBLE_EQ(v[4], 14.5); // (15+14)/2
}

// ts_sum(close, 3): first two rows NaN, then trailing 3-sum.
TEST(AlphaTs_Sum, Window3_TrailingSum) {
  const Panel panel = single_instrument_panel({10.0, 12.0, 11.0, 15.0, 14.0});
  const std::vector<atx::f64> v = vm_values("ts_sum(close, 3)", panel);
  ASSERT_EQ(v.size(), 5u);
  EXPECT_TRUE(std::isnan(v[0]));
  EXPECT_TRUE(std::isnan(v[1]));
  EXPECT_DOUBLE_EQ(v[2], 33.0); // 10+12+11
  EXPECT_DOUBLE_EQ(v[3], 38.0); // 12+11+15
  EXPECT_DOUBLE_EQ(v[4], 40.0); // 11+15+14
}

// ts_argmax(close, 3): 1-based position of the FIRST max in the trailing window.
// Window at t=4 is [11,15,14] -> max at offset 2 (1-based). At t=3 [12,11,15]
// -> max at offset 3. At t=2 [10,12,11] -> max at offset 2.
TEST(AlphaTs_ArgMax, KnownPeak_OneBasedFirstExtreme) {
  const Panel panel = single_instrument_panel({10.0, 12.0, 11.0, 15.0, 14.0});
  const std::vector<atx::f64> v = vm_values("ts_argmax(close, 3)", panel);
  ASSERT_EQ(v.size(), 5u);
  EXPECT_TRUE(std::isnan(v[0]));
  EXPECT_TRUE(std::isnan(v[1]));
  EXPECT_DOUBLE_EQ(v[2], 2.0);
  EXPECT_DOUBLE_EQ(v[3], 3.0);
  EXPECT_DOUBLE_EQ(v[4], 2.0);
}

// argmax tie-break: the FIRST (oldest) of equal maxima wins. Window [5,5,3]
// at t=2 -> offset 1.
TEST(AlphaTs_ArgMax, TiedMax_FirstWins) {
  const Panel panel = single_instrument_panel({5.0, 5.0, 3.0});
  const std::vector<atx::f64> v = vm_values("ts_argmax(close, 3)", panel);
  ASSERT_EQ(v.size(), 3u);
  EXPECT_DOUBLE_EQ(v[2], 1.0);
}

// ===========================================================================
//  Causality / lookahead — row t depends ONLY on rows <= t (the §3.3 rail).
//  Evaluate on the full panel, then on the panel truncated after date t; the
//  surviving rows must be bit-identical (no future leakage).
// ===========================================================================

TEST(AlphaTs_Causality, TruncatedPanel_RowsUpToCutoffUnchanged) {
  const atx::usize full_dates = 12;
  const atx::usize instruments = 4;
  const auto cols_full = random_cols(full_dates * instruments, 0xCA0541ULL);
  const Panel full = make_panel(full_dates, instruments, cols_full);

  const std::vector<std::string_view> exprs = {
      "ts_mean(close, 4)",      "stddev(close, 4)",
      "ts_argmax(close, 4)",    "correlation(close, volume, 4)",
      "decay_linear(close, 4)", "ts_rank(close, 4)",
  };
  for (const atx::usize cutoff : {atx::usize{5}, atx::usize{8}}) {
    // Truncate every field column to the first `cutoff` dates.
    std::vector<std::vector<atx::f64>> cut(cols_full.size());
    for (atx::usize f = 0; f < cols_full.size(); ++f) {
      cut[f].assign(cols_full[f].begin(),
                    cols_full[f].begin() + static_cast<std::ptrdiff_t>(cutoff * instruments));
    }
    const Panel truncated = make_panel(cutoff, instruments, std::move(cut));
    for (const std::string_view e : exprs) {
      const std::vector<atx::f64> vf = vm_values(e, full);
      const std::vector<atx::f64> vt = vm_values(e, truncated);
      ASSERT_EQ(vt.size(), cutoff * instruments) << e;
      for (atx::usize i = 0; i < cutoff * instruments; ++i) {
        EXPECT_TRUE(same_cell(vf[i], vt[i]))
            << "expr '" << e << "' cutoff " << cutoff << " cell " << i << ": full=" << vf[i]
            << " truncated=" << vt[i] << " (future leakage)";
      }
    }
  }
}

// ===========================================================================
//  NaN policy — full-window any-NaN -> NaN; short window -> NaN.
// ===========================================================================

// A NaN in the middle of the series (out-of-universe cell) NaNs every window
// that overlaps it; rows whose trailing window clears the NaN recover. Single
// instrument, date 2 knocked out of universe.
TEST(AlphaTs_NaNPolicy, NaNInWindow_PropagatesUntilWindowClears) {
  const atx::usize dates = 6;
  auto cols = random_cols(dates, 0x4A4ULL);
  cols[0] = {10.0, 11.0, 12.0, 13.0, 14.0, 15.0};
  std::vector<std::uint8_t> universe(dates, std::uint8_t{1});
  universe[2] = 0; // date 2 out-of-universe -> close reads NaN there
  const Panel panel = make_panel(dates, 1, std::move(cols), std::move(universe));

  const std::vector<atx::f64> v = vm_values("ts_mean(close, 2)", panel);
  ASSERT_EQ(v.size(), dates);
  EXPECT_TRUE(std::isnan(v[0])); // short window
  EXPECT_DOUBLE_EQ(v[1], 10.5);  // (10+11)/2, clean
  EXPECT_TRUE(std::isnan(v[2])); // window [11,NaN] -> NaN
  EXPECT_TRUE(std::isnan(v[3])); // window [NaN,13] -> NaN
  EXPECT_DOUBLE_EQ(v[4], 13.5);  // window [13,14], NaN cleared
  EXPECT_DOUBLE_EQ(v[5], 14.5);  // window [14,15]
  // The VM must still agree with the oracle on the NaN pattern.
  expect_vm_matches_oracle("ts_mean(close, 2)", panel);
  expect_vm_matches_oracle("correlation(close, volume, 3)", panel);
}

// Short window: when t+1 < d every cell up to the first full window is NaN.
TEST(AlphaTs_NaNPolicy, ShortWindow_LeadingRowsNaN) {
  const Panel panel = single_instrument_panel({1.0, 2.0, 3.0, 4.0, 5.0});
  const std::vector<atx::f64> v = vm_values("ts_sum(close, 4)", panel);
  ASSERT_EQ(v.size(), 5u);
  for (atx::usize t = 0; t < 3; ++t) {
    EXPECT_TRUE(std::isnan(v[t])) << "row " << t << " short window must be NaN";
  }
  EXPECT_DOUBLE_EQ(v[3], 10.0); // 1+2+3+4
  EXPECT_DOUBLE_EQ(v[4], 14.0); // 2+3+4+5
}

// ===========================================================================
//  Boundary — d > date count, d=1, nested ts.
// ===========================================================================

// A window larger than the number of dates: no full window ever exists -> all
// NaN. Confirm the VM matches the oracle on the degenerate shape too.
TEST(AlphaTs_Boundary, WindowExceedsDates_AllNaN) {
  const atx::usize dates = 3;
  const atx::usize instruments = 2;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0xB16ULL));
  const std::vector<atx::f64> v = vm_values("ts_mean(close, 9)", panel);
  ASSERT_EQ(v.size(), dates * instruments);
  for (const atx::f64 c : v) {
    EXPECT_TRUE(std::isnan(c));
  }
  expect_vm_matches_oracle("ts_mean(close, 9)", panel);
  expect_vm_matches_oracle("ts_sum(close, 9)", panel);
}

// d=1: a single-element window. mean/sum == the value; var/std NaN (n<2);
// argmax == 1; rank singleton == 0.5.
TEST(AlphaTs_Boundary, WindowOne_SingletonSemantics) {
  const Panel panel = single_instrument_panel({7.0, 8.0, 9.0});
  const std::vector<atx::f64> mean = vm_values("ts_mean(close, 1)", panel);
  const std::vector<atx::f64> var = vm_values("ts_var(close, 1)", panel);
  const std::vector<atx::f64> rank = vm_values("ts_rank(close, 1)", panel);
  const std::vector<atx::f64> amax = vm_values("ts_argmax(close, 1)", panel);
  ASSERT_EQ(mean.size(), 3u);
  for (atx::usize t = 0; t < 3; ++t) {
    EXPECT_FALSE(std::isnan(mean[t]));
    EXPECT_TRUE(std::isnan(var[t])); // sample var needs 2+
    EXPECT_DOUBLE_EQ(rank[t], 0.5);  // singleton
    EXPECT_DOUBLE_EQ(amax[t], 1.0);
  }
  EXPECT_DOUBLE_EQ(mean[1], 8.0);
  // Differential on the degenerate window too.
  expect_vm_matches_oracle("ts_mean(close, 1)", panel);
  expect_vm_matches_oracle("ts_var(close, 1)", panel);
}

// Nested time-series: ts_mean of a delta — proves a Ts result composes as the
// input of another Ts op inside the DAG, still matching the oracle.
TEST(AlphaTs_Boundary, NestedTimeSeries_MatchesOracle) {
  const atx::usize dates = 13;
  const atx::usize instruments = 5;
  const Panel panel = make_panel(dates, instruments, random_cols(dates * instruments, 0x4E57EDULL));
  expect_vm_matches_oracle("ts_mean(delta(close, 2), 3)", panel);
  expect_vm_matches_oracle("ts_sum(ts_mean(close, 2), 4)", panel);
  expect_vm_matches_oracle("rank(ts_mean(close, 3))", panel);
}


}  // namespace atxtest_alpha_ts_test

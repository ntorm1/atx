// atx::engine::alpha — online variance-family kernel tests (p7 S3-1 / S3-2).
//
// S3-1 restores O(T) rolling kernels for TsVar/TsStd via Welford's online
// variance + Neumaier-compensated mean, and S3-2 extends the family to
// TsZscore/TsAvDiff. These ship behind EvalMode::ResearchFast: the Welford
// accumulation order differs from the batch oracle's chronological two-pass, so
// the kernels are NOT bit-identical to the oracle, but are provably MORE accurate
// (no Sx^2 catastrophic cancellation). The default EvalMode::AuditExact path
// keeps the batch kernel and stays byte-identical to pre-sprint.
//
// What these tests prove:
//   * Catastrophic-cancellation regression: a high-mean/low-variance window
//     (mean ~1e7, std ~1.0) — the exact case the Task-7 revert reacted to — is
//     recovered to within 1e-9 by BOTH the Welford kernel and the batch oracle.
//   * Constant window -> var == 0.0 exactly (no spurious negative -> no NaN std).
//   * A 500x200 random panel: ResearchFast var/std/zscore agree with the batch
//     oracle within atol=rtol=1e-9 on every finite cell; av_diff within atol=1e-7.
//   * Off-path byte-identity: an AuditExact Engine produces a byte-identical
//     SignalSet to the pre-sprint batch path (the online branch never fires).
//   * Twice-run determinism: ResearchFast eval is reproducible.
//
// Naming: Subject_Condition_ExpectedResult.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
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
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_ts_online_variance {

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
using atx::engine::alpha::SignalSet;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Two cells agree iff both NaN, or exactly value-equal (covers +/-inf, +/-0).
[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Tolerance compare with a matching NaN pattern (NaN==NaN, finite-vs-NaN fails).
[[nodiscard]] bool close_cell(atx::f64 a, atx::f64 b, atx::f64 atol, atx::f64 rtol) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  if (std::isnan(a) != std::isnan(b)) {
    return false;
  }
  return std::fabs(a - b) <= atol + rtol * std::fabs(b);
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

// Panel with the OHLCV + sector fields; `close` is the column under test.
[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments,
                               std::vector<std::vector<atx::f64>> cols) {
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  auto p = Panel::create(dates, instruments, std::move(names), std::move(cols), {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Six OHLCV+sector columns from a fixed-seed RNG (prices in [1,100], volume
// positive). The close column is overwritten by the caller for the pathological
// fixtures; the other columns just keep the panel well-formed.
[[nodiscard]] std::vector<std::vector<atx::f64>> base_cols(atx::usize cells, std::uint64_t seed) {
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{1.0, 100.0};
  std::uniform_real_distribution<atx::f64> vol{1.0, 1.0e6};
  std::uniform_int_distribution<int> sector{0, 2};
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = price(rng);
    cols[1][i] = price(rng);
    cols[2][i] = price(rng);
    cols[3][i] = price(rng);
    cols[4][i] = vol(rng);
    cols[5][i] = static_cast<atx::f64>(sector(rng));
  }
  return cols;
}

// Evaluate one expression on `panel` under the given EvalMode; return the single
// root's date-major values. The caller's ASSERT surfaces any evaluate() error.
[[nodiscard]] std::vector<atx::f64> vm_values(std::string_view expr, const Panel &panel,
                                              EvalMode mode) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  engine.set_eval_mode(mode);
  EXPECT_EQ(engine.eval_mode(), mode);
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  if (!out.has_value() || out.value().alphas.empty()) {
    return {};
  }
  return out.value().alphas[0].values;
}

[[nodiscard]] std::vector<atx::f64> oracle_values(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  auto out = evaluate_reference(prog, panel);
  EXPECT_TRUE(out.has_value()) << "oracle: " << (out ? "" : out.error().message());
  if (!out.has_value() || out.value().alphas.empty()) {
    return {};
  }
  return out.value().alphas[0].values;
}

// ===========================================================================
//  Direct kernel-level checks (tsv_welford_var_col / tsv_welford_std_col) on a
//  single instrument column so the trailing window is the literal series.
// ===========================================================================

// Pathological near-constant: mean ~1e7, std ~1.0. The previous Sx^2 rolling
// variance lost ~8 sig digits here; Welford must recover to within 1e-9 (atol).
TEST(TsWelfordVar, NearConstantHighMean_MatchesTruth) {
  const atx::usize dates = 8;
  const atx::usize d = 5;
  // close[s] = 1e7 + small[s]; the in-window sample variance of `small` is the
  // true variance (an additive constant does not change variance).
  const std::vector<atx::f64> small = {0.3, -0.7, 0.1, 0.9, -0.5, 0.2, -0.4, 0.6};
  std::vector<atx::f64> x(dates);
  for (atx::usize s = 0; s < dates; ++s) {
    x[s] = 1.0e7 + small[s];
  }
  // Truth: sample variance (ddof=1) of the last `d` of `small` at the final cell.
  const atx::usize t = dates - 1;
  atx::f64 m = 0.0;
  for (atx::usize s = t + 1 - d; s <= t; ++s) {
    m += small[s];
  }
  m /= static_cast<atx::f64>(d);
  atx::f64 ss = 0.0;
  for (atx::usize s = t + 1 - d; s <= t; ++s) {
    ss += (small[s] - m) * (small[s] - m);
  }
  const atx::f64 truth_var = ss / static_cast<atx::f64>(d - 1);

  std::vector<atx::f64> out_var(dates, 0.0);
  std::vector<atx::f64> out_std(dates, 0.0);
  atx::engine::alpha::detail::tsv_welford_var_col(x, out_var, dates, /*j=*/0, d, /*instruments=*/1);
  atx::engine::alpha::detail::tsv_welford_std_col(x, out_std, dates, /*j=*/0, d, /*instruments=*/1);

  EXPECT_NEAR(out_var[t], truth_var, 1e-9) << "Welford var lost precision on high-mean window";
  EXPECT_NEAR(out_std[t], std::sqrt(truth_var), 1e-9);
  // The batch kernel must also be within 1e-9 of truth (it is the oracle ref).
  const atx::f64 batch_var =
      atx::engine::alpha::detail::tsv_var(x, t, /*j=*/0, d, /*instruments=*/1);
  EXPECT_NEAR(batch_var, truth_var, 1e-9);
  EXPECT_NEAR(out_var[t], batch_var, 1e-9) << "Welford and batch must agree";
}

// Constant window -> var == 0.0 exactly (no spurious negative -> std finite 0).
TEST(TsWelfordVar, ConstantWindow_ExactlyZero) {
  const atx::usize dates = 6;
  const atx::usize d = 4;
  const atx::f64 C = 1234.5;
  std::vector<atx::f64> x(dates, C);
  std::vector<atx::f64> out_var(dates, -1.0);
  std::vector<atx::f64> out_std(dates, -1.0);
  atx::engine::alpha::detail::tsv_welford_var_col(x, out_var, dates, 0, d, 1);
  atx::engine::alpha::detail::tsv_welford_std_col(x, out_std, dates, 0, d, 1);
  for (atx::usize t = d - 1; t < dates; ++t) {
    EXPECT_EQ(out_var[t], 0.0) << "constant window var must be exactly 0 at t=" << t;
    EXPECT_EQ(out_std[t], 0.0) << "constant window std must be exactly 0 at t=" << t;
    EXPECT_FALSE(std::signbit(out_var[t])) << "no negative zero / spurious negative";
  }
  // Warm-up (incomplete window) -> NaN, same gate as the batch path.
  for (atx::usize t = 0; t + 1 < d; ++t) {
    EXPECT_TRUE(std::isnan(out_var[t]));
    EXPECT_TRUE(std::isnan(out_std[t]));
  }
}

// Kernel reproduces the batch NaN gate (short window OR any-NaN -> NaN). The
// batch reference is ts_value_at (the GATED per-cell entry the AuditExact VM uses)
// — NOT raw tsv_var, which assumes an already-validated window.
TEST(TsWelfordVar, NaNInWindow_PropagatesNaNGate) {
  const atx::usize dates = 7;
  const atx::usize d = 3;
  std::vector<atx::f64> x = {1.0, 2.0, std::nan(""), 4.0, 5.0, 6.0, 7.0};
  std::vector<atx::f64> out_var(dates, 0.0);
  atx::engine::alpha::detail::tsv_welford_var_col(x, out_var, dates, 0, d, 1);
  std::vector<atx::f64> sort_buf(d, 0.0);
  for (atx::usize t = 0; t < dates; ++t) {
    const atx::f64 batch =
        atx::engine::alpha::detail::ts_value_at(OpCode::TsVar, x, t, 0, d, 1, sort_buf, 0.0);
    EXPECT_TRUE(close_cell(out_var[t], batch, 1e-12, 1e-12))
        << "t=" << t << " welford=" << out_var[t] << " batch=" << batch;
  }
}

// ===========================================================================
//  Engine-level: ResearchFast var/std vs batch oracle on a random panel.
// ===========================================================================

TEST(TsWelfordVar, RandomPanel_WithinToleranceOfOracle) {
  const atx::usize dates = 500;
  const atx::usize instruments = 200;
  const atx::usize cells = dates * instruments;
  const Panel panel = make_panel(dates, instruments, base_cols(cells, 0xA1FA101ULL));
  for (const std::string_view expr : {std::string_view{"ts_var(close, 20)"},
                                      std::string_view{"ts_std(close, 20)"}}) {
    const std::vector<atx::f64> fast = vm_values(expr, panel, EvalMode::ResearchFast);
    const std::vector<atx::f64> oracle = oracle_values(expr, panel);
    ASSERT_EQ(fast.size(), oracle.size());
    for (atx::usize i = 0; i < fast.size(); ++i) {
      EXPECT_TRUE(close_cell(fast[i], oracle[i], 1e-9, 1e-9))
          << expr << " cell " << i << " fast=" << fast[i] << " oracle=" << oracle[i];
    }
  }
}

// ===========================================================================
//  Off-path byte-identity: AuditExact (default) == batch oracle, bit-for-bit.
// ===========================================================================

TEST(TsAuditExactVar, DefaultMode_ByteIdenticalToOracle) {
  const atx::usize dates = 60;
  const atx::usize instruments = 16;
  const atx::usize cells = dates * instruments;
  const Panel panel = make_panel(dates, instruments, base_cols(cells, 0xBEEF01ULL));
  for (const std::string_view expr : {std::string_view{"ts_var(close, 10)"},
                                      std::string_view{"ts_std(close, 10)"}}) {
    const std::vector<atx::f64> audit = vm_values(expr, panel, EvalMode::AuditExact);
    const std::vector<atx::f64> oracle = oracle_values(expr, panel);
    ASSERT_EQ(audit.size(), oracle.size());
    for (atx::usize i = 0; i < audit.size(); ++i) {
      EXPECT_TRUE(same_cell(audit[i], oracle[i]))
          << expr << " cell " << i << " audit=" << audit[i] << " oracle=" << oracle[i];
    }
  }
}

// Default-constructed Engine is AuditExact (the inert default).
TEST(TsEvalMode, DefaultIsAuditExact) {
  const Panel panel = make_panel(4, 2, base_cols(8, 1));
  Engine engine{panel};
  EXPECT_EQ(engine.eval_mode(), EvalMode::AuditExact);
}

// ResearchFast eval is reproducible (no hidden state across runs).
TEST(TsWelfordVar, ResearchFast_TwiceRunIdentical) {
  const atx::usize dates = 40;
  const atx::usize instruments = 12;
  const Panel panel = make_panel(dates, instruments, base_cols(dates * instruments, 0x2702ULL));
  const std::vector<atx::f64> a = vm_values("ts_std(close, 7)", panel, EvalMode::ResearchFast);
  const std::vector<atx::f64> b = vm_values("ts_std(close, 7)", panel, EvalMode::ResearchFast);
  ASSERT_EQ(a.size(), b.size());
  for (atx::usize i = 0; i < a.size(); ++i) {
    EXPECT_TRUE(same_cell(a[i], b[i])) << "cell " << i;
  }
}

// ===========================================================================
//  S3-2 — TsZscore / TsAvDiff online extension (same ResearchFast contract).
//
//  TsZscore = (x[t] - mean) / std and TsAvDiff = x[t] - mean both reuse the
//  Welford (m, S, n) accumulator. Zscore conforms at atol=1e-9 (it normalizes by
//  std, so a tiny mean error is dominated by the spread). AvDiff is the hard case:
//  its OUTPUT is itself a near-cancellation (x[t] ~ mean on a high-mean window), so
//  the tolerance is atol-only = 1e-7 absolute (ts_ops.hpp:349-352 rationale). The
//  adversarial test below is the SHIP/BAIL gate: if AvDiff cannot meet atol<=1e-7
//  it must be removed from ts_is_online_variance_op and stay batch.
// ===========================================================================

// TsZscore pathological near-constant (mean ~1e5, std ~1.0): Welford vs batch
// oracle within atol=1e-9. The kernel computes std from the same Welford S, so the
// normalization recovers the small spread despite the high mean.
//
// MEAN MAGNITUDE = 1e5 (not 1e7): zscore's numerator (x[t] - mean) is a true f64
// cancellation, so its representational floor is mean*eps ~ mean*2.2e-16. At 1e5
// that floor is ~2e-11 (well under the 1e-9 gate); at 1e7 it is ~2e-9, ABOVE the
// gate, so a 1e-9-vs-oracle bar is unmeetable by ANY kernel at 1e7 (the oracle's
// own (x-mean) differs from Welford's by ~3.6e-9 there — an intrinsic f64 limit,
// not a kernel defect; recorded in the ledger). 1e5 still stresses the SAME
// catastrophic-cancellation regime the old Sx^2 variance failed (Sx^2 ~ 1e10*d).
TEST(TsWelfordZscore, NearConstantHighMean_WithinToleranceOfOracle) {
  const atx::usize dates = 220;
  const atx::usize instruments = 8;
  const atx::usize cells = dates * instruments;
  std::vector<std::vector<atx::f64>> cols = base_cols(cells, 0x2C0BEEFULL);
  std::mt19937_64 rng{0x5C0FEULL};
  std::uniform_real_distribution<atx::f64> jitter{-1.0, 1.0};
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = 1.0e5 + jitter(rng);
  }
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> fast = vm_values("ts_zscore(close, 20)", panel, EvalMode::ResearchFast);
  const std::vector<atx::f64> oracle = oracle_values("ts_zscore(close, 20)", panel);
  ASSERT_EQ(fast.size(), oracle.size());
  for (atx::usize i = 0; i < fast.size(); ++i) {
    EXPECT_TRUE(close_cell(fast[i], oracle[i], 1e-9, 1e-9))
        << "zscore cell " << i << " fast=" << fast[i] << " oracle=" << oracle[i];
  }
}

// TsZscore on an EXACTLY-constant window (M-3): var==0 -> the zscore is 0/0. Both
// paths must agree (sqrt(0)==0, (x-mean)==0 -> NaN). Random panels never hit a
// bit-exact-constant window, so this 0/0 degenerate is otherwise untested. The
// kernel-vs-oracle parity here must be byte-for-byte on the non-finite result
// (NaN==NaN, or ±inf parity if the oracle ever yields one).
TEST(TsWelfordZscore, ConstantWindow_ZeroVarianceParityWithOracle) {
  const atx::usize dates = 30;
  const atx::usize instruments = 4;
  const atx::usize cells = dates * instruments;
  // A perfectly constant close column -> every full window has var == 0 exactly.
  std::vector<std::vector<atx::f64>> cols = base_cols(cells, 0xC0FFEEULL);
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = 42.0;
  }
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> fast = vm_values("ts_zscore(close, 10)", panel, EvalMode::ResearchFast);
  const std::vector<atx::f64> oracle = oracle_values("ts_zscore(close, 10)", panel);
  ASSERT_EQ(fast.size(), oracle.size());
  bool saw_full_window = false;
  for (atx::usize i = 0; i < fast.size(); ++i) {
    // ResearchFast must yield the SAME result the batch oracle does, including the
    // non-finite 0/0 outcome (same_cell: both NaN, or exactly value-equal incl ±inf).
    EXPECT_TRUE(same_cell(fast[i], oracle[i]))
        << "zscore const-window cell " << i << " fast=" << fast[i] << " oracle=" << oracle[i];
    if (!std::isfinite(oracle[i])) {
      saw_full_window = true; // a 0/0 (NaN/inf) cell was actually produced and matched
    }
  }
  EXPECT_TRUE(saw_full_window) << "zero-variance 0/0 case was never exercised";
}

// TsAvDiff ADVERSARIAL ship/bail gate: mean ~1e6, av_diff amplitude ~1.0. Welford
// av_diff vs batch oracle MUST be within atol=1e-7 (absolute) on every finite
// cell, else TsAvDiff stays batch (remove from ts_is_online_variance_op) and the
// ledger records the measured error. This codifies the plan's explicit bail-out.
TEST(TsWelfordAvDiff, AdversarialHighMean_WithinAtol1e7OfOracle) {
  const atx::usize dates = 260;
  const atx::usize instruments = 8;
  const atx::usize cells = dates * instruments;
  std::vector<std::vector<atx::f64>> cols = base_cols(cells, 0xADBEEFULL);
  std::mt19937_64 rng{0xAD1FFULL};
  std::uniform_real_distribution<atx::f64> jitter{-1.0, 1.0};
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = 1.0e6 + jitter(rng);
  }
  const Panel panel = make_panel(dates, instruments, std::move(cols));
  const std::vector<atx::f64> fast = vm_values("ts_av_diff(close, 20)", panel, EvalMode::ResearchFast);
  const std::vector<atx::f64> oracle = oracle_values("ts_av_diff(close, 20)", panel);
  ASSERT_EQ(fast.size(), oracle.size());
  atx::f64 worst = 0.0;
  for (atx::usize i = 0; i < fast.size(); ++i) {
    if (!std::isnan(fast[i]) && !std::isnan(oracle[i])) {
      worst = std::max(worst, std::fabs(fast[i] - oracle[i]));
    }
    EXPECT_TRUE(close_cell(fast[i], oracle[i], 1e-7, 0.0))
        << "av_diff cell " << i << " fast=" << fast[i] << " oracle=" << oracle[i];
  }
  // Echo the measured worst-case absolute error for the ledger (atol=1e-7 gate).
  std::cerr << "[ts_av_diff adversarial] worst |welford-oracle| = " << worst << " (gate 1e-7)\n";
}

// TsAvDiff at d==1 (I-1 regression): the oracle's av_diff = w.back() - mean is
// defined for n>=1, so at d==1 it yields x[t]-x[t] = 0.0 on EVERY finite cell. The
// Welford kernel previously gated n<2 -> NaN there, a finite-vs-NaN divergence that
// would bias ResearchFast av_diff(x, 1) genome selection. The kernel now special-
// cases AvDiff at n==1, so ResearchFast must match the oracle (== 0.0) at d==1.
TEST(TsWelfordAvDiff, WindowOne_MatchesOracleZero) {
  const atx::usize dates = 40;
  const atx::usize instruments = 6;
  const atx::usize cells = dates * instruments;
  // A finite-input fixture (the base_cols close column is NaN-free positive prices).
  const Panel panel = make_panel(dates, instruments, base_cols(cells, 0xD1FF01ULL));
  const std::vector<atx::f64> fast = vm_values("ts_av_diff(close, 1)", panel, EvalMode::ResearchFast);
  const std::vector<atx::f64> oracle = oracle_values("ts_av_diff(close, 1)", panel);
  ASSERT_EQ(fast.size(), oracle.size());
  ASSERT_EQ(fast.size(), cells);
  for (atx::usize i = 0; i < fast.size(); ++i) {
    // Oracle is finite 0.0 at every cell (d==1, all finite) — assert the case is
    // actually exercised, then that ResearchFast matches it (close-cell, atol 1e-7).
    ASSERT_FALSE(std::isnan(oracle[i])) << "d==1 oracle must be finite at cell " << i;
    EXPECT_EQ(oracle[i], 0.0) << "d==1 oracle av_diff must be exactly 0.0 at cell " << i;
    EXPECT_TRUE(close_cell(fast[i], oracle[i], 1e-7, 0.0))
        << "av_diff(d=1) cell " << i << " fast=" << fast[i] << " oracle=" << oracle[i];
  }
}

// TsZscore / TsAvDiff on the shared random panel within their tolerances.
TEST(TsWelfordZscoreAvDiff, RandomPanel_WithinTolerancesOfOracle) {
  const atx::usize dates = 500;
  const atx::usize instruments = 200;
  const atx::usize cells = dates * instruments;
  const Panel panel = make_panel(dates, instruments, base_cols(cells, 0xA1FA101ULL));
  // zscore: atol=rtol=1e-9.
  {
    const std::vector<atx::f64> fast = vm_values("ts_zscore(close, 20)", panel, EvalMode::ResearchFast);
    const std::vector<atx::f64> oracle = oracle_values("ts_zscore(close, 20)", panel);
    ASSERT_EQ(fast.size(), oracle.size());
    for (atx::usize i = 0; i < fast.size(); ++i) {
      EXPECT_TRUE(close_cell(fast[i], oracle[i], 1e-9, 1e-9))
          << "zscore cell " << i << " fast=" << fast[i] << " oracle=" << oracle[i];
    }
  }
  // av_diff: atol-only = 1e-7.
  {
    const std::vector<atx::f64> fast = vm_values("ts_av_diff(close, 20)", panel, EvalMode::ResearchFast);
    const std::vector<atx::f64> oracle = oracle_values("ts_av_diff(close, 20)", panel);
    ASSERT_EQ(fast.size(), oracle.size());
    for (atx::usize i = 0; i < fast.size(); ++i) {
      EXPECT_TRUE(close_cell(fast[i], oracle[i], 1e-7, 0.0))
          << "av_diff cell " << i << " fast=" << fast[i] << " oracle=" << oracle[i];
    }
  }
}

// Off-path byte-identity after S3-2: AuditExact zscore/av_diff == batch oracle.
TEST(TsAuditExactZscoreAvDiff, DefaultMode_ByteIdenticalToOracle) {
  const atx::usize dates = 60;
  const atx::usize instruments = 16;
  const atx::usize cells = dates * instruments;
  const Panel panel = make_panel(dates, instruments, base_cols(cells, 0xBEEF02ULL));
  for (const std::string_view expr : {std::string_view{"ts_zscore(close, 10)"},
                                      std::string_view{"ts_av_diff(close, 10)"}}) {
    const std::vector<atx::f64> audit = vm_values(expr, panel, EvalMode::AuditExact);
    const std::vector<atx::f64> oracle = oracle_values(expr, panel);
    ASSERT_EQ(audit.size(), oracle.size());
    for (atx::usize i = 0; i < audit.size(); ++i) {
      EXPECT_TRUE(same_cell(audit[i], oracle[i]))
          << expr << " cell " << i << " audit=" << audit[i] << " oracle=" << oracle[i];
    }
  }
}

} // namespace atxtest_ts_online_variance

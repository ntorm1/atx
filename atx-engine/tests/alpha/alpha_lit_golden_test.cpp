// Golden digest over the EXISTING DSL op set (platform-v7 W2 / A7 guard).
//
// W2 adds new opcodes (appended after OpCode::Free) and new kernels. This suite
// proves every pre-W2 opcode still produces the same bits: one expression per
// registered named op plus every infix / prefix / ternary opcode, evaluated on a
// fixed-seed panel with NaN holes, a forward-filled field and universe gaps,
// folded into FNV-1a digests (NaN canonicalized) for three paths:
//   * the production VM under AuditExact (the default, publication path),
//   * the production VM under ResearchFast (online / sliding lanes),
//   * the reference oracle.
//
// CAPTURE PROTOCOL (no binary may run in the implementing lane): this file uses
// ONLY pre-W2 operators, so it compiles unchanged on the W2 base commit. Root
// builds atx-engine-alpha-tests at the commit that introduced this file (base +
// this test only), runs --gtest_filter=AlphaLitGolden_*, reads the printed
// `[lit-golden]` digests and pins them below; the W2 head must print the SAME
// three values. Until pinned (all zero) the test fails and prints the digests.
//
// SPLIT (P9 Ruling M2c-RED-2): the pins are a Debug-CRT golden on the capture host, because the
// Pow / Spow kernels call std::pow on run-time operands, i.e. the dynamic C runtime, and
// ucrtbased.dll (/MDd) and ucrtbase.dll (/MD) differ in the last bit on ~0.2% of pow arguments
// (S2R: power(close, 0.5), signedpower(returns, 2) and close ^ 3 move 1 ULP in Release). So:
//   * EveryPreW2OpcodeVmMatchesOracle runs in EVERY build: the correctness property (VM
//     AuditExact == oracle bit for bit; ResearchFast deterministic and inside its contract).
//   * EveryPreW2OpcodeDigestIsPinned runs where the pins' CRT is linked and is SKIPPED, visibly,
//     elsewhere. No pin was edited or added.
//   * DISABLED_PerRowDigestDump names the row / mode that moved when two builds disagree.
//
// Naming: Subject_Condition_ExpectedResult.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib> // _dupenv_s / std::free (the diagnostic dump; std::getenv trips /WX)
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
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx_test_alpha_lit_golden {

using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::Program;

// PIN: captured by root on the merged W2 HEAD (pool-2 96f3ed96, build v7-w2) — the
// pre-W2 opcode battery; any change here means an existing opcode's bits moved.
constexpr std::uint64_t kGoldenVmAuditExact = 0x971c3da60ca89aa3ULL;
constexpr std::uint64_t kGoldenVmResearchFast = 0xe9e7128fd5359900ULL;
constexpr std::uint64_t kGoldenOracle = 0x971c3da60ca89aa3ULL;

// The CRT the pins were captured with: the debug DLL runtime, ucrtbased.dll (Debug /MDd).
// Keyed on the CRT macros, not on NDEBUG: MSVC and clang-cl define _DEBUG exactly when a debug
// CRT is selected (/MDd or /MTd) and _DLL exactly when the DLL CRT is (/MD or /MDd), whatever
// NDEBUG / assert settings say; NDEBUG is a CMake build-type define that does not choose the
// library std::pow resolves to. _DEBUG && _DLL is therefore /MDd, i.e. ucrtbased.dll. _MSC_VER
// follows the debug-heap precedent in alpha_vm_test.cpp (no other toolchain links either DLL).
#if defined(_MSC_VER) && defined(_DEBUG) && defined(_DLL)
constexpr bool kPinnedCrtLinked = true;
#else
constexpr bool kPinnedCrtLinked = false;
#endif

// Opcodes the ResearchFast tier reroutes to its tolerance kernels (not bit-exact): vm.hpp
// eval_time_series routes TsDecayExp (:1838), TsSum / TsMean (:1860), TsCorr / TsCov /
// TsRegression (:1876), the decay / time-regression lanes (:1915) and the variance family (:1931).
// The contract there is atol = rtol = 1e-9 (ts_ops.hpp ts_is_online_variance_op; the check form
// of alpha_ts_sliding_routing_test.cpp). Every other pre-W2 op must give ResearchFast the same
// bits as AuditExact (TsDelta's ResearchFast SIMD subtraction is elementwise, so bit-exact).
// FROZEN: a new ResearchFast reroute is added here deliberately, never inferred from the VM.
constexpr std::array<OpCode, 15> kResearchFastToleranceOps = {
    OpCode::TsSum,       OpCode::TsMean,        OpCode::TsDecayExp, OpCode::TsCorr,
    OpCode::TsCov,       OpCode::TsRegression,  OpCode::TsDecayLinear, OpCode::TsWma,
    OpCode::TsSlope,     OpCode::TsRsquare,     OpCode::TsResid,    OpCode::TsVar,
    OpCode::TsStd,       OpCode::TsZscore,      OpCode::TsAvDiff};
// Battery rows that contain one of the ops above (ts_sum, ts_mean, stddev, ts_std, ts_var,
// correlation, covariance, decay_linear, wma, slope, rsquare, resid, ts_zscore, ts_av_diff,
// ts_corr, ts_regression, ts_decay_exp, trade_when(... ts_mean ...)).
constexpr std::size_t kResearchFastToleranceRows = 18;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

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

// 64 dates x 12 instruments, seed-fixed. THIS FIXTURE IS FROZEN once pinned.
Panel golden_panel() {
  constexpr atx::usize kD = 64;
  constexpr atx::usize kI = 12;
  const std::vector<std::string> names = {"open",    "high",    "low",
                                          "close",   "volume",  "vwap",
                                          "returns", "IndClass.sector", "fund"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(kD * kI));
  std::vector<std::uint8_t> uni(kD * kI, 1);
  std::mt19937_64 rng{0x6011D3A7ULL};
  std::normal_distribution<atx::f64> nd{0.0, 1.0};
  std::uniform_real_distribution<atx::f64> u{0.0, 1.0};
  std::vector<atx::f64> px(kI);
  for (atx::usize j = 0; j < kI; ++j) {
    px[j] = 20.0 + 3.0 * static_cast<atx::f64>(j);
  }
  for (atx::usize t = 0; t < kD; ++t) {
    for (atx::usize j = 0; j < kI; ++j) {
      const atx::usize i = t * kI + j;
      const atx::f64 prev = px[j];
      px[j] = std::max(1.0, px[j] * (1.0 + 0.02 * nd(rng)));
      const atx::f64 o = prev * (1.0 + 0.005 * nd(rng));
      const atx::f64 c = px[j];
      const atx::f64 h = std::max(o, c) * (1.0 + 0.01 * u(rng));
      const atx::f64 l = std::min(o, c) * (1.0 - 0.01 * u(rng));
      cols[0][i] = o;
      cols[1][i] = h;
      cols[2][i] = l;
      cols[3][i] = c;
      cols[4][i] = 1.0e5 * (0.5 + u(rng));
      cols[5][i] = (h + l + c) / 3.0;
      cols[6][i] = c / prev - 1.0;
      cols[7][i] = static_cast<atx::f64>(j % 3);
      cols[8][i] = 0.1 * static_cast<atx::f64>(1 + (j % 4)) + 0.05 * static_cast<atx::f64>(t / 8);
      if (u(rng) < 0.02) {
        cols[6][i] = kNaN; // returns holes
      }
      if (u(rng) < 0.01) {
        cols[3][i] = kNaN; // close holes
      }
    }
  }
  for (atx::usize t = 20; t < 30; ++t) {
    uni[t * kI + 2] = 0; // instrument 2 leaves for 10 dates
  }
  for (atx::usize t = 0; t < 9; ++t) {
    uni[t * kI + 5] = 0; // instrument 5 enters late
  }
  auto p = Panel::create(kD, kI, names, std::move(cols), std::move(uni));
  EXPECT_TRUE(p.has_value());
  return p.value();
}

std::uint64_t fnv_cells(std::uint64_t h, const std::vector<atx::f64> &v) {
  for (const atx::f64 x : v) {
    std::uint64_t bits = 0;
    if (std::isnan(x)) {
      bits = 0x7ff8000000000000ULL;
    } else {
      std::memcpy(&bits, &x, sizeof(bits));
    }
    for (int k = 0; k < 8; ++k) {
      h ^= (bits >> (8 * k)) & 0xFFULL;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

// A cell's bits with every NaN canonicalized: the digests' equality rule.
std::uint64_t canonical_bits(atx::f64 x) noexcept {
  std::uint64_t bits = 0x7ff8000000000000ULL;
  if (!std::isnan(x)) {
    std::memcpy(&bits, &x, sizeof(bits));
  }
  return bits;
}

std::string describe_cell(atx::usize c, atx::f64 got, atx::f64 want) {
  constexpr atx::usize kI = 12; // golden_panel instruments
  char buf[160];
  std::snprintf(buf, sizeof(buf), "cell %zu (t=%zu, j=%zu): %016llx (%.17g) vs %016llx (%.17g)", c,
                c / kI, c % kI, static_cast<unsigned long long>(canonical_bits(got)), got,
                static_cast<unsigned long long>(canonical_bits(want)), want);
  return buf;
}

// Empty when `got` and `want` agree bit for bit (NaN canonical); else the first differing cell.
std::string bit_mismatch(const std::vector<atx::f64> &got, const std::vector<atx::f64> &want) {
  if (got.size() != want.size()) {
    return "size " + std::to_string(got.size()) + " vs " + std::to_string(want.size());
  }
  for (atx::usize c = 0; c < got.size(); ++c) {
    if (canonical_bits(got[c]) != canonical_bits(want[c])) {
      return describe_cell(c, got[c], want[c]);
    }
  }
  return {};
}

// Empty when ResearchFast `rf` meets its value contract against AuditExact `ae` (the form of
// alpha_ts_sliding_routing_test.cpp): where both are finite, |rf - ae| <= 1e-9 * (1 + |ae|); an
// infinity on either side must be the same infinity on both. The NaN pattern is NOT compared on
// these rows, because two documented divergences exist: the sliding lanes return NaN where the
// batch value is pure roundoff (ts_sliding.hpp NUMERICS), and the A-09 flat-window guard is
// batch-only (ts_ops.hpp FlatGuard), so ResearchFast ts_zscore of a flat window is 0 where
// AuditExact is NaN (204 cells of ts_zscore(fund, 5); S2R report). P9 Ruling S2R-1: that
// flat-window 0 is a documented property of ResearchFast through P9, not fixed now (the
// streaming-path fix is a P10 item); this skip of the NaN pattern stands under that ruling.
std::string tolerance_mismatch(const std::vector<atx::f64> &rf, const std::vector<atx::f64> &ae) {
  if (rf.size() != ae.size()) {
    return "size " + std::to_string(rf.size()) + " vs " + std::to_string(ae.size());
  }
  for (atx::usize c = 0; c < rf.size(); ++c) {
    const atx::f64 r = rf[c];
    const atx::f64 a = ae[c];
    if (std::isnan(a) || std::isnan(r)) {
      continue; // the NaN pattern: see above (Ruling S2R-1)
    }
    const bool ok = (std::isinf(a) || std::isinf(r))
                        ? (r == a)
                        : std::fabs(r - a) <= 1e-9 * (1.0 + std::fabs(a));
    if (!ok) {
      return describe_cell(c, r, a);
    }
  }
  return {};
}

bool uses_research_fast_tolerance_op(const Program &prog) {
  return std::any_of(prog.code.begin(), prog.code.end(), [](const auto &in) {
    return std::find(kResearchFastToleranceOps.begin(), kResearchFastToleranceOps.end(),
                     in.op) != kResearchFastToleranceOps.end();
  });
}

// One expression per pre-W2 named operator (registry order) + every infix /
// prefix / ternary opcode. Every one compiles on the W2 base.
const std::vector<std::string_view> &existing_ops() {
  static const std::vector<std::string_view> k = {
      "abs(close - open)", "sign(close - open)", "log(volume)", "sigmoid(returns * 10)",
      "tanh(returns * 10)", "power(close, 0.5)", "signedpower(returns, 2)", "min(open, close)",
      "max(open, close)", "rank(close)", "zscore(volume)", "scale(returns, 2)",
      "normalize(close)", "winsorize(returns, 2)", "indneutralize(close, IndClass.sector)",
      "group_neutralize(returns, IndClass.sector)", "group_rank(volume, IndClass.sector)",
      "group_zscore(close, IndClass.sector)", "group_count(close, IndClass.sector)",
      "group_mean(close, IndClass.sector)", "group_scale(returns, IndClass.sector)",
      "cs_residualize(close, IndClass.sector)", "cs_residualize(close, IndClass.sector, volume)",
      "quantile(close, 4)", "reverse(close)", "vec_sum(returns)", "vec_avg(returns)",
      "delay(close, 3)", "delta(close, 3)", "ts_sum(returns, 5)", "ts_mean(close, 5)",
      "stddev(returns, 6)", "ts_std(close, 6)", "ts_var(returns, 6)", "ts_min(low, 4)",
      "ts_max(high, 4)", "ts_argmin(close, 5)", "ts_argmax(close, 5)", "ts_rank(close, 7)",
      "correlation(close, volume, 8)", "covariance(close, volume, 8)", "product(1 + returns, 4)",
      "decay_linear(close, 5)", "ema(close, 6)", "wma(close, 6)", "skew(returns, 8)",
      "kurt(returns, 9)", "med(close, 5)", "mad(close, 5)", "slope(close, 6)",
      "rsquare(close, 6)", "resid(close, 6)", "ts_zscore(fund, 5)", "ts_backfill(returns, 5)",
      "ts_av_diff(close, 5)", "ts_quantile(close, 7)", "ts_scale(close, 7)",
      "ts_count_nans(returns, 6)", "ts_skew(close, 8)", "ts_kurt(close, 9)",
      "ts_corr(close, open, 6)", "ts_regression(close, vwap, 7)", "ts_decay_exp(close, 6, 0.7)",
      "ts_moment(returns, 6, 3)", "ts_entropy(returns, 10, 4)",
      "trade_when(volume > ts_mean(volume, 5), rank(returns), returns < -0.02)",
      "hump(rank(close), 0.05)", "kalman_level(close, 0.01, 1.0)", "ou_filter(close, 0.2, 50)",
      "split2(close).lo", "kalman(close, vwap, 0.5, 1.0).beta",
      "kalman(close, vwap, 0.5, 1.0).resid", "ou_theta(close, 10)", "ou_halflife(close, 10)",
      "ou_mean(close, 10)", "ou_zscore(close, 10)",
      // infix / prefix / ternary opcodes (Add Sub Mul Div Pow Neg Cmp* And Or Not Select)
      "(close > open) ? close : open", "((close < open) && (volume >= 100000)) ? 1 : 0",
      "((close <= open) || !(volume != 100000)) ? high : low", "(close == open) ? 1 : 0",
      "close / open - 1", "-close + high * low", "close ^ 2", "close ^ 3"};
  return k;
}

struct Digests {
  std::uint64_t vm_ae{1469598103934665603ULL};
  std::uint64_t vm_rf{1469598103934665603ULL};
  std::uint64_t oracle{1469598103934665603ULL};
};

Digests run_battery() {
  const Panel panel = golden_panel();
  Digests d;
  for (const std::string_view e : existing_ops()) {
    const Program prog = compile_ok(e);
    Engine ae{panel};
    auto a = ae.evaluate(prog);
    EXPECT_TRUE(a.has_value()) << e;
    Engine rf{panel};
    rf.set_eval_mode(EvalMode::ResearchFast);
    auto r = rf.evaluate(prog);
    EXPECT_TRUE(r.has_value()) << e;
    auto o = atx::engine::alpha::evaluate_reference(prog, panel);
    EXPECT_TRUE(o.has_value()) << e;
    if (!a || !r || !o) {
      continue;
    }
    d.vm_ae = fnv_cells(d.vm_ae, a.value().alphas.front().values);
    d.vm_rf = fnv_cells(d.vm_rf, r.value().alphas.front().values);
    d.oracle = fnv_cells(d.oracle, o.value().alphas.front().values);
  }
  return d;
}

TEST(AlphaLitGolden_ExistingOps, EveryPreW2OpcodeDigestIsPinned) {
  const Digests d = run_battery();
  std::printf("[lit-golden] vm_audit_exact=0x%016llx vm_research_fast=0x%016llx "
              "oracle=0x%016llx\n",
              static_cast<unsigned long long>(d.vm_ae), static_cast<unsigned long long>(d.vm_rf),
              static_cast<unsigned long long>(d.oracle));
  if constexpr (!kPinnedCrtLinked) {
    GTEST_SKIP() << "Ruling M2c-RED-2: the pins are a Debug-CRT golden on this host "
                    "(ucrtbased.dll, /MDd). This build links another CRT (Release: ucrtbase.dll), "
                    "whose std::pow differs from ucrtbased.dll's by 1 ULP on ~0.2% of arguments "
                    "(rows power(close, 0.5), signedpower(returns, 2), close ^ 3). Correctness "
                    "runs in every build: EveryPreW2OpcodeVmMatchesOracle.";
  } else {
    if (kGoldenVmAuditExact == 0 && kGoldenVmResearchFast == 0 && kGoldenOracle == 0) {
      ADD_FAILURE() << "golden digests unpinned: capture on the W2 base + this test, then pin";
      return;
    }
    EXPECT_EQ(d.vm_ae, kGoldenVmAuditExact);
    EXPECT_EQ(d.vm_rf, kGoldenVmResearchFast);
    EXPECT_EQ(d.oracle, kGoldenOracle);
  }
}

// The correctness property behind the pins, in EVERY build (Ruling M2c-RED-2): on each battery
// row, (1) the VM under AuditExact equals the independent reference oracle bit for bit; (2)
// ResearchFast is deterministic: a serial run and a 2-worker DetPool run give the same bits; (3)
// ResearchFast equals AuditExact bit for bit on every row without a ResearchFast tolerance op,
// and meets the 1e-9 value contract on the finite cells of the rows with one. CRT-independent:
// VM and oracle call the same CRT inside one build, so a CRT difference moves both sides alike.
TEST(AlphaLitGolden_ExistingOps, EveryPreW2OpcodeVmMatchesOracle) {
  const Panel panel = golden_panel();
  atx::engine::parallel::DetPool pool{2};
  std::size_t tolerance_rows = 0;
  std::vector<atx::f64> first_row_ae;
  for (const std::string_view e : existing_ops()) {
    SCOPED_TRACE(e);
    const Program prog = compile_ok(e);
    Engine ae{panel};
    auto a = ae.evaluate(prog);
    Engine rf_serial{panel};
    rf_serial.set_eval_mode(EvalMode::ResearchFast);
    auto r1 = rf_serial.evaluate(prog);
    Engine rf_pooled{panel};
    rf_pooled.set_eval_mode(EvalMode::ResearchFast);
    rf_pooled.set_ts_pool(&pool);
    auto r2 = rf_pooled.evaluate(prog);
    auto o = atx::engine::alpha::evaluate_reference(prog, panel);
    ASSERT_TRUE(a.has_value() && r1.has_value() && r2.has_value() && o.has_value());
    const std::vector<atx::f64> &va = a.value().alphas.front().values;
    const std::vector<atx::f64> &vr1 = r1.value().alphas.front().values;
    const std::vector<atx::f64> &vr2 = r2.value().alphas.front().values;
    const std::vector<atx::f64> &vo = o.value().alphas.front().values;
    ASSERT_EQ(va.size(), panel.cells());
    EXPECT_TRUE(std::any_of(va.begin(), va.end(), [](atx::f64 x) { return std::isfinite(x); }))
        << "row has no finite cell: its comparisons would be vacuous";
    if (first_row_ae.empty()) {
      first_row_ae = va;
    }

    EXPECT_EQ(bit_mismatch(va, vo), "") << "VM AuditExact != oracle";
    EXPECT_EQ(bit_mismatch(vr2, vr1), "") << "ResearchFast not deterministic (pool 2 vs serial)";
    if (uses_research_fast_tolerance_op(prog)) {
      ++tolerance_rows;
      EXPECT_EQ(tolerance_mismatch(vr1, va), "") << "ResearchFast outside its 1e-9 contract";
    } else {
      EXPECT_EQ(bit_mismatch(vr1, va), "") << "ResearchFast != AuditExact on a bit-exact row";
    }
  }
  EXPECT_EQ(tolerance_rows, kResearchFastToleranceRows)
      << "the battery's ResearchFast-tolerance rows changed: re-derive kResearchFastToleranceRows";
  // Teeth: a 1-ULP perturbation of one finite cell of a real VM output is reported as a
  // mismatch, so the bit comparison (NaN canonicalization included) cannot pass vacuously.
  const auto victim = std::find_if(first_row_ae.begin(), first_row_ae.end(),
                                   [](atx::f64 x) { return std::isfinite(x) && x != 0.0; });
  ASSERT_TRUE(victim != first_row_ae.end());
  std::vector<atx::f64> perturbed = first_row_ae;
  const auto k = static_cast<std::size_t>(victim - first_row_ae.begin());
  perturbed[k] = std::nextafter(perturbed[k], std::numeric_limits<atx::f64>::infinity());
  EXPECT_NE(bit_mismatch(perturbed, first_row_ae), "");
}

// DIAGNOSTIC (P9 S2R, M2c-RED): the pinned test above folds every row into one digest, so a
// mismatch says only THAT a bit moved. This dump says WHERE: one line per input column and one
// per battery row (that row alone, each path from the FNV offset basis). Diff the output of two
// builds (e.g. Debug vs Release) to name the moved column / opcode / eval mode. Disabled, so it
// never counts in a suite total; run it with
//   --gtest_also_run_disabled_tests --gtest_filter=AlphaLitGolden_ExistingOps.DISABLED_*
// With ATX_LIT_GOLDEN_CELLS=1 it also prints every cell's bits ([lit-cell] row cell ae rf oracle).
TEST(AlphaLitGolden_ExistingOps, DISABLED_PerRowDigestDump) {
  constexpr std::uint64_t kBasis = 1469598103934665603ULL;
  char *env = nullptr;
  std::size_t env_len = 0;
  const bool cells = _dupenv_s(&env, &env_len, "ATX_LIT_GOLDEN_CELLS") == 0 && env != nullptr &&
                     std::string_view{env} == "1";
  std::free(env);
  const Panel panel = golden_panel();
  for (atx::usize f = 0; f < panel.num_fields(); ++f) {
    const auto col = panel.field_all(static_cast<atx::engine::alpha::FieldId>(f));
    // Copied so the digest is the same fnv_cells fold as the battery rows.
    const std::vector<atx::f64> v(col.begin(), col.end());
    std::printf("[lit-input] %-16s 0x%016llx\n", panel.field_name(f).c_str(),
                static_cast<unsigned long long>(fnv_cells(kBasis, v)));
  }
  const auto &rows = existing_ops();
  for (atx::usize k = 0; k < rows.size(); ++k) {
    const Program prog = compile_ok(rows[k]);
    Engine ae{panel};
    auto a = ae.evaluate(prog);
    Engine rf{panel};
    rf.set_eval_mode(EvalMode::ResearchFast);
    auto r = rf.evaluate(prog);
    auto o = atx::engine::alpha::evaluate_reference(prog, panel);
    ASSERT_TRUE(a.has_value() && r.has_value() && o.has_value()) << rows[k];
    const auto &va = a.value().alphas.front().values;
    const auto &vr = r.value().alphas.front().values;
    const auto &vo = o.value().alphas.front().values;
    std::printf("[lit-row] %02zu ae=0x%016llx rf=0x%016llx oracle=0x%016llx  %.*s\n", k,
                static_cast<unsigned long long>(fnv_cells(kBasis, va)),
                static_cast<unsigned long long>(fnv_cells(kBasis, vr)),
                static_cast<unsigned long long>(fnv_cells(kBasis, vo)),
                static_cast<int>(rows[k].size()), rows[k].data());
    for (atx::usize c = 0; cells && c < va.size(); ++c) {
      std::uint64_t ba = 0;
      std::uint64_t br = 0;
      std::uint64_t bo = 0;
      std::memcpy(&ba, &va[c], sizeof(ba));
      std::memcpy(&br, &vr[c], sizeof(br));
      std::memcpy(&bo, &vo[c], sizeof(bo));
      std::printf("[lit-cell] %02zu %03zu %016llx %016llx %016llx\n", k, c,
                  static_cast<unsigned long long>(ba), static_cast<unsigned long long>(br),
                  static_cast<unsigned long long>(bo));
    }
  }
}

// Each battery row is a distinct expression and exercises the op it names.
TEST(AlphaLitGolden_ExistingOps, BatteryCoversEveryPreW2NamedOp) {
  const std::vector<std::string_view> names = {
      "abs", "sign", "log", "sigmoid", "tanh", "power", "signedpower", "min", "max", "rank",
      "zscore", "scale", "normalize", "winsorize", "indneutralize", "group_neutralize",
      "group_rank", "group_zscore", "group_count", "group_mean", "group_scale", "cs_residualize",
      "quantile", "reverse", "vec_sum", "vec_avg", "delay", "delta", "ts_sum", "ts_mean",
      "stddev", "ts_std", "ts_var", "ts_min", "ts_max", "ts_argmin", "ts_argmax", "ts_rank",
      "correlation", "covariance", "product", "decay_linear", "ema", "wma", "skew", "kurt",
      "med", "mad", "slope", "rsquare", "resid", "ts_zscore", "ts_backfill", "ts_av_diff",
      "ts_quantile", "ts_scale", "ts_count_nans", "ts_skew", "ts_kurt", "ts_corr",
      "ts_regression", "ts_decay_exp", "ts_moment", "ts_entropy", "trade_when", "hump",
      "kalman_level", "ou_filter", "split2", "kalman", "ou_theta", "ou_halflife", "ou_mean",
      "ou_zscore"};
  for (const std::string_view n : names) {
    ASSERT_NE(lib().find(n), nullptr) << n;
    const std::string call = std::string{n} + "(";
    const bool used = std::any_of(existing_ops().begin(), existing_ops().end(),
                                  [&call](std::string_view e) {
                                    return e.find(call) != std::string_view::npos;
                                  });
    EXPECT_TRUE(used) << "no golden row exercises " << n;
  }
}

} // namespace atx_test_alpha_lit_golden

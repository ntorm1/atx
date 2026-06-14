// atx::engine::alpha — S3.6 widened-op conformance gate.
//
// The p0 battery (alpha_conformance_test.cpp) already pins the pre-S3 op set +
// the Alpha101 expressible subset. THIS suite is the S3 increment:
//
//   * Widened conformance — oracle == VM bit-for-bit over the ENTIRE S3.1–S3.4
//     widened op set (cs_residualize, the four BRAIN ts_*, quantile/reverse/
//     vec_*) AND the S3.3 datafields (vwap / dollar_volume / adv20) derived
//     through datafields.hpp — the non-regression gate for the new surface.
//   * p1 corpus — a frozen list of pre-S3 formulas, oracle == VM + a hand anchor
//     (the proven-core guarantee that S3's additions did not perturb the existing
//     ops, now that op_swap's contract rail also runs in analyze).
//   * Alpha101 (widened) — canonical alphas reachable via the datafields panel,
//     evaluated to finite in-universe output. The not-yet-expressible remainder is
//     recorded in tests/fixtures/alpha101_subset.txt + the ledger.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/datafields.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_widened_conformance_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
namespace df = atx::engine::alpha::datafields;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << src << ": " << (ast ? "" : ast.error().message());
  if (!ast) {
    return Program{};
  }
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << src << ": " << (ana ? "" : ana.error().message());
  if (!ana) {
    return Program{};
  }
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << src << ": " << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// A datafields panel: OHLCV + IndClass.sector, then vwap / dollar_volume / adv20
// derived through datafields.hpp, over enough dates that full-window ops + adv20
// produce finite tail rows. Deterministic — no <random>.
[[nodiscard]] Panel rich_panel() {
  constexpr atx::usize dates = 30;
  constexpr atx::usize instruments = 8;
  constexpr atx::usize cells = dates * instruments;
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  std::uint64_t s = 0x1234567ULL;
  auto next = [&s]() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(s >> 11) / static_cast<atx::f64>(1ULL << 53);
  };
  for (atx::usize i = 0; i < cells; ++i) {
    const atx::f64 base = 20.0 + next() * 80.0;
    const atx::f64 spread = 1.0 + next() * 4.0;
    cols[0][i] = base;                         // close
    cols[1][i] = base + (next() - 0.5) * 2.0;  // open
    cols[2][i] = base + spread;                // high
    cols[3][i] = base - spread;                // low
    cols[4][i] = 1.0e4 + next() * 9.0e5;       // volume
    cols[5][i] = static_cast<atx::f64>(i % 4); // IndClass.sector
  }
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  std::vector<atx::u16> adv = {20};
  auto p = df::with_datafields(dates, instruments, std::move(names), std::move(cols), {}, adv);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Evaluate `expr` through BOTH paths; assert oracle == VM and return the VM alpha
// (callers may ignore the return when only the differential assertion matters).
std::vector<atx::f64> eval_diff(std::string_view expr, const Panel &panel) {
  const Program prog = compile_ok(expr);
  Engine engine{panel};
  auto vm = engine.evaluate(prog);
  EXPECT_TRUE(vm.has_value()) << expr << " VM: " << (vm ? "" : vm.error().message());
  auto ref = evaluate_reference(prog, panel);
  EXPECT_TRUE(ref.has_value()) << expr << " oracle: " << (ref ? "" : ref.error().message());
  if (!vm || !ref) {
    return {};
  }
  const SignalSet &v = vm.value();
  const SignalSet &r = ref.value();
  EXPECT_EQ(v.alphas.size(), r.alphas.size());
  for (atx::usize a = 0; a < v.alphas.size() && a < r.alphas.size(); ++a) {
    EXPECT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
    for (atx::usize i = 0; i < v.alphas[a].values.size(); ++i) {
      if (!same_cell(v.alphas[a].values[i], r.alphas[a].values[i])) {
        ADD_FAILURE() << expr << " alpha " << a << " cell " << i
                      << ": VM=" << v.alphas[a].values[i] << " oracle=" << r.alphas[a].values[i];
        break;
      }
    }
  }
  return v.alphas.empty() ? std::vector<atx::f64>{} : v.alphas[0].values;
}

[[nodiscard]] bool any_finite(const std::vector<atx::f64> &values) {
  for (const atx::f64 v : values) {
    if (std::isfinite(v)) {
      return true;
    }
  }
  return false;
}

// ===========================================================================
//  Widened conformance — oracle == VM over the whole S3.1–S3.4 op surface.
// ===========================================================================

TEST(WidenedConformance, AllNewOps_OracleEqualsVm) {
  const Panel panel = rich_panel();
  const std::vector<std::string_view> exprs = {
      // S3.1 residualizer (boundary pin + FWL covariate)
      "cs_residualize(close, IndClass.sector)",
      "cs_residualize(close, IndClass.sector, open)",
      // S3.2 BRAIN ts_*
      "ts_regression(close, open, 10)",
      "ts_decay_exp(close, 10, 2.0)",
      "ts_moment(close, 10, 2)",
      "ts_entropy(close, 10, 8)",
      // S3.3 cross-sectional gap-fill + reverse alias
      "quantile(close, 5)",
      "reverse(close)",
      "vec_sum(close)",
      "vec_avg(close)",
      // S3.3 datafields referenced like raw fields
      "rank(close / adv20)",
      "power(high * low, 0.5) - vwap",
      "dollar_volume / adv20",
      // composed across families (CSE + nesting + the widened ops)
      "rank(cs_residualize(quantile(close, 4), IndClass.sector)) + vec_avg(ts_moment(volume, 5, 2))",
      "scale(ts_regression(close, vwap, 8) - vec_avg(close), 1)",
      "ts_entropy(reverse(close), 12, 16) * ts_decay_exp(volume, 6, 1.5)",
  };
  for (const std::string_view e : exprs) {
    eval_diff(e, panel); // asserts oracle == VM internally
  }
}

// ===========================================================================
//  p1 corpus — pre-S3 formulas unperturbed (oracle == VM + hand anchor).
// ===========================================================================

TEST(WidenedConformance, P1Corpus_Unperturbed) {
  const Panel panel = rich_panel();
  const std::vector<std::string_view> corpus = {
      "rank(close)",
      "rank(close) - rank(open)",
      "ts_mean(close, 5) / (ts_mean(volume, 5) + 1)",
      "correlation(close, volume, 10)",
      "scale(ts_mean(close, 5) - close, 1)",
      "indneutralize(close, IndClass.sector)",
      "delta(close, 2) * rank(close)",
      "signedpower(close - open, 2)",
  };
  for (const std::string_view e : corpus) {
    eval_diff(e, panel);
  }
  // Hand anchor: close - open is a pure element-wise IEEE difference per cell.
  const std::vector<atx::f64> close = eval_diff("close", panel);
  const std::vector<atx::f64> open = eval_diff("open", panel);
  const std::vector<atx::f64> diff = eval_diff("close - open", panel);
  ASSERT_EQ(diff.size(), close.size());
  ASSERT_EQ(diff.size(), open.size());
  for (atx::usize i = 0; i < diff.size(); ++i) {
    EXPECT_TRUE(same_cell(diff[i], close[i] - open[i])) << "cell " << i;
  }
}

// ===========================================================================
//  Alpha101 (widened/datafields path) — finite output + oracle == VM.
// ===========================================================================

TEST(WidenedConformance, Alpha101Subset_FiniteAndConsistent) {
  const Panel panel = rich_panel();
  // The S3-expressible subset over the datafields panel (mirrors
  // tests/fixtures/alpha101_subset.txt). Every alpha uses only ops + fields S3
  // ships; the not-yet-expressible remainder (those needing a daily `returns`
  // field or sign-conditional forms) is recorded in the fixture + ledger.
  const std::vector<std::string_view> alphas = {
      "(close - open) / ((high - low) + 0.001)",                                    // #101
      "power(high * low, 0.5) - vwap",                                              // #41
      "(-1 * ((low - close) * power(open, 5))) / ((low - high) * power(close, 5))", // #54
      "sign(delta(volume, 1)) * (-1 * delta(close, 1))",                            // #12
      "-1 * correlation(open, volume, 10)",                                         // #6
      "rank(-1 * (1 - (open / close)))",                                            // #33
      "-1 * rank(covariance(rank(close), rank(volume), 5))",                        // #13
      "-1 * correlation(rank(close), rank(volume), 6)",                             // #45-ish
      "rank(ts_max(close, 10) - close)",                                            // ts_max
      "scale(rank(close) - rank(open), 1)",                                         // scale
      "rank(close / adv20)",                                                        // adv20 datafield
      "ts_rank(volume, 5) * (-1 * ts_rank(high - close, 5))",                       // #9-ish
  };
  for (const std::string_view a : alphas) {
    const std::vector<atx::f64> out = eval_diff(a, panel); // oracle == VM
    EXPECT_TRUE(any_finite(out)) << "alpha produced no finite cell: " << a;
  }
}


}  // namespace atxtest_alpha_widened_conformance_test

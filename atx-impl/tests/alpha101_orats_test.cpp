// atx-impl — Alpha101 ORATS verification harness.
//
// GOAL: prove the ATX alpha-expression DSL + engine can PARSE, COMPILE, and
// EVALUATE every one of the "101 Formulaic Alphas" (Kakushadze 2016,
// arXiv:1601.00991) on an ORATS-derived panel.
//
// The 101 canonical formulas live in fixtures/alpha101.txt (one `<id>: <dsl>`
// per line). For each alpha this harness runs the full pipeline —
// parse_expr -> analyze -> compile -> Engine::evaluate — over a panel that
// materializes the complete Alpha101 input vocabulary (see alpha101_support.hpp)
// and records, per alpha, whether each stage succeeded and whether the signal
// has at least one finite in-universe cell. It prints a 101-row report and
// asserts all 101 pass every stage.
//
// PANEL SOURCE:
//   * ATX_ALPHA101_PANEL set  -> read that serialized APNL panel (built from the
//     real ORATS zip via the `load`+`panel` stages) and augment it. This is the
//     real-ORATS-dataset run.
//   * otherwise               -> a deterministic synthetic ORATS-shaped panel
//     (CI / portable default). Same code path, same augmentation.
//
// The FAST-vs-ORACLE differential (correctness of the VM against the reference
// interpreter) runs on the synthetic panel only — it is skipped when a (large)
// real panel is supplied, to keep the real-data run fast.

// std::getenv is flagged deprecated by clang-cl's CRT; this harness only reads
// env knobs (panel/fixture paths), so silence the MSVC CRT deprecation.
#define _CRT_SECURE_NO_WARNINGS 1

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/eval/perf_metrics.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "serialize_panel.hpp" // atx::impl::read_panel

#include "alpha101_support.hpp"

#ifndef ATX_IMPL_TESTS_DIR
#define ATX_IMPL_TESTS_DIR "."
#endif

namespace atxtest_alpha101_orats_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

// Process-lifetime Library: parsed Asts borrow OpSig pointers from it.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] std::string fixture_path() {
  if (const char *e = std::getenv("ATX_ALPHA101_FIXTURE")) {
    return std::string{e};
  }
  return std::string{ATX_IMPL_TESTS_DIR} + "/fixtures/alpha101.txt";
}

// Build the evaluation panel: a real serialized ORATS panel if ATX_ALPHA101_PANEL
// is set, else the synthetic ORATS-shaped panel. Either way it is augmented with
// the full Alpha101 field vocabulary.
[[nodiscard]] Panel build_eval_panel(std::span<const atx::u16> adv_windows, bool &is_real) {
  is_real = false;
  if (const char *p = std::getenv("ATX_ALPHA101_PANEL")) {
    auto raw = atx::impl::read_panel(p);
    EXPECT_TRUE(raw.has_value()) << "read_panel('" << p
                                 << "'): " << (raw ? "" : raw.error().message());
    if (raw.has_value()) {
      auto aug = atx_impl_test::augment_for_alpha101(*raw, adv_windows);
      EXPECT_TRUE(aug.has_value()) << "augment(real): " << (aug ? "" : aug.error().message());
      if (aug.has_value()) {
        is_real = true;
        return std::move(*aug);
      }
    }
  }
  Panel synth = atx_impl_test::make_synth_orats_panel();
  auto aug = atx_impl_test::augment_for_alpha101(synth, adv_windows);
  EXPECT_TRUE(aug.has_value()) << "augment(synth): " << (aug ? "" : aug.error().message());
  return aug.has_value() ? std::move(*aug) : std::move(synth);
}

// Per-alpha pipeline outcome.
struct Outcome {
  int id{};
  bool parse_ok{};
  bool compile_ok{};
  bool eval_ok{};
  atx::usize finite{};
  atx::usize in_univ{};
  std::string err;
};

[[nodiscard]] Outcome run_one(const atx_impl_test::FixtureAlpha &fa, const Panel &panel) {
  Outcome o;
  o.id = fa.id;

  auto ast = parse_expr(fa.dsl, shared_lib());
  if (!ast) {
    o.err = "PARSE: " + ast.error().message();
    return o;
  }
  o.parse_ok = true;

  auto ana = analyze(*ast);
  if (!ana) {
    o.err = "ANALYZE: " + ana.error().message();
    return o;
  }
  auto prog = compile(*ast, *ana);
  if (!prog) {
    o.err = "COMPILE: " + prog.error().message();
    return o;
  }
  o.compile_ok = true;

  Engine engine{panel};
  auto ss = engine.evaluate(*prog);
  if (!ss) {
    o.err = "EVAL: " + ss.error().message();
    return o;
  }
  o.eval_ok = true;

  // Finite-in-universe coverage of the (single) alpha root.
  if (!ss->alphas.empty()) {
    const std::vector<atx::f64> &v = ss->alphas[0].values;
    const atx::usize I = panel.instruments();
    for (atx::usize d = 0; d < panel.dates(); ++d) {
      for (atx::usize n = 0; n < I; ++n) {
        if (!panel.in_universe(d, n)) {
          continue;
        }
        ++o.in_univ;
        const atx::usize i = d * I + n;
        if (i < v.size() && std::isfinite(v[i])) {
          ++o.finite;
        }
      }
    }
  }
  return o;
}

// ===========================================================================
//  Main verification: every Alpha101 parses, compiles, evaluates, and yields
//  at least one finite in-universe cell.
// ===========================================================================

TEST(Alpha101Orats, AllParseCompileEvaluate) {
  const std::string fx_path = fixture_path();
  const std::vector<atx_impl_test::FixtureAlpha> alphas =
      atx_impl_test::read_alpha_fixture(fx_path);
  ASSERT_EQ(alphas.size(), 101U)
      << "fixture '" << fx_path << "' must contain 101 alphas; got " << alphas.size();

  const std::vector<atx::u16> adv = atx_impl_test::collect_adv_windows(alphas);
  bool is_real = false;
  const Panel panel = build_eval_panel(adv, is_real);

  std::cout << "\n=== Alpha101 ORATS verification ===\n"
            << "panel: " << (is_real ? "REAL ORATS (ATX_ALPHA101_PANEL)" : "synthetic ORATS-shaped")
            << "  dates=" << panel.dates() << " instruments=" << panel.instruments()
            << " fields=" << panel.num_fields() << "\n"
            << "fixture: " << fx_path << "\n"
            << "adv windows materialized: ";
  for (const atx::u16 w : adv) {
    std::cout << w << " ";
  }
  std::cout << "\n\n id | parse compile eval | finite/in-univ | note\n"
            << "----+--------------------+----------------+-----------------------------\n";

  int n_parse = 0;
  int n_compile = 0;
  int n_eval = 0;
  int n_finite = 0;
  for (const atx_impl_test::FixtureAlpha &fa : alphas) {
    const Outcome o = run_one(fa, panel);
    n_parse += o.parse_ok ? 1 : 0;
    n_compile += o.compile_ok ? 1 : 0;
    n_eval += o.eval_ok ? 1 : 0;
    const bool has_finite = o.finite > 0;
    n_finite += has_finite ? 1 : 0;

    std::cout << " " << (o.id < 10 ? "  " : (o.id < 100 ? " " : "")) << o.id << " |   "
              << (o.parse_ok ? "Y" : "N") << "      " << (o.compile_ok ? "Y" : "N") << "     "
              << (o.eval_ok ? "Y" : "N") << "  | " << o.finite << "/" << o.in_univ;
    if (!o.err.empty()) {
      std::cout << "   <- " << o.err;
    } else if (!has_finite) {
      std::cout << "   <- all-nonfinite";
    }
    std::cout << "\n";
  }

  std::cout << "\nsummary: parse " << n_parse << "/101  compile " << n_compile << "/101  eval "
            << n_eval << "/101  finite " << n_finite << "/101\n"
            << "===================================\n\n";

  EXPECT_EQ(n_parse, 101) << "not all alphas parsed";
  EXPECT_EQ(n_compile, 101) << "not all alphas compiled (type-checked + lowered)";
  EXPECT_EQ(n_eval, 101) << "not all alphas evaluated without error";
  EXPECT_EQ(n_finite, 101) << "some alpha produced no finite in-universe cell";
}

// ===========================================================================
//  Correctness: the fast VM agrees with the reference oracle, cell-by-cell, for
//  every Alpha101. Runs on the synthetic panel only (skipped for a supplied
//  real panel, which may be large).
// ===========================================================================

TEST(Alpha101Orats, FastEqualsOracle_Synthetic) {
  if (std::getenv("ATX_ALPHA101_PANEL") != nullptr) {
    GTEST_SKIP() << "real panel supplied; oracle differential runs on synthetic only";
  }
  const std::vector<atx_impl_test::FixtureAlpha> alphas =
      atx_impl_test::read_alpha_fixture(fixture_path());
  ASSERT_EQ(alphas.size(), 101U);

  const std::vector<atx::u16> adv = atx_impl_test::collect_adv_windows(alphas);
  bool is_real = false;
  const Panel panel = build_eval_panel(adv, is_real);

  int checked = 0;
  for (const atx_impl_test::FixtureAlpha &fa : alphas) {
    auto ast = parse_expr(fa.dsl, shared_lib());
    ASSERT_TRUE(ast.has_value()) << "#" << fa.id << " parse: " << ast.error().message();
    auto ana = analyze(*ast);
    ASSERT_TRUE(ana.has_value()) << "#" << fa.id << " analyze: " << ana.error().message();
    auto prog = compile(*ast, *ana);
    ASSERT_TRUE(prog.has_value()) << "#" << fa.id << " compile: " << prog.error().message();

    Engine engine{panel};
    auto fast = engine.evaluate(*prog);
    ASSERT_TRUE(fast.has_value()) << "#" << fa.id << " vm: " << fast.error().message();
    auto oracle = evaluate_reference(*prog, panel);
    ASSERT_TRUE(oracle.has_value()) << "#" << fa.id << " oracle: " << oracle.error().message();

    ASSERT_EQ(fast->alphas.size(), oracle->alphas.size());
    for (atx::usize a = 0; a < fast->alphas.size(); ++a) {
      const std::vector<atx::f64> &fv = fast->alphas[a].values;
      const std::vector<atx::f64> &ov = oracle->alphas[a].values;
      ASSERT_EQ(fv.size(), ov.size());
      for (atx::usize i = 0; i < fv.size(); ++i) {
        const bool agree = (std::isnan(fv[i]) && std::isnan(ov[i])) || fv[i] == ov[i];
        ASSERT_TRUE(agree) << "#" << fa.id << " cell " << i << " FAST=" << fv[i]
                           << " ORACLE=" << ov[i];
      }
    }
    ++checked;
  }
  EXPECT_EQ(checked, 101);
}

// ===========================================================================
//  Sharpe ranking — backtest each alpha as a daily-rebalanced, dollar-neutral,
//  L1-normalized cross-sectional signal and report its annualized Sharpe.
//
//  Per date d: weights w_i = (s_i - mean) / sum|s_j - mean| over in-universe,
//  finite signal cells (dollar-neutral, gross-1). PnL realized at d+1 is
//  sum_i w_i * returns_i(d+1) (close-to-close forward return; the augmented
//  `returns` field). Sharpe is the engine's single convention via
//  eval::compute_return_metrics (sqrt(252)*mean/std_pop over pnl[1..T)).
//
//  Most meaningful on the real ORATS panel; on the synthetic random-walk panel
//  Sharpes are ~0 by construction. Informational (prints a ranking); the only
//  hard assertion is that every alpha yields a finite Sharpe.
// ===========================================================================

// Cross-sectional signal conditioning applied per date, over in-universe
// finite cells only, BEFORE the demean/IV/L1/pnl pipeline.
enum class Cond {
  Identity,    // no-op: values passed through unchanged
  Rank,        // ordinal percentile in [0,1]; stable ascending sort,
               //   tie-break by ascending instrument index; singleton -> 0.5
  WinsorZscore // population winsorize to [mu-4*sigma, mu+4*sigma], then
               //   z = (clamped - mu)/sigma; guard sigma==0 -> all zero
};

// Core backtesting function used by all weighting variants.
//
// Pipeline per date d:
//   raw -> CONDITION (cond) -> demean -> [optional /IV] -> L1-normalize -> pnl[d+1]
//
// Conditioning is within-date only (no cross-date / look-ahead). Reductions
// over in-universe finite cells only, in fixed ascending instrument-index order.
//
// Turnover proxy: mean over rebalance days of sum_i |w_i(d) - w_i(d-1)| where
// w_i(d) are the post-L1 weights at date d. The first valid rebalance day has
// no predecessor so it is skipped (contributes neither to the numerator nor the
// denominator of the mean). When turnover_out != nullptr, the turnover proxy is
// written there; otherwise turnover is not computed (no extra allocations).
//
// Returns annualized Sharpe. If inverse_vol=true, each demeaned weight is
// divided by the name's ATM 126d implied vol before L1-normalizing (risk-parity
// sizing); names with missing/non-positive IV are dropped.
[[nodiscard]] atx::f64 weighting_score(const SignalSet &ss, const Panel &panel,
                                       Cond cond, bool inverse_vol,
                                       atx::f64 *turnover_out) {
  if (ss.alphas.empty()) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  auto rid = panel.field_id("returns");
  if (!rid) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  const std::span<const atx::f64> ret = panel.field_all(*rid);

  std::span<const atx::f64> iv;
  if (inverse_vol) {
    auto vid = panel.field_id("atmCenI_126d");
    if (!vid) {
      return std::numeric_limits<atx::f64>::quiet_NaN();
    }
    iv = panel.field_all(*vid);
  }

  const std::vector<atx::f64> &v = ss.alphas[0].values;
  const atx::usize D = panel.dates();
  const atx::usize I = panel.instruments();

  // Working buffers.
  std::vector<atx::f64> pnl(D, 0.0);
  std::vector<atx::f64> w(I, 0.0);      // current day post-L1 weights
  std::vector<atx::f64> w_prev(I, 0.0); // previous rebalance day post-L1 weights
  // buf holds conditioned values for in-universe finite cells.
  // We also record indices for the Rank sort.
  std::vector<atx::f64> buf;
  std::vector<atx::usize> valid_idx;
  buf.reserve(I);
  valid_idx.reserve(I);

  atx::f64 turnover_sum = 0.0;
  atx::usize turnover_days = 0; // count of rebalance days that had a valid book AND a predecessor
  bool has_prev = false;        // whether w_prev is initialized from a prior valid day

  for (atx::usize d = 0; d + 1 < D; ++d) {
    // --- Step 1: gather in-universe finite cells into working buffers. ---
    buf.clear();
    valid_idx.clear();
    for (atx::usize i = 0; i < I; ++i) {
      const atx::usize idx = d * I + i;
      if (panel.in_universe(d, i) && idx < v.size() && std::isfinite(v[idx])) {
        buf.push_back(v[idx]);
        valid_idx.push_back(i);
      }
    }
    const atx::usize cnt = valid_idx.size();
    if (cnt == 0) {
      continue;
    }

    // --- Step 2: condition (in-place on buf). ---
    if (cond == Cond::Rank) {
      // Ordinal percentile in [0,1]. Stable ascending sort of indices by value,
      // tie-break by ascending instrument index (stable_sort preserves the
      // ascending-index order we gathered in). Matches cs_rank_row semantics.
      std::vector<atx::usize> order(cnt);
      for (atx::usize k = 0; k < cnt; ++k) {
        order[k] = k; // indices into buf/valid_idx
      }
      std::stable_sort(order.begin(), order.end(),
                       [&](atx::usize a, atx::usize b) { return buf[a] < buf[b]; });
      std::vector<atx::f64> ranked(cnt);
      for (atx::usize r = 0; r < cnt; ++r) {
        ranked[order[r]] = (cnt == 1) ? 0.5
                                      : static_cast<atx::f64>(r) / static_cast<atx::f64>(cnt - 1);
      }
      buf = std::move(ranked);
    } else if (cond == Cond::WinsorZscore) {
      // Population mean and population std (ddof=0) over the in-universe
      // finite cells. Winsorize to [mu-4*sigma, mu+4*sigma], then z-score.
      // Guard: sigma==0 -> all zero (day contributes nothing after demean).
      atx::f64 sum = 0.0;
      for (const atx::f64 x : buf) {
        sum += x;
      }
      const atx::f64 mu = sum / static_cast<atx::f64>(cnt);
      atx::f64 ss2 = 0.0;
      for (const atx::f64 x : buf) {
        const atx::f64 d2 = x - mu;
        ss2 += d2 * d2;
      }
      const atx::f64 sigma = std::sqrt(ss2 / static_cast<atx::f64>(cnt));
      if (sigma == 0.0) {
        // All values identical -> z-score is zero everywhere; demean will zero
        // them further, so this day contributes nothing to PnL. Zero out buf.
        for (atx::f64 &x : buf) {
          x = 0.0;
        }
      } else {
        const atx::f64 lo = mu - 4.0 * sigma;
        const atx::f64 hi = mu + 4.0 * sigma;
        for (atx::f64 &x : buf) {
          const atx::f64 clamped = (x < lo) ? lo : (x > hi ? hi : x);
          x = (clamped - mu) / sigma;
        }
      }
    }
    // Cond::Identity: buf unchanged.

    // --- Step 3: demean conditioned values. ---
    atx::f64 csum = 0.0;
    for (const atx::f64 x : buf) {
      csum += x;
    }
    const atx::f64 cmean = csum / static_cast<atx::f64>(cnt);

    // --- Step 4: build weight vector (demean + optional /IV), L1-normalize. ---
    std::fill(w.begin(), w.end(), 0.0);
    atx::f64 l1 = 0.0;
    for (atx::usize k = 0; k < cnt; ++k) {
      const atx::usize i = valid_idx[k];
      atx::f64 wi = buf[k] - cmean;
      if (inverse_vol) {
        const atx::usize idx = d * I + i;
        const atx::f64 s = (idx < iv.size()) ? iv[idx]
                                              : std::numeric_limits<atx::f64>::quiet_NaN();
        // No risk estimate -> cannot size; drop the name.
        wi = (std::isfinite(s) && s > 0.0) ? (wi / s) : 0.0;
      }
      w[i] = wi;
      l1 += std::fabs(wi);
    }
    if (l1 == 0.0) {
      continue;
    }
    // Normalize weights to gross-1.
    for (atx::usize i = 0; i < I; ++i) {
      w[i] /= l1;
    }

    // --- Step 5: turnover proxy (post-L1 weights). ---
    if (turnover_out != nullptr) {
      if (has_prev) {
        atx::f64 tv = 0.0;
        for (atx::usize i = 0; i < I; ++i) {
          tv += std::fabs(w[i] - w_prev[i]);
        }
        turnover_sum += tv;
        ++turnover_days;
      }
      // Update prev regardless (first valid day becomes the predecessor for the next).
      w_prev = w;
      has_prev = true;
    }

    // --- Step 6: forward PnL at d+1. ---
    atx::f64 p = 0.0;
    for (atx::usize i = 0; i < I; ++i) {
      if (w[i] == 0.0) {
        continue;
      }
      const atx::usize idx1 = (d + 1) * I + i;
      const atx::f64 r = (idx1 < ret.size()) ? ret[idx1]
                                              : std::numeric_limits<atx::f64>::quiet_NaN();
      if (panel.in_universe(d + 1, i) && std::isfinite(r)) {
        p += w[i] * r;
      }
    }
    pnl[d + 1] = p;
  }

  if (turnover_out != nullptr) {
    *turnover_out = (turnover_days > 0)
                       ? (turnover_sum / static_cast<atx::f64>(turnover_days))
                       : std::numeric_limits<atx::f64>::quiet_NaN();
  }

  const atx::engine::eval::ReturnMetricsCfg cfg{};
  return atx::engine::eval::compute_return_metrics(pnl, cfg).sharpe;
}

// Keep existing signature + behavior EXACTLY (delegate to weighting_score).
// inverse_vol=true: risk-parity sizing — each demeaned cross-sectional weight is
// divided by the name's ATM 126d implied move (atmCenI_126d) before L1-normalizing,
// so two names with the same signal tilt but different vol carry equal *risk*
// rather than equal dollars. Names with missing/non-positive IV are dropped
// (cannot be risk-sized), which shrinks the effective universe to IV-covered
// names. Dollar-neutrality drifts slightly after the per-name vol scale (the
// demeaning is applied to the raw signal, not re-applied post-scale).
[[nodiscard]] atx::f64 alpha_sharpe_impl(const SignalSet &ss, const Panel &panel,
                                         bool inverse_vol) {
  return weighting_score(ss, panel, Cond::Identity, inverse_vol, nullptr);
}

[[nodiscard]] atx::f64 alpha_sharpe(const SignalSet &ss, const Panel &panel) {
  return alpha_sharpe_impl(ss, panel, /*inverse_vol=*/false);
}

TEST(Alpha101Orats, RankBySharpe) {
  const std::vector<atx_impl_test::FixtureAlpha> alphas =
      atx_impl_test::read_alpha_fixture(fixture_path());
  ASSERT_EQ(alphas.size(), 101U);
  const std::vector<atx::u16> adv = atx_impl_test::collect_adv_windows(alphas);
  bool is_real = false;
  const Panel panel = build_eval_panel(adv, is_real);

  struct Row {
    int id{};
    atx::f64 sharpe{};
  };
  std::vector<Row> rows;
  rows.reserve(alphas.size());
  int finite = 0;
  for (const atx_impl_test::FixtureAlpha &fa : alphas) {
    auto ast = parse_expr(fa.dsl, shared_lib());
    ASSERT_TRUE(ast.has_value()) << "#" << fa.id << ": " << ast.error().message();
    auto ana = analyze(*ast);
    ASSERT_TRUE(ana.has_value()) << "#" << fa.id << ": " << ana.error().message();
    auto prog = compile(*ast, *ana);
    ASSERT_TRUE(prog.has_value()) << "#" << fa.id << ": " << prog.error().message();
    Engine engine{panel};
    auto ss = engine.evaluate(*prog);
    ASSERT_TRUE(ss.has_value()) << "#" << fa.id << ": " << ss.error().message();
    const atx::f64 sr = alpha_sharpe(*ss, panel);
    finite += std::isfinite(sr) ? 1 : 0;
    rows.push_back({fa.id, sr});
  }

  std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    const bool an = std::isnan(a.sharpe);
    const bool bn = std::isnan(b.sharpe);
    if (an != bn) {
      return !an; // finite before NaN
    }
    return a.sharpe > b.sharpe;
  });

  std::cout << "\n=== Alpha101 Sharpe ranking ("
            << (is_real ? "REAL ORATS" : "synthetic") << ", daily LS dollar-neutral) ===\n"
            << "rank |  # | annualized Sharpe\n"
            << "-----+----+------------------\n";
  for (atx::usize k = 0; k < rows.size(); ++k) {
    std::cout << " " << (k + 1 < 10 ? "  " : (k + 1 < 100 ? " " : "")) << (k + 1) << " | "
              << (rows[k].id < 10 ? " " : "") << rows[k].id << " | " << rows[k].sharpe << "\n";
  }
  std::cout << "============================================\n\n";

  EXPECT_EQ(finite, 101) << "every alpha must yield a finite Sharpe";
}

// Risk-parity comparison: equal-dollar (L1) vs inverse-IV (equal-risk) sizing.
// Per alpha, evaluate once and score both weightings; rank by the inverse-vol
// Sharpe and print the equal-dollar Sharpe alongside with the delta. Reports
// IV126 coverage so the universe-shrink effect of dropping IV-less names is
// visible. Informational; hard assertion only that both are finite.
TEST(Alpha101Orats, RankBySharpeRiskParity) {
  const std::vector<atx_impl_test::FixtureAlpha> alphas =
      atx_impl_test::read_alpha_fixture(fixture_path());
  ASSERT_EQ(alphas.size(), 101U);
  const std::vector<atx::u16> adv = atx_impl_test::collect_adv_windows(alphas);
  bool is_real = false;
  const Panel panel = build_eval_panel(adv, is_real);

  // IV126 coverage among in-universe cells (context for the universe shrink).
  {
    auto vid = panel.field_id("atmCenI_126d");
    ASSERT_TRUE(vid.has_value()) << "panel missing atmCenI_126d field";
    const std::span<const atx::f64> iv = panel.field_all(*vid);
    const atx::usize D = panel.dates();
    const atx::usize I = panel.instruments();
    atx::usize inu = 0, cov = 0;
    for (atx::usize d = 0; d < D; ++d) {
      for (atx::usize i = 0; i < I; ++i) {
        if (!panel.in_universe(d, i)) {
          continue;
        }
        ++inu;
        const atx::usize idx = d * I + i;
        if (idx < iv.size() && std::isfinite(iv[idx]) && iv[idx] > 0.0) {
          ++cov;
        }
      }
    }
    const double pct = (inu == 0) ? 0.0 : 100.0 * static_cast<double>(cov) /
                                              static_cast<double>(inu);
    std::cout << "\nIV126 coverage: " << cov << " / " << inu << " in-universe cells ("
              << pct << "%) carry a usable atmCenI_126d\n";
  }

  struct Row {
    int id{};
    atx::f64 eqdollar{};
    atx::f64 eqrisk{};
  };
  std::vector<Row> rows;
  rows.reserve(alphas.size());
  int finite = 0;
  for (const atx_impl_test::FixtureAlpha &fa : alphas) {
    auto ast = parse_expr(fa.dsl, shared_lib());
    ASSERT_TRUE(ast.has_value()) << "#" << fa.id << ": " << ast.error().message();
    auto ana = analyze(*ast);
    ASSERT_TRUE(ana.has_value()) << "#" << fa.id << ": " << ana.error().message();
    auto prog = compile(*ast, *ana);
    ASSERT_TRUE(prog.has_value()) << "#" << fa.id << ": " << prog.error().message();
    Engine engine{panel};
    auto ss = engine.evaluate(*prog);
    ASSERT_TRUE(ss.has_value()) << "#" << fa.id << ": " << ss.error().message();
    const atx::f64 d0 = alpha_sharpe_impl(*ss, panel, /*inverse_vol=*/false);
    const atx::f64 d1 = alpha_sharpe_impl(*ss, panel, /*inverse_vol=*/true);
    finite += (std::isfinite(d0) && std::isfinite(d1)) ? 1 : 0;
    rows.push_back({fa.id, d0, d1});
  }

  std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    const bool an = std::isnan(a.eqrisk);
    const bool bn = std::isnan(b.eqrisk);
    if (an != bn) {
      return !an;
    }
    return a.eqrisk > b.eqrisk;
  });

  int improved = 0;
  for (const Row &r : rows) {
    if (std::isfinite(r.eqrisk) && std::isfinite(r.eqdollar) && r.eqrisk > r.eqdollar) {
      ++improved;
    }
  }

  std::cout << "\n=== Alpha101 Sharpe: equal-dollar (L1) vs inverse-IV risk-parity ("
            << (is_real ? "REAL ORATS" : "synthetic") << ") ===\n"
            << "rank |  # | eq-dollar | eq-risk(1/IV) |  delta\n"
            << "-----+----+-----------+---------------+--------\n";
  for (atx::usize k = 0; k < rows.size(); ++k) {
    std::cout << " " << (k + 1 < 10 ? "  " : (k + 1 < 100 ? " " : "")) << (k + 1) << " | "
              << (rows[k].id < 10 ? " " : "") << rows[k].id << " | " << rows[k].eqdollar
              << " | " << rows[k].eqrisk << " | "
              << (rows[k].eqrisk - rows[k].eqdollar) << "\n";
  }
  std::cout << "improved by risk-parity: " << improved << " / " << rows.size() << "\n";
  std::cout << "============================================\n\n";

  EXPECT_EQ(finite, 101) << "every alpha must yield finite Sharpe in both weightings";
}

// ===========================================================================
//  Signal-conditioning comparison — four weighting variants side-by-side.
//
//  Per alpha, computes four Sharpes (+ their turnover proxies):
//    eq-dollar   = Identity conditioning + equal-dollar sizing
//    inverse-IV  = Identity conditioning + inverse-IV (risk-parity) sizing
//    rank        = Rank conditioning       + equal-dollar sizing
//    wins+z      = WinsorZscore conditioning + equal-dollar sizing
//
//  Regression pin (hard): eq-dollar column == alpha_sharpe(*ss, panel) exactly.
//  Since Identity+eq-dollar delegates to the same code, this is exact equality.
//
//  TODO(Phase 2): add an mv(lambda*) column to this table (mean-variance
//  optimal weights via quadratic program over the cross-section).
// ===========================================================================
TEST(Alpha101Orats, RankBySharpeWeightings) {
  const std::vector<atx_impl_test::FixtureAlpha> alphas =
      atx_impl_test::read_alpha_fixture(fixture_path());
  ASSERT_EQ(alphas.size(), 101U);
  const std::vector<atx::u16> adv = atx_impl_test::collect_adv_windows(alphas);
  bool is_real = false;
  const Panel panel = build_eval_panel(adv, is_real);

  struct Row {
    int id{};
    atx::f64 eq_dollar{};
    atx::f64 inv_iv{};
    atx::f64 rank{};
    atx::f64 wins_z{};
    atx::f64 to_eq{};    // turnover proxy for eq-dollar
    atx::f64 to_inv{};   // turnover proxy for inverse-IV
    atx::f64 to_rank{};  // turnover proxy for rank
    atx::f64 to_wz{};    // turnover proxy for wins+z
    atx::f64 best{};
    const char *best_method{};
    atx::f64 best_to{};
  };

  std::vector<Row> rows;
  rows.reserve(alphas.size());

  // IV availability: inverse-IV scoring requires atmCenI_126d; on the synthetic
  // panel this field is absent, so inverse-IV Sharpes will be NaN (matching the
  // existing RankBySharpeRiskParity behaviour on the synthetic panel). We include
  // it in the all-finite assertion only when the field is present.
  const bool has_iv = panel.field_id("atmCenI_126d").has_value();

  int finite_cs_count  = 0; // count with eq-dollar/rank/wins+z all finite (always asserted)
  int finite_all_count = 0; // count with all four finite (asserted only when IV present)

  for (const atx_impl_test::FixtureAlpha &fa : alphas) {
    auto ast = parse_expr(fa.dsl, shared_lib());
    ASSERT_TRUE(ast.has_value()) << "#" << fa.id << ": " << ast.error().message();
    auto ana = analyze(*ast);
    ASSERT_TRUE(ana.has_value()) << "#" << fa.id << ": " << ana.error().message();
    auto prog = compile(*ast, *ana);
    ASSERT_TRUE(prog.has_value()) << "#" << fa.id << ": " << prog.error().message();
    Engine engine{panel};
    auto ss = engine.evaluate(*prog);
    ASSERT_TRUE(ss.has_value()) << "#" << fa.id << ": " << ss.error().message();

    Row r;
    r.id = fa.id;
    r.eq_dollar = weighting_score(*ss, panel, Cond::Identity,     false, &r.to_eq);
    r.inv_iv    = weighting_score(*ss, panel, Cond::Identity,     true,  &r.to_inv);
    r.rank      = weighting_score(*ss, panel, Cond::Rank,         false, &r.to_rank);
    r.wins_z    = weighting_score(*ss, panel, Cond::WinsorZscore, false, &r.to_wz);

    // Regression pin: eq-dollar column must exactly equal alpha_sharpe(*ss, panel).
    // Identity+eq-dollar delegates to the same weighting_score code path, so
    // this is structural equality — any mismatch means the refactor changed behavior.
    EXPECT_EQ(r.eq_dollar, alpha_sharpe(*ss, panel))
        << "Regression pin failed for alpha #" << fa.id
        << ": eq-dollar Sharpe mismatch vs alpha_sharpe()";

    // Pick best-of-four (NaN treated as -inf for comparison purposes).
    struct Candidate { atx::f64 sharpe; atx::f64 to; const char *name; };
    const Candidate candidates[] = {
      {r.eq_dollar, r.to_eq,   "eq-dollar"},
      {r.inv_iv,    r.to_inv,  "inverse-IV"},
      {r.rank,      r.to_rank, "rank"},
      {r.wins_z,    r.to_wz,   "wins+z"},
    };
    r.best = std::numeric_limits<atx::f64>::quiet_NaN();
    r.best_method = "none";
    r.best_to = std::numeric_limits<atx::f64>::quiet_NaN();
    for (const auto &c : candidates) {
      if (!std::isfinite(r.best) || (std::isfinite(c.sharpe) && c.sharpe > r.best)) {
        r.best = c.sharpe;
        r.best_method = c.name;
        r.best_to = c.to;
      }
    }

    const bool cs_finite = std::isfinite(r.eq_dollar) && std::isfinite(r.rank)
                        && std::isfinite(r.wins_z);
    const bool all_finite = cs_finite && std::isfinite(r.inv_iv);
    finite_cs_count  += cs_finite  ? 1 : 0;
    finite_all_count += all_finite ? 1 : 0;

    rows.push_back(r);
  }

  // Hard assertions: the three conditioning methods (no IV dependency) must always
  // be finite on every panel. The all-four assertion is applied only when the panel
  // carries atmCenI_126d (the real ORATS panel does; the synthetic panel does not).
  EXPECT_EQ(finite_cs_count, 101)
      << "every alpha must yield finite Sharpe for eq-dollar/rank/wins+z weightings";
  if (has_iv) {
    EXPECT_EQ(finite_all_count, 101)
        << "every alpha must yield finite Sharpe in all four weightings (real panel)";
  }

  // Sort by best-of-four Sharpe descending; NaN last.
  std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    const bool an = std::isnan(a.best);
    const bool bn = std::isnan(b.best);
    if (an != bn) {
      return !an;
    }
    return a.best > b.best;
  });

  // Summary statistics per method.
  auto method_stats = [&](auto sharpe_fn, auto name) {
    int improved = 0;
    atx::f64 peak = std::numeric_limits<atx::f64>::quiet_NaN();
    atx::f64 sum_sr = 0.0;
    int fin = 0;
    for (const Row &r : rows) {
      const atx::f64 sr = sharpe_fn(r);
      const atx::f64 eq = r.eq_dollar;
      if (std::isfinite(sr)) {
        ++fin;
        sum_sr += sr;
        if (!std::isfinite(peak) || sr > peak) {
          peak = sr;
        }
        if (std::isfinite(eq) && sr > eq) {
          ++improved;
        }
      }
    }
    const atx::f64 mean_sr = (fin > 0) ? (sum_sr / static_cast<atx::f64>(fin))
                                        : std::numeric_limits<atx::f64>::quiet_NaN();
    std::cout << "  " << name << ": improved=" << improved << "/101"
              << "  peak=" << peak << "  mean=" << mean_sr << "\n";
  };

  std::cout << "\n=== Alpha101 Sharpe: signal-conditioning comparison ("
            << (is_real ? "REAL ORATS" : "synthetic") << ") ===\n"
            << " # | eq-dollar | inverse-IV |   rank  |  wins+z | best-method | turnover(best)\n"
            << "---+-----------+------------+---------+---------+-------------+---------------\n";
  for (const Row &r : rows) {
    std::cout << (r.id < 10 ? "  " : (r.id < 100 ? " " : "")) << r.id
              << " | " << r.eq_dollar
              << " | " << r.inv_iv
              << " | " << r.rank
              << " | " << r.wins_z
              << " | " << r.best_method
              << " | " << r.best_to
              << "\n";
  }

  std::cout << "\nSummary (count improved vs eq-dollar, peak Sharpe, mean Sharpe over finite):\n";
  method_stats([](const Row &r) { return r.eq_dollar; }, "eq-dollar ");
  method_stats([](const Row &r) { return r.inv_iv;    }, "inverse-IV");
  method_stats([](const Row &r) { return r.rank;      }, "rank      ");
  method_stats([](const Row &r) { return r.wins_z;    }, "wins+z    ");
  std::cout << "============================================\n\n";
}

} // namespace atxtest_alpha101_orats_test

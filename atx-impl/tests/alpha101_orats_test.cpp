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

#include <cstdlib>
#include <iostream>
#include <string>
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

} // namespace atxtest_alpha101_orats_test

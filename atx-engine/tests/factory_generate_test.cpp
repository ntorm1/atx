// atx::engine::factory — grammar-typed generation (S3.5).
//
// The headline: a type-targeted sampler emits genomes that are analyze-VALID by
// construction (rejection rate ≈ 0), measured against a type-BLIND control that
// ignores dtype/shape/role (high rejection rate). The delta is the unit's payoff.
// Plus: determinism (same seed ⇒ same expression) and a differential check that
// generated genomes actually run (oracle == VM), i.e. valid-by-construction holds
// end-to-end, not just through analyze.
//
// Naming: Subject_Condition_ExpectedResult.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/oracle.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/factory/generate.hpp"

namespace {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::evaluate_reference;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::factory::GenConfig;
using atx::engine::factory::generate_control_expr;
using atx::engine::factory::generate_expr;
using atx::engine::factory::generate_genome;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// True iff `src` parses AND analyzes (i.e. the type-checker accepts it).
[[nodiscard]] bool accepts(const std::string &src) {
  auto ast = parse_expr(src, shared_lib());
  if (!ast) {
    return false;
  }
  return analyze(ast.value()).has_value();
}

// A panel carrying every field the generator can reference (so generated genomes
// can be compiled + evaluated). Numeric fields get arbitrary values; the group
// classifiers get small integer labels.
[[nodiscard]] Panel make_panel() {
  constexpr atx::usize dates = 6;
  constexpr atx::usize instruments = 5;
  constexpr atx::usize cells = dates * instruments;
  const std::vector<std::string> names = {
      "close",          "open",           "high",  "low", "volume", "vwap", "adv20",
      "IndClass.sector", "IndClass.industry", "IndClass.subindustry"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(cells));
  for (atx::usize f = 0; f < names.size(); ++f) {
    const bool is_group = f >= 7;
    for (atx::usize i = 0; i < cells; ++i) {
      cols[f][i] = is_group ? static_cast<atx::f64>(i % 3)
                            : 1.0 + static_cast<atx::f64>((i * (f + 1)) % 97);
    }
  }
  auto p = Panel::create(dates, instruments, std::vector<std::string>{names},
                         std::move(cols), {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// ===========================================================================
//  Rejection rate — the headline: typed ≈ 0, control high.
// ===========================================================================

TEST(GrammarGen, TypedAcceptsAll_ControlRejectsMany) {
  GenConfig cfg;
  constexpr int kN = 3000;
  Xoshiro256pp rng_typed(0x6311ULL);
  Xoshiro256pp rng_ctrl(0x6311ULL);
  int typed_rejected = 0;
  int ctrl_rejected = 0;
  for (int i = 0; i < kN; ++i) {
    if (!accepts(generate_expr(cfg, rng_typed))) {
      ++typed_rejected;
    }
    if (!accepts(generate_control_expr(cfg, rng_ctrl))) {
      ++ctrl_rejected;
    }
  }
  // Valid by construction: the type-targeted sampler is NEVER rejected.
  EXPECT_EQ(typed_rejected, 0) << "typed rejection rate = " << typed_rejected << "/" << kN;
  // The type-blind control is rejected at a materially higher rate (the yield
  // delta is the unit's headline — kept well clear of noise).
  EXPECT_GT(ctrl_rejected, kN / 4) << "control rejection rate = " << ctrl_rejected << "/" << kN;
  EXPECT_GT(ctrl_rejected, typed_rejected + kN / 4);
}

// ===========================================================================
//  Determinism — same seed ⇒ byte-identical expression (composes with S4).
// ===========================================================================

TEST(GrammarGen, SameSeed_SameExpression) {
  GenConfig cfg;
  Xoshiro256pp a(0xD1CEULL);
  Xoshiro256pp b(0xD1CEULL);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(generate_expr(cfg, a), generate_expr(cfg, b)) << "divergence at " << i;
  }
}

// ===========================================================================
//  End-to-end — generated genomes compile + run, oracle == VM bit-for-bit.
// ===========================================================================

TEST(GrammarGen, GeneratedGenomes_RunOracleEqualsVm) {
  GenConfig cfg;
  const Panel panel = make_panel();
  Xoshiro256pp rng(0x9AA9ULL);
  int evaluated = 0;
  for (int i = 0; i < 300; ++i) {
    auto g = generate_genome(cfg, shared_lib(), rng);
    ASSERT_TRUE(g.has_value()) << "generator emitted an invalid genome: "
                               << (g ? "" : g.error().message());
    auto prog = compile(g->ast, g->analysis);
    ASSERT_TRUE(prog.has_value()) << "compile: " << (prog ? "" : prog.error().message());
    Engine engine{panel};
    auto vm = engine.evaluate(prog.value_or(Program{}));
    ASSERT_TRUE(vm.has_value()) << "VM: " << (vm ? "" : vm.error().message());
    auto ref = evaluate_reference(prog.value_or(Program{}), panel);
    ASSERT_TRUE(ref.has_value()) << "oracle: " << (ref ? "" : ref.error().message());
    const SignalSet &v = vm.value();
    const SignalSet &r = ref.value();
    ASSERT_EQ(v.alphas.size(), r.alphas.size());
    for (atx::usize a = 0; a < v.alphas.size(); ++a) {
      ASSERT_EQ(v.alphas[a].values.size(), r.alphas[a].values.size());
      for (atx::usize c = 0; c < v.alphas[a].values.size(); ++c) {
        EXPECT_TRUE(same_cell(v.alphas[a].values[c], r.alphas[a].values[c])) << "genome " << i
                                                                             << " cell " << c;
      }
    }
    ++evaluated;
  }
  EXPECT_EQ(evaluated, 300); // every generated genome was VM-safe
}

} // namespace

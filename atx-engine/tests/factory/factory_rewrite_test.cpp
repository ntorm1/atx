// atx::engine::factory — semantic rewrite tests (L3, suite FactoryRewrite).
//
// The rule table in factory/rewrite.hpp is only allowed to contain identities
// the VM honours BIT-FOR-BIT. Every rule is driven here through the real
// parse -> analyze -> compile -> Engine path on a fixture panel that carries
// NaNs, signed zeros and ties, and the rewrite is checked to be idempotent.

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/factory/canonical.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/op_catalog.hpp"
#include "atx/engine/factory/rewrite.hpp"

namespace atx_test_l3_search_rewrite {

using atx::f64;
using atx::usize;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::factory::canonical_hash;
using atx::engine::factory::canonical_rewrite;
using atx::engine::factory::CanonCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::op_invariance;
using atx::engine::factory::RewriteCfg;
using atx::engine::factory::rewrite_ast;

[[nodiscard]] Genome genome_of(std::string_view src, const Library &lib) {
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << src;
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << src;
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// 40 dates x 7 instruments: `close` has ties within a date, `rev` carries NaNs,
// +0.0 and -0.0 cells so the NaN / signed-zero / tie paths of every kernel run.
[[nodiscard]] Panel rewrite_panel() {
  constexpr usize kDates = 40;
  constexpr usize kInsts = 7;
  std::vector<f64> close(kDates * kInsts);
  std::vector<f64> rev(kDates * kInsts);
  std::uint64_t s = 0x5EEDULL;
  for (usize i = 0; i < close.size(); ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const f64 u = static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U);
    close[i] = 50.0 + std::floor(u * 8.0); // coarse grid -> ties within a date
    rev[i] = u - 0.5;
    if (i % 11 == 3) {
      rev[i] = std::nan("");
    } else if (i % 13 == 5) {
      rev[i] = -0.0;
    } else if (i % 17 == 2) {
      rev[i] = 0.0;
    }
  }
  auto r = Panel::create(kDates, kInsts, {"close", "rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

[[nodiscard]] std::vector<std::uint64_t> eval_bits(const Genome &g, const Panel &panel) {
  auto prog = compile(g.ast, g.analysis);
  EXPECT_TRUE(prog.has_value());
  if (!prog) {
    return {};
  }
  Engine engine{panel};
  auto out = engine.evaluate(*prog);
  EXPECT_TRUE(out.has_value());
  if (!out || out->alphas.empty()) {
    return {};
  }
  std::vector<std::uint64_t> bits;
  bits.reserve(out->alphas.front().values.size());
  for (const f64 v : out->alphas.front().values) {
    bits.push_back(std::bit_cast<std::uint64_t>(v));
  }
  return bits;
}

struct Case {
  const char *before;
  const char *after;
};

// Every rule in the table, alone and composed.
[[nodiscard]] std::vector<Case> rule_cases() {
  return {
      {"-(-close)", "close"},
      {"-(-(-rev))", "-rev"},
      {"rank(rank(close))", "rank(close)"},
      {"rank(rank(rank(rev)))", "rank(rev)"},
      {"abs(abs(rev))", "abs(rev)"},
      {"sign(sign(rev))", "sign(rev)"},
      {"abs(-rev)", "abs(rev)"},
      {"abs(-(abs(rev)))", "abs(rev)"},
      {"rank(2 * close)", "rank(close)"},
      {"rank(close * 0.25)", "rank(close)"},
      {"rank(rev / 4)", "rank(rev)"},
      {"sign(rev * 8)", "sign(rev)"},
      {"rank(4 * rank(close))", "rank(close)"},
      {"rank(rank(-(-close)))", "rank(close)"},
      {"ts_mean(rank(rank(close)), 3)", "ts_mean(rank(close), 3)"},
      {"rank(close) - rank(rank(rev))", "rank(close) - rank(rev)"},
  };
}

TEST(FactoryRewrite, EveryRuleIsVmBitIdentical) {
  Library lib{};
  const Panel panel = rewrite_panel();
  for (const Case &c : rule_cases()) {
    const Genome before = genome_of(c.before, lib);
    const Genome after = genome_of(c.after, lib);
    const Genome rw = canonical_rewrite(before);
    EXPECT_EQ(canonical_hash(rw), canonical_hash(after)) << c.before;
    const auto b0 = eval_bits(before, panel);
    const auto b1 = eval_bits(rw, panel);
    ASSERT_FALSE(b0.empty()) << c.before;
    EXPECT_EQ(b0, b1) << c.before << " is not bit-identical after rewrite";
    EXPECT_EQ(b0, eval_bits(after, panel)) << c.before;
  }
}

TEST(FactoryRewrite, RewriteIsIdempotent) {
  Library lib{};
  std::vector<std::string> corpus;
  for (const Case &c : rule_cases()) {
    corpus.emplace_back(c.before);
  }
  corpus.emplace_back("ts_mean(close, 5) - rank(rev)");
  corpus.emplace_back("zscore(zscore(close))");
  for (const std::string &src : corpus) {
    const Genome once = canonical_rewrite(genome_of(src, lib));
    const Genome twice = canonical_rewrite(once);
    EXPECT_EQ(canonical_hash(once), canonical_hash(twice)) << src;
    EXPECT_EQ(once.ast.nodes().size(), twice.ast.nodes().size()) << src;
    usize fired = 99;
    const auto ast3 = rewrite_ast(once.ast, once.ast.roots().front().root, RewriteCfg{}, &fired);
    EXPECT_EQ(fired, 0U) << src << ": a normal form must contain no redex";
    EXPECT_EQ(ast3.nodes().size(), once.ast.nodes().size());
  }
}

TEST(FactoryRewrite, CompactsDeadNodes) {
  Library lib{};
  usize fired = 0;
  const Genome g = genome_of("-(-close)", lib);
  const auto ast = rewrite_ast(g.ast, g.ast.roots().front().root, RewriteCfg{}, &fired);
  EXPECT_EQ(fired, 1U);
  EXPECT_EQ(ast.nodes().size(), 1U); // just the `close` leaf
  ASSERT_EQ(ast.roots().size(), 1U);
}

TEST(FactoryRewrite, UnsafeFormsAreLeftAlone) {
  Library lib{};
  // c < 0, non-power-of-two c under the default pow2 rail, and ops that are
  // only value-level (zscore) or not idempotent (ts_mean) must NOT rewrite.
  for (const char *src : {"rank(-2 * close)", "rank(1.0001 * close)", "zscore(zscore(close))",
                          "ts_mean(ts_mean(close, 3), 3)", "rank(0 * close)", "rank(close + 2)",
                          "-rank(close)"}) {
    const Genome g = genome_of(src, lib);
    usize fired = 99;
    static_cast<void>(rewrite_ast(g.ast, g.ast.roots().front().root, RewriteCfg{}, &fired));
    EXPECT_EQ(fired, 0U) << src;
    EXPECT_EQ(canonical_hash(canonical_rewrite(g)), canonical_hash(g)) << src;
  }
}

TEST(FactoryRewrite, AnyPositiveScaleWhenPow2RailOff) {
  Library lib{};
  RewriteCfg cfg{};
  cfg.scale_pow2_only = false;
  const Genome g = genome_of("rank(1.0001 * close)", lib);
  const Genome rw = canonical_rewrite(g, cfg);
  EXPECT_EQ(canonical_hash(rw), canonical_hash(genome_of("rank(close)", lib)));
  // The fixture's close grid is coarse (integers), so no two cells merge.
  const Panel panel = rewrite_panel();
  EXPECT_EQ(eval_bits(g, panel), eval_bits(rw, panel));
}

TEST(FactoryRewrite, RulesCanBeDisabled) {
  Library lib{};
  RewriteCfg off{};
  off.exact_rules = false;
  off.pos_scale = false;
  const Genome g = genome_of("rank(rank(2 * close))", lib);
  usize fired = 99;
  static_cast<void>(rewrite_ast(g.ast, g.ast.roots().front().root, off, &fired));
  EXPECT_EQ(fired, 0U);
}

TEST(FactoryRewrite, InvarianceFlagsMatchTheRuleTable) {
  EXPECT_TRUE(op_invariance(OpCode::Neg).involution);
  EXPECT_TRUE(op_invariance(OpCode::Abs).idempotent);
  EXPECT_TRUE(op_invariance(OpCode::Abs).even);
  EXPECT_TRUE(op_invariance(OpCode::Sign).idempotent);
  EXPECT_TRUE(op_invariance(OpCode::CsRank).idempotent);
  EXPECT_TRUE(op_invariance(OpCode::CsRank).pos_scale);
  EXPECT_TRUE(op_invariance(OpCode::CsZscore).pos_scale);
  EXPECT_FALSE(op_invariance(OpCode::CsZscore).bit_exact);
  EXPECT_FALSE(op_invariance(OpCode::Add).bit_exact);
}

TEST(FactoryRewrite, SemanticCanonHashMergesEquivalentGenomes) {
  Library lib{};
  const Genome a = genome_of("rank(rank(close))", lib);
  const Genome b = genome_of("rank(close)", lib);
  CanonCfg sem{};
  sem.semantic = true;
  EXPECT_NE(canonical_hash(a), canonical_hash(b)); // structural hash keeps them apart
  EXPECT_EQ(canonical_hash(a, sem), canonical_hash(b, sem));
  // semantic=false is exactly the structural hash (byte-identical off path).
  EXPECT_EQ(canonical_hash(a, CanonCfg{}), canonical_hash(a));
}

} // namespace atx_test_l3_search_rewrite

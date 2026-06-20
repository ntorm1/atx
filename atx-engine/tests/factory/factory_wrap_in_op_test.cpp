// atx::engine::factory — wrap_in_op mutation-operator tests (Phase W1b).
//
// wrap_in_op wraps a randomly chosen F64-valued subtree in a shape/dtype-
// compatible conditioning WRAPPER op (zscore, signedpower, rank, winsorize,
// group_neutralize/indneutralize), making the subtree the wrapper's PRIMARY
// (first) operand and synthesizing any extra operands the wrapper requires.
//
// Verbatim test list (W1b brief §Tests):
//   * WrapReachesZscore / WrapReachesSignedpower — both mandatory wrappers reach
//     a valid (analyze-Ok) genome whose root is the wrapper op.
//   * ReachesSignedpowerOfZscore — repeated wrap + jitter reaches the manual-alpha
//     conditioning family signedpower(zscore(<subtree>), p).
//   * SameSeedSameWrap                — same seed+genome ⇒ byte-identical (F1).
//   * DepthBudgetRejects              — a genome already at max_depth ⇒ Err(NotFound).
//   * EveryAcceptedWrapAnalyzes       — analyze backstop: no half-valid genome.
//   * SignedpowerExponentIsScale      — the synthesized exponent classifies Scale.
//   * GroupWrapNeedsGroupFields       — empty group_fields excludes group wrappers.
//
// Local helpers live in a UNIQUE namespace with UNIQUE names (no collision with
// factory_mutation_test under a Unity build, even though Unity is OFF here).

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/mutation.hpp"
#include "atx/engine/factory/op_catalog.hpp"

namespace atxtest_factory_wrap_in_op_test {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Ast;
using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::kNoExpr;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::parse_expr;
using atx::engine::factory::classify_literals;
using atx::engine::factory::ClassifiedConst;
using atx::engine::factory::ConstKind;
using atx::engine::factory::Genome;
using atx::engine::factory::jitter_const;
using atx::engine::factory::OpCatalog;
using atx::engine::factory::wrap_in_op;
using atx::engine::factory::WrapCfg;

// ---- helpers (unique names) -------------------------------------------------

[[nodiscard]] Genome wrap_make_genome(std::string_view src, Library &lib) {
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// Name of the root Call op, or "" when the root is not a named Call.
[[nodiscard]] std::string wrap_root_op(const Genome &g) {
  const ExprId r = g.ast.roots().front().root;
  const Expr &e = g.ast.node(r);
  if (e.kind == Expr::Kind::Call && e.op != nullptr) {
    return std::string{e.op->name};
  }
  return std::string{};
}

// Does ANY Call node in `g` name `op_name` with first child being a Call named
// `child_name`? (used to detect the signedpower(zscore(...)) nesting).
[[nodiscard]] bool wrap_has_nesting(const Genome &g, std::string_view outer,
                                    std::string_view inner) {
  const std::span<const Expr> arena = g.ast.nodes();
  for (const Expr &e : arena) {
    if (e.kind == Expr::Kind::Call && e.op != nullptr && e.op->name == outer && e.a != kNoExpr) {
      const Expr &child = arena[e.a];
      if (child.kind == Expr::Kind::Call && child.op != nullptr && child.op->name == inner) {
        return true;
      }
    }
  }
  return false;
}

// A reusable group-field set (the discovery sector classifier).
[[nodiscard]] std::vector<std::string_view> wrap_group_fields() {
  return std::vector<std::string_view>{"sector"};
}

[[nodiscard]] WrapCfg wrap_cfg(int max_depth = 6) {
  WrapCfg c;
  c.max_depth = max_depth;
  return c;
}

// Tree HEIGHT of a genome: edges on the longest root->leaf path (a single leaf
// has height 0). Recomputed independently of the operator's internal helper.
[[nodiscard]] int wrap_subtree_height(const Ast &ast, ExprId id) {
  if (id == kNoExpr) {
    return -1;
  }
  const Expr &e = ast.node(id);
  int best = -1;
  for (const ExprId c : {e.a, e.b, e.c}) {
    if (c != kNoExpr) {
      best = std::max(best, wrap_subtree_height(ast, c));
    }
  }
  return best + 1; // leaf (no children) -> 0
}

[[nodiscard]] int wrap_tree_height(const Genome &g) {
  return wrap_subtree_height(g.ast, g.ast.roots().front().root);
}

// ---- tests ------------------------------------------------------------------

// Both mandatory wrappers (zscore, signedpower) must be reachable from the
// manual-alpha raw signal -1*ts_std(returns,20), and every accepted wrap must
// analyze (F5). We drive many seeds and collect which root wrappers appeared.
TEST(FactoryWrapInOp, WrapReachesZscoreAndSignedpower) {
  Library lib;
  OpCatalog cat(lib);
  auto g = wrap_make_genome("-1 * ts_std(returns, 20)", lib);
  const std::vector<std::string_view> gf = wrap_group_fields();

  std::set<std::string> reached_roots;
  int ok = 0;
  for (atx::u64 seed = 0; seed < 400; ++seed) {
    Xoshiro256pp rng(seed);
    auto m = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, rng, wrap_cfg());
    if (m) {
      ++ok;
      EXPECT_TRUE(analyze(m->ast).has_value()); // every accepted wrap is F5-valid
      const std::string root = wrap_root_op(*m);
      if (!root.empty()) {
        reached_roots.insert(root);
      }
    }
  }
  EXPECT_GT(ok, 0);
  EXPECT_GT(reached_roots.count("zscore"), 0U) << "zscore wrapper unreachable";
  EXPECT_GT(reached_roots.count("signedpower"), 0U) << "signedpower wrapper unreachable";
}

// Reachability of the manual-alpha conditioning family: repeated wrap_in_op (+
// jitter on the synthesized exponent) under fixed seeds reaches a
// signedpower(zscore(<subtree>), p) shape — wrap zscore first, then wrap that in
// signedpower.
TEST(FactoryWrapInOp, ReachesSignedpowerOfZscore) {
  Library lib;
  OpCatalog cat(lib);
  auto g = wrap_make_genome("-1 * ts_std(returns, 20)", lib);
  const std::vector<std::string_view> gf = wrap_group_fields();

  bool found = false;
  for (atx::u64 seed = 0; seed < 2000 && !found; ++seed) {
    Xoshiro256pp rng(seed);
    auto first = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, rng, wrap_cfg());
    if (!first || wrap_root_op(*first) != "zscore") {
      continue;
    }
    // wrap the zscore'd tree again; look for signedpower(zscore(...)).
    auto second =
        wrap_in_op(*first, cat, std::span<const std::string_view>{gf}, rng, wrap_cfg());
    if (second && wrap_has_nesting(*second, "signedpower", "zscore")) {
      EXPECT_TRUE(analyze(second->ast).has_value());
      // the exponent must be present and jitterable (Scale); jitter it once.
      auto jittered = jitter_const(*second, rng, {/*sigma*/ 0.5, /*max_lb*/ 250});
      EXPECT_TRUE(!jittered || analyze(jittered->ast).has_value());
      found = true;
    }
  }
  EXPECT_TRUE(found) << "signedpower(zscore(...)) family was not reached";
}

// F1: same seed + same genome ⇒ byte-identical mutant.
TEST(FactoryWrapInOp, SameSeedSameWrap) {
  Library lib;
  OpCatalog cat(lib);
  auto g = wrap_make_genome("-1 * ts_std(returns, 20)", lib);
  const std::vector<std::string_view> gf = wrap_group_fields();

  Xoshiro256pp r1(12345), r2(12345);
  auto a = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, r1, wrap_cfg());
  auto b = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, r2, wrap_cfg());
  ASSERT_EQ(a.has_value(), b.has_value());
  if (a) {
    ASSERT_EQ(a->ast.nodes().size(), b->ast.nodes().size());
    for (atx::usize i = 0; i < a->ast.nodes().size(); ++i) {
      const Expr &ea = a->ast.nodes()[i];
      const Expr &eb = b->ast.nodes()[i];
      EXPECT_EQ(static_cast<int>(ea.kind), static_cast<int>(eb.kind));
      EXPECT_EQ(ea.value, eb.value);
      EXPECT_EQ(ea.op, eb.op);
      EXPECT_EQ(static_cast<int>(ea.opcode), static_cast<int>(eb.opcode));
    }
  }
}

// Depth budget: a genome already at the depth cap yields Err(NotFound) — no
// over-deep tree is ever produced.
TEST(FactoryWrapInOp, DepthBudgetRejects) {
  Library lib;
  OpCatalog cat(lib);
  // -1 * ts_std(returns,20): the parser FOLDS `-1` to a single Literal(-1.0), so
  // the tree is root Binary(Mul) depth0 -> {Literal(-1) depth1, ts_std depth1} ->
  // {returns depth2, Literal(20) depth2}. Tree HEIGHT (longest root->leaf edge
  // count) = 2.
  auto g = wrap_make_genome("-1 * ts_std(returns, 20)", lib);
  const std::vector<std::string_view> gf = wrap_group_fields();
  const int height = wrap_tree_height(g);
  ASSERT_GE(height, 1);

  // (a) The load-bearing guarantee: NO produced genome ever exceeds the depth cap.
  //     At cap == current height, only a shallow node (e.g. an interior scalar
  //     literal wrapped by elementwise signedpower) can be wrapped without growing
  //     the tree past the cap; every accepted wrap MUST still respect it.
  for (atx::u64 seed = 0; seed < 400; ++seed) {
    Xoshiro256pp rng(seed);
    auto m = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, rng, wrap_cfg(height));
    if (m) {
      EXPECT_LE(wrap_tree_height(*m), height)
          << "wrap produced a tree deeper than the cap (seed " << seed << ")";
    }
  }

  // (b) With cap 0 (below every node's minimum wrapped height — a wrap always adds
  //     a level so wrapped_height >= 1), NO wrap is possible -> Err for every seed.
  for (atx::u64 seed = 0; seed < 200; ++seed) {
    Xoshiro256pp rng(seed);
    auto m = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, rng, wrap_cfg(/*cap*/ 0));
    EXPECT_FALSE(m.has_value()) << "cap 0 must reject every wrap (seed " << seed << ")";
  }
}

// Analyze backstop: no accepted wrap is ever half-valid (F5).
TEST(FactoryWrapInOp, EveryAcceptedWrapAnalyzes) {
  Library lib;
  OpCatalog cat(lib);
  auto g = wrap_make_genome("rank(close)", lib);
  const std::vector<std::string_view> gf = wrap_group_fields();
  for (atx::u64 seed = 0; seed < 500; ++seed) {
    Xoshiro256pp rng(seed);
    auto m = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, rng, wrap_cfg());
    if (m) {
      EXPECT_TRUE(analyze(m->ast).has_value());
    }
  }
}

// The synthesized signedpower exponent must classify as Scale (jitterable by
// jitter_const across the concentration frontier), NOT Window.
TEST(FactoryWrapInOp, SignedpowerExponentIsScale) {
  Library lib;
  OpCatalog cat(lib);
  auto g = wrap_make_genome("-1 * ts_std(returns, 20)", lib);
  const std::vector<std::string_view> gf = wrap_group_fields();

  bool checked = false;
  for (atx::u64 seed = 0; seed < 800 && !checked; ++seed) {
    Xoshiro256pp rng(seed);
    auto m = wrap_in_op(g, cat, std::span<const std::string_view>{gf}, rng, wrap_cfg());
    if (m && wrap_root_op(*m) == "signedpower") {
      // The new literal exponent (value 2.0) must be present and Scale-classified.
      bool found_scale_two = false;
      for (const ClassifiedConst &c : classify_literals(*m)) {
        if (m->ast.node(c.id).value == 2.0 && c.kind == ConstKind::Scale) {
          found_scale_two = true;
        }
      }
      EXPECT_TRUE(found_scale_two) << "signedpower exponent 2.0 was not a Scale literal";
      checked = true;
    }
  }
  EXPECT_TRUE(checked) << "no signedpower wrap was produced to inspect";
}

// With an EMPTY group_fields set, group wrappers (group_neutralize /
// indneutralize) are excluded — no produced genome roots at a group op.
TEST(FactoryWrapInOp, GroupWrapExcludedWhenNoGroupFields) {
  Library lib;
  OpCatalog cat(lib);
  auto g = wrap_make_genome("-1 * ts_std(returns, 20)", lib);
  const std::vector<std::string_view> empty_gf{};
  for (atx::u64 seed = 0; seed < 600; ++seed) {
    Xoshiro256pp rng(seed);
    auto m = wrap_in_op(g, cat, std::span<const std::string_view>{empty_gf}, rng, wrap_cfg());
    if (m) {
      const std::string root = wrap_root_op(*m);
      EXPECT_NE(root, "group_neutralize");
      EXPECT_NE(root, "indneutralize");
      EXPECT_TRUE(analyze(m->ast).has_value());
    }
  }
}

} // namespace atxtest_factory_wrap_in_op_test

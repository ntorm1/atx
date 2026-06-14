// atx::engine::factory — mutation-operator tests (S3-1, suite FactoryMutation).
//
// Covers the plan's verbatim test list:
//   * OpSwapStaysInGrammar            — every accepted op-swap mutant analyzes (F5)
//   * JitterConstClassifiesWindowVsScale — classify_literals tags Window vs Scale
//   * JitterWindowStaysInBounds       — jittered windows stay in [1, max] & valid
//   * SameSeedSameMutation            — same seed ⇒ byte-identical mutant (F1)
//
// Local helpers (no engine `unparse`/`make_genome`/`window_of`/`has_kind`):
//   * make_genome / unparse — shared shape with factory_genome_test (the test-
//     only `add/sub/greater` aliases let the plan's call syntax parse, §0.4).
//   * has_kind(consts, kind) — does any classified literal carry `kind`?
//   * window_of(g) — the value of the genome's first Window-classified literal.

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

namespace atxtest_factory_mutation_test {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Ast;
using atx::engine::alpha::DType;
using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::kNoExpr;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::OpSig;
using atx::engine::alpha::parse_expr;
using atx::engine::factory::classify_literals;
using atx::engine::factory::ClassifiedConst;
using atx::engine::factory::ConstKind;
using atx::engine::factory::Genome;
using atx::engine::factory::jitter_const;
using atx::engine::factory::op_swap;
using atx::engine::factory::OpCatalog;

// ---- shared helpers (mirror factory_genome_test) ----------------------------

inline void register_aliases(Library &lib) {
  using atx::engine::alpha::shape_elementwise;
  const OpSig add{"add", 2, 2, OpCode::Add, DType::F64, true, {}, &shape_elementwise};
  const OpSig sub{"sub", 2, 2, OpCode::Sub, DType::F64, true, {}, &shape_elementwise};
  const OpSig greater{"greater", 2, 2, OpCode::CmpGt, DType::Mask, true, {}, &shape_elementwise};
  static_cast<void>(lib.register_op(add));
  static_cast<void>(lib.register_op(sub));
  static_cast<void>(lib.register_op(greater));
}

[[nodiscard]] Genome make_genome(std::string_view src, Library &lib) {
  register_aliases(lib);
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

void unparse_into(const Ast &ast, ExprId id, std::string &out) {
  if (id == kNoExpr) {
    return;
  }
  const Expr &e = ast.node(id);
  switch (e.kind) {
  case Expr::Kind::Literal:
    out += std::to_string(e.value);
    return;
  case Expr::Kind::Field:
    if (e.dollar) {
      out += '$';
    }
    out += ast.field_name(e.name_id);
    return;
  case Expr::Kind::Unary:
    out += "(u";
    out += std::to_string(static_cast<int>(e.opcode));
    out += ' ';
    unparse_into(ast, e.a, out);
    out += ')';
    return;
  case Expr::Kind::Binary:
    out += "(b";
    out += std::to_string(static_cast<int>(e.opcode));
    out += ' ';
    unparse_into(ast, e.a, out);
    out += ' ';
    unparse_into(ast, e.b, out);
    out += ')';
    return;
  case Expr::Kind::Call:
    out += '(';
    out += e.op->name;
    for (const ExprId ch : {e.a, e.b, e.c}) {
      if (ch != kNoExpr) {
        out += ' ';
        unparse_into(ast, ch, out);
      }
    }
    out += ')';
    return;
  case Expr::Kind::Select:
    out += "(? ";
    unparse_into(ast, e.a, out);
    out += ' ';
    unparse_into(ast, e.b, out);
    out += ' ';
    unparse_into(ast, e.c, out);
    out += ')';
    return;
  case Expr::Kind::Member:
    out += "(. ";
    unparse_into(ast, e.a, out);
    out += ' ';
    out += ast.field_name(e.name_id);
    out += ')';
    return;
  }
}

[[nodiscard]] std::string unparse(const Ast &ast) {
  std::string out;
  if (!ast.roots().empty()) {
    unparse_into(ast, ast.roots().front().root, out);
  }
  return out;
}

[[nodiscard]] bool has_kind(const std::vector<ClassifiedConst> &consts, ConstKind kind) {
  for (const ClassifiedConst &c : consts) {
    if (c.kind == kind) {
      return true;
    }
  }
  return false;
}

// The value of the first Window-classified literal in `g` (the Ts* window).
[[nodiscard]] atx::f64 window_of(const Genome &g) {
  for (const ClassifiedConst &c : classify_literals(g)) {
    if (c.kind == ConstKind::Window) {
      return g.ast.node(c.id).value;
    }
  }
  return -1.0; // no window literal found (a test failure)
}

// ---- tests ------------------------------------------------------------------

TEST(FactoryMutation, OpSwapStaysInGrammar) {
  Library lib;
  OpCatalog cat(lib);
  Xoshiro256pp rng(1234);
  auto g = make_genome("ts_mean(close, 10)", lib);
  for (int i = 0; i < 200; ++i) {
    auto m = op_swap(g, cat, rng);
    if (m) {
      EXPECT_TRUE(analyze(m->ast).has_value()); // every accepted mutant is valid (F5)
    }
  }
}

TEST(FactoryMutation, JitterConstClassifiesWindowVsScale) {
  Library lib;
  Xoshiro256pp rng(7);
  auto g = make_genome("scale(ts_mean(close, 10), 2.0)", lib);
  auto consts = classify_literals(g);
  EXPECT_TRUE(has_kind(consts, ConstKind::Window)); // the 10
  EXPECT_TRUE(has_kind(consts, ConstKind::Scale));  // the 2.0
  static_cast<void>(rng);
}

TEST(FactoryMutation, JitterWindowStaysInBounds) {
  Library lib;
  Xoshiro256pp rng(9);
  auto g = make_genome("ts_mean(close, 10)", lib);
  for (int i = 0; i < 500; ++i) {
    auto m = jitter_const(g, rng, {/*sigma*/ 0.5, /*max_lb*/ 250});
    if (m) {
      auto w = window_of(*m);
      EXPECT_GE(w, 1);
      EXPECT_LE(w, 250);
      EXPECT_TRUE(analyze(m->ast).has_value());
    }
  }
}

TEST(FactoryMutation, SameSeedSameMutation) { // F1
  Library lib;
  OpCatalog cat(lib);
  auto g = make_genome("add(rank(close), ts_mean(volume,10))", lib);
  Xoshiro256pp r1(42), r2(42);
  auto a = op_swap(g, cat, r1);
  auto b = op_swap(g, cat, r2);
  ASSERT_EQ(a.has_value(), b.has_value());
  if (a) {
    EXPECT_EQ(unparse(a->ast), unparse(b->ast)); // byte-identical under same seed
  }
}

// op_swap must reach the bare-opcode infix surface: a Binary(Add) root can be
// swapped to another arithmetic opcode (Sub/Mul/Div/Pow). Proves the OpCode half
// of the catalog is wired (§0.4 / §4.2) AND is non-vacuous (Ok-rate > 0, and the
// swap genuinely produces >= 2 distinct arithmetic opcodes over the loop).
TEST(FactoryMutation, OpSwapReachesInfixArithmetic) {
  Library lib;
  OpCatalog cat(lib);
  Xoshiro256pp rng(2024);
  auto g = make_genome("close + open", lib); // a Binary(Add) root, no OpSig*
  ASSERT_EQ(g.ast.node(g.ast.roots().front().root).kind, Expr::Kind::Binary);

  std::set<OpCode> reached;
  int ok = 0;
  for (int i = 0; i < 300; ++i) {
    auto m = op_swap(g, cat, rng);
    if (m) {
      ++ok;
      EXPECT_TRUE(analyze(m->ast).has_value()); // every accepted mutant valid (F5)
      const auto &mroot = m->ast.node(m->ast.roots().front().root);
      reached.insert(mroot.opcode);
    }
  }
  EXPECT_GT(ok, 0);                  // non-vacuous: the guard body actually ran
  EXPECT_GE(reached.size(), 2U);     // genuinely swaps Add -> {Sub|Mul|Div|Pow}
  EXPECT_EQ(reached.count(OpCode::Add), 0U); // never a no-op swap back to itself
}


}  // namespace atxtest_factory_mutation_test

// atx::engine::factory — Genome substrate tests (S3-1, suite FactoryGenome).
//
// Covers the plan's verbatim test list:
//   * CloneRoundTripsAst        — clone() is a structurally identical, valid copy
//   * CloneSubtreeRemapsAndReinterns — clone_subtree offset-remaps + re-interns
//   * ValidateRejectsRecordRoot — a record-valued root fails analyze (§0.2)
//
// Local helpers (the engine exposes no `unparse`/`make_genome`/`find_call`):
//   * make_genome(src, lib) — register the test-only `add/sub/greater` aliases
//     into the (mutable) Library, parse the bare expression, analyze it, and
//     package a Genome. The aliases let the plan's `add(...)`/`sub(...)` call
//     syntax resolve (the real grammar spells these `+`/`-`/`>` infix, which are
//     not Library rows — §0.4).
//   * unparse(ast) — a canonical S-expression rendering used for structural
//     equality (deterministic, child-order-sensitive).
//
// Naming: Subject_Condition_ExpectedResult (suite name only per the plan).

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/genome.hpp"

namespace {

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
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Shape;
using atx::engine::factory::clone_subtree;
using atx::engine::factory::Genome;

// ---- test-only op aliases (so the plan's call syntax resolves) --------------

// The plan writes `add(a,b)` / `sub(a,b)` / `greater(a,b)`; the real grammar
// spells these `+` / `-` / `>` (infix, not Library rows). Register named aliases
// so the verbatim tests parse. Idempotent: a duplicate registration is ignored.
inline void register_aliases(Library &lib) {
  using atx::engine::alpha::shape_elementwise;
  const OpSig add{"add", 2, 2, OpCode::Add, DType::F64, true, {}, &shape_elementwise};
  const OpSig sub{"sub", 2, 2, OpCode::Sub, DType::F64, true, {}, &shape_elementwise};
  const OpSig greater{"greater", 2, 2, OpCode::CmpGt, DType::Mask, true, {}, &shape_elementwise};
  static_cast<void>(lib.register_op(add));
  static_cast<void>(lib.register_op(sub));
  static_cast<void>(lib.register_op(greater));
}

// Parse + analyze a bare expression into a Genome. ASSERT-free (uses EXPECT so a
// failure surfaces the message) — returns an empty Genome on failure.
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

// Canonical S-expression rendering of the subtree at `id` (child-order
// sensitive — used for structural equality, NOT for canonical dedup).
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

// The ExprId of the first Call node whose op spells `name` (for clone_subtree).
[[nodiscard]] ExprId find_call(const Genome &g, std::string_view name) {
  const std::span<const Expr> arena = g.ast.nodes();
  for (atx::usize i = 0; i < arena.size(); ++i) {
    if (arena[i].kind == Expr::Kind::Call && arena[i].op != nullptr && arena[i].op->name == name) {
      return static_cast<ExprId>(i);
    }
  }
  return kNoExpr;
}

// Every Field-leaf name in `ast`, in ascending-ExprId order (re-interned names).
[[nodiscard]] std::vector<std::string> field_names_of(const Ast &ast) {
  std::vector<std::string> names;
  for (const Expr &e : ast.nodes()) {
    if (e.kind == Expr::Kind::Field) {
      names.emplace_back(ast.field_name(e.name_id));
    }
  }
  return names;
}

// ---- tests ------------------------------------------------------------------

TEST(FactoryGenome, CloneRoundTripsAst) {
  Library lib;
  auto g = make_genome("ts_mean(close, 5)", lib);
  Genome c = g.clone();
  EXPECT_EQ(unparse(c.ast), unparse(g.ast)); // structurally identical
  EXPECT_TRUE(analyze(c.ast).has_value());   // still a valid causal program
}

TEST(FactoryGenome, CloneSubtreeRemapsAndReinterns) {
  Library lib;
  auto src = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  Ast dst;
  ExprId r = clone_subtree(src.ast, /*the ts_mean subtree*/ find_call(src, "ts_mean"), dst);
  dst.add_root("a", r);
  EXPECT_TRUE(analyze(dst).has_value());
  EXPECT_EQ(field_names_of(dst), std::vector<std::string>{"volume"}); // re-interned, no stale ids
}

TEST(FactoryGenome, ValidateRejectsRecordRoot) {
  Library lib;
  auto g = parse_program("a = kalman(ret, hedge, 1e-4, 1e-3)\n", lib); // record root
  ASSERT_TRUE(g.has_value()) << (g ? "" : g.error().message());
  EXPECT_FALSE(analyze(g.value()).has_value()); // §0.2: record root -> InvalidArgument
}

} // namespace

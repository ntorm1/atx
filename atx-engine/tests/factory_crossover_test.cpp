// atx::engine::factory — subtree-crossover tests (S3-2, suite FactoryCrossover).
//
// Covers the plan's verbatim test list (§4.3):
//   * ProducesValidCausalProgram — every accepted child analyzes (F5)
//   * TypeIncompatibleCutRejected— a Mask donor never splices into an F64 slot
//                                  (Err, or analyze() rejects — never silently
//                                  accepted invalid)
//   * SameSeedSameChild          — same seed => byte-identical child (F1)
//
// Local helpers mirror factory_genome_test (the engine exposes no make_genome /
// unparse): the test-only add/sub/greater/mul aliases let the plan's call syntax
// parse (§0.4), and unparse gives a child-order-sensitive structural string.

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/crossover.hpp"
#include "atx/engine/factory/genome.hpp"

namespace {

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
using atx::engine::alpha::shape_elementwise;
using atx::engine::factory::Genome;
using atx::engine::factory::subtree_crossover;

// ---- shared helpers (mirror factory_genome_test) ----------------------------

inline void register_aliases(Library &lib) {
  const OpSig add{"add", 2, 2, OpCode::Add, DType::F64, true, {}, &shape_elementwise};
  const OpSig sub{"sub", 2, 2, OpCode::Sub, DType::F64, true, {}, &shape_elementwise};
  const OpSig mul{"mul", 2, 2, OpCode::Mul, DType::F64, true, {}, &shape_elementwise};
  const OpSig greater{"greater", 2, 2, OpCode::CmpGt, DType::Mask, true, {}, &shape_elementwise};
  static_cast<void>(lib.register_op(add));
  static_cast<void>(lib.register_op(sub));
  static_cast<void>(lib.register_op(mul));
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

// ---- tests ------------------------------------------------------------------

TEST(FactoryCrossover, ProducesValidCausalProgram) {
  Library lib;
  Xoshiro256pp rng(3);
  auto a = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  auto b = make_genome("sub(ts_std(close, 20), rank(high))", lib);
  for (int i = 0; i < 200; ++i) {
    auto c = subtree_crossover(a, b, rng);
    if (c) {
      EXPECT_TRUE(analyze(c->ast).has_value()); // F5
    }
  }
}

TEST(FactoryCrossover, TypeIncompatibleCutRejected) {
  // a Mask-typed donor cannot splice into an F64 slot -> Err or analyze() rejects
  Library lib;
  Xoshiro256pp rng(5);
  auto a = make_genome("ts_mean(close, 5)", lib);
  auto b = make_genome("greater(close, open)", lib); // Mask result
  bool any_invalid_accepted = false;
  for (int i = 0; i < 200; ++i) {
    auto c = subtree_crossover(a, b, rng);
    if (c && !analyze(c->ast).has_value()) {
      any_invalid_accepted = true;
    }
  }
  EXPECT_FALSE(any_invalid_accepted);
}

TEST(FactoryCrossover, SameSeedSameChild) { // F1
  Library lib;
  auto a = make_genome("add(rank(close),ts_mean(volume,10))", lib);
  auto b = make_genome("sub(ts_std(close,20),rank(high))", lib);
  Xoshiro256pp r1(11), r2(11);
  auto c1 = subtree_crossover(a, b, r1);
  auto c2 = subtree_crossover(a, b, r2);
  ASSERT_EQ(c1.has_value(), c2.has_value());
  if (c1) {
    EXPECT_EQ(unparse(c1->ast), unparse(c2->ast));
  }
}

} // namespace

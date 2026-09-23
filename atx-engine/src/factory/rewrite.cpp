// atx::engine::factory — canonical_rewrite out-of-line definitions (L3).
#include "atx/engine/factory/rewrite.hpp"

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/factory/op_catalog.hpp"

namespace atx::engine::factory {

namespace {

using atx::engine::alpha::OpCode;

// A single-operand application of `opcode`: a Unary node, or a Call with exactly
// one materialized operand and no peeled hparams (abs/sign/rank/reverse). Both
// dispatch to the same VM kernel by opcode, so the rule table treats them alike.
[[nodiscard]] bool is_unary_app(const Expr &e) noexcept {
  if (e.kind == Expr::Kind::Unary) {
    return e.a != kNoExpr;
  }
  return e.kind == Expr::Kind::Call && e.n_hparams == 0 && alpha::call_arity(e) == 1;
}

// True iff `c` is a finite positive literal the pos_scale rule may strip.
[[nodiscard]] bool strippable_scale(atx::f64 c, bool pow2_only) noexcept {
  if (!std::isfinite(c) || !(c > 0.0)) {
    return false;
  }
  if (!pow2_only) {
    return true;
  }
  int exp = 0;
  const atx::f64 m = std::frexp(c, &exp); // c == m * 2^exp, m in [0.5, 1)
  return m == 0.5;
}

// If `e` (a dst node) is `c*x`, `x*c` or `x/c` with a strippable c, return x.
[[nodiscard]] ExprId scaled_operand(const Ast &dst, const Expr &e, bool pow2_only) noexcept {
  if (e.kind != Expr::Kind::Binary) {
    return kNoExpr;
  }
  const Expr &lhs = dst.node(e.a);
  const Expr &rhs = dst.node(e.b);
  const bool lhs_lit = lhs.kind == Expr::Kind::Literal;
  const bool rhs_lit = rhs.kind == Expr::Kind::Literal;
  if (e.opcode == OpCode::Mul) {
    if (lhs_lit && !rhs_lit && strippable_scale(lhs.value, pow2_only)) {
      return e.b;
    }
    if (rhs_lit && !lhs_lit && strippable_scale(rhs.value, pow2_only)) {
      return e.a;
    }
  } else if (e.opcode == OpCode::Div) {
    if (rhs_lit && !lhs_lit && strippable_scale(rhs.value, pow2_only)) {
      return e.a;
    }
  }
  return kNoExpr;
}

struct Rewriter {
  const Ast &src;
  Ast &dst;
  const RewriteCfg &cfg;
  std::vector<ExprId> memo;
  atx::usize fired{0};

  // Simplify `e` (children already normal, living in dst) to a fixpoint. Returns
  // either an existing dst id (a collapse) or the id of the appended node. Each
  // firing strictly shrinks the candidate subtree, so the loop is bounded by the
  // number of dst nodes.
  [[nodiscard]] ExprId simplify(Expr e) {
    const atx::usize bound = dst.nodes().size() + 1U;
    for (atx::usize it = 0; it < bound && is_unary_app(e); ++it) {
      const OpInvariance inv = op_invariance(e.opcode);
      if (!inv.bit_exact) {
        break;
      }
      const Expr &ch = dst.node(e.a);
      const bool same_op = is_unary_app(ch) && ch.opcode == e.opcode;
      if (cfg.exact_rules && inv.involution && same_op) {
        ++fired;
        return ch.a; // f(f(x)) -> x
      }
      if (cfg.exact_rules && inv.idempotent && same_op) {
        ++fired;
        return e.a; // f(f(x)) -> f(x): the inner node is already normal
      }
      if (cfg.exact_rules && inv.even && is_unary_app(ch) &&
          op_invariance(ch.opcode).involution) {
        ++fired;
        e.a = ch.a; // f(-x) -> f(x), then re-examine
        continue;
      }
      if (cfg.pos_scale && inv.pos_scale) {
        const ExprId x = scaled_operand(dst, ch, cfg.scale_pow2_only);
        if (x != kNoExpr) {
          ++fired;
          e.a = x; // f(c*x) -> f(x), then re-examine
          continue;
        }
      }
      break;
    }
    return dst.add(e);
  }

  // NOLINTNEXTLINE(misc-no-recursion): depth bounded by the AST depth.
  [[nodiscard]] ExprId visit(ExprId s) {
    if (memo[s] != kNoExpr) {
      return memo[s];
    }
    Expr e = src.node(s);
    if (e.a != kNoExpr) {
      e.a = visit(e.a);
    }
    if (e.b != kNoExpr) {
      e.b = visit(e.b);
    }
    if (e.c != kNoExpr) {
      e.c = visit(e.c);
    }
    if (e.kind == Expr::Kind::Field || e.kind == Expr::Kind::Member) {
      e.name_id = dst.intern(src.field_name(e.name_id));
    }
    const ExprId d = simplify(e);
    memo[s] = d;
    return d;
  }
};

} // namespace

[[nodiscard]] Ast rewrite_ast(const Ast &src, ExprId root, const RewriteCfg &cfg,
                              atx::usize *n_applied) {
  Ast scratch;
  scratch.reserve(src.nodes().size());
  Rewriter rw{src, scratch, cfg, std::vector<ExprId>(src.nodes().size(), kNoExpr)};
  const ExprId top = rw.visit(root);
  if (n_applied != nullptr) {
    *n_applied = rw.fired;
  }
  // Compact: a collapse leaves the bypassed nodes dead in `scratch`; a clone of
  // the reachable sub-DAG drops them so the arena holds only live nodes.
  Ast out;
  out.reserve(scratch.nodes().size());
  const ExprId new_root = clone_subtree(scratch, top, out);
  out.add_root(std::string{}, new_root);
  return out;
}

[[nodiscard]] Genome canonical_rewrite(const Genome &g, const RewriteCfg &cfg) {
  atx::usize fired = 0;
  Ast ast = rewrite_ast(g.ast, g.ast.roots().front().root, cfg, &fired);
  if (fired == 0) {
    return g.clone();
  }
  auto analyzed = analyze_into(std::move(ast));
  if (!analyzed.has_value()) {
    return g.clone(); // F5 backstop: never hand back an invalid genome
  }
  Genome out = std::move(*analyzed);
  out.canon_hash = g.canon_hash;
  out.from_seed = g.from_seed;
  return out;
}

} // namespace atx::engine::factory

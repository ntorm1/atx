#include "atx/engine/factory/genome.hpp"

#include <string>
#include <utility>
#include <vector>

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atx::engine::factory {

namespace detail {

[[nodiscard]] ExprId clone_visit(const Ast &src, ExprId s, Ast &dst,
                                 std::vector<ExprId> &memo) {
  if (memo[s] != kNoExpr) {
    return memo[s];
  }
  // A value copy of the node (Expr is trivially copyable POD). Children are
  // remapped below; Field/Member names are re-interned into dst's pool.
  Expr e = src.node(s);
  const ExprId ca = e.a, cb = e.b, cc = e.c;
  if (ca != kNoExpr) {
    e.a = clone_visit(src, ca, dst, memo);
  }
  if (cb != kNoExpr) {
    e.b = clone_visit(src, cb, dst, memo);
  }
  if (cc != kNoExpr) {
    e.c = clone_visit(src, cc, dst, memo);
  }
  if (e.kind == Expr::Kind::Field || e.kind == Expr::Kind::Member) {
    e.name_id = dst.intern(src.field_name(e.name_id));
  }
  const ExprId d = dst.add(e);
  memo[s] = d;
  return d;
}

} // namespace detail

// Deep-copy the subtree rooted at `root` from `src` into `dst`, returning the
// new root id in `dst`. Does NOT add a root binding (the caller decides whether
// the spliced subtree is a whole-program root or an interior child).
[[nodiscard]] ExprId clone_subtree(const Ast &src, ExprId root, Ast &dst) {
  std::vector<ExprId> memo(src.nodes().size(), kNoExpr);
  return detail::clone_visit(src, root, dst, memo);
}

// Run analyze on `ast`; on Ok, package it with `ast` into a value-owned Genome
// (canon_hash defaulted 0, set in S3-2). On Err, propagate the typecheck error.
// This is the single backstop every mutation/crossover funnels through: a
// returned Ok(Genome) is GUARANTEED to satisfy the F5 oracle.
[[nodiscard]] atx::core::Result<Genome> analyze_into(Ast ast) {
  ATX_TRY(Analysis info, analyze(ast));
  return atx::core::Ok(Genome{std::move(ast), std::move(info), 0});
}

// Re-analyze a genome's current ast (the §0.2 validity oracle). Ok iff the
// genome typechecks (shape/dtype/causality, non-record root).
[[nodiscard]] atx::core::Status validate(const Genome &g) {
  ATX_TRY_VOID(analyze(g.ast));
  return atx::core::Ok();
}

Genome Genome::clone() const {
  Ast dst;
  dst.reserve(ast.nodes().size());
  // SAFETY: clone_subtree carries each `Expr::op` verbatim — valid because the
  // clone shares the genome's single run-wide Library (op rows outlive both).
  const ExprId new_root = clone_subtree(ast, ast.roots().front().root, dst);
  dst.add_root(std::string{}, new_root);
  // Re-derive the analysis from the rebuilt arena (cheap, COLD path). The clone
  // is a structural copy of a valid genome, so analyze must succeed; if it
  // somehow does not, an empty Analysis is returned (the caller's validate()
  // backstop would catch a corrupted clone, which never happens by construction).
  auto info = analyze(dst);
  return Genome{std::move(dst), info ? std::move(*info) : Analysis{}, canon_hash};
}

} // namespace atx::engine::factory

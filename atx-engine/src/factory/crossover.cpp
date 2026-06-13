#include "atx/engine/factory/crossover.hpp"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/engine/alpha/parser.hpp"

namespace atx::engine::factory {

namespace detail {

// Post-order replay of A into `dst`, but when the cut is reached, splice the
// donor subtree (already deep-copied into `dst`) instead of A's own subtree. The
// memo dedups shared sub-DAG nodes exactly as clone_visit does.
ExprId splice_visit(const Ast &src, ExprId s, ExprId cut, ExprId spliced, Ast &dst,
                    std::unordered_map<ExprId, ExprId> &memo) {
  if (s == cut) {
    return spliced; // replace A's whole subtree at the cut with the donor copy
  }
  if (const auto it = memo.find(s); it != memo.end()) {
    return it->second;
  }
  Expr e = src.node(s);
  if (e.a != kNoExpr) {
    e.a = splice_visit(src, src.node(s).a, cut, spliced, dst, memo);
  }
  if (e.b != kNoExpr) {
    e.b = splice_visit(src, src.node(s).b, cut, spliced, dst, memo);
  }
  if (e.c != kNoExpr) {
    e.c = splice_visit(src, src.node(s).c, cut, spliced, dst, memo);
  }
  // Re-intern Field/Member names into dst's own pool (the pools differ — §0.1).
  if (e.kind == Expr::Kind::Field || e.kind == Expr::Kind::Member) {
    e.name_id = dst.intern(src.field_name(src.node(s).name_id));
  }
  const ExprId d = dst.add(e);
  memo.emplace(s, d);
  return d;
}

// Draw a uniform index in [0, n). Precondition: n > 0. One u64 of entropy.
// NOTE: named distinctly from mutation.hpp's identical helper so a TU that
// includes BOTH headers (e.g. the S3-5 search driver) does not hit an inline
// redefinition — the two live in the same factory::detail namespace.
[[nodiscard]] atx::usize uniform_cut_index(Xoshiro256pp &rng, atx::usize n) noexcept {
  return static_cast<atx::usize>(rng.next_u64() % n);
}

} // namespace detail

// =========================================================================
//  subtree_crossover — splice a B subtree into an A cut (§4.3).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome>
subtree_crossover(const Genome &a, const Genome &b, Xoshiro256pp &rng, CrossoverCfg cfg) {
  const ExprId a_root = a.ast.roots().front().root;

  // (1) Non-root cut points of A in ascending-ExprId order (fixed before any draw).
  std::vector<ExprId> cuts;
  const atx::usize a_n = a.ast.nodes().size();
  for (atx::usize i = 0; i < a_n; ++i) {
    if (static_cast<ExprId>(i) != a_root) {
      cuts.push_back(static_cast<ExprId>(i));
    }
  }
  if (cuts.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "subtree_crossover: A has no non-root cut");
  }

  // (2) Draw the cut; the slot type it must yield.
  const ExprId cut = cuts[detail::uniform_cut_index(rng, cuts.size())];
  const TypeInfo &want = a.analysis.info(cut);

  // (3) Donor candidates in B: type-compatible AND within the lookback cap, in
  //     ascending-ExprId order (fixed before the donor draw).
  std::vector<ExprId> donors;
  const atx::usize b_n = b.ast.nodes().size();
  for (atx::usize i = 0; i < b_n; ++i) {
    const TypeInfo &have = b.analysis.info(static_cast<ExprId>(i));
    if (compatible(have, want) && have.lookback <= cfg.max_lookback) {
      donors.push_back(static_cast<ExprId>(i));
    }
  }
  if (donors.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "subtree_crossover: no compatible donor in B");
  }

  // (4) Draw the donor.
  const ExprId donor = donors[detail::uniform_cut_index(rng, donors.size())];

  // (5) Rebuild A, splicing a deep copy of B's donor subtree at the cut.
  Ast dst;
  // SAFETY: clone_subtree carries each donor `Expr::op` verbatim — valid because
  // A and B share the one run-wide Library (op rows outlive both arenas — §0.1).
  const ExprId spliced = clone_subtree(b.ast, donor, dst);
  std::unordered_map<ExprId, ExprId> memo;
  const ExprId new_root = detail::splice_visit(a.ast, a_root, cut, spliced, dst, memo);
  dst.add_root(std::string{}, new_root);

  // (6) Validity oracle backstop (F5): typecheck/causality of the spliced program.
  return analyze_into(std::move(dst));
}

} // namespace atx::engine::factory

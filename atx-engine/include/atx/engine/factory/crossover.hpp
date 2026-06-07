#pragma once

// atx::engine::factory — subtree_crossover at TypeInfo-compatible cuts (S3-2, §4.3).
//
// Splice a donor subtree from genome B into a recipient cut in genome A, staying
// in-grammar by a TYPE pre-check and validated by `analyze` (the F5 backstop):
//
//   1. enumerate the non-root cut points of A in CANONICAL-ID (ascending ExprId)
//      order — the fixed order is fixed BEFORE any RNG draw (F1);
//   2. draw ONE cut uniformly; read `want = A.analysis.info(cut)` (the shape/dtype
//      the slot must yield);
//   3. enumerate the donor candidates in B — every node whose result type is
//      `compatible(have, want)` (shape broadcastable on the lattice + EXACT DType)
//      AND whose causal `lookback ≤ cfg.max_lookback` — in ascending-ExprId order;
//   4. draw ONE donor uniformly;
//   5. REBUILD A through the public builder, but when the post-order replay reaches
//      `cut`, splice `clone_subtree(B.ast, donor, dst)` in place of A's subtree
//      (the offset-remap deep copy re-interns fields and carries `Expr::op`);
//   6. funnel through `analyze_into` — a child that fails typecheck/causality is
//      returned as `Err`, never a half-valid genome.
//
// `Err(NotFound)` when A has no non-root cut or the chosen cut has no compatible
// donor in B. Determinism (F1): the two draws are the sole entropy; both candidate
// lists are in fixed ExprId order, so the same seeded `Xoshiro256pp` yields a
// byte-identical child.
//
// SAFETY: A and B MUST share the ONE run-wide Library — `clone_subtree` carries
// each donor `Expr::op` (a `const OpSig*`) verbatim, valid only because both
// arenas were built against the same Library (see genome.hpp). Header-only; COLD
// path (one rebuild per crossover), so std::vector / hash-map allocation is fine.

#include <unordered_map>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/genome.hpp"

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::Ast;
using atx::engine::alpha::DType;
using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::kNoExpr;
using atx::engine::alpha::Shape;
using atx::engine::alpha::TypeInfo;

// Tuning for subtree_crossover.
struct CrossoverCfg {
  atx::u16 max_lookback{250}; // a donor whose causal lookback exceeds this is excluded
};

// =========================================================================
//  compatible — the shape-lattice + exact-DType cut/donor test (§4.3).
// =========================================================================

// True iff a value of shape `have` can broadcast into a slot of shape `want`.
// The shape lattice `Scalar < CrossSection < Panel` (registry.hpp) is a TOTAL
// order and a binary op broadcasts to the WIDER operand, so the donor's narrower-
// or-wider shape always combines with the slot — every shape pair is broadcastable.
// The explicit `<=`-style predicate documents the lattice and would tighten
// automatically should a future non-total shape lattice be introduced.
[[nodiscard]] inline bool shape_broadcastable(Shape have, Shape want) noexcept {
  const auto h = static_cast<atx::u8>(have);
  const auto w = static_cast<atx::u8>(want);
  return h <= w || w <= h; // total order ⇒ always true; intent is explicit
}

// A donor of result type `have` may splice into a cut whose slot must yield
// `want` iff shapes broadcast (lattice) AND the DTypes are EXACTLY equal. The
// dtype gate is the discriminating one: a Mask-typed donor must never splice into
// an F64 slot (analyze would reject it, and we reject it here first so it is never
// even offered). `analyze` is the residual backstop for the per-arg constraints
// the type-checker hard-codes (e.g. a group/select-condition operand).
[[nodiscard]] inline bool compatible(const TypeInfo &have, const TypeInfo &want) noexcept {
  return shape_broadcastable(have.shape, want.shape) && have.dtype == want.dtype;
}

namespace detail {

// Post-order replay of A into `dst`, but when the cut is reached, splice the
// donor subtree (already deep-copied into `dst`) instead of A's own subtree. The
// memo dedups shared sub-DAG nodes exactly as clone_visit does.
inline ExprId splice_visit(const Ast &src, ExprId s, ExprId cut, ExprId spliced, Ast &dst,
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
[[nodiscard]] inline atx::usize uniform_index(Xoshiro256pp &rng, atx::usize n) noexcept {
  return static_cast<atx::usize>(rng.next_u64() % n);
}

} // namespace detail

// =========================================================================
//  subtree_crossover — splice a B subtree into an A cut (§4.3).
// =========================================================================

[[nodiscard]] inline atx::core::Result<Genome>
subtree_crossover(const Genome &a, const Genome &b, Xoshiro256pp &rng, CrossoverCfg cfg = {}) {
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
  const ExprId cut = cuts[detail::uniform_index(rng, cuts.size())];
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
  const ExprId donor = donors[detail::uniform_index(rng, donors.size())];

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

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

// =========================================================================
//  subtree_crossover — splice a B subtree into an A cut (§4.3).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome>
subtree_crossover(const Genome &a, const Genome &b, Xoshiro256pp &rng, CrossoverCfg cfg = {});

} // namespace atx::engine::factory

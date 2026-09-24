#pragma once

// atx::engine::factory — canonical_rewrite: semantic normalization before hashing
// (L3 search quality).
//
// The structural canonical hash (canonical.hpp) only reorders commutative
// operands, so `rank(rank(close))` and `rank(close)` — the SAME signal, bit for
// bit — occupy two dedup slots, two evaluations and two trials. This pass
// rewrites a genome's AST into a normal form under a small rule table of
// identities that the VM honours BIT-FOR-BIT (op_catalog.hpp `op_invariance`):
//
//   involution  neg(neg(x))            -> x
//   idempotent  abs(abs(x))            -> abs(x)
//               sign(sign(x))          -> sign(x)
//               rank(rank(x))          -> rank(x)
//   even        abs(neg(x))            -> abs(x)
//   pos_scale   rank(c*x) | rank(x*c)  -> rank(x)      c > 0, finite
//               rank(x/c)              -> rank(x)      (also sign)
//
// `pos_scale` is exact whenever c*x neither overflows nor underflows and does
// not round two distinct cells together. For c an exact power of two, rounding
// never merges cells, so the default (`scale_pow2_only`) keeps the rule exact
// except at the representable-range edge (|x| near DBL_MAX or subnormal) —
// never reached by price/volume data. `scale_pow2_only=false` widens it to any
// c > 0 (value-level invariance; the fingerprint layer covers that case anyway).
//
// Properties (FactoryRewrite_*): every applied rule is VM bit-identical on a
// fixture panel, and the rewrite is IDEMPOTENT — each node is simplified to a
// fixpoint over already-normal children, so a second pass finds no redex.
//
// COLD path (once per candidate, off the VM hot loop): allocation is fine.

#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"

#include "atx/engine/factory/genome.hpp"

namespace atx::engine::factory {

struct RewriteCfg {
  bool exact_rules{true};     // involution / idempotent / even rules
  bool pos_scale{true};       // f(c*x) -> f(x) for pos_scale ops, c > 0
  bool scale_pow2_only{true}; // restrict pos_scale to power-of-two c (bit-exact)
};

// Rewrite the sub-DAG rooted at `root` of `src` into a fresh, compacted Ast
// (single anonymous root, no dead nodes). `n_applied` (optional) receives the
// number of rule firings. The result is NOT re-analyzed.
[[nodiscard]] Ast rewrite_ast(const Ast &src, ExprId root, const RewriteCfg &cfg,
                              atx::usize *n_applied = nullptr);

// Rewrite a genome into its normal form and re-analyze it. When no rule fires,
// or the rewritten AST fails analyze (the F5 backstop), the result is a clone of
// `g`. `canon_hash` / `from_seed` are carried from `g` verbatim.
[[nodiscard]] Genome canonical_rewrite(const Genome &g, const RewriteCfg &cfg = {});

} // namespace atx::engine::factory

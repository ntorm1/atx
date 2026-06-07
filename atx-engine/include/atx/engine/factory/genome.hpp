#pragma once

// atx::engine::factory — Genome: the rebuild substrate (S3-1, plan §4.1 / §0.1).
//
// The genome IS an `alpha::Ast` (a flat, index-addressed, relocatable arena)
// plus its cached `alpha::Analysis` (the validity-oracle result) and a canonical
// dedup hash (set in S3-2; defaulted 0 here). The `alpha::Library` is NOT part of
// the genome — the Factory owns ONE Library for the whole run and lends a
// `const Library&` to every operator; `Expr::op` is a non-owning `const OpSig*`
// borrowed from that single Library and is carried verbatim across genomes.
//
// As-built reconciliation (§0.1): the `Ast` has NO in-place mutator — its only
// node API is the append-only builder (`add`/`intern`/`add_root`). So a mutation
// or crossover is NOT an edit: it REBUILDS a fresh `Ast` by replaying nodes
// through the builder, remapping `ExprId` children and re-interning field names,
// applying the edit during the replay. Because children are indices (not
// pointers) and `Expr` is trivially-copyable POD, an offset-remap deep copy is a
// valid deep copy and the arena stays topologically ordered (children before
// parents — the invariant `analyze` relies on).
//
// INVARIANT: a `Genome` returned `Ok` has `analysis == analyze(ast)` and that
// `analyze` is `Ok` (the F5 validity oracle). `canon_hash` is left 0 this unit.
//
// Header-only; every free function is `inline`. The rebuild path is COLD (run
// once per candidate, never on the VM hot path), so std::vector / hash-map
// allocation is fine.

#include <functional>
#include <unordered_map>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atx::engine::factory {

using atx::engine::alpha::Analysis;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Ast;
using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::kNoExpr;

// =========================================================================
//  Genome — the value-owned evolutionary unit (plan §4.1).
// =========================================================================

struct Genome {
  Ast ast;               // the flat, relocatable arena (value-owned)
  Analysis analysis;     // cached analyze() result (shape/dtype/lookback per node)
  atx::u64 canon_hash{0}; // factory/canonical.hpp key (set in S3-2; 0 here)
  // INVARIANT: `analysis` is analyze(ast) and is Ok; `canon_hash` matches `ast`.

  // A structural deep copy: rebuild the arena and re-derive the analysis. The
  // copy shares the same Library (every `Expr::op` pointer is preserved), so it
  // remains valid for the lifetime of that one Library.
  // SAFETY: every `Expr::op` in `ast` borrows a `const OpSig*` from the single
  // run-wide Library; the clone carries those pointers verbatim, so the clone
  // is valid only while that Library outlives it (the documented run contract).
  [[nodiscard]] Genome clone() const;
};

// =========================================================================
//  clone_subtree — the offset-remap deep copy (the core rebuild primitive).
//
//  Post-order DFS so children are appended to `dst` before their parents (the
//  arena must stay topologically ordered for `analyze`). A `memo` maps each
//  visited src id to its dst id so a shared sub-DAG is copied exactly once.
//  Field/Member names are RE-INTERNED into `dst`'s own string pool (the pools
//  differ between src and dst — carrying the stale `name_id` would dangle).
//
//  SAFETY: `Expr::op` (a `const OpSig*`) is carried verbatim. It is valid in
//  `dst` iff `src` and `dst` were built against the SAME Library — the caller
//  MUST guarantee the one run-wide Library owns every op row in both arenas.
// =========================================================================

namespace detail {

[[nodiscard]] inline ExprId clone_visit(const Ast &src, ExprId s, Ast &dst,
                                        std::unordered_map<ExprId, ExprId> &memo) {
  if (const auto it = memo.find(s); it != memo.end()) {
    return it->second;
  }
  // A value copy of the node (Expr is trivially copyable POD). Children are
  // remapped below; Field/Member names are re-interned into dst's pool.
  Expr e = src.node(s);
  if (e.a != kNoExpr) {
    e.a = clone_visit(src, src.node(s).a, dst, memo);
  }
  if (e.b != kNoExpr) {
    e.b = clone_visit(src, src.node(s).b, dst, memo);
  }
  if (e.c != kNoExpr) {
    e.c = clone_visit(src, src.node(s).c, dst, memo);
  }
  if (e.kind == Expr::Kind::Field || e.kind == Expr::Kind::Member) {
    e.name_id = dst.intern(src.field_name(src.node(s).name_id));
  }
  const ExprId d = dst.add(e);
  memo.emplace(s, d);
  return d;
}

} // namespace detail

// Deep-copy the subtree rooted at `root` from `src` into `dst`, returning the
// new root id in `dst`. Does NOT add a root binding (the caller decides whether
// the spliced subtree is a whole-program root or an interior child).
[[nodiscard]] inline ExprId clone_subtree(const Ast &src, ExprId root, Ast &dst) {
  std::unordered_map<ExprId, ExprId> memo;
  return detail::clone_visit(src, root, dst, memo);
}

// =========================================================================
//  rebuild_with — clone the whole tree, but emit an EDITED Expr at `target`.
//
//  `edit(Expr&, Ast& dst)` receives the node about to be appended for the target
//  id (with its children ALREADY remapped into `dst`) plus the destination arena
//  and mutates the node in place — swap the opcode/op, the literal value, or the
//  field name (re-interning the new name into `dst` via `dst.intern(...)`). Every
//  other node is copied verbatim. The result is a fresh `Ast` carrying a single
//  root binding named "" (anonymous, matching parse_expr's convention).
// =========================================================================

namespace detail {

template <class EditFn>
[[nodiscard]] inline ExprId rebuild_visit(const Ast &src, ExprId s, ExprId target, Ast &dst,
                                          std::unordered_map<ExprId, ExprId> &memo, EditFn &edit) {
  if (const auto it = memo.find(s); it != memo.end()) {
    return it->second;
  }
  Expr e = src.node(s);
  if (e.a != kNoExpr) {
    e.a = rebuild_visit(src, src.node(s).a, target, dst, memo, edit);
  }
  if (e.b != kNoExpr) {
    e.b = rebuild_visit(src, src.node(s).b, target, dst, memo, edit);
  }
  if (e.c != kNoExpr) {
    e.c = rebuild_visit(src, src.node(s).c, target, dst, memo, edit);
  }
  if (e.kind == Expr::Kind::Field || e.kind == Expr::Kind::Member) {
    e.name_id = dst.intern(src.field_name(src.node(s).name_id));
  }
  if (s == target) {
    edit(e, dst); // apply the edit AFTER children/names are remapped into dst
  }
  const ExprId d = dst.add(e);
  memo.emplace(s, d);
  return d;
}

} // namespace detail

// Rebuild a fresh Ast from `g.ast`, applying `edit` to the node `target`. The
// produced Ast is NOT yet analyzed (use `analyze_into` for the validated form).
// Precondition: `target` is reachable from the genome's (single) root.
template <class EditFn>
[[nodiscard]] inline Ast rebuild_with(const Genome &g, ExprId target, EditFn edit) {
  Ast dst;
  std::unordered_map<ExprId, ExprId> memo;
  // S3-1 genomes carry a single root (built from parse_expr / a bare splice).
  const ExprId src_root = g.ast.roots().front().root;
  const ExprId new_root = detail::rebuild_visit(g.ast, src_root, target, dst, memo, edit);
  dst.add_root(std::string{}, new_root);
  return dst;
}

// =========================================================================
//  analyze_into / validate — the F5 validity-oracle wrappers.
// =========================================================================

// Run analyze on `ast`; on Ok, package it with `ast` into a value-owned Genome
// (canon_hash defaulted 0, set in S3-2). On Err, propagate the typecheck error.
// This is the single backstop every mutation/crossover funnels through: a
// returned Ok(Genome) is GUARANTEED to satisfy the F5 oracle.
[[nodiscard]] inline atx::core::Result<Genome> analyze_into(Ast ast) {
  ATX_TRY(Analysis info, analyze(ast));
  return atx::core::Ok(Genome{std::move(ast), std::move(info), 0});
}

// Re-analyze a genome's current ast (the §0.2 validity oracle). Ok iff the
// genome typechecks (shape/dtype/causality, non-record root).
[[nodiscard]] inline atx::core::Status validate(const Genome &g) {
  ATX_TRY_VOID(analyze(g.ast));
  return atx::core::Ok();
}

inline Genome Genome::clone() const {
  Ast dst;
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

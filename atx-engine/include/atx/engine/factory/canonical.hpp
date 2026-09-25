#pragma once

// atx::engine::factory — canonical_hash: the sound, stable dedup key (S3-2, §4.4 / §0.5).
//
// As-built reconciliation (§0.5): p0 has NO commutative-operand normalization —
// `dag.hpp` hashes child order sensitively, so `Add(a,b)` and `Add(b,a)` do NOT
// dedup — and its in-process `NodeKeyHash` is wyhash with compile-time seeds, so
// it is NOT stable across process restarts (unusable as a persisted key). This
// header builds the missing pass FROM SCRATCH:
//
//   canonical_hash(ast, root) — a recursive, MEMOIZED structural hash over the
//   sub-DAG with a FIXED-byte-layout FNV-1a fold (`mix`), STABLE across runs and
//   platforms (no wyhash, no compile-time seeds, no endian-dependent reads):
//     * Literal — bit pattern of the value (bit_cast<u64>).
//     * Field   — the $-sigil bit + the field NAME bytes (never the Ast-local
//                 `name_id`, which differs between two structurally-identical
//                 arenas — keying by name is what makes clone() hash-stable).
//     * Unary   — opcode byte + child sub-hash.
//     * Binary  — opcode byte + child sub-hashes; for a DECLARED-commutative
//                 opcode the two sub-hashes are SORTED before mixing (the missing
//                 commutative-ordering pass).
//     * Call    — op NAME bytes + opcode byte + hparam bits + child sub-hashes;
//                 for a commutative Call (min/max/…) the child sub-hashes SORT.
//     * Select / Member — structural mix in FIXED slot order (NOT commutative).
//
// F6 SOUNDNESS (the load-bearing invariant): hash-equal ⇒ the VM evaluates the
// two expressions BIT-IDENTICAL. The ONLY normalization applied is the
// commutative-operand reorder for `is_hash_commutative` + the parse-time folds,
// both value-preserving. The reorder is sound ONLY for an op whose VM kernel is
// bit-SYMMETRIC under operand swap across ALL of {distinct values, ties, NaN,
// signed-zero}. The hash-commutative set is therefore a STRICT SUBSET of
// op_catalog's eight declared-commutative opcodes — only these SIX qualify:
//     Add / Mul    — IEEE `a+b == b+a`, `a*b == b*a` bit-for-bit (incl. NaN
//                    payloads and ±0: -0.0 + +0.0 == +0.0 + -0.0 == +0.0);
//     And / Or     — finite-nonzero→1 / 0→0 / NaN→NaN, swap-symmetric;
//     CmpEq / CmpNe— `x==y`/`x!=y` symmetric (and ±0 compare equal both ways),
//                    NaN→0 mask symmetric.
// MinP / MaxP are EXCLUDED (the recon fix to cf30730): the VM kernels are
// `a<b?a:b` / `a>b?a:b`, and on an OPPOSITE-SIGNED-ZERO pair the strict compare
// is FALSE (`-0.0 < +0.0` is false — they compare equal), so the kernel returns
// the SECOND operand:
//     min(-0.0, +0.0) = +0.0   but   min(+0.0, -0.0) = -0.0
// a VERIFIED bit-difference (signbit 0 vs 1; driven through the real VM by
// FactoryCanonical.MinMaxSwapIsNotBitSafe). Hashing min/max commutative would
// collide that bit-different pair — an F6 violation — so we DROP them and
// under-canonicalize: `min(a,b)` and `min(b,a)` hash DIFFERENTLY and do not dedup.
// Soundness > dedup rate: never claim two non-bit-equal exprs equal.
//
// NOTE: op_catalog's `is_commutative` stays the FULL eight — op-swap BUCKETING
// only needs value-interchangeability, not bit-equality. Only the canonical HASH
// needs the stricter property, so `is_hash_commutative` below is its own set.
//
// Header-only; COLD path (run once per candidate, never on the VM hot path), so
// the recursion + memo map allocate freely.

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/rewrite.hpp"

namespace atx::engine::factory {

using atx::engine::alpha::Ast;
using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::kNoExpr;
using atx::engine::alpha::OpCode;

// =========================================================================
//  is_hash_commutative — the operand-reorder set used by the hash.
//
//  A STRICT SUBSET of op_catalog's eight declared-commutative opcodes:
//      { Add, Mul, And, Or, CmpEq, CmpNe }   (MinP / MaxP DROPPED)
//  Only an op whose VM kernel is bit-SYMMETRIC under operand swap across ALL of
//  {distinct values, ties, NaN, signed-zero} may be hashed commutative (else the
//  hash would claim two bit-DIFFERENT exprs equal — an F6 violation).
//
//  MinP / MaxP are EXCLUDED: the VM kernels are `a<b?a:b` / `a>b?a:b`, and on an
//  opposite-signed-zero pair the strict comparison is FALSE (-0.0 < +0.0 is
//  false: they compare equal), so the kernel returns the SECOND operand —
//      min(-0.0, +0.0) = +0.0   but   min(+0.0, -0.0) = -0.0
//  a verified bit-difference (signbit 0 vs 1). Hashing them commutative would
//  collide a bit-different pair, so we under-canonicalize: min(a,b) and min(b,a)
//  hash DIFFERENTLY and simply do not dedup. Soundness > dedup rate.
//
//  This is INTENTIONALLY decoupled from op_catalog's `is_commutative` (which stays
//  the full eight): op-swap BUCKETING only needs value-interchangeability, not
//  bit-equality — only the canonical HASH needs the stricter property (§0.4/§4.4).
// =========================================================================

[[nodiscard]] inline bool is_hash_commutative(OpCode op) noexcept {
  switch (op) {
  case OpCode::Add:
  case OpCode::Mul:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::CmpEq:
  case OpCode::CmpNe:
    return true;
  // MinP / MaxP deliberately omitted (signed-zero swap-asymmetry — see above).
  default:
    return false;
  }
}

// =========================================================================
//  canonical_hash — recursive, memoized, stable, sound structural hash.
// =========================================================================

// Stable, sound, discriminating canonical hash of the sub-DAG rooted at `root`.
// Recursive + memoized over the sub-DAG so a shared sub-expression is hashed once.
[[nodiscard]] atx::u64 canonical_hash(const Ast &ast, ExprId root) noexcept;

// Convenience: hash a genome's single (first) root. A genome carries one root
// (built from parse_expr / a bare splice), so this is the whole-program key.
[[nodiscard]] atx::u64 canonical_hash(const Genome &g) noexcept;

// =========================================================================
//  CanonCfg — opt-in SEMANTIC canonicalization (L3).
//
//  semantic=false (default): exactly the structural `canonical_hash(g)` above —
//  byte-identical off path. semantic=true: the genome is first normalized by
//  factory/rewrite.hpp (bit-exact identities: neg∘neg, rank∘rank, abs∘abs,
//  sign∘sign, abs∘neg, rank(c·x)), then hashed, so `rank(rank(close))` and
//  `rank(close)` share one dedup slot. F6 soundness is preserved because every
//  rule is VM bit-identical (FactoryRewrite_*).
// =========================================================================
struct CanonCfg {
  bool semantic{false};
  RewriteCfg rewrite{};
};

[[nodiscard]] atx::u64 canonical_hash(const Genome &g, const CanonCfg &cfg);

// =========================================================================
//  canonical_string (W0-A0 / A-18) — the EXACT canonical form behind the hash.
//
//  A deterministic, self-delimiting serialization of the same normalized
//  structure canonical_hash folds: literals by bit pattern, fields/pins/op names
//  length-prefixed, hparams by bit pattern, and the children of an
//  is_hash_commutative op SORTED (lexicographically by their own canonical
//  strings). Two sub-DAGs have equal strings iff they are structurally equal up
//  to that commutative reorder — so equal strings imply equal canonical hashes,
//  and a hash hit with a DIFFERENT string is a proven 64-bit collision.
//  COLD path: the string grows with the expanded tree (shared sub-DAGs are
//  re-spelled at each use), which is fine for genome-sized expressions.
// =========================================================================
[[nodiscard]] std::string canonical_string(const Ast &ast, ExprId root);
[[nodiscard]] std::string canonical_string(const Genome &g);
// The CanonCfg-aware form (semantic=true normalizes via rewrite first, exactly
// as canonical_hash(g, cfg) does).
[[nodiscard]] std::string canonical_string(const Genome &g, const CanonCfg &cfg);

// =========================================================================
//  CanonSet — the dedup set (the driver skips a candidate on a hit).
//
//  A-18: a bare u64 set silently treats a 64-bit hash collision as a duplicate
//  and reuses the other genome's score. The verified API stores each distinct
//  canonical string under its hash and COMPARES the string on a hash hit, so a
//  colliding-but-different structure is admitted as fresh (and counted in
//  collisions()). The hash-only contains(h)/insert(h) remain for callers that
//  have not been moved to the verified API yet (they keep the old,
//  collision-blind behaviour) and for resume snapshots, which persist hashes
//  only: a hash known WITHOUT a form cannot be disproven and counts as seen.
// =========================================================================
struct CanonSet {
  std::unordered_set<atx::u64> seen; // every distinct hash (persisted by resume snapshots)
  std::unordered_map<atx::u64, std::vector<std::string>> forms; // hash -> distinct forms
  atx::usize n_distinct{0};   // distinct structures admitted (both APIs)
  atx::usize n_collisions{0}; // hash hits whose canonical string differed

  // ---- hash-only (legacy, collision-blind) ----
  [[nodiscard]] bool contains(atx::u64 h) const noexcept { return seen.find(h) != seen.end(); }

  // Insert `h`; return true iff it was not already present (a fresh structure).
  bool insert(atx::u64 h) {
    const bool fresh = seen.insert(h).second;
    n_distinct += fresh ? 1U : 0U;
    return fresh;
  }

  // ---- verified (hash + canonical string) ----
  // True iff a structure with canonical string `form` was admitted under `h`
  // (or `h` is known only as a bare hash — see the struct comment).
  [[nodiscard]] bool contains(atx::u64 h, std::string_view form) const;

  // Admit (h, form); return true iff it is a NEW structure. A hash hit whose
  // stored forms all differ from `form` is a 64-bit collision: the structure is
  // admitted as fresh and n_collisions is incremented.
  bool insert(atx::u64 h, std::string form);

  [[nodiscard]] atx::usize size() const noexcept { return n_distinct; }
  [[nodiscard]] atx::usize collisions() const noexcept { return n_collisions; }
};

} // namespace atx::engine::factory

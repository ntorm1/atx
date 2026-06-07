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
// commutative-operand reorder for the declared set + the parse-time folds, both
// value-preserving. The commutative reorder is sound ONLY for ops whose VM kernel
// is bit-SYMMETRIC under operand swap — including the NaN policy. The as-built VM
// (`vm.hpp`) makes:
//     Add/Mul          — IEEE `a+b == b+a`, `a*b == b*a` bit-for-bit;
//     And/Or           — finite-nonzero→true / 0→false / NaN→NaN, symmetric;
//     MinP/MaxP        — "NaN if EITHER operand is NaN" then `a<b?a:b` — the NaN
//                        branch is symmetric and the tie/strict branch returns
//                        the same bit pattern under swap (verified by the
//                        directed FactoryCanonical.MinMaxCommuteIsBitIdentical);
//     CmpEq/CmpNe      — `x==y`/`x!=y` are symmetric, NaN→0 mask symmetric.
// So ALL EIGHT op_catalog declared-commutative opcodes are bit-symmetric and the
// hash-commutative set EQUALS op_catalog's `is_commutative` — no narrowing needed.
// (If a future VM change broke a kernel's swap-symmetry, the directed test would
// fail and that opcode would be removed from `is_hash_commutative` below —
// under-canonicalize rather than ever claim two non-bit-equal exprs equal.)
//
// Header-only; COLD path (run once per candidate, never on the VM hot path), so
// the recursion + memo map allocate freely.

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/op_catalog.hpp"

namespace atx::engine::factory {

using atx::engine::alpha::Ast;
using atx::engine::alpha::Expr;
using atx::engine::alpha::ExprId;
using atx::engine::alpha::kNoExpr;
using atx::engine::alpha::OpCode;

// =========================================================================
//  Stable FNV-1a primitives — fixed byte order, no seeds, no wyhash.
// =========================================================================

namespace detail {

inline constexpr atx::u64 kFnvOffset = 1469598103934665603ULL;
inline constexpr atx::u64 kFnvPrime = 1099511628211ULL;

// FNV-1a one byte. The fold is byte-explicit (no reinterpret of a wider word), so
// the result is identical on every platform and process — the stable-key property.
[[nodiscard]] inline atx::u64 fnv_byte(atx::u64 h, atx::u8 b) noexcept {
  h ^= static_cast<atx::u64>(b);
  h *= kFnvPrime;
  return h;
}

// Fold a u64 little-end-first, byte by byte (explicit order ⇒ endian-independent).
[[nodiscard]] inline atx::u64 fnv_u64(atx::u64 h, atx::u64 v) noexcept {
  for (int i = 0; i < 8; ++i) {
    h = fnv_byte(h, static_cast<atx::u8>(v & 0xFFULL));
    v >>= 8;
  }
  return h;
}

// Fold a string's raw bytes (length-prefixed so "ab"+"c" ≠ "a"+"bc").
[[nodiscard]] inline atx::u64 fnv_bytes(atx::u64 h, std::string_view s) noexcept {
  h = fnv_u64(h, static_cast<atx::u64>(s.size()));
  for (const char c : s) {
    h = fnv_byte(h, static_cast<atx::u8>(c));
  }
  return h;
}

// Per-kind tag bytes — disjoint so a Literal can never collide a Field etc.
enum class Tag : atx::u8 {
  Lit = 0x01,
  Fld = 0x02,
  Unary = 0x03,
  Binary = 0x04,
  Call = 0x05,
  Select = 0x06,
  Member = 0x07,
};

[[nodiscard]] inline atx::u64 tagged(Tag t) noexcept {
  return fnv_byte(kFnvOffset, static_cast<atx::u8>(t));
}

} // namespace detail

// =========================================================================
//  is_hash_commutative — the operand-reorder set used by the hash.
//
//  EQUALS op_catalog's declared-commutative set: every one of those eight VM
//  kernels is bit-symmetric under operand swap (see the soundness note above and
//  the directed MinMax test). Re-exposed under this name so a future narrowing
//  (if a kernel ever loses swap-symmetry) is a one-line change here, NOT a
//  re-declaration of op_catalog's set.
// =========================================================================

[[nodiscard]] inline bool is_hash_commutative(OpCode op) noexcept {
  return is_commutative(op); // op_catalog.hpp — single source of truth (§0.4)
}

// =========================================================================
//  canonical_hash — recursive, memoized, stable, sound structural hash.
// =========================================================================

namespace detail {

// Mix a list of (already-final) child sub-hashes into `h` in order.
[[nodiscard]] inline atx::u64 mix_children(atx::u64 h, std::span<const atx::u64> hs) noexcept {
  h = fnv_u64(h, static_cast<atx::u64>(hs.size())); // arity is structural
  for (const atx::u64 ch : hs) {
    h = fnv_u64(h, ch);
  }
  return h;
}

[[nodiscard]] inline atx::u64 canon_visit(const Ast &ast, ExprId id,
                                          std::unordered_map<ExprId, atx::u64> &memo) {
  if (const auto it = memo.find(id); it != memo.end()) {
    return it->second;
  }
  const Expr &e = ast.node(id);
  atx::u64 h = 0;
  switch (e.kind) {
  case Expr::Kind::Literal:
    h = fnv_u64(tagged(Tag::Lit), std::bit_cast<atx::u64>(e.value));
    break;
  case Expr::Kind::Field:
    h = tagged(Tag::Fld);
    h = fnv_byte(h, e.dollar ? atx::u8{1} : atx::u8{0});
    h = fnv_bytes(h, ast.field_name(e.name_id)); // by NAME, never name_id
    break;
  case Expr::Kind::Unary: {
    h = fnv_byte(tagged(Tag::Unary), static_cast<atx::u8>(e.opcode));
    const std::array<atx::u64, 1> ch{canon_visit(ast, e.a, memo)};
    h = mix_children(h, ch);
    break;
  }
  case Expr::Kind::Binary: {
    h = fnv_byte(tagged(Tag::Binary), static_cast<atx::u8>(e.opcode));
    std::array<atx::u64, 2> ch{canon_visit(ast, e.a, memo), canon_visit(ast, e.b, memo)};
    if (is_hash_commutative(e.opcode) && ch[1] < ch[0]) {
      std::swap(ch[0], ch[1]); // the missing commutative-ordering pass
    }
    h = mix_children(h, ch);
    break;
  }
  case Expr::Kind::Call: {
    h = tagged(Tag::Call);
    h = fnv_byte(h, static_cast<atx::u8>(e.opcode));
    // Op identity by NAME (stable, persists to S4) — `op` is non-null for a Call.
    h = fnv_bytes(h, (e.op != nullptr) ? e.op->name : std::string_view{});
    // Peeled compile-time hparams are part of the call's identity (§0.3).
    h = fnv_byte(h, e.n_hparams);
    for (atx::u8 k = 0; k < e.n_hparams; ++k) {
      h = fnv_u64(h, std::bit_cast<atx::u64>(e.hparams[k]));
    }
    // Materialized operand children (a/b/c), sorted only for a commutative call.
    std::vector<atx::u64> ch;
    for (const ExprId c : {e.a, e.b, e.c}) {
      if (c != kNoExpr) {
        ch.push_back(canon_visit(ast, c, memo));
      }
    }
    if (is_hash_commutative(e.opcode)) {
      std::sort(ch.begin(), ch.end());
    }
    h = mix_children(h, std::span<const atx::u64>{ch});
    break;
  }
  case Expr::Kind::Select: {
    h = fnv_byte(tagged(Tag::Select), static_cast<atx::u8>(e.opcode));
    const std::array<atx::u64, 3> ch{canon_visit(ast, e.a, memo), canon_visit(ast, e.b, memo),
                                     canon_visit(ast, e.c, memo)}; // FIXED slot order
    h = mix_children(h, ch);
    break;
  }
  case Expr::Kind::Member: {
    h = tagged(Tag::Member);
    const std::array<atx::u64, 1> ch{canon_visit(ast, e.a, memo)};
    h = mix_children(h, ch);
    h = fnv_bytes(h, ast.field_name(e.name_id)); // pin name (stable)
    break;
  }
  }
  memo.emplace(id, h);
  return h;
}

} // namespace detail

// Stable, sound, discriminating canonical hash of the sub-DAG rooted at `root`.
// Recursive + memoized over the sub-DAG so a shared sub-expression is hashed once.
[[nodiscard]] inline atx::u64 canonical_hash(const Ast &ast, ExprId root) noexcept {
  std::unordered_map<ExprId, atx::u64> memo;
  return detail::canon_visit(ast, root, memo);
}

// Convenience: hash a genome's single (first) root. A genome carries one root
// (built from parse_expr / a bare splice), so this is the whole-program key.
[[nodiscard]] inline atx::u64 canonical_hash(const Genome &g) noexcept {
  return canonical_hash(g.ast, g.ast.roots().front().root);
}

// =========================================================================
//  CanonSet — the u64 dedup set (the driver skips a candidate on a hit).
// =========================================================================

// A thin wrapper over a u64 hash set giving the dedup vocabulary the search
// driver uses: `contains(h)` to test, `insert(h)` returning true iff `h` was
// NEW (false ⇒ a structural duplicate, skip re-evaluation).
struct CanonSet {
  std::unordered_set<atx::u64> seen;

  [[nodiscard]] bool contains(atx::u64 h) const noexcept { return seen.find(h) != seen.end(); }

  // Insert `h`; return true iff it was not already present (a fresh structure).
  bool insert(atx::u64 h) { return seen.insert(h).second; }

  [[nodiscard]] atx::usize size() const noexcept { return seen.size(); }
};

} // namespace atx::engine::factory

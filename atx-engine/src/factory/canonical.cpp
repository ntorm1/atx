#include "atx/engine/factory/canonical.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace atx::engine::factory {

// =========================================================================
//  Stable FNV-1a primitives — fixed byte order, no seeds, no wyhash.
// =========================================================================

namespace detail {

inline constexpr atx::u64 kFnvOffset = 1469598103934665603ULL;
inline constexpr atx::u64 kFnvPrime = 1099511628211ULL;

// FNV-1a one byte. The fold is byte-explicit (no reinterpret of a wider word), so
// the result is identical on every platform and process — the stable-key property.
[[nodiscard]] atx::u64 fnv_byte(atx::u64 h, atx::u8 b) noexcept {
  h ^= static_cast<atx::u64>(b);
  h *= kFnvPrime;
  return h;
}

// Fold a u64 little-end-first, byte by byte (explicit order ⇒ endian-independent).
[[nodiscard]] atx::u64 fnv_u64(atx::u64 h, atx::u64 v) noexcept {
  for (int i = 0; i < 8; ++i) {
    h = fnv_byte(h, static_cast<atx::u8>(v & 0xFFULL));
    v >>= 8;
  }
  return h;
}

// Fold a string's raw bytes (length-prefixed so "ab"+"c" ≠ "a"+"bc").
[[nodiscard]] atx::u64 fnv_bytes(atx::u64 h, std::string_view s) noexcept {
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

[[nodiscard]] atx::u64 tagged(Tag t) noexcept {
  return fnv_byte(kFnvOffset, static_cast<atx::u8>(t));
}

// Mix a list of (already-final) child sub-hashes into `h` in order.
[[nodiscard]] atx::u64 mix_children(atx::u64 h, std::span<const atx::u64> hs) noexcept {
  h = fnv_u64(h, static_cast<atx::u64>(hs.size())); // arity is structural
  for (const atx::u64 ch : hs) {
    h = fnv_u64(h, ch);
  }
  return h;
}

[[nodiscard]] atx::u64 canon_visit(const Ast &ast, ExprId id,
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
[[nodiscard]] atx::u64 canonical_hash(const Ast &ast, ExprId root) noexcept {
  std::unordered_map<ExprId, atx::u64> memo;
  return detail::canon_visit(ast, root, memo);
}

// Convenience: hash a genome's single (first) root. A genome carries one root
// (built from parse_expr / a bare splice), so this is the whole-program key.
[[nodiscard]] atx::u64 canonical_hash(const Genome &g) noexcept {
  return canonical_hash(g.ast, g.ast.roots().front().root);
}

} // namespace atx::engine::factory

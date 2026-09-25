#include "atx/engine/factory/canonical.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
                                   std::vector<atx::u64> &memo,
                                   std::vector<std::uint8_t> &seen) {
  if (seen[id]) {
    return memo[id];
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
    const std::array<atx::u64, 1> ch{canon_visit(ast, e.a, memo, seen)};
    h = mix_children(h, ch);
    break;
  }
  case Expr::Kind::Binary: {
    h = fnv_byte(tagged(Tag::Binary), static_cast<atx::u8>(e.opcode));
    std::array<atx::u64, 2> ch{canon_visit(ast, e.a, memo, seen), canon_visit(ast, e.b, memo, seen)};
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
    std::array<atx::u64, 3> ch{};
    atx::u8 nch = 0;
    for (const ExprId c : {e.a, e.b, e.c}) {
      if (c != kNoExpr) {
        ch[nch++] = canon_visit(ast, c, memo, seen);
      }
    }
    if (is_hash_commutative(e.opcode)) {
      // ascending sort of nch (<=3) elements — byte-identical ordering to std::sort.
      if (nch == 2) {
        if (ch[1] < ch[0]) std::swap(ch[0], ch[1]);
      } else if (nch == 3) {
        if (ch[1] < ch[0]) std::swap(ch[0], ch[1]);
        if (ch[2] < ch[1]) std::swap(ch[1], ch[2]);
        if (ch[1] < ch[0]) std::swap(ch[0], ch[1]);
      }
    }
    h = mix_children(h, std::span<const atx::u64>{ch.data(), nch});
    break;
  }
  case Expr::Kind::Select: {
    h = fnv_byte(tagged(Tag::Select), static_cast<atx::u8>(e.opcode));
    const std::array<atx::u64, 3> ch{canon_visit(ast, e.a, memo, seen), canon_visit(ast, e.b, memo, seen),
                                     canon_visit(ast, e.c, memo, seen)}; // FIXED slot order
    h = mix_children(h, ch);
    break;
  }
  case Expr::Kind::Member: {
    h = tagged(Tag::Member);
    const std::array<atx::u64, 1> ch{canon_visit(ast, e.a, memo, seen)};
    h = mix_children(h, ch);
    h = fnv_bytes(h, ast.field_name(e.name_id)); // pin name (stable)
    break;
  }
  }
  seen[id] = std::uint8_t{1};
  memo[id] = h;
  return h;
}

// ---- canonical_string (W0-A0 / A-18) --------------------------------------

// Append `v` as 16 lowercase hex digits (fixed width ⇒ self-delimiting).
void put_hex(std::string &s, atx::u64 v) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  for (int sh = 60; sh >= 0; sh -= 4) {
    s.push_back(kDigits[static_cast<atx::usize>((v >> static_cast<unsigned>(sh)) & 0xFULL)]);
  }
}

// Append a length-prefixed name ("<len>:<bytes>") so any byte content is safe.
void put_name(std::string &s, std::string_view name) {
  s += std::to_string(name.size());
  s.push_back(':');
  s.append(name);
}

// Join the children as "(c0,c1,...)"; each child form is itself balanced and
// tag-prefixed, so the comma-joined list parses unambiguously.
void put_children(std::string &s, std::vector<std::string> &ch, bool commutative) {
  if (commutative) {
    std::sort(ch.begin(), ch.end()); // canonical operand order for a symmetric op
  }
  s.push_back('(');
  for (atx::usize i = 0; i < ch.size(); ++i) {
    if (i > 0) {
      s.push_back(',');
    }
    s += ch[i];
  }
  s.push_back(')');
}

// Recursive, memoized canonical form of node `id` (mirrors canon_visit's cases).
const std::string &form_visit(const Ast &ast, ExprId id, std::vector<std::string> &memo,
                              std::vector<std::uint8_t> &seen) {
  if (seen[id] != 0) {
    return memo[id];
  }
  const Expr &e = ast.node(id);
  std::string s;
  std::vector<std::string> ch;
  switch (e.kind) {
  case Expr::Kind::Literal:
    s = "L";
    put_hex(s, std::bit_cast<atx::u64>(e.value));
    break;
  case Expr::Kind::Field:
    s = e.dollar ? "F$" : "F";
    put_name(s, ast.field_name(e.name_id));
    break;
  case Expr::Kind::Unary:
    s = "U" + std::to_string(static_cast<unsigned>(e.opcode));
    ch.push_back(form_visit(ast, e.a, memo, seen));
    put_children(s, ch, false);
    break;
  case Expr::Kind::Binary:
    s = "B" + std::to_string(static_cast<unsigned>(e.opcode));
    ch.push_back(form_visit(ast, e.a, memo, seen));
    ch.push_back(form_visit(ast, e.b, memo, seen));
    put_children(s, ch, is_hash_commutative(e.opcode));
    break;
  case Expr::Kind::Call:
    s = "C" + std::to_string(static_cast<unsigned>(e.opcode));
    put_name(s, (e.op != nullptr) ? std::string_view{e.op->name} : std::string_view{});
    s.push_back('[');
    for (atx::u8 k = 0; k < e.n_hparams; ++k) {
      put_hex(s, std::bit_cast<atx::u64>(e.hparams[k]));
    }
    s.push_back(']');
    for (const ExprId c : {e.a, e.b, e.c}) {
      if (c != kNoExpr) {
        ch.push_back(form_visit(ast, c, memo, seen));
      }
    }
    put_children(s, ch, is_hash_commutative(e.opcode));
    break;
  case Expr::Kind::Select:
    s = "S" + std::to_string(static_cast<unsigned>(e.opcode));
    ch.push_back(form_visit(ast, e.a, memo, seen));
    ch.push_back(form_visit(ast, e.b, memo, seen));
    ch.push_back(form_visit(ast, e.c, memo, seen));
    put_children(s, ch, false); // FIXED slot order (not commutative)
    break;
  case Expr::Kind::Member:
    s = "M";
    put_name(s, ast.field_name(e.name_id));
    ch.push_back(form_visit(ast, e.a, memo, seen));
    put_children(s, ch, false);
    break;
  }
  seen[id] = std::uint8_t{1};
  memo[id] = std::move(s);
  return memo[id];
}

} // namespace detail

// ---- CanonSet verified API (W0-A0 / A-18) ---------------------------------

bool CanonSet::contains(atx::u64 h, std::string_view form) const {
  if (seen.find(h) == seen.end()) {
    return false;
  }
  const auto it = forms.find(h);
  if (it == forms.end() || it->second.empty()) {
    return true; // hash known without a form (legacy insert / resume): cannot disprove
  }
  return std::find(it->second.begin(), it->second.end(), form) != it->second.end();
}

bool CanonSet::insert(atx::u64 h, std::string form) {
  const bool hash_known = seen.find(h) != seen.end();
  const auto it = forms.find(h);
  // Decide BEFORE touching `forms`: a hash-only key must never gain an (empty)
  // forms entry, or contains(h, form) would flip it to "unseen" (review A-18 fix 1).
  if (hash_known && (it == forms.end() || it->second.empty())) {
    return false; // legacy hash-only entry: treated as seen (see CanonSet)
  }
  if (it != forms.end() &&
      std::find(it->second.begin(), it->second.end(), form) != it->second.end()) {
    return false; // the same structure — a true duplicate
  }
  if (hash_known) {
    ++n_collisions; // same 64-bit hash, different canonical form
  }
  forms[h].push_back(std::move(form));
  seen.insert(h);
  ++n_distinct;
  return true;
}

[[nodiscard]] std::string canonical_string(const Ast &ast, ExprId root) {
  std::vector<std::string> memo(ast.nodes().size());
  std::vector<std::uint8_t> seen(ast.nodes().size(), std::uint8_t{0});
  return detail::form_visit(ast, root, memo, seen);
}

[[nodiscard]] std::string canonical_string(const Genome &g) {
  return canonical_string(g.ast, g.ast.roots().front().root);
}

[[nodiscard]] std::string canonical_string(const Genome &g, const CanonCfg &cfg) {
  if (!cfg.semantic) {
    return canonical_string(g);
  }
  const Ast normal = rewrite_ast(g.ast, g.ast.roots().front().root, cfg.rewrite);
  return canonical_string(normal, normal.roots().front().root);
}

// Stable, sound, discriminating canonical hash of the sub-DAG rooted at `root`.
// Recursive + memoized over the sub-DAG so a shared sub-expression is hashed once.
[[nodiscard]] atx::u64 canonical_hash(const Ast &ast, ExprId root) noexcept {
  std::vector<atx::u64> memo(ast.nodes().size());
  std::vector<std::uint8_t> seen(ast.nodes().size(), std::uint8_t{0});
  return detail::canon_visit(ast, root, memo, seen);
}

// Convenience: hash a genome's single (first) root. A genome carries one root
// (built from parse_expr / a bare splice), so this is the whole-program key.
[[nodiscard]] atx::u64 canonical_hash(const Genome &g) noexcept {
  return canonical_hash(g.ast, g.ast.roots().front().root);
}

[[nodiscard]] atx::u64 canonical_hash(const Genome &g, const CanonCfg &cfg) {
  if (!cfg.semantic) {
    return canonical_hash(g);
  }
  const Ast normal = rewrite_ast(g.ast, g.ast.roots().front().root, cfg.rewrite);
  return canonical_hash(normal, normal.roots().front().root);
}

} // namespace atx::engine::factory

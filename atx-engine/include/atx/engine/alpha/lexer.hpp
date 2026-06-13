#pragma once

// atx::engine::alpha — hand-written lexer for the alpha-expression DSL (P3-1).
//
// Public API:
//   Result<std::vector<Token>> lex(std::string_view src);
//
// Contract:
//   * Total scan: every byte in `src` is consumed.
//   * Whitespace (' ', '\t', '\r', '\n') is silently skipped.
//   * Maximal munch for two-character operators (<=, >=, ==, !=, &&, ||).
//   * A lone '&' or '|' is a ParseError (no single-char boolean ops in the DSL).
//   * Numbers are parsed with std::from_chars<double> (C++17, locale-independent,
//     no strtod locale surprises). Handles integer, decimal, and scientific forms
//     (1, 0.5, 1e3, 1E3, 2.5e-2). A '.' that begins a numeric literal (i.e. is
//     followed by a digit) is consumed as part of that literal; an isolated '.'
//     (not preceded by a digit context, not followed by a digit) lexes as Dot.
//   * Identifiers: [A-Za-z_] start, then [A-Za-z0-9_] continue. A '.' is NOT
//     consumed inside an identifier — it emits a separate Dot token (B4), which
//     the parser uses for member access (`kf.beta` → Ident Dot Ident).
//   * '$' lexes as Dollar; the parser pairs it with the subsequent Ident.
//   * '.' lexes as Dot (TokenKind::Dot) whenever it is not part of a number.
//   * Every successful scan appends exactly one End token with span {len, len}.
//   * On any unrecognised byte, returns Err(ParseError, message with offset).
//   * Return type is NOT noexcept — std::vector allocation may throw.
//
// Ownership / lifetime: `src` is only borrowed during the call; tokens are
// returned by value (the caller owns the vector).

#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

// Pull in the forward declarations; we provide the full definitions below.
#include "atx/engine/alpha/fwd.hpp"

namespace atx::engine::alpha {

// =========================================================================
//  Full type definitions (forward-declared in fwd.hpp)
// =========================================================================

// Lexer token category.  Underlying type must match fwd.hpp (atx::u8).
enum class TokenKind : atx::u8 {
  Number,
  Ident,
  Dollar,
  Dot, // '.' — member-access separator (B4); not consumed inside identifiers or numbers
  Plus,
  Minus,
  Star,
  Slash,
  Caret,
  Lt,
  Gt,
  Le,
  Ge,
  EqEq,
  BangEq,
  AmpAmp,
  PipePipe,
  Bang,
  Question,
  Colon,
  LParen,
  RParen,
  Comma,
  Assign,
  End,
};

// Half-open byte range [begin, end) into the source string_view.
struct Span {
  atx::u32 begin{};
  atx::u32 end{};
};

// A single lexed token.  `number` is populated only for TokenKind::Number.
struct Token {
  TokenKind kind{TokenKind::End};
  Span span{};
  atx::f64 number{};
};

// =========================================================================
//  Internal helpers (detail namespace — not part of the public API).
//  Header-defined free functions are `inline` to avoid multiple-definition
//  errors once a second translation unit (e.g. the P3-2 parser) includes
//  this header.
// =========================================================================

namespace detail {

[[nodiscard]] constexpr bool is_ws(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool is_alpha_under(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] constexpr bool is_ident_continue(char c) noexcept {
  return is_alpha_under(c) || is_digit(c);
  // Note: '.' is NOT an ident-continue character (B4). A '.' after an ident
  // becomes a separate Dot token enabling member-access syntax (kf.beta).
}

// Scan a number starting at src[pos].  Returns the parsed value and advances
// *pos past the consumed chars.  Uses std::from_chars (locale-independent).
// Not noexcept: the (defensive) error branch constructs a diagnostic string,
// which may allocate. The success path does not allocate.
[[nodiscard]] atx::core::Result<atx::f64> scan_number(std::string_view src, atx::usize &pos);

// Scan an identifier starting at src[pos].
// Precondition: src[pos] satisfies is_alpha_under.
// Returns the end offset (exclusive). '.' is NOT consumed; it will be emitted
// as a separate Dot token by the main scan loop (B4).
[[nodiscard]] inline atx::usize scan_ident(std::string_view src, atx::usize pos) noexcept {
  const atx::usize len = src.size();
  // Consume the start character (already verified by caller).
  ++pos;
  while (pos < len && is_ident_continue(src[pos])) {
    ++pos;
  }
  return pos;
}

// Scan a single operator or punctuation token. `c` is the first char (already
// consumed by the caller); `pos` points just past it and is advanced over a
// second char for the two-character operators (<=, >=, ==, !=, &&, ||). A lone
// '&'/'|' or any unrecognised byte yields ParseError carrying the offending
// offset. `start` is the byte offset of `c` (for the span and diagnostics).
[[nodiscard]] atx::core::Result<Token> scan_operator(std::string_view src, char c, atx::u32 start,
                                                     atx::usize &pos);

} // namespace detail

// =========================================================================
//  Public API
// =========================================================================

// Lex `src` into a token vector.
//
// Returns Err(ParseError, message) on the first unrecognised byte.
// The message encodes the byte value and its decimal offset so tests and
// diagnostics can surface both without a dedicated span field on Error.
[[nodiscard]] atx::core::Result<std::vector<Token>> lex(std::string_view src);

} // namespace atx::engine::alpha

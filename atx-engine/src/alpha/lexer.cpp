#include "atx/engine/alpha/lexer.hpp"

#include <charconv> // std::from_chars (locale-independent numeric scan)
#include <string>   // std::string / std::to_string (diagnostic messages)
#include <utility>  // std::move (token vector into the result aggregate)

namespace atx::engine::alpha {

namespace detail {

[[nodiscard]] atx::core::Result<atx::f64> scan_number(std::string_view src, atx::usize &pos) {
  const char *begin = src.data() + pos;
  const char *end = src.data() + src.size();
  atx::f64 value{};
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{}) {
    // from_chars failed — should not happen if caller checked is_digit,
    // but guard defensively.
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          std::string{"lex error: invalid number at offset "} +
                              std::to_string(pos));
  }
  pos = static_cast<atx::usize>(ptr - src.data());
  return atx::core::Ok(value);
}

[[nodiscard]] atx::core::Result<Token> scan_operator(std::string_view src, char c, atx::u32 start,
                                                     atx::usize &pos) {
  const auto peek = [&]() -> char { return pos < src.size() ? src[pos] : '\0'; };
  TokenKind kind{TokenKind::End}; // overwritten on every non-error path

  switch (c) {
  case '$':
    kind = TokenKind::Dollar;
    break;
  case '+':
    kind = TokenKind::Plus;
    break;
  case '-':
    kind = TokenKind::Minus;
    break;
  case '*':
    kind = TokenKind::Star;
    break;
  case '/':
    kind = TokenKind::Slash;
    break;
  case '^':
    kind = TokenKind::Caret;
    break;
  case '?':
    kind = TokenKind::Question;
    break;
  case ':':
    kind = TokenKind::Colon;
    break;
  case '(':
    kind = TokenKind::LParen;
    break;
  case ')':
    kind = TokenKind::RParen;
    break;
  case ',':
    kind = TokenKind::Comma;
    break;
  case '<':
    kind = (peek() == '=') ? (++pos, TokenKind::Le) : TokenKind::Lt;
    break;
  case '>':
    kind = (peek() == '=') ? (++pos, TokenKind::Ge) : TokenKind::Gt;
    break;
  case '=':
    kind = (peek() == '=') ? (++pos, TokenKind::EqEq) : TokenKind::Assign;
    break;
  case '!':
    kind = (peek() == '=') ? (++pos, TokenKind::BangEq) : TokenKind::Bang;
    break;
  case '&':
    if (peek() != '&') {
      return atx::core::Err(atx::core::ErrorCode::ParseError,
                            std::string{"lex error: unexpected byte '&' at offset "} +
                                std::to_string(start));
    }
    ++pos;
    kind = TokenKind::AmpAmp;
    break;
  case '|':
    if (peek() != '|') {
      return atx::core::Err(atx::core::ErrorCode::ParseError,
                            std::string{"lex error: unexpected byte '|' at offset "} +
                                std::to_string(start));
    }
    ++pos;
    kind = TokenKind::PipePipe;
    break;
  case '.':
    // A '.' that reaches here is NOT part of a number (digits are handled
    // before scan_operator is called) — emit a standalone Dot token (B4).
    kind = TokenKind::Dot;
    break;
  default:
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          std::string{"lex error: unexpected byte '"} + c + "' at offset " +
                              std::to_string(start));
  }

  return atx::core::Ok(Token{kind, Span{start, static_cast<atx::u32>(pos)}, {}});
}

} // namespace detail

[[nodiscard]] atx::core::Result<std::vector<Token>> lex(std::string_view src) {
  std::vector<Token> tokens;
  tokens.reserve(src.size() / 2 + 4); // rough heuristic; avoids many reallocs

  const atx::usize len = src.size();
  atx::usize pos{0};

  while (pos < len) {
    // Skip whitespace.
    if (detail::is_ws(src[pos])) {
      ++pos;
      continue;
    }

    const auto start = static_cast<atx::u32>(pos);
    const char c = src[pos];

    // ---- numbers --------------------------------------------------------
    if (detail::is_digit(c)) {
      auto res = detail::scan_number(src, pos);
      if (!res) {
        return atx::core::Err(std::move(res).error());
      }
      tokens.push_back(Token{TokenKind::Number, Span{start, static_cast<atx::u32>(pos)}, *res});
      continue;
    }

    // ---- identifiers ----------------------------------------------------
    if (detail::is_alpha_under(c)) {
      const atx::usize end_pos = detail::scan_ident(src, pos);
      tokens.push_back(Token{TokenKind::Ident, Span{start, static_cast<atx::u32>(end_pos)}, {}});
      pos = end_pos;
      continue;
    }

    // ---- operators & punctuation (single and multi-char) ----------------
    ++pos; // consume the first char; scan_operator peeks for a second
    auto tok = detail::scan_operator(src, c, start, pos);
    if (!tok) {
      return atx::core::Err(std::move(tok).error());
    }
    tokens.push_back(*tok);
  }

  // Always append the End sentinel.
  const auto end_off = static_cast<atx::u32>(len);
  tokens.push_back(Token{TokenKind::End, Span{end_off, end_off}, {}});
  return atx::core::Ok(std::move(tokens));
}

} // namespace atx::engine::alpha

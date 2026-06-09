#pragma once

// atx::engine::alpha — unparse(Ast) -> DSL text (S4b-1).
//
// Renders an `Ast` sub-tree back to a re-parseable DSL string. A discovered
// alpha is worthless if you cannot read (and re-parse) its formula: every
// admitted alpha carries a textual form that `parse_expr` accepts again.
//
// The LOAD-BEARING contract is round-trip THROUGH THE CANONICAL KEY, not textual
// prettiness:
//
//     canonical_hash(parse_expr(unparse(ast))) == canonical_hash(ast)
//
// i.e. the re-parse must reproduce the SAME structural identity (factory's
// canonical_hash). That fixes three things the printer MUST get right — all of
// which `canonical.hpp` keys on:
//   * operand ORDER for non-commutative ops (a - b ≠ b - a),
//   * the WINDOW / lag operand in its ORIGINAL slot (the `8` in ts_mean(close,8)
//     is a CHILD literal — registry n_hparams==0 for Ts* window ops — so it is
//     emitted as a trailing child, NOT an hparam),
//   * constant PRECISION: canonical_hash hashes bit_cast<u64>(value), so the
//     emitted literal must re-lex to the IDENTICAL double bit pattern. We use
//     std::to_chars shortest round-trip (the unique shortest decimal that
//     round-trips a double exactly) — the 8.22237 fixture is the guard.
//
// AST-shape notes (see parser.hpp Expr):
//   * Infix arithmetic / comparison / logical are Binary/Unary nodes carrying an
//     OpCode (NOT Call nodes) — there is no `add`/`sub`/`mul` registry function.
//     We render them in infix form and PARENTHESISE every compound subtree so
//     the structure re-parses regardless of precedence/associativity.
//   * Call nodes carry the resolved `op->name`; their compile-time hparams
//     (kalman_level's Q/R, etc.) live in `hparams[0..n_hparams)` and were peeled
//     from the TRAILING args — so they print AFTER the operand children, in slot
//     order, exactly where the parser peels them back off.
//   * A bare numeric literal Field-leaf store distinction: Field prints its
//     (optionally $-sigilled) interned name; Literal prints its value.
//
// Header-only; this is a RECORD-time path (run once per admitted candidate), so
// correctness beats cleverness — std::string concatenation is fine.

#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error> // std::errc (to_chars_result::ec)

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace atx::engine::alpha {

namespace detail {

// Shortest decimal that round-trips a finite double EXACTLY (std::to_chars with
// no precision arg gives the unique shortest round-tripping representation).
// canonical_hash keys on bit_cast<u64>(value), so re-lexing this string MUST
// reproduce the identical bit pattern — to_chars guarantees that for finite
// values. Non-finite values cannot occur in a parsed Ast literal (the lexer has
// no inf/nan token and parse-time folds of field operands never fire), but we
// emit a lexer-safe finite fallback defensively rather than print "inf"/"nan".
[[nodiscard]] inline std::string format_double(atx::f64 v) {
  if (!std::isfinite(v)) {
    return "0"; // defensive: an Ast literal is always finite (see note above).
  }
  std::array<char, 64> buf{};
  const std::to_chars_result r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  ATX_ASSERT(r.ec == std::errc{}); // 64 bytes is always enough for a double's shortest form
  return std::string(buf.data(), r.ptr);
}

// Infix spelling for a Binary opcode (the parser's binary_opcode inverse). A
// compound Binary subtree is parenthesised by the caller, so precedence need not
// be reconstructed — only the operator token matters for a faithful re-parse.
[[nodiscard]] inline std::string_view binary_op_text(OpCode op) noexcept {
  switch (op) {
  case OpCode::Add:
    return "+";
  case OpCode::Sub:
    return "-";
  case OpCode::Mul:
    return "*";
  case OpCode::Div:
    return "/";
  case OpCode::Pow:
    return "^";
  case OpCode::CmpLt:
    return "<";
  case OpCode::CmpGt:
    return ">";
  case OpCode::CmpLe:
    return "<=";
  case OpCode::CmpGe:
    return ">=";
  case OpCode::CmpEq:
    return "==";
  case OpCode::CmpNe:
    return "!=";
  case OpCode::And:
    return "&&";
  case OpCode::Or:
    return "||";
  default:
    return ""; // unreachable: a Binary node carries one of the above opcodes.
  }
}

// Prefix spelling for a Unary opcode. Neg/Not are the only prefix operators the
// parser produces directly; abs/sign/log are Call nodes (registry functions),
// not Unary, so they are handled in the Call arm. A Unary node here is therefore
// always Neg or Not.
[[nodiscard]] inline std::string_view unary_op_text(OpCode op) noexcept {
  switch (op) {
  case OpCode::Neg:
    return "-";
  case OpCode::Not:
    return "!";
  default:
    return ""; // unreachable for a parser-produced Unary node.
  }
}

// Recursive descent over the arena. Children are referenced by ExprId; every
// compound subtree is parenthesised so the re-parse is structure-faithful.
// NOLINTBEGIN(misc-no-recursion): the Ast is a finite, cold-path tree.
[[nodiscard]] inline std::string unparse_node(const Ast &ast, ExprId id) {
  const Expr &e = ast.node(id);
  switch (e.kind) {
  case Expr::Kind::Literal:
    return format_double(e.value);

  case Expr::Kind::Field: {
    std::string out;
    if (e.dollar) {
      out.push_back('$');
    }
    out.append(ast.field_name(e.name_id));
    return out;
  }

  case Expr::Kind::Unary: {
    std::string out{unary_op_text(e.opcode)};
    out.push_back('(');
    out.append(unparse_node(ast, e.a));
    out.push_back(')');
    return out;
  }

  case Expr::Kind::Binary: {
    std::string out{"("};
    out.append(unparse_node(ast, e.a));
    out.push_back(' ');
    out.append(binary_op_text(e.opcode));
    out.push_back(' ');
    out.append(unparse_node(ast, e.b));
    out.push_back(')');
    return out;
  }

  case Expr::Kind::Call: {
    std::string out;
    out.append((e.op != nullptr) ? e.op->name : std::string_view{});
    out.push_back('(');
    bool first = true;
    // Operand children in fixed a,b,c slot order (only the populated slots).
    for (const ExprId child : {e.a, e.b, e.c}) {
      if (child == kNoExpr) {
        break; // slots fill left-to-right; first empty terminates the operands.
      }
      if (!first) {
        out.append(", ");
      }
      first = false;
      out.append(unparse_node(ast, child));
    }
    // Compile-time hparams were peeled from the TRAILING args — re-emit them
    // after the operands, in slot order, so the parser peels them back the same.
    for (atx::u8 k = 0; k < e.n_hparams; ++k) {
      if (!first) {
        out.append(", ");
      }
      first = false;
      out.append(format_double(e.hparams[k]));
    }
    out.push_back(')');
    return out;
  }

  case Expr::Kind::Select: {
    std::string out{"("};
    out.append(unparse_node(ast, e.a)); // cond
    out.append(" ? ");
    out.append(unparse_node(ast, e.b)); // then
    out.append(" : ");
    out.append(unparse_node(ast, e.c)); // else
    out.push_back(')');
    return out;
  }

  case Expr::Kind::Member: {
    std::string out{"("};
    out.append(unparse_node(ast, e.a));
    out.push_back(')');
    out.push_back('.');
    out.append(ast.field_name(e.name_id));
    return out;
  }
  }
  return {}; // unreachable: all Kind cases return above.
}
// NOLINTEND(misc-no-recursion)

} // namespace detail

// Render the sub-tree rooted at `root` to a re-parseable DSL string. The
// contract is round-trip through factory::canonical_hash, NOT textual identity.
[[nodiscard]] inline std::string unparse(const Ast &ast, ExprId root) {
  return detail::unparse_node(ast, root);
}

// Render the Ast's single/anonymous root (the parse_expr / genome shape: one
// root). Precondition: the Ast has at least one root.
[[nodiscard]] inline std::string unparse(const Ast &ast) {
  return detail::unparse_node(ast, ast.roots().front().root);
}

} // namespace atx::engine::alpha

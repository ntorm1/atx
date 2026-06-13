#include "atx/engine/alpha/parser.hpp"

#include <limits>  // std::numeric_limits (NaN hparam sentinel in parse_call)
#include <string>  // std::string, std::to_string (diagnostics + field names)
#include <utility> // std::move (warnings out, Ast result)
#include <vector>  // std::vector (argument lists)

namespace atx::engine::alpha {

namespace detail {

// The parselet cluster below is mutually recursive by construction — this is
// the defining shape of a recursive-descent / Pratt parser (an expression
// nests sub-expressions). Recursion depth is bounded by the source's nesting
// depth (a cold, finite parse), so the recursion is intentional and safe.
// NOLINTBEGIN(misc-no-recursion)

// Parse a comma-separated argument list after a consumed '(' up to ')'.
// Appends each argument id to `out`. Leaves the cursor just past the ')'.
[[nodiscard]] atx::core::Status parse_args(Parser &p, std::vector<ExprId> &out) {
  if (p.peek_kind() == TokenKind::RParen) {
    p.advance(); // empty arg list
    return atx::core::Ok();
  }
  for (;;) {
    if (p.peek_kind() == TokenKind::RParen || p.peek_kind() == TokenKind::End) {
      // A ')' or End right where an expression must start ⇒ trailing comma.
      return atx::core::Err(parse_error("expected expression (trailing comma?)", p.peek()));
    }
    ATX_TRY(const ExprId arg, parse_precedence(p, detail::kBpTernary));
    out.push_back(arg);
    const TokenKind k = p.peek_kind();
    if (k == TokenKind::Comma) {
      p.advance();
      continue;
    }
    if (k == TokenKind::RParen) {
      p.advance();
      return atx::core::Ok();
    }
    return atx::core::Err(parse_error("expected ',' or ')' in argument list", p.peek()));
  }
}

// Resolve an `IDENT(` call: look up the op, range-check arity, default-fill
// omitted trailing args, then build a Call (folding foldable unary fns on a
// literal). Precondition: cursor is on the '(' token.
[[nodiscard]] atx::core::Result<ExprId> parse_call(Parser &p, std::string_view name,
                                                   const Token &name_tok) {
  const OpSig *sig = p.lib->find(name);
  if (sig == nullptr) {
    return atx::core::Err(
        parse_error(std::string{"unknown operator '"} + std::string{name} + "'", name_tok));
  }
  p.advance(); // consume '('
  std::vector<ExprId> args;
  ATX_TRY_VOID(parse_args(p, args));
  if (args.size() < sig->min_arity || args.size() > sig->max_arity) {
    const std::string expected =
        (sig->min_arity == sig->max_arity)
            ? std::to_string(sig->min_arity)
            : (std::to_string(sig->min_arity) + ".." + std::to_string(sig->max_arity));
    return atx::core::Err(parse_error(std::string{"arity mismatch for '"} + std::string{name} +
                                          "': expected " + expected + ", got " +
                                          std::to_string(args.size()),
                                      name_tok));
  }
  fill_default_args(p, *sig, args);

  // Constant-fold foldable unary functions on a literal operand (log(1)→0).
  // A foldable unary is fixed-arity 1, so the materialized list is exactly one.
  // NOTE: this fast-path fires BEFORE hparam peeling; foldable unaries have
  // n_hparams==0, so no interaction — peeling would be a no-op for them anyway.
  if (args.size() == 1 && detail::is_foldable_unary(sig->opcode)) {
    const Expr &arg0 = p.ast->node(args[0]);
    if (arg0.kind == Expr::Kind::Literal) {
      return p.lit(detail::fold_unary(sig->opcode, arg0.value));
    }
  }

  // Peel the last `sig->n_hparams` args as compile-time constant immediates.
  // They leave the operand child slots (`n_oper` leading args) unchanged. For
  // ops with n_hparams==0 (the majority), n_oper == args.size() — no-op.
  Expr e;
  e.kind = Expr::Kind::Call;
  e.op = sig;
  e.opcode = sig->opcode;
  e.n_hparams = sig->n_hparams;
  const atx::usize n_oper = args.size() - sig->n_hparams;
  for (atx::usize k = 0; k < sig->n_hparams; ++k) {
    const Expr &h = p.ast->node(args[n_oper + k]);
    e.hparams[k] = (h.kind == Expr::Kind::Literal)
                       ? h.value
                       : std::numeric_limits<atx::f64>::quiet_NaN(); // analyze rejects NaN sentinel
  }
  e.a = n_oper > 0 ? args[0] : kNoExpr;
  e.b = n_oper > 1 ? args[1] : kNoExpr;
  e.c = n_oper > 2 ? args[2] : kNoExpr;
  return atx::core::Ok(p.ast->add(e));
}

// Parse a prefix construct (the "nud" in TDOP): literals, fields, calls,
// parenthesised groups, and prefix unary (- !).
[[nodiscard]] atx::core::Result<ExprId> parse_prefix(Parser &p) {
  const Token tok = p.peek();
  switch (tok.kind) {
  case TokenKind::Number: {
    p.advance();
    return atx::core::Ok(p.lit(tok.number));
  }
  case TokenKind::Dollar: {
    p.advance();
    if (p.peek_kind() != TokenKind::Ident) {
      return atx::core::Err(parse_error("expected identifier after '$'", p.peek()));
    }
    const Token id = p.peek();
    p.advance();
    Expr e;
    e.kind = Expr::Kind::Field;
    e.dollar = true;
    e.name_id = p.ast->intern(p.text(id));
    return atx::core::Ok(p.ast->add(e));
  }
  case TokenKind::Ident: {
    p.advance();
    if (p.peek_kind() == TokenKind::LParen) {
      return parse_call(p, p.text(tok), tok); // cursor on '('
    }
    if (const ExprId bound = p.lookup_binding(p.text(tok)); bound != kNoExpr) {
      p.warnings.push_back(std::string{"binding '"} + std::string{p.text(tok)} +
                           "' shadows a possible panel field of the same name");
      return atx::core::Ok(bound); // reference an earlier binding (reuse its ExprId)
    }
    // Collect dotted field segments: Ident [Dot Ident]* — e.g. "IndClass.sector".
    // The B4 lexer emits '.' as a Dot token instead of consuming it inside an
    // ident, so the parser must reassemble dotted paths into a single Field name.
    // (Full member-access AST nodes are deferred to B5; here we produce one Field
    // whose interned name is the dot-joined string, matching pre-B4 behaviour.)
    std::string field_name{p.text(tok)};
    while (p.peek_kind() == TokenKind::Dot) {
      // Peek one more ahead: consume Dot+Ident only; a trailing '.' without an
      // ident continuation remains as a Dot token for the caller to handle.
      const atx::usize saved = p.pos;
      p.advance(); // consume Dot
      if (p.peek_kind() != TokenKind::Ident) {
        p.pos = saved; // back-track: trailing dot is not part of this field
        break;
      }
      field_name += '.';
      field_name += p.text(p.peek());
      p.advance(); // consume trailing Ident segment
    }
    Expr e;
    e.kind = Expr::Kind::Field;
    e.name_id = p.ast->intern(field_name);
    return atx::core::Ok(p.ast->add(e));
  }
  case TokenKind::LParen: {
    p.advance();
    ATX_TRY(const ExprId inner, parse_precedence(p, detail::kBpTernary));
    if (p.peek_kind() != TokenKind::RParen) {
      return atx::core::Err(parse_error("expected ')'", p.peek()));
    }
    p.advance();
    return atx::core::Ok(inner);
  }
  case TokenKind::Minus:
  case TokenKind::Bang: {
    const OpCode op = (tok.kind == TokenKind::Minus) ? OpCode::Neg : OpCode::Not;
    p.advance();
    ATX_TRY(const ExprId operand, parse_precedence(p, detail::kBpUnary));
    // Fold a literal under unary minus (-3 → -3.0); `!` is a mask op, never fold.
    const Expr &child = p.ast->node(operand);
    if (op == OpCode::Neg && child.kind == Expr::Kind::Literal) {
      return p.lit(-child.value);
    }
    Expr e;
    e.kind = Expr::Kind::Unary;
    e.opcode = op;
    e.a = operand;
    return atx::core::Ok(p.ast->add(e));
  }
  // Tokens that cannot start an expression.
  case TokenKind::Dot: // member-access postfix; cannot open a prefix position (B5 handles it)
  case TokenKind::Plus:
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Caret:
  case TokenKind::Lt:
  case TokenKind::Gt:
  case TokenKind::Le:
  case TokenKind::Ge:
  case TokenKind::EqEq:
  case TokenKind::BangEq:
  case TokenKind::AmpAmp:
  case TokenKind::PipePipe:
  case TokenKind::Question:
  case TokenKind::Colon:
  case TokenKind::RParen:
  case TokenKind::Comma:
  case TokenKind::Assign:
  case TokenKind::End:
    return atx::core::Err(parse_error("expected an expression", tok));
  }
  return atx::core::Err(parse_error("expected an expression", tok)); // unreachable
}

// Parse the ternary tail `? then : else` after the condition. Desugars to a
// Select node. Precondition: cursor is on the '?' token.
[[nodiscard]] atx::core::Result<ExprId> parse_ternary(Parser &p, ExprId cond) {
  p.advance(); // consume '?'
  ATX_TRY(const ExprId then_e, parse_precedence(p, detail::kBpTernary));
  if (p.peek_kind() != TokenKind::Colon) {
    return atx::core::Err(parse_error("expected ':' in ternary", p.peek()));
  }
  p.advance(); // consume ':'
  // Right operand at kBpTernary so `a?b:c?d:e` = `a?b:(c?d:e)` (right-nested).
  ATX_TRY(const ExprId else_e, parse_precedence(p, detail::kBpTernary));
  Expr e;
  e.kind = Expr::Kind::Select;
  e.opcode = OpCode::Select;
  e.a = cond;
  e.b = then_e;
  e.c = else_e;
  return atx::core::Ok(p.ast->add(e));
}

// Build a Binary node, folding when both operands are literals and the op is a
// pure numeric arithmetic/power op (never comparisons/logical).
[[nodiscard]] ExprId make_binary(Parser &p, OpCode op, ExprId lhs, ExprId rhs) {
  if (!detail::is_compare_or_logical(op)) {
    const Expr &le = p.ast->node(lhs);
    const Expr &re = p.ast->node(rhs);
    if (le.kind == Expr::Kind::Literal && re.kind == Expr::Kind::Literal) {
      return p.lit(detail::fold_binary(op, le.value, re.value));
    }
  }
  Expr e;
  e.kind = Expr::Kind::Binary;
  e.opcode = op;
  e.a = lhs;
  e.b = rhs;
  return p.ast->add(e);
}

// The core precedence-climbing loop. Parses a prefix, then folds in infix
// operators whose left binding power is ≥ `min_bp`. A postfix `.pin` member-
// access parselet is checked first (before any infix-bp test) so that `.`
// always binds tighter than any binary operator — matching call syntax priority.
[[nodiscard]] atx::core::Result<ExprId> parse_precedence(Parser &p, atx::u8 min_bp) {
  ATX_TRY(ExprId left, parse_prefix(p));
  for (;;) {
    // Postfix member access: `expr.pin` — binds tighter than any binary op.
    // This arm only fires when a Dot token is left over after parse_prefix,
    // which only happens if the prefix was a binding-ref, a Call, or a
    // parenthesised expression. Non-binding dotted idents (e.g. IndClass.sector)
    // were already consumed inside parse_prefix's dot-join loop and do NOT
    // leave a Dot here — so this parselet never mis-parses a field name.
    if (p.peek_kind() == TokenKind::Dot) {
      p.advance(); // consume '.'
      if (p.peek_kind() != TokenKind::Ident) {
        return atx::core::Err(parse_error("expected pin name after '.'", p.peek()));
      }
      const Token pin = p.peek();
      p.advance(); // consume pin ident
      Expr m;
      m.kind = Expr::Kind::Member;
      m.a = left;
      m.name_id = p.ast->intern(p.text(pin));
      left = p.ast->add(m);
      continue;
    }
    const TokenKind k = p.peek_kind();
    const atx::u8 lbp = detail::infix_bp(k);
    if (lbp == detail::kBpNone || lbp < min_bp) {
      break;
    }
    if (k == TokenKind::Question) {
      ATX_TRY(left, parse_ternary(p, left)); // right-nested, consumes ?…:…
      continue;
    }
    const OpCode op = detail::binary_opcode(k);
    p.advance();
    // Power is right-assoc (recurse at lbp); all others left-assoc (lbp + 1).
    const atx::u8 next_bp = (k == TokenKind::Caret) ? lbp : static_cast<atx::u8>(lbp + 1);
    ATX_TRY(const ExprId right, parse_precedence(p, next_bp));
    left = make_binary(p, op, left, right);
  }
  return atx::core::Ok(left);
}
// NOLINTEND(misc-no-recursion)

} // namespace detail

// Parse a program (`{ IDENT '=' expr }`) into an Ast with one root per binding.
// An empty source yields an Ast with zero roots. Advisory warnings (e.g. a bare
// identifier resolved to a local binding, which could shadow a panel field of the
// same name) are appended to `*warnings` when the pointer is non-null.
[[nodiscard]] atx::core::Result<Ast>
parse_program(std::string_view source, const Library &lib, std::vector<std::string> *warnings) {
  ATX_TRY(auto toks, detail::lex_checked(source));
  Ast ast;
  detail::Parser p{std::span<const Token>{toks}, source, &lib, &ast, 0, {}, {}};

  while (p.peek_kind() != TokenKind::End) {
    if (p.peek_kind() != TokenKind::Ident) {
      return atx::core::Err(detail::parse_error("expected assignment target (IDENT)", p.peek()));
    }
    const Token name_tok = p.peek();
    p.advance();
    if (p.peek_kind() != TokenKind::Assign) {
      return atx::core::Err(detail::parse_error("expected '=' after assignment target", p.peek()));
    }
    p.advance();
    ATX_TRY(const ExprId root, detail::parse_precedence(p, detail::kBpTernary));
    p.bindings.push_back(detail::Parser::Binding{std::string{p.text(name_tok)}, root});
    ast.add_root(std::string{p.text(name_tok)}, root);
  }
  if (warnings != nullptr) {
    *warnings = std::move(p.warnings);
  }
  return atx::core::Ok(std::move(ast));
}

} // namespace atx::engine::alpha

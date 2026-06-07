#pragma once

// atx::engine::alpha — Pratt parser → AST (P3-2).
//
// A top-down operator-precedence (Pratt/TDOP) parser driven by a single
// ParseRule table. Consumes the lexer's token stream and produces an `Ast`:
// an arena of `Expr` nodes plus the assignment roots. Build-time constant
// folding and ternary desugaring happen as the tree is built.
//
// Public API:
//   Result<Ast> parse_program(std::string_view source, const Library&);
//       grammar `program := { assignment }`, each `IDENT '=' expr`.
//   Result<Ast> parse_expr(std::string_view source, const Library&);
//       a single bare expression → one anonymous assignment root.
//
// Grammar (plan §5, precedence low→high):
//   ternary < || < && < == != < < > <= >= < + - < * / < unary(- !) < ^ (right) < primary
//
// AST representation:
//   * `Expr` is arena-indexed (ExprId = index into Ast::nodes), NOT pointer-
//     linked — children are referenced by id. This keeps nodes small and POD-ish
//     and makes the arena relocatable.
//   * Field and assignment names are OWNED by the Ast (interned into a string
//     pool) so the Ast never dangles on the source string. (The source itself
//     need NOT outlive the Ast.)
//   * Call nodes carry a resolved, non-owning `const OpSig*` borrowed from the
//     Library — arity is checked at parse time (unknown op / arity mismatch →
//     ParseError). The Library MUST outlive any Ast produced against it.
//
// Desugar / fold:
//   * `a - b` → Binary(Sub); unary `-x` → Unary(Neg); `!x` → Unary(Not).
//   * `cond ? a : b` → an Expr::Kind::Select node (3 children).
//   * `signedpower(x,a)` stays a Call to OpCode::Spow (NOT expanded).
//   * Constant subtrees (all-Literal operands) fold for the pure numeric ops
//     (+ - * / ^, unary -, and abs/sign/log of a literal): `2*3`→`6`,
//     `log(1)`→`0`. Ops with field/panel operands never fold.
//   * `pow(x,2)`→`x*x` strength reduction is NOT done here (deferred to P3-4).
//
// Header-only; every free function is `inline`. Parsing is a COLD path —
// std::vector allocation is fine (zero-alloc is a VM hot-path concern only).

#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/lexer.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace atx::engine::alpha {

// Index of a node in the Ast arena. u32 is ample for any single program.
using ExprId = atx::u32;

// Sentinel for "no child" in fixed-arity slots.
inline constexpr ExprId kNoExpr = ~ExprId{0};

// =========================================================================
//  Expr — one AST node (arena-indexed, tagged by Kind).
//
//  The payload is a flat set of fields; only those relevant to `kind` are
//  meaningful (a small tagged layout, not a union — the node is tiny and a
//  union buys nothing here while complicating the value semantics). Children
//  are ExprIds into Ast::nodes; names are u32 ids into Ast's string pool.
// =========================================================================

struct Expr {
  enum class Kind : atx::u8 {
    Literal, // numeric constant            → `value`
    Field,   // field leaf (close, $vwap)   → `name_id`, `dollar`
    Unary,   // -x, !x, abs/sign/log(x)     → `opcode`, `a`
    Binary,  // a + b, a < b, a && b, …      → `opcode`, `a`, `b`
    Call,    // f(args…)                    → `op` (resolved sig), `a`,`b`,`c`
    Select,  // desugared ternary           → `a` (cond), `b` (then), `c` (else)
  };

  Kind kind{Kind::Literal};
  bool dollar{false};           // Field only: was it `$name`?
  OpCode opcode{OpCode::Const}; // Unary/Binary: the operator's opcode
  atx::f64 value{};             // Literal: the folded numeric value
  atx::u32 name_id{};           // Field: index into Ast string pool
  const OpSig *op{nullptr};     // Call: resolved registry row (non-null)
  ExprId a{kNoExpr};            // child 0 (Unary/Binary/Call/Select)
  ExprId b{kNoExpr};            // child 1 (Binary/Call/Select)
  ExprId c{kNoExpr};            // child 2 (Call/Select)
};

// Materialized argument count of a Call node: the number of populated child
// slots (a/b/c). After P3b-1 default-fill the Call's slots reflect the fully-
// applied call, so this is the count downstream consumers (DAG child walk,
// type-checker window selection) must use — NOT a static OpSig field, since a
// variadic op's materialized count varies per call site. Precondition: `e` is a
// Call node. Slots populate left-to-right (a before b before c), so the first
// kNoExpr terminates the count.
[[nodiscard]] inline atx::usize call_arity(const Expr &e) noexcept {
  if (e.a == kNoExpr) {
    return 0;
  }
  if (e.b == kNoExpr) {
    return 1;
  }
  return (e.c == kNoExpr) ? 2 : 3;
}

// One top-level `name = expr` binding (anonymous name for parse_expr).
struct Assignment {
  std::string name; // owned; empty for a bare parse_expr root
  ExprId root{kNoExpr};
};

// =========================================================================
//  Ast — the parser output (owns the node arena + name pool + roots).
//
//  NOT named `Program` (that name is reserved for P3-4 bytecode). The Ast owns
//  everything by value: it borrows nothing from the source string and only a
//  non-owning `const OpSig*` from the Library (which must outlive the Ast).
// =========================================================================

class Ast {
public:
  [[nodiscard]] std::span<const Expr> nodes() const noexcept { return nodes_; }
  [[nodiscard]] std::span<const Assignment> roots() const noexcept { return roots_; }

  [[nodiscard]] const Expr &node(ExprId id) const noexcept { return nodes_[id]; }
  [[nodiscard]] std::string_view field_name(atx::u32 id) const noexcept { return strings_[id]; }

  // Builder API (used by the parser; public so free parselets can populate it).
  [[nodiscard]] ExprId add(const Expr &e) {
    nodes_.push_back(e);
    return static_cast<ExprId>(nodes_.size() - 1);
  }
  [[nodiscard]] atx::u32 intern(std::string_view name) {
    strings_.emplace_back(name);
    return static_cast<atx::u32>(strings_.size() - 1);
  }
  void add_root(std::string name, ExprId root) {
    roots_.push_back(Assignment{std::move(name), root});
  }

private:
  std::vector<Expr> nodes_;          // the arena; ExprId = index
  std::vector<std::string> strings_; // owned field/identifier names
  std::vector<Assignment> roots_;    // top-level bindings
};

// =========================================================================
//  ParseRule table — binding powers (precedence) per infix token.
//
//  `parse_precedence(bp)` parses a prefix, then loops infix while the next
//  token's left binding power (`lbp`) is ≥ `bp`. Left-assoc operators recurse
//  the right operand at `lbp + 1`; the right-assoc power operator recurses at
//  `lbp` (so `a^b^c` = `a^(b^c)`). bp 0 means "not an infix operator".
// =========================================================================

namespace detail {

// Binding-power tiers (low → high). Gaps leave room and make +1 unambiguous.
// Plain constants (not an enum) so the `min_bp` arithmetic (`lbp + 1`) stays in
// the integer domain without casts and the names follow variable conventions.
inline constexpr atx::u8 kBpNone = 0;
inline constexpr atx::u8 kBpTernary = 1;  // ?:
inline constexpr atx::u8 kBpOr = 2;       // ||
inline constexpr atx::u8 kBpAnd = 3;      // &&
inline constexpr atx::u8 kBpEquality = 4; // == !=
inline constexpr atx::u8 kBpCompare = 5;  // < > <= >=
inline constexpr atx::u8 kBpAdd = 6;      // + -
inline constexpr atx::u8 kBpMul = 7;      // * /
inline constexpr atx::u8 kBpUnary = 8;    // prefix - ! (right side parsed here)
inline constexpr atx::u8 kBpPower = 9;    // ^ (right-assoc)

// Left binding power of an infix/postfix token. Non-infix tokens → kBpNone.
[[nodiscard]] inline atx::u8 infix_bp(TokenKind k) noexcept {
  switch (k) {
  case TokenKind::Question:
    return kBpTernary;
  case TokenKind::PipePipe:
    return kBpOr;
  case TokenKind::AmpAmp:
    return kBpAnd;
  case TokenKind::EqEq:
  case TokenKind::BangEq:
    return kBpEquality;
  case TokenKind::Lt:
  case TokenKind::Gt:
  case TokenKind::Le:
  case TokenKind::Ge:
    return kBpCompare;
  case TokenKind::Plus:
  case TokenKind::Minus:
    return kBpAdd;
  case TokenKind::Star:
  case TokenKind::Slash:
    return kBpMul;
  case TokenKind::Caret:
    return kBpPower;
  // Non-infix tokens.
  case TokenKind::Number:
  case TokenKind::Ident:
  case TokenKind::Dollar:
  case TokenKind::Dot: // member-access separator; postfix handled in B5, not infix here
  case TokenKind::Bang:
  case TokenKind::Colon:
  case TokenKind::LParen:
  case TokenKind::RParen:
  case TokenKind::Comma:
  case TokenKind::Assign:
  case TokenKind::End:
    return kBpNone;
  }
  return kBpNone; // unreachable for valid TokenKind
}

// Opcode for a binary infix token (comparisons/logical/arithmetic). Power is
// handled as a Call-free Binary too. Precondition: `k` is a binary infix token.
[[nodiscard]] inline OpCode binary_opcode(TokenKind k) noexcept {
  switch (k) {
  case TokenKind::Plus:
    return OpCode::Add;
  case TokenKind::Minus:
    return OpCode::Sub;
  case TokenKind::Star:
    return OpCode::Mul;
  case TokenKind::Slash:
    return OpCode::Div;
  case TokenKind::Caret:
    return OpCode::Pow;
  case TokenKind::Lt:
    return OpCode::CmpLt;
  case TokenKind::Gt:
    return OpCode::CmpGt;
  case TokenKind::Le:
    return OpCode::CmpLe;
  case TokenKind::Ge:
    return OpCode::CmpGe;
  case TokenKind::EqEq:
    return OpCode::CmpEq;
  case TokenKind::BangEq:
    return OpCode::CmpNe;
  case TokenKind::AmpAmp:
    return OpCode::And;
  case TokenKind::PipePipe:
    return OpCode::Or;
  // Not binary-infix tokens (defensive default-free coverage).
  case TokenKind::Number:
  case TokenKind::Ident:
  case TokenKind::Dollar:
  case TokenKind::Dot: // member-access separator; postfix handled in B5, not binary-infix
  case TokenKind::Bang:
  case TokenKind::Question:
  case TokenKind::Colon:
  case TokenKind::LParen:
  case TokenKind::RParen:
  case TokenKind::Comma:
  case TokenKind::Assign:
  case TokenKind::End:
    return OpCode::Const;
  }
  return OpCode::Const; // unreachable for valid TokenKind
}

// True for comparison/logical opcodes (they yield a Mask, not a fold target).
[[nodiscard]] inline bool is_compare_or_logical(OpCode op) noexcept {
  switch (op) {
  case OpCode::CmpLt:
  case OpCode::CmpGt:
  case OpCode::CmpLe:
  case OpCode::CmpGe:
  case OpCode::CmpEq:
  case OpCode::CmpNe:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::Not:
    return true;
  default:
    return false;
  }
}

// ----- constant folding -------------------------------------------------

// Fold a binary numeric op on two literal operands. Returns the folded value.
// Precondition: `op` is one of Add/Sub/Mul/Div/Pow. Div by zero / pow domain
// issues yield IEEE results (inf/nan) — not an error (matches runtime).
[[nodiscard]] inline atx::f64 fold_binary(OpCode op, atx::f64 x, atx::f64 y) noexcept {
  switch (op) {
  case OpCode::Add:
    return x + y;
  case OpCode::Sub:
    return x - y;
  case OpCode::Mul:
    return x * y;
  case OpCode::Div:
    return x / y;
  case OpCode::Pow:
    return std::pow(x, y);
  default:
    return x; // unreachable: caller restricts to foldable binaries
  }
}

// True if a unary opcode folds on a literal (Neg + abs/sign/log).
[[nodiscard]] inline bool is_foldable_unary(OpCode op) noexcept {
  return op == OpCode::Neg || op == OpCode::Abs || op == OpCode::Sign || op == OpCode::Log;
}

// Fold a foldable unary op on a literal. Precondition: is_foldable_unary(op).
[[nodiscard]] inline atx::f64 fold_unary(OpCode op, atx::f64 x) noexcept {
  switch (op) {
  case OpCode::Neg:
    return -x;
  case OpCode::Abs:
    return std::fabs(x);
  case OpCode::Sign:
    if (x > 0.0) {
      return 1.0;
    }
    return (x < 0.0) ? -1.0 : 0.0;
  case OpCode::Log:
    return std::log(x);
  default:
    return x; // unreachable
  }
}

// ----- the Pratt driver -------------------------------------------------

// Build a ParseError carrying the byte offset of the offending token. A free
// function (not a Parser member) since the diagnostic derives only from its
// arguments, never from parser state.
[[nodiscard]] inline atx::core::Error parse_error(std::string_view what, const Token &t) {
  return atx::core::Error{atx::core::ErrorCode::ParseError, std::string{"parse error: "} +
                                                                std::string{what} + " at offset " +
                                                                std::to_string(t.span.begin)};
}

// Parser state: the token stream + a cursor + the Ast being built + the
// Library for call resolution. Pure recursive descent; errors via Result.
struct Parser {
  std::span<const Token> toks;
  std::string_view src;
  const Library *lib{nullptr};
  Ast *ast{nullptr};
  atx::usize pos{0};

  // Local bindings (Phase 3d-A): name -> the bound expression's ExprId. A bare
  // identifier resolves binding-first, field-fallback. Declaration order; a later
  // binding may shadow an earlier one (last wins) and shadow a panel field.
  struct Binding {
    std::string name;
    ExprId id{kNoExpr};
  };
  std::vector<Binding> bindings;

  // Advisory warnings accumulated during parsing (e.g. binding shadows a field).
  // Non-fatal; surfaced via the 3-arg parse_program overload.
  std::vector<std::string> warnings;

  [[nodiscard]] ExprId lookup_binding(std::string_view name) const noexcept {
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
      if (it->name == name) {
        return it->id; // last binding wins (shadowing)
      }
    }
    return kNoExpr;
  }

  [[nodiscard]] const Token &peek() const noexcept { return toks[pos]; }
  [[nodiscard]] TokenKind peek_kind() const noexcept { return toks[pos].kind; }
  void advance() noexcept { ++pos; }

  // The text a token spans in the source (for field/ident names + diagnostics).
  [[nodiscard]] std::string_view text(const Token &t) const noexcept {
    return src.substr(t.span.begin, t.span.end - t.span.begin);
  }

  // Make a literal node.
  [[nodiscard]] ExprId lit(atx::f64 v) const {
    Expr e;
    e.kind = Expr::Kind::Literal;
    e.value = v;
    return ast->add(e);
  }
};

// The parselet cluster below is mutually recursive by construction — this is
// the defining shape of a recursive-descent / Pratt parser (an expression
// nests sub-expressions). Recursion depth is bounded by the source's nesting
// depth (a cold, finite parse), so the recursion is intentional and safe.
// NOLINTBEGIN(misc-no-recursion)
[[nodiscard]] inline atx::core::Result<ExprId> parse_precedence(Parser &p, atx::u8 min_bp);

// Parse a comma-separated argument list after a consumed '(' up to ')'.
// Appends each argument id to `out`. Leaves the cursor just past the ')'.
[[nodiscard]] inline atx::core::Status parse_args(Parser &p, std::vector<ExprId> &out) {
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

// Append default Literal nodes for arguments the call omitted (P3b-1). For each
// missing trailing arg index k in [supplied, max_arity), the op's default is
// `defaults[k - min_arity]`: a finite value is materialized as a Literal so the
// DAG/VM see a fully-applied call; a NaN sentinel is SKIPPED (the kernel handles
// the absence). Precondition: min_arity ≤ args.size() ≤ max_arity.
inline void fill_default_args(Parser &p, const OpSig &sig, std::vector<ExprId> &args) {
  for (atx::usize k = args.size(); k < sig.max_arity; ++k) {
    // SAFETY: `k - sig.min_arity` lies in [0, max_arity - min_arity). The
    // Library (static built-in table + register_op validation) guarantees
    // max_arity - min_arity <= kMaxDefaults, so this index is in-bounds of the
    // fixed-size defaults array — no out-of-bounds read.
    const atx::f64 value = sig.defaults[k - sig.min_arity];
    if (std::isnan(value)) {
      break; // NaN sentinel: leave this (and any later) optional arg absent.
    }
    args.push_back(p.lit(value));
  }
}

// Resolve an `IDENT(` call: look up the op, range-check arity, default-fill
// omitted trailing args, then build a Call (folding foldable unary fns on a
// literal). Precondition: cursor is on the '(' token.
[[nodiscard]] inline atx::core::Result<ExprId> parse_call(Parser &p, std::string_view name,
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
  if (args.size() == 1 && detail::is_foldable_unary(sig->opcode)) {
    const Expr &arg0 = p.ast->node(args[0]);
    if (arg0.kind == Expr::Kind::Literal) {
      return p.lit(detail::fold_unary(sig->opcode, arg0.value));
    }
  }

  Expr e;
  e.kind = Expr::Kind::Call;
  e.op = sig;
  e.opcode = sig->opcode;
  e.a = !args.empty() ? args[0] : kNoExpr;
  e.b = args.size() > 1 ? args[1] : kNoExpr;
  e.c = args.size() > 2 ? args[2] : kNoExpr;
  return atx::core::Ok(p.ast->add(e));
}

// Parse a prefix construct (the "nud" in TDOP): literals, fields, calls,
// parenthesised groups, and prefix unary (- !).
[[nodiscard]] inline atx::core::Result<ExprId> parse_prefix(Parser &p) {
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
[[nodiscard]] inline atx::core::Result<ExprId> parse_ternary(Parser &p, ExprId cond) {
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
[[nodiscard]] inline ExprId make_binary(Parser &p, OpCode op, ExprId lhs, ExprId rhs) {
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
// operators whose left binding power is ≥ `min_bp`.
[[nodiscard]] inline atx::core::Result<ExprId> parse_precedence(Parser &p, atx::u8 min_bp) {
  ATX_TRY(ExprId left, parse_prefix(p));
  for (;;) {
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

// Lex `source` and assert success, returning the token vector.
[[nodiscard]] inline atx::core::Result<std::vector<Token>> lex_checked(std::string_view source) {
  ATX_TRY(auto toks, lex(source));
  return atx::core::Ok(std::move(toks));
}

} // namespace detail

// =========================================================================
//  Public API
// =========================================================================

// Parse a single bare expression into an Ast with one anonymous root.
[[nodiscard]] inline atx::core::Result<Ast> parse_expr(std::string_view source,
                                                       const Library &lib) {
  ATX_TRY(auto toks, detail::lex_checked(source));
  Ast ast;
  detail::Parser p{std::span<const Token>{toks}, source, &lib, &ast, 0, {}, {}};
  ATX_TRY(const ExprId root, detail::parse_precedence(p, detail::kBpTernary));
  if (p.peek_kind() != TokenKind::End) {
    return atx::core::Err(detail::parse_error("unexpected trailing token", p.peek()));
  }
  ast.add_root(std::string{}, root);
  return atx::core::Ok(std::move(ast));
}

// Parse a program (`{ IDENT '=' expr }`) into an Ast with one root per binding.
// An empty source yields an Ast with zero roots. Advisory warnings (e.g. a bare
// identifier resolved to a local binding, which could shadow a panel field of the
// same name) are appended to `*warnings` when the pointer is non-null.
[[nodiscard]] inline atx::core::Result<Ast>
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

// 2-arg overload: delegates to the 3-arg form, discarding warnings.
[[nodiscard]] inline atx::core::Result<Ast> parse_program(std::string_view source,
                                                          const Library &lib) {
  return parse_program(source, lib, nullptr);
}

} // namespace atx::engine::alpha

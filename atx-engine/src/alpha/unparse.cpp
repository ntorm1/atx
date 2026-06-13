#include "atx/engine/alpha/unparse.hpp"

namespace atx::engine::alpha {

namespace detail {

// Recursive descent over the arena. Children are referenced by ExprId; every
// compound subtree is parenthesised so the re-parse is structure-faithful.
// NOLINTBEGIN(misc-no-recursion): the Ast is a finite, cold-path tree.
[[nodiscard]] std::string unparse_node(const Ast &ast, ExprId id) {
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

} // namespace atx::engine::alpha

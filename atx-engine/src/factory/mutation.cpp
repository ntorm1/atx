#include "atx/engine/factory/mutation.hpp"

#include <cmath>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atx::engine::factory {

[[nodiscard]] std::vector<ClassifiedConst> classify_literals(const Genome &g) {
  const std::span<const Expr> arena = g.ast.nodes();
  std::vector<ConstKind> kind(arena.size(), ConstKind::Scale);
  std::vector<bool> is_literal(arena.size(), false);
  std::vector<bool> referenced(arena.size(), false);

  for (atx::usize i = 0; i < arena.size(); ++i) {
    is_literal[i] = (arena[i].kind == Expr::Kind::Literal);
  }
  // Mark window operands and child-referenced ids.
  for (const Expr &e : arena) {
    for (const ExprId child : {e.a, e.b, e.c}) {
      if (child != kNoExpr) {
        referenced[child] = true;
      }
    }
    if (e.kind == Expr::Kind::Call && e.op != nullptr &&
        alpha::detail::is_time_series(e.op->opcode)) {
      const ExprId win = (call_arity(e) == 3) ? e.c : e.b;
      if (win != kNoExpr && is_literal[win]) {
        kind[win] = ConstKind::Window;
      }
    }
  }
  // Mark orphan literals that match a Call's peeled hparam value as Hparam.
  // Collect every call's peeled hparam values once, then a single arena pass —
  // O(num_nodes + orphan_literals * total_hparams) instead of rescanning the whole
  // arena per (call, hparam). Result is identical: a node is Hparam iff it is an
  // orphan Scale literal whose value equals some call's hparam (exact f64 equality;
  // a NaN hparam matches nothing, as before).
  std::vector<atx::f64> hparam_values;
  for (const Expr &e : arena) {
    if (e.kind == Expr::Kind::Call) {
      for (atx::u8 k = 0; k < e.n_hparams; ++k) {
        hparam_values.push_back(e.hparams[k]);
      }
    }
  }
  if (!hparam_values.empty()) {
    for (atx::usize i = 0; i < arena.size(); ++i) {
      if (is_literal[i] && !referenced[i] && kind[i] == ConstKind::Scale) {
        for (const atx::f64 v : hparam_values) {
          if (arena[i].value == v) {
            kind[i] = ConstKind::Hparam;
            break;
          }
        }
      }
    }
  }

  std::vector<ClassifiedConst> out;
  for (atx::usize i = 0; i < arena.size(); ++i) {
    if (is_literal[i]) {
      out.push_back(ClassifiedConst{static_cast<ExprId>(i), kind[i]});
    }
  }
  return out;
}

namespace detail {

// Draw a uniform index in [0, n). Precondition: n > 0. One u64 of entropy.
[[nodiscard]] atx::usize uniform_index(Xoshiro256pp &rng, atx::usize n) noexcept {
  return static_cast<atx::usize>(rng.next_u64() % n);
}

} // namespace detail

// =========================================================================
//  op_swap — swap a Call node's named op (§4.2).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome> op_swap(const Genome &g, const OpCatalog &cat,
                                                Xoshiro256pp &rng) {
  const std::span<const Expr> arena = g.ast.nodes();
  // Candidate nodes in canonical-id order (§4.2 {Unary, Binary, Call}): Call
  // nodes carry a named OpSig* (swapped via the named-op buckets); Unary/Binary
  // carry a bare `opcode` for the parser's infix/prefix ops (swapped via the
  // OpCode buckets, §0.4). Record-producing calls are skipped (no record op is
  // ever offered as a replacement, so they have no candidate).
  std::vector<ExprId> cands;
  for (atx::usize i = 0; i < arena.size(); ++i) {
    const Expr &e = arena[i];
    // A named Call is swappable unless it is a record op (pins) or an
    // hparam-peeling op (n_hparams > 0): hparam ops are swap-inert (their peeled
    // immediates are not re-derived on swap), matching add_op's bucket exclusion.
    const bool is_named_call =
        e.kind == Expr::Kind::Call && e.op != nullptr && e.op->pins.empty() && e.n_hparams == 0;
    const bool is_bare_op = e.kind == Expr::Kind::Unary || e.kind == Expr::Kind::Binary;
    if (is_named_call || is_bare_op) {
      cands.push_back(static_cast<ExprId>(i));
    }
  }
  if (cands.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "op_swap: no swappable node");
  }
  const ExprId target = cands[detail::uniform_index(rng, cands.size())];
  const Expr &tnode = g.ast.node(target);
  const alpha::TypeInfo &ti = g.analysis.info(target);

  if (tnode.kind == Expr::Kind::Call) {
    const OpSig *current = tnode.op;
    const atx::usize arity = call_arity(tnode);
    const auto repl = cat.sample_compatible(ti.shape, ti.dtype, arity, current, rng);
    if (!repl) {
      return atx::core::Err(atx::core::ErrorCode::NotFound, "op_swap: no compatible replacement op");
    }
    const OpSig *new_op = *repl;
    Ast rebuilt = rebuild_with(g, target, [new_op](Expr &e, Ast & /*dst*/) {
      e.op = new_op;
      e.opcode = new_op->opcode;
      e.n_hparams = new_op->n_hparams; // keep peeled-hparam count consistent
    });
    return analyze_into(std::move(rebuilt));
  }

  // Unary/Binary: swap the bare opcode within the same (shape, dtype, arity)
  // OpCode bucket. These nodes have no OpSig* — the parser maps infix/prefix
  // operators straight to OpCodes (§0.4), so we leave `op` untouched (null).
  const atx::usize arity = (tnode.kind == Expr::Kind::Unary) ? 1 : 2;
  const auto repl = cat.sample_compatible_opcode(ti.shape, ti.dtype, arity, tnode.opcode, rng);
  if (!repl) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "op_swap: no compatible replacement opcode");
  }
  const OpCode new_opcode = *repl;
  Ast rebuilt = rebuild_with(g, target, [new_opcode](Expr &e, Ast & /*dst*/) {
    e.opcode = new_opcode; // bare opcode swap; `op` stays null for Unary/Binary
  });
  return analyze_into(std::move(rebuilt));
}

// =========================================================================
//  field_swap — swap a Field leaf for another panel field (§4.2).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome>
field_swap(const Genome &g, std::span<const std::string_view> panel_fields, Xoshiro256pp &rng) {
  const std::span<const Expr> arena = g.ast.nodes();
  std::vector<ExprId> leaves;
  for (atx::usize i = 0; i < arena.size(); ++i) {
    if (arena[i].kind == Expr::Kind::Field) {
      leaves.push_back(static_cast<ExprId>(i));
    }
  }
  if (leaves.empty() || panel_fields.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "field_swap: no field leaf / no candidates");
  }
  const ExprId target = leaves[detail::uniform_index(rng, leaves.size())];
  const std::string_view newf = panel_fields[detail::uniform_index(rng, panel_fields.size())];
  Ast rebuilt = rebuild_with(g, target, [newf](Expr &e, Ast &dst) {
    // Replace the field name: re-intern the new spelling into dst's pool and
    // clear the `$`-sigil flag (panel fields swapped in are plain identifiers).
    e.dollar = false;
    e.name_id = dst.intern(newf);
  });
  return analyze_into(std::move(rebuilt));
}

// =========================================================================
//  jitter_const — perturb a classified Literal (§4.2 / §0.3).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome> jitter_const(const Genome &g, Xoshiro256pp &rng,
                                                     JitterCfg cfg) {
  const std::vector<ClassifiedConst> consts = classify_literals(g);
  if (consts.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "jitter_const: no literal to perturb");
  }
  const ClassifiedConst pick = consts[detail::uniform_index(rng, consts.size())];
  const atx::f64 old = g.ast.node(pick.id).value;
  const atx::f64 factor = std::exp(cfg.sigma * rng.normal());
  atx::f64 next = old * factor;
  if (pick.kind == ConstKind::Window) {
    // Floor to an integer window, then clamp to [1, max_lookback] (§0.3). The
    // type-checker floors fractional windows too, but we pre-floor so the
    // post-jitter value the genome carries already satisfies the rail.
    atx::f64 floored = std::floor(next);
    if (floored < 1.0) {
      floored = 1.0;
    }
    const atx::f64 cap = static_cast<atx::f64>(cfg.max_lookback);
    if (floored > cap) {
      floored = cap;
    }
    next = floored;
  }
  // Scale / Hparam: keep the multiplicative jitter as-is (analyze backstops any
  // range violation, e.g. a filter hparam that must stay positive).
  Ast rebuilt = rebuild_with(g, pick.id, [next](Expr &e, Ast & /*dst*/) { e.value = next; });
  return analyze_into(std::move(rebuilt));
}

} // namespace atx::engine::factory

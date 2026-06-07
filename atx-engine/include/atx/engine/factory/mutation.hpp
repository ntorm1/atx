#pragma once

// atx::engine::factory — mutation operators (S3-1, plan §4.2 / §0.2 / §0.3).
//
// Three type-safe, seeded, single-point mutations over a Genome. Each:
//   1. enumerates its candidate targets in CANONICAL-ID (ascending ExprId)
//      order — the fixed order is established BEFORE any RNG draw (F1/F2);
//   2. draws ONE target (and one replacement) from a seeded `Xoshiro256pp`;
//   3. REBUILDS a fresh Ast via `rebuild_with` (the Ast has no in-place edit);
//   4. funnels through `analyze_into` — the F5 validity oracle. A candidate that
//      fails analyze (shape/dtype/causality) is returned as `Err`, never a
//      half-valid genome. `Err(NotFound)` when no target/replacement exists.
//
//   op_swap     — swap a Call node's named op for another in the same
//                 (shape, dtype, arity) OpCatalog bucket (!= current op).
//   field_swap  — swap a Field leaf for another panel field (same F64 slot).
//   jitter_const— perturb a Literal, classified Window vs Scale vs Hparam (§0.3):
//                 Window (the trailing integer operand of a Ts* op) is jittered
//                 multiplicatively then floored & clamped to [1, max_lookback];
//                 Scale (any other numeric literal) is jittered multiplicatively.
//
// Determinism (F1): every draw is from the caller-seeded `Xoshiro256pp`; same
// seed ⇒ byte-identical mutant. NEVER seed by worker/thread/time.
//
// Header-only; COLD path (one rebuild per candidate). SAFETY: the OpCatalog and
// every genome borrow ops from the ONE run-wide Library (see genome.hpp).

#include <cmath>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/op_catalog.hpp"

namespace atx::engine::factory {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::call_arity;
using atx::engine::alpha::DType;
using atx::engine::alpha::OpSig;

// =========================================================================
//  Literal classification (§0.3) — Window vs Scale vs Hparam.
// =========================================================================

enum class ConstKind : atx::u8 { Window, Scale, Hparam };

struct ClassifiedConst {
  ExprId id{kNoExpr};
  ConstKind kind{ConstKind::Scale};
};

// Tuning for jitter_const. `sigma` is the log-normal step (multiplicative);
// `max_lookback` is the inclusive upper bound a jittered Window is clamped to.
struct JitterCfg {
  atx::f64 sigma{0.5};
  atx::u16 max_lookback{250};
};

// Walk the arena and classify every Literal node. A Literal is:
//   * Window — iff it is the trailing window operand of a temporal Ts* Call
//     (arg `c` for arity-3 corr/cov, else arg `b`; the operand `window_value`
//     reads in the type-checker);
//   * Hparam — iff it is an ORPHAN literal whose value equals a Call's peeled
//     `hparams[k]` (the parser peels trailing hparam args into Expr::hparams and
//     leaves the source Literal node unreferenced by any a/b/c, §0.3);
//   * Scale  — any other numeric literal (the fractional-constant / coefficient).
//
// Returned in ascending-ExprId (canonical) order so a downstream seeded draw is
// deterministic (F1).
[[nodiscard]] inline std::vector<ClassifiedConst> classify_literals(const Genome &g) {
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
  for (const Expr &e : arena) {
    if (e.kind != Expr::Kind::Call) {
      continue;
    }
    for (atx::u8 k = 0; k < e.n_hparams; ++k) {
      for (atx::usize i = 0; i < arena.size(); ++i) {
        if (is_literal[i] && !referenced[i] && kind[i] == ConstKind::Scale &&
            arena[i].value == e.hparams[k]) {
          kind[i] = ConstKind::Hparam;
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
[[nodiscard]] inline atx::usize uniform_index(Xoshiro256pp &rng, atx::usize n) noexcept {
  return static_cast<atx::usize>(rng.next_u64() % n);
}

} // namespace detail

// =========================================================================
//  op_swap — swap a Call node's named op (§4.2).
// =========================================================================

[[nodiscard]] inline atx::core::Result<Genome> op_swap(const Genome &g, const OpCatalog &cat,
                                                       Xoshiro256pp &rng) {
  const std::span<const Expr> arena = g.ast.nodes();
  // Candidate Call nodes in canonical-id order. Record-producing calls are
  // skipped (the catalog never offers a record op, so they have no replacement).
  std::vector<ExprId> cands;
  for (atx::usize i = 0; i < arena.size(); ++i) {
    if (arena[i].kind == Expr::Kind::Call && arena[i].op != nullptr && arena[i].op->pins.empty()) {
      cands.push_back(static_cast<ExprId>(i));
    }
  }
  if (cands.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "op_swap: no Call node to mutate");
  }
  const ExprId target = cands[detail::uniform_index(rng, cands.size())];
  const alpha::TypeInfo &ti = g.analysis.info(target);
  const OpSig *current = g.ast.node(target).op;
  const atx::usize arity = call_arity(g.ast.node(target));
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

// =========================================================================
//  field_swap — swap a Field leaf for another panel field (§4.2).
// =========================================================================

[[nodiscard]] inline atx::core::Result<Genome>
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

[[nodiscard]] inline atx::core::Result<Genome> jitter_const(const Genome &g, Xoshiro256pp &rng,
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

#include "atx/engine/factory/mutation.hpp"

#include <cmath>
#include <limits>
#include <span>
#include <string>
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

namespace {

// ----- depth measurement (wrap_in_op) ---------------------------------------
//
// analyze() does NOT enforce a depth budget (depth is a generation-time GenConfig
// concern), so wrap_in_op must compute the resulting tree height itself and reject
// a wrap that would exceed `max_depth`. "Height" here is the number of EDGES on
// the longest root→leaf path (a single leaf has height 0).
//
// Two pure passes over the arena (children always precede parents — the analyze
// invariant), both keyed by ExprId:
//   * sub_height[i]  = height of the subtree rooted at node i.
//   * node_depth[i]  = depth of node i below the program root (root depth 0);
//     kNoExpr-marked until reached from the root downward.
// Wrapping node `t` inserts ONE new level above t's subtree, so the resulting
// tree height is max(current_height, node_depth[t] + 1 + sub_height[t]). A node
// is a depth-legal wrap target iff that value <= max_depth.

[[nodiscard]] std::vector<atx::u16> subtree_heights(const std::span<const Expr> arena) {
  std::vector<atx::u16> h(arena.size(), 0);
  // children precede parents -> a forward pass sees each child's height first.
  for (atx::usize i = 0; i < arena.size(); ++i) {
    const Expr &e = arena[i];
    atx::u16 child_max = 0;
    bool has_child = false;
    for (const ExprId c : {e.a, e.b, e.c}) {
      if (c != kNoExpr) {
        has_child = true;
        if (h[c] > child_max) {
          child_max = h[c];
        }
      }
    }
    h[i] = has_child ? static_cast<atx::u16>(child_max + 1) : atx::u16{0};
  }
  return h;
}

// Depth of every node below the single program root (root depth 0); a node not
// reachable from the root keeps the u16::max sentinel. The arena is topologically
// ordered children-before-parents (post-order append), so a parent always has a
// LARGER ExprId than its children. A DESCENDING-ExprId walk therefore visits each
// parent before its children, letting us seed the root's depth and propagate it
// down to every child in a single pass (the `min` is defensive for a shared
// sub-DAG reached by two parents — it keeps the shallowest depth).
[[nodiscard]] std::vector<atx::u16> node_depths(const std::span<const Expr> arena, ExprId root) {
  constexpr atx::u16 kUnset = (std::numeric_limits<atx::u16>::max)();
  std::vector<atx::u16> d(arena.size(), kUnset);
  if (root == kNoExpr || arena.empty()) {
    return d;
  }
  d[root] = 0;
  // Parents have a LARGER ExprId than their children (post-order append), so a
  // descending walk visits a parent before its children — propagate depth down.
  for (atx::usize ii = arena.size(); ii-- > 0;) {
    if (d[ii] == kUnset) {
      continue;
    }
    const Expr &e = arena[ii];
    const atx::u16 child_d = static_cast<atx::u16>(d[ii] + 1);
    for (const ExprId c : {e.a, e.b, e.c}) {
      if (c != kNoExpr && child_d < d[c]) {
        d[c] = child_d;
      }
    }
  }
  return d;
}

// ----- wrapper-op candidate set ---------------------------------------------

// A wrapper to apply: the resolved built-in OpSig* plus how its extra operand(s)
// are synthesized. `kind` selects the synthesis path at rebuild time.
enum class WrapOperand : atx::u8 {
  None,       // arity-1 wrapper (zscore, rank, winsorize-as-1): only child a.
  ScaleExp,   // signedpower(x, p): synthesize a Scale literal exponent as child b.
  GroupField, // group_neutralize/indneutralize(x, g): synthesize a Group field b.
};

struct WrapCandidate {
  const OpSig *op{nullptr};
  WrapOperand operand{WrapOperand::None};
};

// Resolve a built-in op by name from the static builtin table. The returned
// pointer aliases `builtin_ops()` (program lifetime) — exactly the rows the
// OpCatalog caches and the rows the run-wide Library copies from, so a genome
// carrying this pointer stays valid for the run (op_catalog.hpp SAFETY).
[[nodiscard]] const OpSig *resolve_builtin(std::string_view name) noexcept {
  for (const OpSig &row : alpha::detail::builtin_ops()) {
    if (row.name == name) {
      return &row;
    }
  }
  return nullptr;
}

// Build the wrapper candidate set in a FIXED order (zscore, signedpower, rank,
// winsorize, group_neutralize, indneutralize), filtered by the cfg toggles and
// (for the group wrappers) the presence of a group field. The order is fixed
// BEFORE any RNG draw so the seeded wrapper pick is replayable (F1).
[[nodiscard]] std::vector<WrapCandidate>
build_wrappers(const WrapCfg &cfg, bool have_group_field) {
  std::vector<WrapCandidate> w;
  auto add = [&](bool enabled, std::string_view name, WrapOperand operand) {
    if (!enabled) {
      return;
    }
    const OpSig *op = resolve_builtin(name);
    if (op != nullptr) {
      w.push_back(WrapCandidate{op, operand});
    }
  };
  add(cfg.wrap_zscore, "zscore", WrapOperand::None);
  add(cfg.wrap_signedpower, "signedpower", WrapOperand::ScaleExp);
  add(cfg.wrap_rank, "rank", WrapOperand::None);
  add(cfg.wrap_winsorize, "winsorize", WrapOperand::None);
  if (have_group_field) {
    add(cfg.wrap_group_neutralize, "group_neutralize", WrapOperand::GroupField);
    add(cfg.wrap_indneutralize, "indneutralize", WrapOperand::GroupField);
  }
  return w;
}

// ----- the wrap rebuild primitive -------------------------------------------
//
// Clone the whole tree from `src` into `dst`; at `target`, emit a NEW wrapper
// Call node whose child a is a deep copy of target's subtree, plus the
// synthesized extra operand(s) appended AFTER the subtree (so the arena stays
// children-before-parents). Mirrors crossover's splice_visit but INSERTS a level
// rather than replacing one. Returns the new root id in `dst`.
ExprId wrap_visit(const Ast &src, ExprId s, ExprId target, const WrapCandidate &wc,
                  const WrapCfg &cfg, std::string_view group_field, Ast &dst,
                  std::vector<ExprId> &memo) {
  if (memo[s] != kNoExpr) {
    return memo[s];
  }
  Expr e = src.node(s);
  const ExprId ca = e.a, cb = e.b, cc = e.c;
  if (ca != kNoExpr) {
    e.a = wrap_visit(src, ca, target, wc, cfg, group_field, dst, memo);
  }
  if (cb != kNoExpr) {
    e.b = wrap_visit(src, cb, target, wc, cfg, group_field, dst, memo);
  }
  if (cc != kNoExpr) {
    e.c = wrap_visit(src, cc, target, wc, cfg, group_field, dst, memo);
  }
  if (e.kind == Expr::Kind::Field || e.kind == Expr::Kind::Member) {
    e.name_id = dst.intern(src.field_name(e.name_id));
  }
  const ExprId inner = dst.add(e); // the (possibly rebuilt) target subtree
  memo[s] = inner;
  if (s != target) {
    return inner;
  }
  // Synthesize the wrapper's extra operand(s) AFTER the inner subtree so the
  // arena stays topologically ordered (children before the wrapper parent).
  Expr wrapper;
  wrapper.kind = Expr::Kind::Call;
  wrapper.op = wc.op;
  wrapper.opcode = wc.op->opcode;
  wrapper.n_hparams = wc.op->n_hparams; // wrapper set carries n_hparams == 0
  wrapper.a = inner;
  switch (wc.operand) {
  case WrapOperand::None:
    break;
  case WrapOperand::ScaleExp: {
    Expr lit;
    lit.kind = Expr::Kind::Literal;
    lit.value = cfg.signedpower_exp; // a Scale literal (jitterable later)
    wrapper.b = dst.add(lit);
    break;
  }
  case WrapOperand::GroupField: {
    Expr field;
    field.kind = Expr::Kind::Field;
    field.dollar = false;
    field.name_id = dst.intern(group_field);
    wrapper.b = dst.add(field);
    break;
  }
  }
  return dst.add(wrapper);
}

} // namespace (wrap_in_op helpers)

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

// =========================================================================
//  wrap_in_op — wrap a subtree in a conditioning op (Phase W1b).
// =========================================================================

[[nodiscard]] atx::core::Result<Genome>
wrap_in_op(const Genome &g, const OpCatalog & /*cat*/,
           std::span<const std::string_view> group_fields, Xoshiro256pp &rng, WrapCfg cfg) {
  const std::span<const Expr> arena = g.ast.nodes();
  const ExprId root = g.ast.roots().front().root;

  // (0) Pre-RNG depth measurement: a wrap inserts ONE level above the target's
  //     subtree. Reject (skip as a candidate) any node whose wrapped tree would
  //     exceed cfg.max_depth (analyze does NOT enforce depth — the operator does).
  const std::vector<atx::u16> sub_h = subtree_heights(arena);
  const std::vector<atx::u16> depth = node_depths(arena, root);
  constexpr atx::u16 kUnset = (std::numeric_limits<atx::u16>::max)();
  const int cap = (cfg.max_depth < 0) ? 0 : cfg.max_depth;

  // (1) Wrappable subtree candidates in CANONICAL ascending-ExprId order, BEFORE
  //     any RNG draw (F1/F2). A candidate is an F64-valued, non-record node whose
  //     wrapped tree stays within the depth cap.
  std::vector<ExprId> cands;
  for (atx::usize i = 0; i < arena.size(); ++i) {
    const alpha::TypeInfo &ti = g.analysis.info(static_cast<ExprId>(i));
    if (ti.dtype != DType::F64 || ti.is_record) {
      continue; // wrapper primaries consume an F64 (non-record) signal
    }
    if (depth[i] == kUnset) {
      continue; // unreachable from the root (defensive; should not happen)
    }
    const int wrapped_height = static_cast<int>(depth[i]) + 1 + static_cast<int>(sub_h[i]);
    if (wrapped_height > cap) {
      continue; // wrapping here would exceed the depth budget — skip
    }
    cands.push_back(static_cast<ExprId>(i));
  }
  if (cands.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "wrap_in_op: no wrappable subtree");
  }

  // (2) The wrapper candidate set (fixed order, gated by cfg + group-field
  //     presence). Established before the wrapper draw (F1).
  const bool have_group = !group_fields.empty();
  const std::vector<WrapCandidate> wrappers = build_wrappers(cfg, have_group);
  if (wrappers.empty()) {
    return atx::core::Err(atx::core::ErrorCode::NotFound, "wrap_in_op: no applicable wrapper");
  }

  // (3) Draw ONE target subtree, then ONE wrapper (two u64 words — fixed stream).
  const ExprId target = cands[detail::uniform_index(rng, cands.size())];
  const WrapCandidate wc = wrappers[detail::uniform_index(rng, wrappers.size())];
  const std::string_view group_field =
      have_group ? group_fields[detail::uniform_index(rng, group_fields.size())]
                 : std::string_view{};

  // (4) Rebuild a fresh Ast inserting the wrapper level at `target`.
  Ast dst;
  dst.reserve(arena.size() + 2); // + wrapper node + at most one synthesized operand
  std::vector<ExprId> memo(arena.size(), kNoExpr);
  const ExprId new_root = wrap_visit(g.ast, root, target, wc, cfg, group_field, dst, memo);
  dst.add_root(std::string{}, new_root);

  // (5) F5 validity oracle backstop — shape/dtype/causality. Invalid (e.g. a Cs
  //     wrapper over a scalar primary) -> Err, never a half-valid genome.
  return analyze_into(std::move(dst));
}

} // namespace atx::engine::factory

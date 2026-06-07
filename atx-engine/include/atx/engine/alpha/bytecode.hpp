#pragma once

// atx::engine::alpha — bytecode linearization (slot allocation + Free
// scheduling) (P3-4).
//
// `linearize` flattens a hash-consed `Dag` into a `Program`: a topologically
// ordered instruction stream over a small pool of recycled SlotIds, with a
// `Free` instruction emitted after each value's last consumer and a
// `StoreAlpha` per alpha root. `compile` is the build_dag + linearize combo.
//
// Public API:
//   Result<Program> linearize(const Dag&);
//   Result<Program> compile(const Ast&, const Analysis&);
//
// Linearization (Sethi-Ullman / refcount-driven liveness, research §5.3):
//   * Nodes emit in NodeId order — the DAG is already topological (a node only
//     references children with smaller NodeIds), so children are produced
//     before parents and `src` never forward-references.
//   * A free-list of SlotIds plus a high-water `next_slot` counter recycle
//     storage: acquire() pops a free slot or grows the pool; release() returns a
//     slot to the free-list. `num_slots` is the peak (high-water) -> the slot
//     pool the VM pre-sizes. Peak <= live node count.
//   * runtime `remaining[NodeId]` starts at each node's refcount. Emitting a
//     node decrements each child's `remaining`; at zero the child's slot is
//     `Free`d and recycled (exactly one Free per live node). A root target's
//     StoreAlpha is its "+1" consumer, decremented after the store — so a root
//     used nowhere else is stored then immediately freed.
//   * refcount-0 nodes (e.g. a strength-reduced-away Const(2)) are skipped:
//     never emitted, never allocated a slot.
//
// Ownership / lifetime: the Program owns its instruction vector, root list, and
// field dictionary by value. `linearize`/`compile` borrow inputs by const ref.
//
// Header-only; every free function is `inline`. Linearization is a COLD path.

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/dag.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atx::engine::alpha {

// Index of a value slot in the VM's runtime slot pool.
using SlotId = atx::u32;

// Sentinel for "no slot" in an instruction's unused operand positions.
inline constexpr SlotId kNoSlot = ~SlotId{0};

// =========================================================================
//  Instr — one linearized VM instruction.
//
//  `dst` is the slot the op writes (for Free, the slot being released — Free is
//  dst-only). `src` holds operand slots in order. `param` carries the LoadField
//  field id / Ts window immediate / StoreAlpha output index. `imm` carries a
//  Const's literal value (so the VM seeds the slot without a side table).
// =========================================================================

struct Instr {
  OpCode op{};
  SlotId dst{kNoSlot};
  std::array<SlotId, 3> src{kNoSlot, kNoSlot, kNoSlot};
  atx::u32 param{};              // LoadField id / Ts window / StoreAlpha out / Pin index
  atx::u8 n_out{1};              // output pins (>=2 for record compute nodes)
  std::array<atx::f64, 2> imm{}; // Const: imm[0]; filter hyperparams: imm[0],imm[1]
};

// =========================================================================
//  Program — the linearized output (owns code + roots + field dictionary).
// =========================================================================

struct Program {
  // One entry per alpha: its name and the output index its StoreAlpha writes.
  struct Root {
    std::string name;  // owned; empty for an anonymous root
    atx::u32 output{}; // StoreAlpha output index (== position in roots)
  };

  std::vector<Instr> code;
  std::vector<Root> roots;
  std::vector<std::string> fields; // field-name dictionary (NodeKey field ids)
  atx::u32 num_slots{};            // peak live slots == the SlotPool capacity
  atx::u16 required_lookback{};

  // CSE / liveness metrics (recorded by the linearizer for the sprint ledger).
  atx::u32 unique_nodes{};
  atx::u32 total_ast_nodes{};
  atx::u32 peak_live_slots{}; // == num_slots

  // Intern cache-hit telemetry, carried from the Dag (the cross-alpha CSE lever).
  // `cache_hits` = intern() calls that collapsed onto an existing node;
  // `intern_attempts` = total intern() calls. Invariant (held by Dag::intern):
  // `intern_attempts == cache_hits + unique_nodes`. `cache_hit_pct()` is the
  // share of lowered nodes deduplicated — non-zero exactly when alphas share a
  // sub-expression. Pure observability; no effect on the emitted code.
  atx::u32 cache_hits{};
  atx::u32 intern_attempts{};

  [[nodiscard]] double cache_hit_pct() const noexcept {
    return intern_attempts == 0
               ? 0.0
               : 100.0 * static_cast<double>(cache_hits) / static_cast<double>(intern_attempts);
  }
};

namespace detail {

// A reusable pool of SlotIds. acquire() reuses a freed slot or grows the pool;
// release() returns a slot to the free-list. `peak()` is the high-water mark =
// the maximum number of simultaneously-live slots (the VM's pool capacity).
class SlotPool {
public:
  [[nodiscard]] SlotId acquire() {
    if (!free_.empty()) {
      const SlotId s = free_.back();
      free_.pop_back();
      return s;
    }
    const auto s = next_slot_;
    ++next_slot_;
    return s;
  }

  void release(SlotId s) { free_.push_back(s); }

  // Acquire `k` CONTIGUOUS slots; returns the first. Reuses a freed same-size
  // block if available, else grows the high-water counter by k. k>=1.
  [[nodiscard]] SlotId acquire_block(atx::u32 k) {
    if (k == 1) {
      return acquire();
    }
    if (auto it = free_blocks_.find(k); it != free_blocks_.end() && !it->second.empty()) {
      const SlotId s = it->second.back();
      it->second.pop_back();
      return s;
    }
    const SlotId s = next_slot_;
    next_slot_ += k;
    return s;
  }

  void release_block(SlotId first, atx::u32 k) {
    if (k == 1) {
      release(first);
      return;
    }
    free_blocks_[k].push_back(first);
  }

  // High-water mark: total distinct slots ever handed out simultaneously. Since
  // a slot is reused only after release, `next_slot_` IS the peak live count.
  [[nodiscard]] atx::u32 peak() const noexcept { return next_slot_; }

private:
  std::vector<SlotId> free_;
  SlotId next_slot_{0};
  std::unordered_map<atx::u32, std::vector<SlotId>> free_blocks_;
};

// Number of populated child slots of a DAG node, by opcode arity in the graph
// (a node's `in` array is kNoNode-padded beyond its real children).
[[nodiscard]] inline atx::usize node_child_count(const Node &n) noexcept {
  atx::usize count = 0;
  for (const NodeId c : n.in) {
    if (c != kNoNode) {
      ++count;
    }
  }
  return count;
}

// Decrement a child's remaining-consumer count; when it hits zero emit a Free
// for its slot and recycle it. `slot[child]` must already be valid.
inline void retire_consumer(NodeId child, std::vector<atx::u32> &remaining,
                            std::vector<SlotId> &slot, std::vector<Instr> &code, SlotPool &pool) {
  // SAFETY: every consumer edge is counted in refcount and decremented exactly
  // once, so `remaining` can never underflow below zero before reaching it.
  --remaining[child];
  if (remaining[child] == 0) {
    Instr fr;
    fr.op = OpCode::Free;
    fr.dst = slot[child];
    code.push_back(fr);
    pool.release(slot[child]);
    slot[child] = kNoSlot;
  }
}

} // namespace detail

// =========================================================================
//  linearize — flatten a Dag into a Program.
// =========================================================================

// SAFETY: the DAG is topologically ordered (NodeId = arena index; a node only
// references children with smaller ids — see build_dag). Emitting in NodeId
// order therefore guarantees every `src` slot was produced by an earlier instr.
[[nodiscard]] inline atx::core::Result<Program> linearize(const Dag &dag) {
  const std::span<const Node> nodes = dag.nodes();

  Program prog;
  prog.required_lookback = dag.required_lookback();
  prog.unique_nodes = dag.unique_nodes();
  prog.total_ast_nodes = dag.total_ast_nodes();
  prog.cache_hits = dag.cache_hits();
  prog.intern_attempts = dag.intern_attempts();
  prog.fields.assign(dag.fields().begin(), dag.fields().end());
  prog.code.reserve(nodes.size() * 2); // node emit + its Free, roughly

  // Per-node runtime state: the slot currently holding the node's value, and
  // the consumers still to come (counts down from refcount to zero -> Free).
  std::vector<SlotId> slot(nodes.size(), kNoSlot);
  std::vector<atx::u32> remaining(nodes.size(), 0);
  for (atx::usize i = 0; i < nodes.size(); ++i) {
    remaining[i] = nodes[i].refcount;
  }

  // A node may be the target of MORE THAN ONE root: two alphas with identical
  // expressions CSE to the same node. Record EVERY output index per node so we
  // emit one StoreAlpha per root, matching the +1-per-root refcount build_dag
  // accumulates (a single shared store would drop outputs and leak the slot).
  std::vector<std::vector<atx::u32>> node_outputs(nodes.size());
  const std::span<const Dag::Root> dag_roots = dag.roots();
  for (atx::usize r = 0; r < dag_roots.size(); ++r) {
    const NodeId t = dag_roots[r].node;
    if (t != kNoNode) {
      node_outputs[t].push_back(static_cast<atx::u32>(r));
    }
    prog.roots.push_back(Program::Root{dag_roots[r].name, static_cast<atx::u32>(r)});
  }

  detail::SlotPool pool;

  for (atx::usize i = 0; i < nodes.size(); ++i) {
    const Node &n = nodes[i];
    // Skip dead nodes (refcount 0, e.g. a strength-reduced-away Const): they are
    // unreachable from every root and must not be emitted nor allocated a slot.
    if (n.refcount == 0) {
      continue;
    }

    const SlotId dst = pool.acquire();
    slot[i] = dst;

    Instr instr;
    instr.op = n.op;
    instr.dst = dst;
    const atx::usize nkids = detail::node_child_count(n);
    for (atx::usize k = 0; k < nkids; ++k) {
      instr.src.at(k) = slot[n.in.at(k)];
    }
    if (n.op == OpCode::LoadField) {
      instr.param = n.param;
    } else if (n.op == OpCode::Const) {
      instr.imm[0] = n.value;
    }
    instr.n_out = n.n_out;
    prog.code.push_back(instr);

    // Retire each child edge of this node (Mul(x,x) retires x twice — its two
    // edges are two separate consumers and both must count down `remaining`).
    for (atx::usize k = 0; k < nkids; ++k) {
      detail::retire_consumer(n.in.at(k), remaining, slot, prog.code, pool);
    }

    // Emit one StoreAlpha per root targeting this node (the "+1" consumers),
    // retiring each after its store. Reading slot[i] each iteration keeps the
    // store sourced from the live slot; the final retire frees it once no reader
    // (interior edge or store) remains. A node that is BOTH a root and an
    // interior child is thus stored then freed only after its last consumer.
    for (const atx::u32 out : node_outputs.at(i)) {
      Instr store;
      store.op = OpCode::StoreAlpha;
      store.dst = kNoSlot;
      store.src[0] = slot[i];
      store.param = out;
      prog.code.push_back(store);
      detail::retire_consumer(static_cast<NodeId>(i), remaining, slot, prog.code, pool);
    }
  }

  prog.num_slots = pool.peak();
  prog.peak_live_slots = pool.peak();
  // Peak live slots can never exceed the number of live nodes interned.
  if (prog.num_slots > nodes.size()) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "linearize: peak live slots exceeded node count (allocator bug)");
  }
  return atx::core::Ok(std::move(prog));
}

// =========================================================================
//  compile — build_dag then linearize.
// =========================================================================

[[nodiscard]] inline atx::core::Result<Program> compile(const Ast &ast, const Analysis &analysis) {
  ATX_TRY(const Dag dag, build_dag(ast, analysis));
  return linearize(dag);
}

// =========================================================================
//  compile_batch — compile N alpha sources into ONE cross-alpha-CSE Program.
// =========================================================================
//
// A thin convenience over the existing pipeline: join the sources one-per-line
// into a single `program` (`{ assignment }`), then parse_program -> analyze ->
// compile. Because every alpha lands in ONE hash-consed Dag, sub-expressions
// shared ACROSS alphas (e.g. two alphas both referencing `rank(close)`) collapse
// to one node — that cross-alpha CSE is exactly what `Program::unique_nodes <
// total_ast_nodes` and `cache_hits` quantify.
//
// This adds NO evaluation logic: `Engine(panel).evaluate(program)` is ALREADY
// the batch path — it returns one `SignalSet::Alpha` per root (per input source),
// so no separate `evaluate_batch` is needed. The roots preserve submission order
// (parse_program appends one root per assignment in order), so `prog.roots[i]`
// and the resulting `SignalSet.alphas[i]` correspond to `alpha_srcs[i]`.
//
// Each `alpha_srcs[i]` must be a single bare expression (no embedded newline /
// `=`); it is auto-named `aN` (its 0-based index) so anonymous batch entries are
// still distinguishable in the SignalSet. A malformed source surfaces as
// Err(ParseError)/Err(InvalidArgument) from the underlying stages — never a
// throw. An empty span yields an empty (zero-root) Program.
//
// CONTRACT ENFORCEMENT (make illegal states unrepresentable): the lexer treats
// '\n' as ordinary whitespace and `parse_program` delimits assignments purely by
// the `IDENT '='` pattern, so a source containing an embedded `IDENT =` (e.g.
// "close\nfoo = open") would lex into a SECOND assignment — injecting an extra
// root from ONE input and silently breaking the `roots[i] <-> alpha_srcs[i]`
// 1:1 mapping the SignalSet correspondence depends on. We close that hole two
// ways: (a) reject any source containing '\n' up front (a batch entry is one
// expression, never a statement); (b) defensively assert post-compile that
// `roots.size() == alpha_srcs.size()` — catching ANY desync regardless of cause.
//
// COLD path (compile-time); std::string assembly is fine (zero-alloc is a VM
// hot-path concern only).
[[nodiscard]] inline atx::core::Result<Program>
compile_batch(std::span<const std::string_view> alpha_srcs, const Library &lib) {
  std::string program;
  for (atx::usize i = 0; i < alpha_srcs.size(); ++i) {
    if (alpha_srcs[i].find('\n') != std::string_view::npos) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "compile_batch: alpha source " + std::to_string(i) +
                                " contains an embedded newline (a batch entry must be a single "
                                "expression, not a statement)");
    }
    program += 'a';
    program += std::to_string(i);
    program += " = ";
    program.append(alpha_srcs[i].data(), alpha_srcs[i].size());
    program += '\n';
  }
  ATX_TRY(const Ast ast, parse_program(program, lib));
  ATX_TRY(const Analysis analysis, analyze(ast));
  ATX_TRY(Program prog, compile(ast, analysis));
  // Defensive 1:1 invariant: one root per input source. An embedded assignment
  // (or any other merge/injection) that slipped past the newline guard would
  // desync this — fail loud rather than corrupt the roots[i] <-> src[i] mapping.
  if (prog.roots.size() != alpha_srcs.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "compile_batch: a source injected/merged roots (embedded assignment?) — "
                          "root count does not match the input source count");
  }
  return atx::core::Ok(std::move(prog));
}

} // namespace atx::engine::alpha

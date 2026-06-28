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
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
// for its slot (block) and recycle it. `slot[child]` must already be valid.
// `child_n_out` is the number of output slots the child node occupies (1 for
// all single-output nodes; >= 2 for multi-output record nodes such as Split2).
// A Free instr carries `n_out` so the VM knows the block width.
inline void retire_consumer(NodeId child, atx::u8 child_n_out, std::vector<atx::u32> &remaining,
                            std::vector<SlotId> &slot, std::vector<Instr> &code, SlotPool &pool) {
  // SAFETY: every consumer edge is counted in refcount and decremented exactly
  // once, so `remaining` can never underflow below zero before reaching it.
  --remaining[child];
  if (remaining[child] == 0) {
    Instr fr;
    fr.op = OpCode::Free;
    fr.dst = slot[child];
    fr.n_out = child_n_out; // carried for tooling/debug; runtime uses
                            // 1-acquire/1-release per instr, so the VM ignores
                            // this on Free
    code.push_back(fr);
    pool.release_block(slot[child], child_n_out);
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
[[nodiscard]] atx::core::Result<Program> linearize(const Dag &dag);

// =========================================================================
//  compile — build_dag then linearize.
// =========================================================================

[[nodiscard]] inline atx::core::Result<Program> compile(const Ast &ast, const Analysis &analysis) {
  ATX_TRY(const Dag dag, build_dag(ast, analysis));
  return linearize(dag);
}

// =========================================================================
//  Compile memoization cache (S1-0).
//
//  Goal: a repeated AST returns its stored Program WITHOUT re-running the
//  compile pipeline (build_dag + linearize). The cached Program is
//  instruction-stream-identical to a fresh cold compile.
//
//  Where the cost is:
//    build_dag (dag.hpp) is the named primary bottleneck — it dominates a
//    cold compile (~90%); linearize is the cheap tail. A cache that still
//    runs build_dag on every call only saves the tail and is nearly useless.
//    Therefore the key is computed DIRECTLY FROM THE Ast, BEFORE build_dag,
//    and a verified hit returns the stored Program with NO build_dag at all.
//
//  Cache key design (keyed on the Ast, not the Dag):
//    The Ast is a flat arena of Expr nodes in topological order (children
//    precede parents). For each Expr we capture its full structural content:
//    kind, opcode, the resolved op name (Call), the literal value bits
//    (Literal), the resolved field/pin name string (Field/Member), the three
//    child ExprIds, and the peeled hparams. Field/Member names are captured as
//    STRINGS (via ast.field_name) rather than raw name_id indices, because
//    name_id indexes a per-Ast string pool — two Asts may map the same id to
//    different names. The Const literal lives in Expr::value, so ts_mean(x,5)
//    and ts_mean(x,10) differ naturally (their window literal Expr differs).
//    Root names + root ExprIds complete the key.
//
//  Correctness — does the compile depend on anything besides the Ast?
//    compile(ast, analysis) calls build_dag(ast, analysis). `analysis` is the
//    output of analyze(const Ast&) — a PURE, deterministic function of the Ast
//    alone (no second input, no global mutable state, no runtime/panel
//    dependency; every TypeInfo is derived from the Ast node's kind/opcode/
//    operands/folded literals). Therefore the Ast fully determines the
//    Program, and an Ast-only key is sufficient AND correct. The documented
//    precondition (analysis == analyze(ast)) is the contract that makes this
//    hold; we do not key on `analysis` because it carries no information the
//    Ast does not already carry.
//
//  Collision safety (mandatory for correctness):
//    The map is keyed by the AstCacheKey itself (unordered_map<AstCacheKey,
//    Program>), so hash collisions chain in buckets and the map's own equality
//    comparison (AstCacheKey::operator==) runs on every lookup. A true
//    structural match returns the stored Program; a hash-only collision with a
//    different Ast simply misses and cold-compiles. Hash-only acceptance is
//    impossible by construction.
//
//  Thread safety:
//    The cache is thread_local. Each worker thread owns its own map; there
//    is no shared mutable state and no synchronisation needed.
//    SAFETY: the eval path is documented as single-threaded per worker (see
//    vm.hpp "Engine is single-owner per worker"). A thread_local cache is
//    therefore the simplest correct choice — no mutex, no atomic, no false
//    sharing across cores.
// =========================================================================

namespace detail {

// One Ast node's structural fingerprint (a flattened, name-resolved Expr).
// All fields are captured so two structurally-equal Asts produce equal entries
// and any structural difference produces an inequality.
struct AstNodeKey {
  atx::u8 kind{};            // Expr::Kind
  atx::u8 opcode{};          // Unary/Binary opcode (0 otherwise)
  atx::u64 value_bits{};     // Literal: bit pattern of Expr::value (0 otherwise)
  std::string name;          // Field/Member: resolved name; Call: op->name (else "")
  bool dollar{};             // Field: was it `$name`?
  atx::u32 a{};              // child ExprIds (kNoExpr sentinel preserved)
  atx::u32 b{};
  atx::u32 c{};
  atx::u64 hparam_bits0{};   // Call: bit pattern of hparams[0]
  atx::u64 hparam_bits1{};   // Call: bit pattern of hparams[1]
  atx::u8 n_hparams{};       // Call: peeled hparam count

  [[nodiscard]] bool operator==(const AstNodeKey &) const = default;
};

// Structural key for the compile cache, derived from the Ast. The map keys on
// this directly, so equality (operator==) is enforced on EVERY lookup — hash
// collisions cannot leak a wrong Program.
struct AstCacheKey {
  std::vector<AstNodeKey> nodes;       // one per Ast Expr, arena order
  std::vector<atx::u32> root_ids;      // root ExprIds
  std::vector<std::string> root_names; // root alpha names

  [[nodiscard]] bool operator==(const AstCacheKey &o) const {
    return nodes == o.nodes && root_ids == o.root_ids && root_names == o.root_names;
  }
};

// Hash for AstCacheKey. Folds every field of every node + roots through the
// golden-ratio mix. Order-sensitive throughout.
// SAFETY: all scalar fields are trivially hashable; std::hash<std::string> is
// noexcept on the supported toolchains; the fold is pure arithmetic.
struct AstCacheKeyHash {
  [[nodiscard]] std::size_t operator()(const AstCacheKey &k) const noexcept {
    std::size_t h{0};
    const std::size_t mix_const{0x9e3779b97f4a7c15ULL};
    auto mix = [&](std::size_t v) noexcept {
      h ^= v + mix_const + (h << 6U) + (h >> 2U);
    };
    for (const AstNodeKey &n : k.nodes) {
      mix(std::hash<atx::u8>{}(n.kind));
      mix(std::hash<atx::u8>{}(n.opcode));
      mix(std::hash<atx::u64>{}(n.value_bits));
      mix(std::hash<std::string>{}(n.name));
      mix(std::hash<bool>{}(n.dollar));
      mix(std::hash<atx::u32>{}(n.a));
      mix(std::hash<atx::u32>{}(n.b));
      mix(std::hash<atx::u32>{}(n.c));
      mix(std::hash<atx::u64>{}(n.hparam_bits0));
      mix(std::hash<atx::u64>{}(n.hparam_bits1));
      mix(std::hash<atx::u8>{}(n.n_hparams));
    }
    for (const atx::u32 id : k.root_ids) {
      mix(std::hash<atx::u32>{}(id));
    }
    for (const std::string &nm : k.root_names) {
      mix(std::hash<std::string>{}(nm));
    }
    return h;
  }
};

// Build an AstCacheKey from a parsed Ast — WITHOUT build_dag. This is the work
// done on every compile_cached call; it must be cheaper than build_dag (it is:
// one linear pass over the arena, no hash-consing, no allocation per node
// beyond the string copies for named leaves).
//
// SAFETY: sizeof(f64)==sizeof(u64) is checked by static_assert. memcpy is the
// defined way to read floating-point bits as an integer without type-punning
// UB; std::bit_cast would be equally correct.
[[nodiscard]] inline AstCacheKey make_ast_key(const Ast &ast) {
  static_assert(sizeof(atx::f64) == sizeof(atx::u64),
                "f64/u64 size mismatch — bit-cast assumption violated");
  AstCacheKey k;
  const auto nodes = ast.nodes();
  k.nodes.reserve(nodes.size());
  for (const Expr &e : nodes) {
    AstNodeKey nk;
    nk.kind   = static_cast<atx::u8>(e.kind);
    nk.opcode = static_cast<atx::u8>(e.opcode);
    std::memcpy(&nk.value_bits, &e.value, sizeof(atx::u64));
    // Resolve Field/Member names to strings (name_id indexes a per-Ast pool);
    // capture the Call's op identity by its registry name (stable across Asts).
    if (e.kind == Expr::Kind::Field || e.kind == Expr::Kind::Member) {
      nk.name = std::string(ast.field_name(e.name_id));
    } else if (e.kind == Expr::Kind::Call && e.op != nullptr) {
      nk.name = std::string(e.op->name);
    }
    nk.dollar = e.dollar;
    nk.a = static_cast<atx::u32>(e.a);
    nk.b = static_cast<atx::u32>(e.b);
    nk.c = static_cast<atx::u32>(e.c);
    std::memcpy(&nk.hparam_bits0, &e.hparams[0], sizeof(atx::u64));
    std::memcpy(&nk.hparam_bits1, &e.hparams[1], sizeof(atx::u64));
    nk.n_hparams = e.n_hparams;
    k.nodes.push_back(std::move(nk));
  }
  k.root_ids.reserve(ast.roots().size());
  k.root_names.reserve(ast.roots().size());
  for (const Assignment &r : ast.roots()) {
    k.root_ids.push_back(static_cast<atx::u32>(r.root));
    k.root_names.push_back(r.name);
  }
  return k;
}

// Process-local, thread-local compile cache: AstCacheKey -> Program.
// Keyed by the structural Ast key directly, so the map's bucket-chaining +
// AstCacheKey::operator== provide collision safety natively (no single-slot
// eviction, no manual equality dance). thread_local: each worker owns its map.
// SAFETY: see module-level comment above.
using CompileCache = std::unordered_map<AstCacheKey, Program, AstCacheKeyHash>;
[[nodiscard]] inline CompileCache &compile_cache() noexcept {
  thread_local CompileCache cache;
  return cache;
}

} // namespace detail

// =========================================================================
//  compile_cached — memoized compile keyed on the Ast (skips build_dag on hit).
//
//  Identical API to compile(). Returns an instruction-stream-identical Program
//  to a cold compile (telemetry fields may differ — see Program). Transparent
//  to call sites: switch from compile() to compile_cached() with no other
//  change.
//
//  Cache hit path (fast):  O(ast key build + map lookup) — NO build_dag.
//  Cache miss path (cold): O(ast key build + build_dag + linearize), store.
//
//  @pre  `analysis` is the result of analyze(ast) for the same ast (the
//        contract that lets an Ast-only key be sufficient — analyze is a pure
//        function of ast, see the module-level "Correctness" note above).
//  @post returned Program's instruction stream / slot layout is identical to
//        compile(ast, analysis).
// =========================================================================

[[nodiscard]] inline atx::core::Result<Program>
compile_cached(const Ast &ast, const Analysis &analysis) {
  // Build the structural key from the Ast directly — cheap, no build_dag.
  detail::AstCacheKey key = detail::make_ast_key(ast);

  detail::CompileCache &cache = detail::compile_cache();
  if (const auto it = cache.find(key); it != cache.end()) {
    // True structural hit (map equality already ran). Return a Program copy
    // WITHOUT touching build_dag — that is the whole point of the cache.
    return atx::core::Ok(it->second);
  }

  // Miss: pay the full cold compile once, then memoize.
  ATX_TRY(const Dag dag, build_dag(ast, analysis));
  ATX_TRY(Program prog, linearize(dag));
  cache.emplace(std::move(key), prog);
  return atx::core::Ok(std::move(prog));
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
[[nodiscard]] atx::core::Result<Program>
compile_batch(std::span<const std::string_view> alpha_srcs, const Library &lib);

} // namespace atx::engine::alpha

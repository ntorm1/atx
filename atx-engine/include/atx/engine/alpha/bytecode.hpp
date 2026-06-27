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
//  Goal: a repeated AST structure (same DAG topology) returns its stored
//  Program without re-running build_dag + linearize. The cached Program is
//  byte-identical to a fresh cold compile (the linearizer is deterministic).
//
//  Cache key design:
//    NodeIds in a freshly-built Dag are arena-local indices that restart at 0
//    for every build_dag() call. Two structurally DIFFERENT programs can share
//    the same root NodeId if they happen to have the same number of nodes
//    (e.g. `close - open` and `close / open` both produce root NodeId=2 with
//    two LoadField leaves). Using raw NodeIds as the key is therefore WRONG.
//
//    Instead the key is derived from the CONTENT of each node: for every node
//    in topological order we record its NodeKey (opcode, param, children[3],
//    imm_bits[2]). Children are NodeIds — but they are into the SAME Dag
//    arena, so two Dags built from the same AST produce identical NodeKey
//    sequences; two Dags from different ASTs (even same node count) differ
//    because the opcodes and/or params differ. Together with the root NodeIds
//    (positions in the sorted node list) and field name dictionary this forms
//    a canonical structural fingerprint that is stable and cross-instance.
//
//  Collision safety (mandatory for correctness):
//    The key is stored WITH the cached Program. On a hash-map lookup hit we
//    perform a FULL key equality check before returning the cached Program.
//    Two distinct ASTs that hash-collide will pass the hash lookup but FAIL
//    the equality check and fall through to a cold compile. Hash-only lookup
//    is explicitly forbidden (see brief §Fix, "Collision safety is mandatory").
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

// Structural key for the compile cache. Equality-checked (not hash-only) on
// every lookup so hash collisions cannot cause correctness bugs.
//
// node_keys: NodeKey for each node in topological order — encodes the full
//   DAG structure (opcode, param, children, imm_bits). Two Dags from the same
//   AST produce identical sequences; different ASTs (even same node count)
//   differ in at least one opcode or param.
// root_ids: NodeId of each Dag root (position in nodes array). Needed to
//   distinguish programs with the same nodes but different output roots.
// root_names: Name of each root alpha. Identifies which alpha is which.
// fields: Field name dictionary (field-id → name). Distinguishes programs
//   that reference different fields but happen to share the same topology.
struct CompileCacheKey {
  std::vector<NodeKey> node_keys;
  std::vector<NodeId> root_ids;
  std::vector<std::string> root_names;
  std::vector<std::string> fields;

  [[nodiscard]] bool operator==(const CompileCacheKey &o) const noexcept {
    return node_keys == o.node_keys && root_ids == o.root_ids &&
           root_names == o.root_names && fields == o.fields;
  }
};

// Hash for CompileCacheKey. Folds all NodeKey fields + root ids + root names +
// field names through the golden-ratio mix. Order-sensitive throughout.
// SAFETY: OpCode is u8-backed enum; all scalar fields are trivially hashable;
// no padding is read — each field is hashed explicitly via std::hash.
struct CompileCacheKeyHash {
  [[nodiscard]] std::size_t operator()(const CompileCacheKey &k) const noexcept {
    std::size_t h{0};
    const std::size_t mix_const{0x9e3779b97f4a7c15ULL};
    auto mix = [&](std::size_t v) noexcept {
      h ^= v + mix_const + (h << 6U) + (h >> 2U);
    };
    // Hash each NodeKey: op, param, children[3], imm_bits[2].
    for (const NodeKey &nk : k.node_keys) {
      mix(std::hash<atx::u8>{}(static_cast<atx::u8>(nk.op)));
      mix(std::hash<atx::u64>{}(nk.param));
      for (const NodeId c : nk.children) {
        mix(std::hash<atx::u32>{}(c));
      }
      for (const atx::u64 ib : nk.imm_bits) {
        mix(std::hash<atx::u64>{}(ib));
      }
    }
    // Hash root positions and names.
    for (const NodeId id : k.root_ids) {
      mix(std::hash<atx::u32>{}(id));
    }
    for (const std::string &n : k.root_names) {
      mix(std::hash<std::string>{}(n));
    }
    // Hash field dictionary.
    for (const std::string &f : k.fields) {
      mix(std::hash<std::string>{}(f));
    }
    return h;
  }
};

// One entry in the compile cache: the key (for full equality re-check) and
// the owned Program (returned by value on a hit — Programs are cheap to copy
// relative to a full re-compile; they are small flat vectors).
struct CompileCacheEntry {
  CompileCacheKey key;
  Program program;
};

// Build a CompileCacheKey from a fully-built Dag.
//
// Reconstructs each node's NodeKey from its content fields. The Dag does not
// expose its cons table after build, so we reconstruct each NodeKey manually:
//
//   NodeKey::param is op-specific:
//     * Const     → bit_cast<u64>(n.value)   (the literal value, per dag.cpp)
//     * LoadField → static_cast<u64>(n.param) (field dictionary id)
//     * Pin       → static_cast<u64>(n.param) (pin index)
//     * all else  → 0                         (param unused; n.param == 0)
//
// SAFETY: sizeof(f64)==sizeof(u64) is checked by static_assert. memcpy is
// the defined way to reinterpret floating-point bits without UB (std::bit_cast
// is equally correct but requires C++20 and a consteval context; memcpy works
// in all non-consteval inline contexts here).
[[nodiscard]] inline CompileCacheKey make_cache_key(const Dag &dag) {
  static_assert(sizeof(atx::f64) == sizeof(atx::u64),
                "f64/u64 size mismatch — bit-cast assumption violated");
  CompileCacheKey k;
  // Reconstruct NodeKey for each node from its stored content.
  const auto nodes = dag.nodes();
  k.node_keys.reserve(nodes.size());
  for (const Node &n : nodes) {
    NodeKey nk;
    nk.op       = n.op;
    // Const nodes store their literal in n.value; LoadField/Pin store their id
    // in n.param. All other ops have param==0 in both Node and NodeKey.
    if (n.op == OpCode::Const) {
      std::memcpy(&nk.param, &n.value, sizeof(atx::u64));
    } else {
      nk.param = static_cast<atx::u64>(n.param);
    }
    nk.children = n.in;
    // hparams are f64 in Node, u64 (bit-cast) in NodeKey. Reconstruct via
    // memcpy to avoid type-punning UB (same pattern as dag.cpp line 79-80).
    std::memcpy(&nk.imm_bits[0], &n.hparams[0], sizeof(atx::u64));
    std::memcpy(&nk.imm_bits[1], &n.hparams[1], sizeof(atx::u64));
    k.node_keys.push_back(nk);
  }
  // Root positions and names.
  k.root_ids.reserve(dag.roots().size());
  k.root_names.reserve(dag.roots().size());
  for (const Dag::Root &r : dag.roots()) {
    k.root_ids.push_back(r.node);
    k.root_names.emplace_back(r.name);
  }
  // Field name dictionary.
  for (const std::string &f : dag.fields()) {
    k.fields.emplace_back(f);
  }
  return k;
}

// Process-local, thread-local compile cache. Maps hash → cache entry.
// thread_local: each worker thread owns its own map (no shared mutable state).
// SAFETY: see module-level comment above.
using CompileCache = std::unordered_map<std::size_t, CompileCacheEntry, std::identity>;
[[nodiscard]] inline CompileCache &compile_cache() noexcept {
  thread_local CompileCache cache;
  return cache;
}

} // namespace detail

// =========================================================================
//  compile_cached — build_dag + linearize with process-local memoization.
//
//  Identical API to compile(). Returns a byte-identical Program to what a
//  cold compile would produce. Transparent to call sites: switch from
//  compile() to compile_cached() with no other change.
//
//  Cache hit path (fast):  O(key hash + key equality check).
//  Cache miss path (cold): O(build_dag + linearize), then store.
//
//  @pre  `analysis` is the result of analyze(ast) for the same ast.
//  @post returned Program is byte-identical to compile(ast, analysis).
// =========================================================================

[[nodiscard]] inline atx::core::Result<Program>
compile_cached(const Ast &ast, const Analysis &analysis) {
  // Always build the DAG — it is needed for the cache key AND for the cold
  // path. build_dag is fast relative to the linearizer for typical programs.
  ATX_TRY(const Dag dag, build_dag(ast, analysis));

  const detail::CompileCacheKey key = detail::make_cache_key(dag);
  const std::size_t h = detail::CompileCacheKeyHash{}(key);

  detail::CompileCache &cache = detail::compile_cache();
  if (const auto it = cache.find(h); it != cache.end()) {
    // Hash hit: perform full key equality check before accepting the entry.
    // This is the mandatory collision-safety guard (see brief §Fix).
    if (it->second.key == key) {
      // True cache hit: return a copy of the stored Program.
      return atx::core::Ok(it->second.program);
    }
    // Hash collision with a DIFFERENT key: fall through to cold compile.
    // The existing entry is evicted and replaced (single-bucket policy — a
    // two-AST collision in one run is a negligible edge case).
  }

  // Cold path: linearize and store.
  ATX_TRY(Program prog, linearize(dag));
  cache.insert_or_assign(h, detail::CompileCacheEntry{key, prog});
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

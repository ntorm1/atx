#pragma once

// atx::engine::alpha — hash-consed expression DAG (free CSE) (P3-4).
//
// `build_dag` lowers a typed `Ast` (one or many assignment roots) into a single
// global directed-acyclic expression graph. Structurally identical computations
// — same OpCode, same immediate parameter, same child NodeIds — are interned to
// ONE `Node` (common-subexpression elimination falls out of the hash-cons for
// free). The DAG is the input the bytecode linearizer (bytecode.hpp) turns into
// a flat instruction stream.
//
// Public API:
//   Result<Dag> build_dag(const Ast&, const Analysis&);
//       Interns every Ast node bottom-up (the arena is already topologically
//       ordered — children precede parents), accumulating consumer refcounts and
//       applying `pow(x,2) -> x*x` strength reduction. Pure; never throws.
//
// Why hash-consing here:
//   * Two alphas that share a sub-expression (e.g. both reference
//     `ts_mean(close,5)`) collapse to one node — computed once, freed once.
//   * `Mul(x,x)` references `x` twice; the DAG counts EDGES, not distinct
//     children, so `x`'s refcount is 2 and the linearizer frees it only after
//     both reads. Refcounts are the liveness signal the slot allocator drives.
//
// Ownership / lifetime:
//   * The Dag owns all nodes (a flat vector; NodeId = index) and the field-name
//     dictionary. It borrows nothing from the Ast/Analysis once built.
//   * `build_dag` borrows the Ast + Analysis by const ref for the call only.
//
// Header-only; every free function is `inline`. Building a DAG is a COLD path
// (once per compiled program), so std::vector / hash-map allocation is fine —
// zero-alloc is a VM hot-path concern, not a compile concern.

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/container/hash_map.hpp"
#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp"

#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

namespace atx::engine::alpha {

// Index of a node in the DAG (= position in Dag::nodes()). u32 is ample.
using NodeId = atx::u32;

// Sentinel for "no child" in a node's fixed-arity child slots.
inline constexpr NodeId kNoNode = ~NodeId{0};

// =========================================================================
//  NodeKey — the structural identity used for hash-consing.
//
//  Two computations are the SAME iff their opcode, immediate parameter, and
//  ordered child NodeIds all match. Because children are already-interned
//  NodeIds, structural equality of a key implies value-equality of the whole
//  sub-DAG it roots — that is what makes the cons table a free CSE pass.
//
//  `param` disambiguates leaves and window-carrying ops that share an opcode:
//    * LoadField — the field-dictionary id (so `close` != `open`).
//    * Const     — bit_cast<u64> of the f64 literal (so 1.0 != 2.0, and -0.0
//                  vs +0.0 stay distinct by their bit pattern; that is fine —
//                  it never *merges* unlike values, only ever fails to merge
//                  bit-distinct ones, which is conservative and correct).
//    * Everything else — 0.
// =========================================================================

struct NodeKey {
  OpCode op{};
  atx::u64 param{};
  std::array<NodeId, 3> children{kNoNode, kNoNode, kNoNode};
  std::array<atx::u64, 2> imm_bits{}; // bit-cast hparams for CSE identity

  // Memberwise structural equality. std::array compares elementwise; the
  // defaulted comparison is exactly the hash-cons identity we want.
  [[nodiscard]] bool operator==(const NodeKey &) const noexcept = default;
};

// =========================================================================
//  Node — one interned computation in the DAG.
//
//  Carries the lowered opcode, the analysis-derived type (shape/dtype/lookback,
//  copied from the originating Ast node), its child NodeIds, the literal value
//  for a Const (so the VM can seed the slot), the field/window immediate, and
//  the consumer refcount the linearizer uses for liveness.
// =========================================================================

struct Node {
  OpCode op{};
  Shape shape{};
  DType dtype{};
  atx::u16 lookback{};
  std::array<NodeId, 3> in{kNoNode, kNoNode, kNoNode};
  atx::f64 value{};                  // Const: the literal value
  std::array<atx::f64, 2> hparams{}; // filter hyperparameters (e.g. upper/lower thresholds)
  atx::u32 param{};                  // LoadField: field id (or Ts window immediate, future)
  atx::u8 n_out{1};                  // output pins (>=2 for record compute nodes)
  atx::u32 refcount{};               // consumers: one per parent EDGE + one per root store
};

namespace detail {

// Hash functor for NodeKey: fold the opcode, param, and three children through
// atx::core::hash_combine (order-sensitive, so child order matters — correct,
// since operand order is semantically significant for non-commutative ops).
//
// SAFETY: OpCode is a scoped enum with underlying type u8; we hash its integer
// value via std::to_underlying-equivalent cast. NodeKey is a plain aggregate of
// trivially-hashable scalars — no padding is ever read (every field is hashed
// explicitly, never the raw object representation).
struct NodeKeyHash {
  [[nodiscard]] atx::usize operator()(const NodeKey &k) const noexcept {
    const auto op_val = static_cast<atx::u8>(k.op);
    return atx::core::hash_combine(atx::usize{0}, op_val, k.param, k.children[0], k.children[1],
                                   k.children[2], k.imm_bits[0], k.imm_bits[1]);
  }
};

} // namespace detail

// =========================================================================
//  Dag — the hash-consed expression graph (owns nodes + field dictionary).
// =========================================================================

class Dag {
public:
  // One DAG root per Ast assignment: the interned target node + the alpha name.
  struct Root {
    std::string name; // owned; empty for an anonymous (parse_expr) root
    NodeId node{kNoNode};
  };

  // ---- accessors --------------------------------------------------------

  [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_; }
  [[nodiscard]] const Node &node(NodeId id) const noexcept { return nodes_[id]; }
  [[nodiscard]] std::span<const Root> roots() const noexcept { return roots_; }
  [[nodiscard]] std::span<const std::string> fields() const noexcept { return fields_; }

  // CSE metric: unique interned nodes vs. the total Ast nodes fed in. A program
  // whose alphas share sub-expressions has `unique_nodes() < total_ast_nodes()`.
  [[nodiscard]] atx::u32 unique_nodes() const noexcept {
    return static_cast<atx::u32>(nodes_.size());
  }
  [[nodiscard]] atx::u32 total_ast_nodes() const noexcept { return total_ast_nodes_; }
  [[nodiscard]] atx::u16 required_lookback() const noexcept { return required_lookback_; }

  // Intern cache-hit telemetry (the cross-alpha CSE lever, made reportable).
  // `intern_attempts()` counts every intern() call (one per lowered Ast node);
  // `cache_hits()` counts those that resolved to an ALREADY-interned node — i.e.
  // a common sub-expression collapsed instead of growing the graph. A hit is a
  // node NOT materialized, so `intern_attempts() == cache_hits() + unique_nodes()`
  // exactly (every attempt either hits or appends one fresh node). These are pure
  // observability counters: interning behaviour is unchanged.
  [[nodiscard]] atx::u32 cache_hits() const noexcept { return cache_hits_; }
  [[nodiscard]] atx::u32 intern_attempts() const noexcept { return intern_attempts_; }

  // ---- builder API (used by build_dag; intentionally public) ------------

  // Hash-cons a key: return the existing NodeId on a structural hit (CSE), or
  // append a fresh node from `proto` and return its NodeId on a miss.
  [[nodiscard]] NodeId intern(const NodeKey &key, const Node &proto) {
    ++intern_attempts_;
    if (const auto it = cons_.find(key); it != cons_.end()) {
      ++cache_hits_; // structural hit: a shared sub-expression was NOT re-materialized
      return it->second;
    }
    const auto id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back(proto);
    cons_.emplace(key, id);
    return id;
  }

  // Intern a field NAME into the dictionary, returning its stable id. The `$`
  // Qlib sigil is stripped by the caller, so `$vwap` and `vwap` share an id.
  [[nodiscard]] atx::u32 intern_field(std::string_view name) {
    for (atx::usize i = 0; i < fields_.size(); ++i) {
      if (fields_[i] == name) {
        return static_cast<atx::u32>(i);
      }
    }
    fields_.emplace_back(name);
    return static_cast<atx::u32>(fields_.size() - 1);
  }

  void add_root(std::string name, NodeId node) { roots_.push_back(Root{std::move(name), node}); }
  void add_edge_refcount(NodeId id) noexcept { ++nodes_[id].refcount; }
  void set_total_ast_nodes(atx::u32 n) noexcept { total_ast_nodes_ = n; }
  void set_required_lookback(atx::u16 lb) noexcept { required_lookback_ = lb; }

private:
  std::vector<Node> nodes_;         // the interned graph; NodeId = index
  std::vector<Root> roots_;         // one per Ast assignment
  std::vector<std::string> fields_; // field-name dictionary (sigil-stripped)
  atx::core::container::HashMap<NodeKey, NodeId, detail::NodeKeyHash> cons_; // cons table
  atx::u32 total_ast_nodes_{};
  atx::u16 required_lookback_{};
  atx::u32 cache_hits_{};      // intern() calls that hit an existing node (CSE)
  atx::u32 intern_attempts_{}; // total intern() calls (hits + fresh inserts)
};

namespace detail {

// Strip an optional leading `$` Qlib sigil so `$vwap` and `vwap` are one field.
[[nodiscard]] inline std::string_view strip_dollar(std::string_view name) noexcept {
  if (!name.empty() && name.front() == '$') {
    return name.substr(1);
  }
  return name;
}

// The opcode a node lowers to. Leaves map to LoadField/Const; Unary/Binary/
// Select carry their opcode directly; Call uses its resolved registry row;
// Member (pin projection) lowers to Pin.
[[nodiscard]] inline OpCode lowered_opcode(const Expr &e) noexcept {
  switch (e.kind) {
  case Expr::Kind::Literal:
    return OpCode::Const;
  case Expr::Kind::Field:
    return OpCode::LoadField;
  case Expr::Kind::Unary:
  case Expr::Kind::Binary:
    return e.opcode;
  case Expr::Kind::Select:
    return OpCode::Select;
  case Expr::Kind::Call:
    return e.op->opcode;
  case Expr::Kind::Member:
    return OpCode::Pin;
  }
  return OpCode::Const; // unreachable for a valid Expr::Kind
}

// Number of child slots an Expr populates, by kind/arity. Drives both the
// NodeKey children and the refcount edge walk.
[[nodiscard]] inline atx::usize child_count(const Expr &e) noexcept {
  switch (e.kind) {
  case Expr::Kind::Literal:
  case Expr::Kind::Field:
    return 0;
  case Expr::Kind::Unary:
  case Expr::Kind::Member: // one child: the record-valued operand
    return 1;
  case Expr::Kind::Binary:
    return 2;
  case Expr::Kind::Select:
    return 3;
  case Expr::Kind::Call:
    return call_arity(e); // materialized arg count (P3b-1 default-fill aware)
  }
  return 0; // unreachable for a valid Expr::Kind
}

// Build a Node prototype from an Ast node and its (already-mapped) children,
// copying the analysis-derived type info. `value`/`param` are op-specific.
[[nodiscard]] inline Node make_node(const Expr &e, const TypeInfo &ti,
                                    const std::array<NodeId, 3> &kids, OpCode op,
                                    atx::u32 param) noexcept {
  Node n;
  n.op = op;
  n.shape = ti.shape;
  n.dtype = ti.dtype;
  n.lookback = ti.lookback;
  n.in = kids;
  n.value = (e.kind == Expr::Kind::Literal) ? e.value : 0.0;
  n.param = param;
  return n;
}

// Lower a `record.pin` Member node to a Pin node whose `param` is the resolved
// pin index (NOT the generic param=0 the fallthrough would produce). The pin
// name is looked up in the record operand's TypeInfo.pins table; the result is
// interned keyed by (OpCode::Pin, pin_index, {rec_node, kNoNode, kNoNode}). The
// record child's refcount is bumped only on a cons MISS (a CSE-hit Pin reuses
// the already-counted edge — mirrors the generic build_dag refcount rule).
// Returns the interned Pin NodeId (caller assigns it to ast_to_node[i]).
//
// SAFETY: analyze_member rejects unknown pin names before build_dag runs, so the
// pin is GUARANTEED to exist in cti.pins; the ATX_ASSERT documents that invariant
// (and aborts in debug if a future caller violates it). Precondition: `e` is a
// Member node and `ast_to_node[e.a]` is already mapped (children precede parents).
[[nodiscard]] NodeId lower_member(Dag &dag, const Ast &ast, const Analysis &analysis,
                                  const std::vector<NodeId> &ast_to_node, const Expr &e,
                                  const TypeInfo &ti);

// Strength reduction: pow(x, 2) -> mul(x, x). When the 2nd operand of a Pow is a
// Const node holding exactly 2.0, rewrite to Mul with both children = x. Mutates
// `emit_op`/`emit_kids` in place; a no-op for any other op. The dropped Const(2)
// is left interned but unreferenced (refcount 0) — linearize skips it.
inline void apply_pow2_strength_reduction(const Dag &dag, OpCode op, atx::usize nkids,
                                          const std::array<NodeId, 3> &kids, OpCode &emit_op,
                                          std::array<NodeId, 3> &emit_kids) noexcept {
  if (op == OpCode::Pow && nkids == 2 && kids[1] != kNoNode) {
    const Node &exp = dag.node(kids[1]);
    if (exp.op == OpCode::Const && exp.value == 2.0) {
      emit_op = OpCode::Mul;
      emit_kids = {kids[0], kids[0], kNoNode};
    }
  }
}

// Count one refcount per emitted child EDGE. Called only when intern appended a
// NEW node (a cons miss) — on a CSE hit the parent and its edges already exist,
// and re-counting would inflate a shared child's refcount and leak its slot.
// (Mul(x,x) bumps x twice: a single node with two distinct edges to one child.)
inline void count_child_edges(Dag &dag, OpCode op, OpCode emit_op, atx::usize nkids,
                              const std::array<NodeId, 3> &emit_kids) noexcept {
  const atx::usize emit_nkids = (emit_op == OpCode::Mul && op == OpCode::Pow) ? 2 : nkids;
  for (atx::usize k = 0; k < emit_nkids; ++k) {
    if (emit_kids.at(k) != kNoNode) {
      dag.add_edge_refcount(emit_kids.at(k));
    }
  }
}

// Lower a non-Member Ast node (leaf/Unary/Binary/Call/Select) to its interned
// NodeId: map children, compute leaf immediates, apply pow(x,2)->mul strength
// reduction, fold Call hparams into imm_bits + n_out, intern, and bump child
// edge refcounts on a cons miss. Returns the interned NodeId. Precondition: `e`
// is NOT a Member node and every child ExprId is already mapped in ast_to_node.
[[nodiscard]] NodeId lower_generic(Dag &dag, const Ast &ast,
                                   const std::vector<NodeId> &ast_to_node, const Expr &e,
                                   const TypeInfo &ti);

} // namespace detail

// =========================================================================
//  build_dag — lower a typed Ast into one hash-consed DAG.
// =========================================================================

// Build a single global DAG shared by every root of `ast`. `analysis` MUST be
// the result of analyze(ast) (same arena, by ExprId). Interns bottom-up,
// accumulates consumer refcounts, and applies pow(x,2)->x*x strength reduction.
//
// SAFETY: the parser appends children before parents, so the Ast arena is
// topologically ordered — node `i` only references children with strictly
// smaller ExprIds (the same invariant typecheck.hpp relies on). We therefore
// intern in one forward pass; every child's NodeId is mapped before its parent.
[[nodiscard]] atx::core::Result<Dag> build_dag(const Ast &ast, const Analysis &analysis);

} // namespace atx::engine::alpha

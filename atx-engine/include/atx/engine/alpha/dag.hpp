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
#include <bit>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/container/hash_map.hpp"
#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
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
  atx::f64 value{};    // Const: the literal value
  atx::u32 param{};    // LoadField: field id (or Ts window immediate, future)
  atx::u32 refcount{}; // consumers: one per parent EDGE + one per root store
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
                                   k.children[2]);
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

  // ---- builder API (used by build_dag; intentionally public) ------------

  // Hash-cons a key: return the existing NodeId on a structural hit (CSE), or
  // append a fresh node from `proto` and return its NodeId on a miss.
  [[nodiscard]] NodeId intern(const NodeKey &key, const Node &proto) {
    if (const auto it = cons_.find(key); it != cons_.end()) {
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
// Select carry their opcode directly; Call uses its resolved registry row.
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
[[nodiscard]] inline atx::core::Result<Dag> build_dag(const Ast &ast, const Analysis &analysis) {
  const std::span<const Expr> arena = ast.nodes();
  if (arena.size() != analysis.nodes().size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "build_dag: analysis arity does not match the Ast arena");
  }

  Dag dag;
  dag.set_total_ast_nodes(static_cast<atx::u32>(arena.size()));
  dag.set_required_lookback(analysis.required_lookback());

  // Per-ExprId -> interned NodeId. kNoNode marks "not yet mapped" (defensive).
  std::vector<NodeId> ast_to_node(arena.size(), kNoNode);

  for (atx::usize i = 0; i < arena.size(); ++i) {
    const Expr &e = arena[i];
    const TypeInfo &ti = analysis.info(static_cast<ExprId>(i));
    const OpCode op = detail::lowered_opcode(e);
    const atx::usize nkids = detail::child_count(e);

    // Map this node's children (already interned) into NodeKey/Node child slots.
    std::array<NodeId, 3> kids{kNoNode, kNoNode, kNoNode};
    const std::array<ExprId, 3> child_ids{e.a, e.b, e.c};
    for (atx::usize k = 0; k < nkids; ++k) {
      kids.at(k) = ast_to_node[child_ids.at(k)];
    }

    // Leaf immediates: LoadField -> field id; Const -> bit pattern of the f64.
    atx::u32 field_param = 0;
    atx::u64 key_param = 0;
    if (e.kind == Expr::Kind::Field) {
      field_param = dag.intern_field(detail::strip_dollar(ast.field_name(e.name_id)));
      key_param = field_param;
    } else if (e.kind == Expr::Kind::Literal) {
      key_param = std::bit_cast<atx::u64>(e.value);
    }

    // Strength reduction: pow(x, 2) -> mul(x, x). When the 2nd operand of a Pow
    // is a Const node holding exactly 2.0, rewrite to Mul with both children = x
    // BEFORE interning. The Const(2) node may be left interned but unreferenced
    // (refcount 0); linearize skips refcount-0 nodes so it is never emitted.
    OpCode emit_op = op;
    std::array<NodeId, 3> emit_kids = kids;
    if (op == OpCode::Pow && nkids == 2 && kids[1] != kNoNode) {
      const Node &exp = dag.node(kids[1]);
      if (exp.op == OpCode::Const && exp.value == 2.0) {
        emit_op = OpCode::Mul;
        emit_kids = {kids[0], kids[0], kNoNode};
      }
    }

    NodeKey key{emit_op, key_param, emit_kids};
    const Node proto = detail::make_node(e, ti, emit_kids, emit_op, field_param);
    const atx::usize before = dag.nodes().size();
    const NodeId id = dag.intern(key, proto);
    ast_to_node[i] = id;

    // Count child edges ONLY when intern actually appended a NEW node. On a CSE
    // hit the parent — and therefore its edges — already exist; re-counting would
    // inflate a shared child's refcount once per duplicate AST occurrence of the
    // same sub-expression, and the over-counted leaf would never be freed (slot
    // leak). One unique DAG edge ⇒ one refcount. (Mul(x,x) still bumps x twice on
    // the miss: a single node with two distinct edges to the same child.)
    if (dag.nodes().size() != before) {
      const atx::usize emit_nkids = (emit_op == OpCode::Mul && op == OpCode::Pow) ? 2 : nkids;
      for (atx::usize k = 0; k < emit_nkids; ++k) {
        if (emit_kids.at(k) != kNoNode) {
          dag.add_edge_refcount(emit_kids.at(k));
        }
      }
    }
  }

  // Each StoreAlpha is a consumer of its root target: +1 refcount, +1 Root.
  for (const Assignment &root : ast.roots()) {
    if (root.root != kNoExpr) {
      const NodeId target = ast_to_node[root.root];
      dag.add_edge_refcount(target);
      dag.add_root(root.name, target);
    }
  }

  return atx::core::Ok(std::move(dag));
}

} // namespace atx::engine::alpha

#include "atx/engine/alpha/dag.hpp"

#include <array>
#include <bit>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/macro.hpp" // ATX_ASSERT

namespace atx::engine::alpha {

namespace detail {

NodeId lower_member(Dag &dag, const Ast &ast, const Analysis &analysis,
                    const std::vector<NodeId> &ast_to_node, const Expr &e,
                    const TypeInfo &ti) {
  const NodeId rec = ast_to_node[e.a];
  const TypeInfo &cti = analysis.info(static_cast<ExprId>(e.a));
  const std::string_view pin = ast.field_name(e.name_id);
  bool found = false;
  atx::u32 pin_idx = 0;
  for (atx::usize k = 0; k < cti.pins.size(); ++k) {
    if (cti.pins[k].name == pin) {
      pin_idx = static_cast<atx::u32>(k);
      found = true;
      break;
    }
  }
  ATX_ASSERT(found); // analyze_member rejects unknown pins before build_dag; unreachable otherwise
  (void)found;       // release builds: ATX_ASSERT compiles out, so `found` is otherwise unused
  Node pn;
  pn.op = OpCode::Pin;
  pn.shape = ti.shape;
  pn.dtype = ti.dtype;
  pn.lookback = ti.lookback;
  pn.in = {rec, kNoNode, kNoNode};
  pn.param = pin_idx;
  const NodeKey pk{OpCode::Pin, atx::u64{pin_idx}, {rec, kNoNode, kNoNode}, {}};
  const atx::usize before = dag.nodes().size();
  const NodeId pid = dag.intern(pk, pn);
  if (dag.nodes().size() != before) {
    dag.add_edge_refcount(rec);
  }
  return pid;
}

NodeId lower_generic(Dag &dag, const Ast &ast,
                     const std::vector<NodeId> &ast_to_node, const Expr &e,
                     const TypeInfo &ti) {
  const OpCode op = lowered_opcode(e);
  const atx::usize nkids = child_count(e);

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
    field_param = dag.intern_field(strip_dollar(ast.field_name(e.name_id)));
    key_param = field_param;
  } else if (e.kind == Expr::Kind::Literal) {
    key_param = std::bit_cast<atx::u64>(e.value);
  }

  // pow(x,2) -> mul(x,x) BEFORE interning (emit_op/emit_kids carry the result).
  OpCode emit_op = op;
  std::array<NodeId, 3> emit_kids = kids;
  apply_pow2_strength_reduction(dag, op, nkids, kids, emit_op, emit_kids);

  // hparams from Call nodes: fold into imm_bits for CSE identity. Non-Call Exprs
  // default hparams to {0,0}, so imm_bits is {} and keys are unchanged — all
  // existing CSE (no record nodes) is preserved exactly.
  const std::array<atx::u64, 2> imm_bits{std::bit_cast<atx::u64>(e.hparams[0]),
                                         std::bit_cast<atx::u64>(e.hparams[1])};
  NodeKey key{emit_op, key_param, emit_kids, imm_bits};
  Node proto = make_node(e, ti, emit_kids, emit_op, field_param);
  // Propagate hparams and n_out onto the proto for Call nodes.
  proto.hparams = e.hparams;
  proto.n_out = (e.kind == Expr::Kind::Call && e.op != nullptr && !e.op->pins.empty())
                    ? static_cast<atx::u8>(e.op->pins.size())
                    : atx::u8{1};

  const atx::usize before = dag.nodes().size();
  const NodeId id = dag.intern(key, proto);
  if (dag.nodes().size() != before) {
    count_child_edges(dag, op, emit_op, nkids, emit_kids);
  }
  return id;
}

} // namespace detail

atx::core::Result<Dag> build_dag(const Ast &ast, const Analysis &analysis) {
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

    // Member (pin projection) lowers to a Pin node with the REAL pin index, so it
    // must not fall through the generic path (which would emit param=0). Every
    // other kind goes through lower_generic.
    ast_to_node[i] = (e.kind == Expr::Kind::Member)
                         ? detail::lower_member(dag, ast, analysis, ast_to_node, e, ti)
                         : detail::lower_generic(dag, ast, ast_to_node, e, ti);
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

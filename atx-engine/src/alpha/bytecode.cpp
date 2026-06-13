#include "atx/engine/alpha/bytecode.hpp"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::alpha {

// =========================================================================
//  linearize — flatten a Dag into a Program.
// =========================================================================

// SAFETY: the DAG is topologically ordered (NodeId = arena index; a node only
// references children with smaller ids — see build_dag). Emitting in NodeId
// order therefore guarantees every `src` slot was produced by an earlier instr.
atx::core::Result<Program> linearize(const Dag &dag) {
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

    // Multi-output nodes (e.g. Split2, n_out>=2) occupy a CONTIGUOUS block of
    // slots [dst, dst+n_out). acquire_block grows the high-water by n_out and
    // returns the first slot; single-output nodes use the fast acquire() path.
    const SlotId dst = (n.n_out > 1) ? pool.acquire_block(n.n_out) : pool.acquire();
    slot[i] = dst;

    Instr instr;
    instr.op = n.op;
    instr.dst = dst;
    const atx::usize nkids = detail::node_child_count(n);
    for (atx::usize k = 0; k < nkids; ++k) {
      instr.src.at(k) = slot[n.in.at(k)];
    }
    // Propagate hparams into the instruction immediates for every node first
    // (filter ops bake Q/R or theta/mu here; all others have hparams == {0,0}).
    // Then apply op-specific overrides: Const replaces imm[0] with its literal
    // value (Const nodes always carry hparams == {0,0}, so no conflict arises).
    instr.imm = n.hparams;
    if (n.op == OpCode::LoadField) {
      instr.param = n.param;
    } else if (n.op == OpCode::Const) {
      instr.imm[0] = n.value; // override: Const uses imm[0] for the literal
    } else if (n.op == OpCode::Pin) {
      // Pin's param is the pin index (which output of the record compute to
      // project). It differs from LoadField / Const, so it must be set here.
      instr.param = n.param;
    }
    instr.n_out = n.n_out;
    prog.code.push_back(instr);

    // Retire each child edge of this node (Mul(x,x) retires x twice — its two
    // edges are two separate consumers and both must count down `remaining`).
    // Pass the child's n_out so retire_consumer can release the right block size.
    for (atx::usize k = 0; k < nkids; ++k) {
      const NodeId child = n.in.at(k);
      detail::retire_consumer(child, nodes[child].n_out, remaining, slot, prog.code, pool);
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
      detail::retire_consumer(static_cast<NodeId>(i), n.n_out, remaining, slot, prog.code, pool);
    }
  }

  prog.num_slots = pool.peak();
  prog.peak_live_slots = pool.peak();
  // Sanity: peak slots bounded by sum of n_out over LIVE nodes. Allows a
  // multi-output node (1 node, n_out slots) while still catching an allocator
  // bug where peak exceeds what the live nodes could legitimately occupy.
  atx::u32 max_possible_slots = 0;
  for (const Node &nd : nodes) {
    if (nd.refcount > 0) {
      max_possible_slots += nd.n_out;
    }
  }
  if (prog.num_slots > max_possible_slots) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "linearize: peak live slots exceeded sum(n_out) — allocator bug");
  }
  return atx::core::Ok(std::move(prog));
}

// =========================================================================
//  compile_batch — compile N alpha sources into ONE cross-alpha-CSE Program.
// =========================================================================

atx::core::Result<Program>
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

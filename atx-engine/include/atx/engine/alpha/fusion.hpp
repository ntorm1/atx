#pragma once

// atx::engine::alpha — element-wise fusion pass (Lane 2).
//
// WHY: the VM is full-buffer columnar — every instruction sweeps a whole
// dates*instruments buffer. A chain like `(close - open) / ((high - low) + 0.001)`
// runs 4 LoadField copies, a Const fill and 4 maps, each streaming 1-3 panel
// buffers through memory. Memory bandwidth, not arithmetic, bounds these ops.
// Fusion turns every maximal tree of element-wise instructions (plus the LoadField
// and Const leaves only it consumes) into ONE FusedKernel: a tiny SSA micro-program
// evaluated block-by-block (kFuseBlock cells) so intermediates live in L1/L2 and
// only the kernel's external inputs and its single output touch DRAM. LoadField
// leaves read the panel column directly (zero-copy: no materialized field slot).
//
// BIT-IDENTITY: each micro-op applies the SAME scalar function the unfused kernel
// applies (Engine shares one per-opcode map implementation between both paths),
// cell by cell, on the same operand values; element-wise ops have no reduction and
// separate statements are never FP-contracted, so a fused program's output equals
// the unfused program's byte-for-byte (AlphaFusion_* tests, whole WQ101 battery).
//
// OUTPUT: a FusedProgram — a re-linearized Program (new slot allocation and Free
// schedule, since interior values no longer occupy slots) plus the kernel table and
// a per-instruction kernel index. Instructions that are not fused keep their exact
// original form. The Program's roots and field dictionary are unchanged.
//
// Header-only; `fuse` is a cold compile-time pass (allocation is fine).

#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/registry.hpp"

namespace atx::engine::alpha {

// Cells per fused block: each register is kFuseBlock f64 (2 KiB), so a typical
// kernel's register file stays L1/L2-resident.
inline constexpr atx::usize kFuseBlock = 256;
// Maximum original instructions absorbed into one kernel (bounds its register file:
// members + at most 3 external aliases each).
inline constexpr atx::usize kFuseMaxMembers = 32;

enum class MicroKind : atx::u8 {
  Slot,  // alias an external slot's buffer (no copy)
  Field, // panel field column, universe-masked (the LoadField semantics)
  Imm,   // broadcast immediate (the Const semantics)
  Op,    // element-wise opcode over earlier registers
};

struct MicroOp {
  MicroKind kind{MicroKind::Op};
  OpCode op{OpCode::Add};                  // Op: the element-wise opcode
  atx::u32 arg{};                          // Slot: SlotId; Field: program field id
  atx::f64 imm{};                          // Imm: the literal
  std::array<atx::u16, 3> in{0, 0, 0};     // Op: operand registers
};

// Register r == micro-op r's result. The LAST op is the kernel output.
struct FusedKernel {
  std::vector<MicroOp> ops;
  std::vector<SlotId> inputs; // external slots read (for tooling / tests)
};

struct FusedProgram {
  Program prog;                        // re-linearized code (fused sinks included)
  std::vector<FusedKernel> kernels;    // kernel table
  std::vector<atx::i32> kernel_of;     // per prog.code entry: kernel index, or -1
  atx::usize fused_instrs{};           // original instructions absorbed into kernels
};

// True for the element-wise opcodes the fused kernel evaluates (single output,
// cell-local, no reduction).
[[nodiscard]] inline bool is_fusible_elementwise(OpCode op) noexcept {
  switch (op) {
  case OpCode::Add:
  case OpCode::Sub:
  case OpCode::Mul:
  case OpCode::Div:
  case OpCode::Neg:
  case OpCode::Abs:
  case OpCode::Sign:
  case OpCode::Log:
  case OpCode::Sigmoid:
  case OpCode::Tanh:
  case OpCode::Pow:
  case OpCode::Spow:
  case OpCode::MinP:
  case OpCode::MaxP:
  case OpCode::CmpLt:
  case OpCode::CmpGt:
  case OpCode::CmpLe:
  case OpCode::CmpGe:
  case OpCode::CmpEq:
  case OpCode::CmpNe:
  case OpCode::And:
  case OpCode::Or:
  case OpCode::Not:
  case OpCode::Select:
    return true;
  default:
    return false;
  }
}

namespace detail {

inline constexpr atx::u32 kFuNone = ~atx::u32{0};

struct FuGraph {
  std::vector<atx::u32> instr;               // node -> original code index
  std::vector<std::array<atx::u32, 3>> prod; // node -> producer node per operand
  std::vector<std::vector<atx::u32>> cons;   // node -> consumer nodes (per edge)
  std::vector<std::vector<atx::u32>> roots;  // node -> root output indices stored
};

inline void fu_build_graph(const Program &p, FuGraph &g) {
  std::vector<atx::u32> slot_node(static_cast<atx::usize>(p.num_slots) + 1U, kFuNone);
  for (atx::usize i = 0; i < p.code.size(); ++i) {
    const Instr &in = p.code[i];
    if (in.op == OpCode::Free) {
      continue;
    }
    std::array<atx::u32, 3> pr{kFuNone, kFuNone, kFuNone};
    for (atx::usize k = 0; k < in.src.size(); ++k) {
      if (in.src[k] != kNoSlot) {
        pr[k] = slot_node[in.src[k]];
      }
    }
    if (in.op == OpCode::StoreAlpha) {
      if (pr[0] != kFuNone) {
        g.roots[pr[0]].push_back(in.param);
      }
      continue;
    }
    const auto v = static_cast<atx::u32>(g.instr.size());
    g.instr.push_back(static_cast<atx::u32>(i));
    g.prod.push_back(pr);
    g.cons.emplace_back();
    g.roots.emplace_back();
    for (const atx::u32 q : pr) {
      if (q != kFuNone) {
        g.cons[q].push_back(v);
      }
    }
    for (atx::usize k = 0; k < in.n_out; ++k) {
      slot_node[static_cast<atx::usize>(in.dst) + k] = v;
    }
  }
}

// Group assignment, reverse topological: an element-wise sink starts a group; a
// fusible producer joins its consumers' group iff ALL its consumer edges come from
// that one group, it stores no root, and the group has room.
inline std::vector<atx::u32> fu_groups(const Program &p, const FuGraph &g,
                                       std::vector<atx::u32> &sink_of_group,
                                       std::vector<atx::u32> &group_size) {
  const atx::usize n = g.instr.size();
  std::vector<atx::u32> group(n, kFuNone);
  for (atx::usize v = n; v-- > 0;) {
    const Instr &in = p.code[g.instr[v]];
    const bool ew = is_fusible_elementwise(in.op) && in.n_out == 1;
    const bool leaf = in.op == OpCode::LoadField || in.op == OpCode::Const;
    if (!ew && !leaf) {
      continue;
    }
    atx::u32 target = kFuNone;
    bool joinable = g.roots[v].empty() && !g.cons[v].empty();
    for (const atx::u32 c : g.cons[v]) {
      if (!joinable) {
        break;
      }
      const atx::u32 gc = group[c];
      joinable = gc != kFuNone && (target == kFuNone || target == gc);
      target = gc;
    }
    if (joinable && group_size[target] < kFuseMaxMembers) {
      group[v] = target;
      ++group_size[target];
      continue;
    }
    if (ew) {
      group[v] = static_cast<atx::u32>(sink_of_group.size());
      sink_of_group.push_back(static_cast<atx::u32>(v));
      group_size.push_back(1);
    }
  }
  return group;
}

// Per emitted node: input nodes (edges) and, for a fused sink, its member list.
struct FuEmit {
  std::vector<atx::u32> order;                 // emitted nodes, topological
  std::vector<std::vector<atx::u32>> inputs;   // per node: input edges (emitted nodes)
  std::vector<std::vector<atx::u32>> members;  // per node: fused members (sink last), or empty
};

inline void fu_plan_emit(const FuGraph &g, const std::vector<atx::u32> &group,
                         const std::vector<atx::u32> &sink_of_group,
                         const std::vector<atx::u32> &group_size, FuEmit &e) {
  const atx::usize n = g.instr.size();
  e.inputs.assign(n, {});
  e.members.assign(n, {});
  for (atx::usize v = 0; v < n; ++v) {
    const atx::u32 gv = group[v];
    const bool fused_group = gv != kFuNone && group_size[gv] >= 2U;
    if (fused_group) {
      const atx::u32 sink = sink_of_group[gv];
      e.members[sink].push_back(static_cast<atx::u32>(v)); // ascending == topological
      for (const atx::u32 q : g.prod[v]) {
        const bool external = q != kFuNone && group[q] != gv;
        if (external && std::find(e.inputs[sink].begin(), e.inputs[sink].end(), q) ==
                            e.inputs[sink].end()) {
          e.inputs[sink].push_back(q);
        }
      }
      if (v == sink) {
        e.order.push_back(static_cast<atx::u32>(v));
      }
      continue;
    }
    for (const atx::u32 q : g.prod[v]) {
      if (q != kFuNone) {
        e.inputs[v].push_back(q);
      }
    }
    e.order.push_back(static_cast<atx::u32>(v));
  }
}

// Build the micro-program of a fused sink, with external producers bound to their
// (new) slots.
[[nodiscard]] inline FusedKernel fu_kernel(const Program &p, const FuGraph &g,
                                           const std::vector<atx::u32> &members,
                                           const std::vector<SlotId> &slot) {
  FusedKernel k;
  std::vector<atx::u32> reg_of(g.instr.size(), kFuNone);
  auto reg_for = [&](atx::u32 q) -> atx::u16 {
    if (reg_of[q] == kFuNone) { // external producer: alias its slot once
      MicroOp m;
      m.kind = MicroKind::Slot;
      m.arg = slot[q];
      reg_of[q] = static_cast<atx::u32>(k.ops.size());
      k.ops.push_back(m);
      k.inputs.push_back(slot[q]);
    }
    return static_cast<atx::u16>(reg_of[q]);
  };
  for (const atx::u32 v : members) {
    const Instr &in = p.code[g.instr[v]];
    MicroOp m;
    if (in.op == OpCode::Const) {
      m.kind = MicroKind::Imm;
      m.imm = in.imm[0];
    } else if (in.op == OpCode::LoadField) {
      m.kind = MicroKind::Field;
      m.arg = in.param;
    } else {
      m.kind = MicroKind::Op;
      m.op = in.op;
      for (atx::usize s = 0; s < in.src.size(); ++s) {
        if (g.prod[v][s] != kFuNone) {
          m.in[s] = reg_for(g.prod[v][s]);
        }
      }
    }
    reg_of[v] = static_cast<atx::u32>(k.ops.size());
    k.ops.push_back(m);
  }
  return k;
}

} // namespace detail

// Fuse element-wise trees of `prog`. The Result wrapper keeps the pass extensible
// (a future rewrite may reject a program); today it always succeeds.
[[nodiscard]] inline atx::core::Result<FusedProgram> fuse(const Program &prog) {
  namespace d = detail;
  d::FuGraph g;
  d::fu_build_graph(prog, g);
  std::vector<atx::u32> sink_of_group;
  std::vector<atx::u32> group_size;
  const std::vector<atx::u32> group = d::fu_groups(prog, g, sink_of_group, group_size);
  d::FuEmit e;
  d::fu_plan_emit(g, group, sink_of_group, group_size, e);

  const atx::usize n = g.instr.size();
  std::vector<atx::u32> remaining(n, 0);
  for (const atx::u32 v : e.order) {
    for (const atx::u32 q : e.inputs[v]) {
      ++remaining[q];
    }
    remaining[v] += static_cast<atx::u32>(g.roots[v].size());
  }

  FusedProgram fp;
  Program &out = fp.prog;
  out.roots = prog.roots;
  out.fields = prog.fields;
  out.required_lookback = prog.required_lookback;
  out.unique_nodes = prog.unique_nodes;
  out.total_ast_nodes = prog.total_ast_nodes;
  out.cache_hits = prog.cache_hits;
  out.intern_attempts = prog.intern_attempts;

  std::vector<SlotId> slot(n, kNoSlot);
  detail::SlotPool pool;
  auto retire = [&](atx::u32 q) {
    --remaining[q];
    if (remaining[q] == 0) {
      const atx::u8 w = prog.code[g.instr[q]].n_out;
      Instr fr;
      fr.op = OpCode::Free;
      fr.dst = slot[q];
      fr.n_out = w;
      out.code.push_back(fr);
      fp.kernel_of.push_back(-1);
      pool.release_block(slot[q], w);
      slot[q] = kNoSlot;
    }
  };
  for (const atx::u32 v : e.order) {
    Instr in = prog.code[g.instr[v]];
    const SlotId dst = in.n_out > 1 ? pool.acquire_block(in.n_out) : pool.acquire();
    atx::i32 kid = -1;
    if (!e.members[v].empty()) {
      fp.kernels.push_back(d::fu_kernel(prog, g, e.members[v], slot));
      fp.fused_instrs += e.members[v].size();
      kid = static_cast<atx::i32>(fp.kernels.size() - 1U);
      in.src = {kNoSlot, kNoSlot, kNoSlot};
    } else {
      for (atx::usize k = 0; k < in.src.size(); ++k) {
        if (g.prod[v][k] != d::kFuNone) {
          in.src[k] = slot[g.prod[v][k]];
        }
      }
    }
    in.dst = dst;
    slot[v] = dst;
    out.code.push_back(in);
    fp.kernel_of.push_back(kid);
    for (const atx::u32 q : e.inputs[v]) {
      retire(q);
    }
    for (const atx::u32 r : g.roots[v]) {
      Instr st;
      st.op = OpCode::StoreAlpha;
      st.dst = kNoSlot;
      st.src[0] = slot[v];
      st.param = r;
      out.code.push_back(st);
      fp.kernel_of.push_back(-1);
      retire(v);
    }
  }
  out.num_slots = pool.peak();
  out.peak_live_slots = pool.peak();
  return atx::core::Ok(std::move(fp));
}

} // namespace atx::engine::alpha

#pragma once

// atx::engine::parallel — STRATEGY B: level-scheduled evaluation of ONE union DAG
// (Lane 2).
//
// Strategy A (batch_eval.hpp) hands one Program per alpha to each worker, so a
// subtree shared by k alphas is computed k times and each worker runs a whole
// alpha serially. Strategy B evaluates the hash-consed UNION program of every alpha
// (compile_batch — cross-alpha CSE computes each shared subtree once) and
// parallelizes INSIDE it:
//
//   1. Levels. Every compute instruction gets level = 1 + max(producer levels)
//      (ASAP); leaves (LoadField / Const) are then pushed as LATE as possible
//      (min consumer level - 1) so their buffers are not held from level 0.
//   2. Work items. Each node of a level is cut into contiguous chunks along its
//      Engine::chunk_axis (cells / date rows / instrument columns); every chunk of
//      every node in the level is one DetPool work item, so a level with one huge
//      Ts op and ten cheap maps still load-balances across all workers.
//   3. Buffers. One whole-panel buffer per live node output, recycled through a
//      free list when the node's LAST consumer level finishes; root outputs are
//      written straight into the returned SignalSet (no final copy).
//   4. Cache (optional). Nodes found in a SubtreeCache are not computed and prune
//      their producer cone; nodes computed here are published after their level
//      (serial phase, between barriers).
//
// DETERMINISM / BIT-IDENTITY: every chunk runs the unchanged serial kernel over a
// sub-range of an axis along which the op is independent (see vm.hpp ChunkAxis),
// on the same input bytes, in the same per-cell order — so the output equals the
// serial Engine::evaluate of the same union Program byte-for-byte, for every worker
// count and any chunking. Which worker runs a chunk never changes a bit; there is
// no cross-worker floating-point accumulation. Errors: the lowest-index failing
// work item's error (items are ordered by level, node, chunk), deterministic.
//
// CONCURRENCY: during a level the slot table and every buffer an item READS are
// immutable (produced at an earlier level); each item WRITES only its own range
// of its own node's buffer(s); each worker uses only engines[wid]. Allocation,
// cache publication and buffer release run between barriers on the calling thread.
//
// Header-only; every function is `inline`. Allocation happens only in the serial
// phases (buffer free-list growth, plan vectors), except the final root-copy pass,
// where each worker allocates the one output vector it fills.

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/fusion.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/subtree_cache.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/parallel/det_pool.hpp"

namespace atx::engine::parallel {

struct GlobalDagStats {
  atx::usize levels{};       // scheduled levels
  atx::usize nodes{};        // live compute nodes in the union program
  atx::usize computed{};     // nodes actually executed (nodes - cache hits - pruned)
  atx::usize cache_hits{};   // nodes served from the SubtreeCache
  atx::usize work_items{};   // chunks dispatched over all levels
  atx::usize peak_buffers{}; // max simultaneously held whole-panel buffers
};

struct GlobalDagOptions {
  // Smallest chunk, in cells, worth a separate work item (amortizes dispatch).
  atx::usize min_chunk_cells{atx::usize{1} << 15};
  // Upper bound on chunks per node, as a multiple of the worker count.
  atx::usize chunks_per_worker{2};
  alpha::EvalMode mode{alpha::EvalMode::AuditExact};
  // Precomputed alpha::panel_content_digest(panel) for the cache key. A caller that
  // evaluates many programs on ONE panel (a GP generation) hashes it once and passes
  // it here, saving an O(cells * fields) pass per call. It MUST be the digest of the
  // panel being evaluated (a stale value would serve another panel's entries);
  // empty = compute it. Ignored without a cache.
  std::optional<atx::u64> panel_digest{};
};

namespace detail {

inline constexpr atx::u32 kGdNone = std::numeric_limits<atx::u32>::max();

// Static plan of a union Program: producers, levels, consumers, table layout.
struct GdPlan {
  std::vector<atx::u32> instr;                  // compute instruction indices (topological)
  // Producer NODE per operand: positional (3 entries, kGdNone = absent) for a plain
  // instruction; one per Slot micro-op, in micro-op order, for a fused kernel.
  std::vector<std::vector<atx::u32>> prod;
  std::vector<atx::i32> kernel; // per node: fused kernel index or -1
  std::vector<atx::u32> level;                  // per node (kGdNone for unneeded / hit)
  std::vector<atx::u32> last_use;               // last consumer level per node
  std::vector<atx::u32> base;                   // first slot-table entry per node
  std::vector<std::shared_ptr<const alpha::PanelBuf>> hit; // cache hit value, else null
  std::vector<std::vector<atx::u32>> root_outputs;         // per node: root indices it stores
  std::vector<atx::u8> needed;
  atx::usize table_size{};
  atx::u32 n_levels{};
};

// Map every compute instruction to a node and record its producer nodes and the
// roots it stores.
inline void gd_build_graph(const alpha::Program &prog,
                           std::span<const alpha::FusedKernel> kernels,
                           std::span<const atx::i32> kernel_of, GdPlan &p) {
  std::vector<atx::u32> slot_node(static_cast<atx::usize>(prog.num_slots) + 1U, kGdNone);
  for (atx::usize i = 0; i < prog.code.size(); ++i) {
    const alpha::Instr &in = prog.code[i];
    if (in.op == alpha::OpCode::Free) {
      continue;
    }
    const atx::i32 kid = kernel_of.empty() ? -1 : kernel_of[i];
    std::vector<atx::u32> pr;
    if (kid >= 0) {
      for (const alpha::MicroOp &m : kernels[static_cast<atx::usize>(kid)].ops) {
        if (m.kind == alpha::MicroKind::Slot) {
          pr.push_back(slot_node[m.arg]);
        }
      }
    } else {
      pr.assign(3, kGdNone);
      for (atx::usize k = 0; k < in.src.size(); ++k) {
        if (in.src[k] != alpha::kNoSlot) {
          pr[k] = slot_node[in.src[k]];
        }
      }
    }
    if (in.op == alpha::OpCode::StoreAlpha) {
      if (pr[0] != kGdNone && in.param < prog.roots.size()) {
        p.root_outputs[pr[0]].push_back(in.param);
      }
      continue;
    }
    const auto node = static_cast<atx::u32>(p.instr.size());
    p.instr.push_back(static_cast<atx::u32>(i));
    p.prod.push_back(std::move(pr));
    p.kernel.push_back(kid);
    p.root_outputs.emplace_back();
    for (atx::usize k = 0; k < in.n_out; ++k) {
      slot_node[static_cast<atx::usize>(in.dst) + k] = node;
    }
  }
}

// Backward liveness from the roots, stopping at cache hits.
inline void gd_mark_needed(const alpha::Program &prog, const alpha::Panel &panel,
                           alpha::SubtreeCache *cache, alpha::EvalMode mode,
                           std::span<const alpha::SubtreeHash> hashes, atx::u64 pdig,
                           GdPlan &p) {
  const atx::usize n = p.instr.size();
  p.needed.assign(n, 0);
  p.hit.assign(n, nullptr);
  for (atx::usize v = 0; v < n; ++v) {
    if (!p.root_outputs[v].empty()) {
      p.needed[v] = 1;
    }
  }
  for (atx::usize v = n; v-- > 0;) {
    if (p.needed[v] == 0) {
      continue;
    }
    const alpha::Instr &in = prog.code[p.instr[v]];
    if (cache != nullptr && p.kernel[v] < 0 && alpha::subtree_cacheable(in)) {
      auto h = cache->find(alpha::SubtreeKey{hashes[p.instr[v]], pdig, mode});
      if (h != nullptr && h->size() == panel.cells()) {
        p.hit[v] = std::move(h);
        continue;
      }
    }
    for (const atx::u32 q : p.prod[v]) {
      if (q != kGdNone) {
        p.needed[q] = 1;
      }
    }
  }
}

// ASAP levels for computed nodes, ALAP for leaves; consumer-driven last use.
inline void gd_levels(GdPlan &p) {
  const atx::usize n = p.instr.size();
  p.level.assign(n, kGdNone);
  std::vector<atx::u32> min_consumer(n, kGdNone);
  atx::u32 top = 0;
  for (atx::usize v = 0; v < n; ++v) {
    if (p.needed[v] == 0 || p.hit[v] != nullptr) {
      continue;
    }
    atx::u32 lv = 0;
    for (const atx::u32 q : p.prod[v]) {
      if (q != kGdNone && p.level[q] != kGdNone) {
        lv = std::max(lv, p.level[q] + 1U);
      }
    }
    p.level[v] = lv;
    top = std::max(top, lv);
  }
  for (atx::usize v = 0; v < n; ++v) {
    if (p.level[v] == kGdNone) {
      continue;
    }
    for (const atx::u32 q : p.prod[v]) {
      if (q != kGdNone) {
        min_consumer[q] = std::min(min_consumer[q], p.level[v]);
      }
    }
  }
  // Leaves (no computed producer) move to just before their first consumer.
  for (atx::usize v = 0; v < n; ++v) {
    if (p.level[v] == kGdNone || min_consumer[v] == kGdNone) {
      continue;
    }
    bool leaf = true;
    for (const atx::u32 q : p.prod[v]) {
      leaf = leaf && (q == kGdNone || p.level[q] == kGdNone);
    }
    if (leaf) {
      p.level[v] = min_consumer[v] - 1U;
    }
  }
  p.last_use.assign(n, 0);
  for (atx::usize v = 0; v < n; ++v) {
    if (p.level[v] == kGdNone) {
      continue;
    }
    for (const atx::u32 q : p.prod[v]) {
      if (q != kGdNone) {
        p.last_use[q] = std::max(p.last_use[q], p.level[v]);
      }
    }
  }
  p.n_levels = top + 1U;
}

// Slot-table layout: each needed node (computed or hit) gets n_out entries.
inline void gd_layout(const alpha::Program &prog, GdPlan &p) {
  p.base.assign(p.instr.size(), kGdNone);
  atx::usize next = 0;
  for (atx::usize v = 0; v < p.instr.size(); ++v) {
    if (p.needed[v] == 0) {
      continue;
    }
    p.base[v] = static_cast<atx::u32>(next);
    next += prog.code[p.instr[v]].n_out;
  }
  p.table_size = next;
}

// Rewrite one instruction onto slot-table indices.
[[nodiscard]] inline alpha::Instr gd_remap(const alpha::Program &prog, const GdPlan &p,
                                           atx::usize v) {
  alpha::Instr in = prog.code[p.instr[v]];
  in.dst = p.base[v];
  if (p.kernel[v] >= 0) {
    return in; // fused: operands live in the remapped kernel, not in src[]
  }
  for (atx::usize k = 0; k < in.src.size(); ++k) {
    if (p.prod[v][k] != kGdNone) {
      in.src[k] = p.base[p.prod[v][k]];
    }
  }
  return in;
}

// Rewrite a fused kernel's Slot aliases onto slot-table indices.
[[nodiscard]] inline alpha::FusedKernel gd_remap_kernel(const alpha::FusedKernel &k,
                                                        const GdPlan &p, atx::usize v) {
  alpha::FusedKernel out = k;
  atx::usize next = 0;
  out.inputs.clear();
  for (alpha::MicroOp &m : out.ops) {
    if (m.kind == alpha::MicroKind::Slot) {
      m.arg = p.base[p.prod[v][next]];
      out.inputs.push_back(m.arg);
      ++next;
    }
  }
  return out;
}

struct GdItem {
  atx::u32 node;
  atx::usize lo;
  atx::usize hi;
};

[[nodiscard]] inline atx::usize gd_axis_len(alpha::ChunkAxis ax, atx::usize dates,
                                            atx::usize instruments) noexcept {
  switch (ax) {
  case alpha::ChunkAxis::Cells:
    return dates * instruments;
  case alpha::ChunkAxis::Dates:
    return dates;
  case alpha::ChunkAxis::Instruments:
    return instruments;
  }
  return 0;
}

// Cells covered by one unit of the axis (for the minimum-chunk rule).
[[nodiscard]] inline atx::usize gd_unit_cells(alpha::ChunkAxis ax, atx::usize dates,
                                              atx::usize instruments) noexcept {
  switch (ax) {
  case alpha::ChunkAxis::Cells:
    return 1;
  case alpha::ChunkAxis::Dates:
    return std::max<atx::usize>(instruments, 1);
  case alpha::ChunkAxis::Instruments:
    return std::max<atx::usize>(dates, 1);
  }
  return 1;
}

// Owns whole-panel scratch buffers and recycles them. A fresh buffer is
// allocated UNINITIALIZED: every kernel writes every cell of its output range
// (recycled buffers already carry stale bytes, so nothing may read before
// writing), and zero-filling here would run serially on the calling thread --
// with first-touch page faults -- for every buffer of the peak working set. The
// workers' first writes fault the pages in parallel instead.
using GdBuf = std::unique_ptr<atx::f64[]>;

class GdBuffers {
public:
  explicit GdBuffers(atx::usize cells) : cells_{cells} {}

  [[nodiscard]] GdBuf take() {
    ++live_;
    peak_ = std::max(peak_, live_);
    if (!free_.empty()) {
      GdBuf b = std::move(free_.back());
      free_.pop_back();
      return b;
    }
    return std::make_unique_for_overwrite<atx::f64[]>(cells_);
  }
  void give(GdBuf &&b) {
    --live_;
    free_.push_back(std::move(b));
  }
  void count_external() {
    ++live_;
    peak_ = std::max(peak_, live_);
  }
  void drop_external() { --live_; }
  [[nodiscard]] atx::usize peak() const noexcept { return peak_; }

private:
  atx::usize cells_;
  atx::usize live_{0};
  atx::usize peak_{0};
  std::vector<GdBuf> free_;
};

} // namespace detail

namespace detail {
[[nodiscard]] inline atx::core::Result<alpha::SignalSet>
global_dag_impl(const alpha::Program &prog, std::span<const alpha::FusedKernel> kernels,
                std::span<const atx::i32> kernel_of, const alpha::Panel &panel, DetPool &pool,
                alpha::SubtreeCache *cache, GlobalDagStats *stats, const GlobalDagOptions &opt) {
  namespace d = detail;
  const atx::usize dates = panel.dates();
  const atx::usize instruments = panel.instruments();
  const atx::usize cells = dates * instruments;

  alpha::SignalSet out;
  out.dates = dates;
  out.instruments = instruments;
  out.alphas.resize(prog.roots.size());
  for (atx::usize r = 0; r < prog.roots.size(); ++r) {
    out.alphas[r].name = prog.roots[r].name;
  }
  if (cells == 0) {
    return atx::core::Ok(std::move(out));
  }

  std::vector<alpha::SubtreeHash> hashes;
  atx::u64 pdig = 0;
  if (cache != nullptr) {
    hashes = alpha::subtree_hashes(prog);
    pdig = opt.panel_digest.has_value() ? *opt.panel_digest : alpha::panel_content_digest(panel);
  }
  d::GdPlan plan;
  d::gd_build_graph(prog, kernels, kernel_of, plan);
  d::gd_mark_needed(prog, panel, cache, opt.mode, hashes, pdig, plan);
  d::gd_levels(plan);
  d::gd_layout(prog, plan);
  const atx::usize n = plan.instr.size();

  // One Engine per worker, fields bound once (a bad field fails here, early).
  const atx::usize workers = pool.n_workers();
  std::vector<std::unique_ptr<alpha::Engine>> engines;
  engines.reserve(workers);
  for (atx::usize w = 0; w < workers; ++w) {
    engines.push_back(std::make_unique<alpha::Engine>(panel));
    engines.back()->set_eval_mode(opt.mode);
    ATX_TRY_VOID(engines.back()->bind_fields(prog));
  }

  std::vector<alpha::ExtSlot> table(plan.table_size);
  std::vector<std::vector<d::GdBuf>> owned(n); // per node, per output
  std::vector<std::vector<atx::u32>> by_level(plan.n_levels);
  std::vector<alpha::Instr> remapped(n);
  std::vector<alpha::FusedKernel> rkernels(n); // remapped kernels (fused nodes only)
  d::GdBuffers bufs{cells};
  GlobalDagStats st{};

  for (atx::usize v = 0; v < n; ++v) {
    if (plan.needed[v] == 0) {
      continue;
    }
    ++st.nodes;
    remapped[v] = d::gd_remap(prog, plan, v);
    if (plan.kernel[v] >= 0) {
      rkernels[v] = d::gd_remap_kernel(kernels[static_cast<atx::usize>(plan.kernel[v])], plan, v);
    }
    if (plan.hit[v] != nullptr) {
      ++st.cache_hits;
      table[plan.base[v]] = alpha::ExtSlot{plan.hit[v]->data(), nullptr, cells};
      continue;
    }
    by_level[plan.level[v]].push_back(static_cast<atx::u32>(v));
  }

  // A root node computed here writes straight into its FIRST output alpha.
  auto bind_outputs = [&](atx::u32 v) {
    const alpha::Instr &in = remapped[v];
    owned[v].resize(in.n_out);
    for (atx::usize k = 0; k < in.n_out; ++k) {
      atx::f64 *buf = nullptr;
      if (k == 0 && in.n_out == 1 && !plan.root_outputs[v].empty()) {
        std::vector<atx::f64> &dst = out.alphas[plan.root_outputs[v].front()].values;
        ATX_ASSERT(dst.size() == cells); // sized by the level's pool pre-pass
        buf = dst.data();
        bufs.count_external();
      } else {
        owned[v][k] = bufs.take();
        buf = owned[v][k].get();
      }
      table[plan.base[v] + k] = alpha::ExtSlot{buf, buf, cells};
    }
  };

  // Publish one computed node's first output into the cache (serial phase only).
  auto publish_node = [&](atx::u32 v) {
    const alpha::Instr &orig = prog.code[plan.instr[v]];
    if (plan.kernel[v] < 0 && alpha::subtree_cacheable(orig)) {
      const alpha::ExtSlot &s = table[plan.base[v]];
      cache->publish(alpha::SubtreeKey{hashes[plan.instr[v]], pdig, opt.mode},
                     alpha::PanelBuf(s.rd, s.rd + cells));
    }
  };

  const atx::usize max_chunks = std::max<atx::usize>(1, workers * opt.chunks_per_worker);
  std::vector<d::GdItem> items;
  std::vector<atx::u32> root_sizing; // alpha indices whose vector a level sizes
  std::vector<std::optional<atx::core::Error>> errs;
  for (atx::u32 lv = 0; lv < plan.n_levels; ++lv) {
    items.clear();
    // Size this level's root outputs across the pool first: the zero-fill (and
    // first-touch page faults) of a whole-panel vector per root would otherwise
    // run serially here. Each worker sizes one distinct alpha's vector.
    root_sizing.clear();
    for (const atx::u32 v : by_level[lv]) {
      if (remapped[v].n_out == 1 && !plan.root_outputs[v].empty()) {
        root_sizing.push_back(plan.root_outputs[v].front());
      }
    }
    pool.parallel_for(root_sizing.size(), [&](atx::usize i, atx::usize /*wid*/) {
      out.alphas[root_sizing[i]].values.resize(cells);
    });
    for (const atx::u32 v : by_level[lv]) {
      bind_outputs(v);
      const alpha::ChunkAxis ax = plan.kernel[v] >= 0
                                      ? alpha::ChunkAxis::Cells
                                      : alpha::Engine::chunk_axis(remapped[v].op);
      const atx::usize len = d::gd_axis_len(ax, dates, instruments);
      const atx::usize unit = d::gd_unit_cells(ax, dates, instruments);
      const atx::usize min_len = std::max<atx::usize>(1, opt.min_chunk_cells / unit);
      const atx::usize chunks =
          std::max<atx::usize>(1, std::min(max_chunks, (len + min_len - 1) / min_len));
      for (atx::usize c = 0; c < chunks; ++c) {
        items.push_back(d::GdItem{v, (len * c) / chunks, (len * (c + 1)) / chunks});
      }
      ++st.computed;
    }
    errs.assign(items.size(), std::nullopt);
    const std::span<const alpha::ExtSlot> tview{table};
    pool.parallel_for(items.size(), [&](atx::usize i, atx::usize wid) {
      const d::GdItem &it = items[i];
      const atx::core::Status s =
          plan.kernel[it.node] >= 0
              ? engines[wid]->execute_fused_range(rkernels[it.node], remapped[it.node].dst, tview,
                                                  it.lo, it.hi)
              : engines[wid]->execute_range(remapped[it.node], tview, it.lo, it.hi);
      if (!s) {
        errs[i] = s.error();
      }
    });
    st.work_items += items.size();
    for (const auto &e : errs) {
      if (e.has_value()) {
        return atx::core::Err(*e);
      }
    }
    // Serial phase: publish interior nodes (roots are published last, below), then
    // release buffers whose last consumer ran now.
    if (cache != nullptr) {
      for (const atx::u32 v : by_level[lv]) {
        if (plan.root_outputs[v].empty()) {
          publish_node(v);
        }
      }
    }
    for (atx::usize v = 0; v < n; ++v) {
      if (plan.needed[v] == 0 || plan.hit[v] != nullptr || plan.level[v] > lv ||
          owned[v].empty() || plan.last_use[v] != lv || !plan.root_outputs[v].empty()) {
        continue;
      }
      for (d::GdBuf &b : owned[v]) {
        bufs.give(std::move(b));
      }
      owned[v].clear();
    }
  }

  // Roots go into the cache LAST so they are the most-recently-used entries: a root
  // hit prunes its whole cone on the next pass, so under a tight byte budget the
  // LRU should evict interior values before any root (WQ101 bench: 72% -> ~100%
  // warm hits at a 2 GiB budget).
  if (cache != nullptr) {
    for (atx::u32 lv = 0; lv < plan.n_levels; ++lv) {
      for (const atx::u32 v : by_level[lv]) {
        if (!plan.root_outputs[v].empty()) {
          publish_node(v);
        }
      }
    }
  }

  // Remaining root outputs: duplicate stores, cache-hit roots, multi-output roots.
  // Each copy fills a DISTINCT alpha's vector from an immutable source (a cache
  // entry or a finished node buffer), so the copies run as one pool pass: on a warm
  // cache they are nearly the whole call, and a serial memcpy of every root would
  // leave the workers idle. Bytes are unaffected by which worker copies.
  std::vector<std::pair<atx::u32, const atx::f64 *>> copies;
  for (atx::usize v = 0; v < n; ++v) {
    const std::vector<atx::u32> &ro = plan.root_outputs[v];
    if (ro.empty() || plan.needed[v] == 0) {
      continue;
    }
    const alpha::ExtSlot &s = table[plan.base[v]];
    for (atx::usize k = 0; k < ro.size(); ++k) {
      const std::vector<atx::f64> &dst = out.alphas[ro[k]].values;
      if (dst.data() == s.rd && dst.size() == cells) {
        continue; // the node computed straight into this alpha
      }
      copies.emplace_back(ro[k], s.rd);
    }
  }
  pool.parallel_for(copies.size(), [&](atx::usize i, atx::usize /*wid*/) {
    out.alphas[copies[i].first].values.assign(copies[i].second, copies[i].second + cells);
  });
  st.levels = plan.n_levels;
  st.peak_buffers = bufs.peak();
  if (stats != nullptr) {
    *stats = st;
  }
  return atx::core::Ok(std::move(out));
}
} // namespace detail

// Evaluate a (union) Program with strategy B. Output: exactly
// Engine(panel).evaluate(prog) byte-for-byte (see header). `cache` / `stats` are
// optional. PRECONDITION: `pool` is not the pool currently running this call.
[[nodiscard]] inline atx::core::Result<alpha::SignalSet>
global_dag_evaluate(const alpha::Program &prog, const alpha::Panel &panel, DetPool &pool,
                    alpha::SubtreeCache *cache = nullptr, GlobalDagStats *stats = nullptr,
                    const GlobalDagOptions &opt = GlobalDagOptions{}) {
  return detail::global_dag_impl(prog, {}, {}, panel, pool, cache, stats, opt);
}

// Strategy B over a FUSED union program: element-wise trees run as one blocked
// kernel per node (chunked by cells). Byte-identical to the unfused serial
// evaluate. No cache: fused kernels are not structurally hashed (a caller caches
// through the unfused Program overload).
[[nodiscard]] inline atx::core::Result<alpha::SignalSet>
global_dag_evaluate(const alpha::FusedProgram &fp, const alpha::Panel &panel, DetPool &pool,
                    GlobalDagStats *stats = nullptr,
                    const GlobalDagOptions &opt = GlobalDagOptions{}) {
  return detail::global_dag_impl(fp.prog, fp.kernels, fp.kernel_of, panel, pool, nullptr, stats,
                                 opt);
}

// Compile `exprs` into ONE union program (compile_batch: cross-alpha CSE; roots
// named a0..aN-1 in order) and evaluate it with strategy B + the shared cache.
[[nodiscard]] inline atx::core::Result<alpha::SignalSet>
parallel_evaluate_shared(std::span<const std::string_view> exprs, const alpha::Library &lib,
                         const alpha::Panel &panel, DetPool &pool, alpha::SubtreeCache *cache,
                         GlobalDagStats *stats = nullptr,
                         const GlobalDagOptions &opt = GlobalDagOptions{}) {
  ATX_TRY(const alpha::Program prog, alpha::compile_batch(exprs, lib));
  return global_dag_evaluate(prog, panel, pool, cache, stats, opt);
}

} // namespace atx::engine::parallel

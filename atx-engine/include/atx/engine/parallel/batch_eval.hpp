#pragma once

// atx::engine::parallel — parallel_evaluate (S2-2): deterministic batch eval.
//
// parallel_evaluate fans N compiled Programs across a DetPool's workers — ONE
// program per work item — and assembles their roots into a single SignalSet in
// (program order, then root order within a program). It is the parallel sibling
// of the single-thread `Engine::evaluate` of the equivalent BATCH program, and
// its digest is BYTE-IDENTICAL to that single-thread path AND invariant across
// worker counts {1,2,4,8}.
//
// ===========================================================================
//  STRATEGY A — per-worker recompute (plan §4.2; CSE lost across the shard
//  boundary, recovered by strategy B in a later unit that needs an
//  Engine::evaluate_root entry which does not exist yet)
// ===========================================================================
//  There is NO `Engine::evaluate_root` — `Engine::evaluate(prog)` computes ALL
//  of a program's roots. So evaluating ONE batch program per worker gives zero
//  speedup. Instead each work item is one whole Program (in practice a single-
//  root program per alpha, from compile_batch({src})); each worker evaluates the
//  programs it grabs on its OWN stateful Engine (Engine is NOT thread-safe), and
//  writes the resulting roots into pre-indexed output slots. Shared sub-
//  expressions are recomputed across programs — that is the documented strategy-A
//  cost.
//
//  WHY BIT-IDENTICAL TO THE BATCH SINGLE-THREAD PATH: alpha_batch_test PROVES
//  "batch == singly" — compiling+evaluating N alphas as ONE Program yields cell-
//  for-cell the SAME values as compiling+evaluating each alpha ALONE (CSE changes
//  compute SHARING, not the math or the bits). So digest(parallel over per-root
//  progs) == digest(Engine::evaluate(batch prog)). Names + shape match too (a
//  single-root compile_batch({src}) keeps the assignment's name; same panel ⇒
//  same dates/instruments).
//
// DETERMINISM (by construction):
//   * the output SignalSet is sized BEFORE the parallel region (no concurrent
//     vector growth); each program k writes ONLY its own disjoint output slots
//     [offsets[k] .. offsets[k]+nroots);
//   * each worker wid touches ONLY engines[wid] — no shared mutable Engine;
//   * the panel is const and shared read-only;
//   * there is NO cross-worker floating-point accumulation, so scheduling order
//     cannot perturb any bit;
//   * the lowest-index program's evaluate error is returned (scan ascending after
//     the barrier) — a deterministic failure.
//
// Header-only; every free function is `inline`. The dispatch loop allocates
// nothing (engines are warmed once before the timed region so their slot pools
// have already grown).

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp" // alpha::Program
#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine

#include "atx/engine/parallel/det_pool.hpp" // DetPool

namespace atx::engine::parallel {

namespace detail {

// Evaluate one program on `engine` and move its roots into `out.alphas` at the
// pre-computed base offset. On evaluate error, records it in `slot` (left
// untouched on success). No shared mutable state: the caller guarantees `engine`
// is this worker's own and the [offset, offset+nroots) output range is disjoint.
inline void evaluate_into(atx::engine::alpha::Engine &engine,
                          const atx::engine::alpha::Program &prog, std::size_t offset,
                          atx::engine::alpha::SignalSet &out,
                          std::optional<atx::core::Error> &slot) {
  auto r = engine.evaluate(prog);
  if (!r.has_value()) {
    slot = r.error();
    return;
  }
  atx::engine::alpha::SignalSet ss = std::move(r).value();
  for (std::size_t j = 0; j < ss.alphas.size(); ++j) {
    out.alphas[offset + j] = std::move(ss.alphas[j]);
  }
}

} // namespace detail

// Evaluate `progs` across `pool`'s workers — one program per work item, each on a
// per-worker stateful Engine — and assemble the roots into one SignalSet in
// (program order, then root order within a program). Byte-identical to the
// single-thread Engine::evaluate of the equivalent batch program, and invariant
// across worker counts. Returns the LOWEST-index program's evaluate error, if any.
[[nodiscard]] inline atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate(std::span<const atx::engine::alpha::Program> progs,
                  const atx::engine::alpha::Panel &panel, DetPool &pool) {
  namespace alpha = atx::engine::alpha;

  // (2) Prefix-sum output offsets so each program's roots land in disjoint slots.
  std::vector<std::size_t> offsets(progs.size());
  std::size_t total_roots = 0;
  for (std::size_t k = 0; k < progs.size(); ++k) {
    offsets[k] = total_roots;
    total_roots += progs[k].roots.size();
  }

  // Size the output BEFORE the parallel region — no concurrent vector growth.
  alpha::SignalSet out;
  out.dates = panel.dates();
  out.instruments = panel.instruments();
  out.alphas.resize(total_roots);

  // (3) One stateful Engine per worker. Engine borrows a `const Panel&` ⇒ not
  // move-assignable ⇒ hold via unique_ptr so the vector never assigns on growth.
  std::vector<std::unique_ptr<alpha::Engine>> engines;
  engines.reserve(pool.n_workers());
  for (atx::usize w = 0; w < pool.n_workers(); ++w) {
    engines.push_back(std::make_unique<alpha::Engine>(panel));
  }

  // (4) Warm each engine once ON ITS OWN WORKER THREAD so the timed dispatch loop
  // allocates nothing (the slot pool has already grown). Ignore the Result here —
  // a real error resurfaces in the main loop below.
  if (!progs.empty()) {
    pool.for_each_worker([&](std::size_t wid) { (void)engines[wid]->evaluate(progs[0]); });
  }

  // (5) Per-item error slots (one per program; written only by the owning index).
  std::vector<std::optional<atx::core::Error>> errs(progs.size());

  // (6) Fan the programs across the workers. SAFETY: each `wid` touches ONLY
  // engines[wid]; each `k` writes ONLY out.alphas[offsets[k] .. +nroots) and
  // errs[k] — disjoint slots, no shared mutable state, panel is const. No cross-
  // worker FP accumulation ⇒ determinism by construction.
  pool.parallel_for(progs.size(), [&](std::size_t k, std::size_t wid) {
    detail::evaluate_into(*engines[wid], progs[k], offsets[k], out, errs[k]);
  });

  // (7) After the barrier, scan ascending: the lowest-index error is the
  // deterministic failure.
  for (std::size_t k = 0; k < errs.size(); ++k) {
    if (errs[k].has_value()) {
      return atx::core::Err(*errs[k]);
    }
  }
  return out;
}

} // namespace atx::engine::parallel

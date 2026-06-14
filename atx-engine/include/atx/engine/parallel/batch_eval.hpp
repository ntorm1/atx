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

#include <span>

#include "atx/core/error.hpp"

#include "atx/engine/alpha/bytecode.hpp" // alpha::Program
#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::SignalSet

#include "atx/engine/parallel/det_pool.hpp" // DetPool
#include "atx/engine/parallel/executor.hpp" // IExecutor (S7.5a substrate-agnostic overload)

namespace atx::engine::parallel {

// Evaluate `progs` across `pool`'s workers — one program per work item, each on a
// per-worker stateful Engine — and assemble the roots into one SignalSet in
// (program order, then root order within a program). Byte-identical to the
// single-thread Engine::evaluate of the equivalent batch program, and invariant
// across worker counts. Returns the LOWEST-index program's evaluate error, if any.
[[nodiscard]] atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate(std::span<const atx::engine::alpha::Program> progs,
                  const atx::engine::alpha::Panel &panel, DetPool &pool);

// S7.5a — the SAME deterministic batch eval over the substrate-agnostic IExecutor
// seam (THREAD substrate this unit). The map BODY is UNCHANGED — this overload
// shares the one map implementation with the DetPool& overload above (no copy-
// paste divergence), substituting exec.parallel_for / exec.workers() for the
// pool's. Output is byte-identical to the DetPool& / single-thread path and
// invariant across worker counts. Returns the LOWEST-index program's evaluate
// error, if any, or an Err if the executor itself rejects the in-process map
// (e.g. ProcessExecutor — its workloads use the serialized submit() path later).
[[nodiscard]] atx::core::Result<atx::engine::alpha::SignalSet>
parallel_evaluate(std::span<const atx::engine::alpha::Program> progs,
                  const atx::engine::alpha::Panel &panel, IExecutor &exec);

} // namespace atx::engine::parallel

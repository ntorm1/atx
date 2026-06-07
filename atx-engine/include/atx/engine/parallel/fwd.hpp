#pragma once

// atx::engine::parallel — deterministic parallel-compute substrate forward declarations (Sprint S2).
//
// A lightweight header other engine headers include to NAME the parallel-layer
// types without pulling in the pool, digest, or batch-eval machinery behind them.
//
// DETERMINISM CONTRACT (the whole point of this subsystem): every result digest
// is byte-identical to the single-thread path AND invariant across worker counts
// {1,2,4,8}. This is achieved BY CONSTRUCTION — map pattern (one worker computes
// each item fully into a pre-indexed output slot), fixed-order assemble (canonical
// id, never completion order), reduce-by-sort for per-item scalars, strict-FP
// flags — NOT by a clever reduction. There is no cross-worker floating-point
// accumulation, so scheduling order can never change the bits.
//
// Full definitions live in (added per unit):
//   parallel/det_pool.hpp     — DetPool (engine-local fallback for the atx-core   (S2-1)
//                               TaskPool; same API: fixed workers + atomic
//                               work-index dispenser, exceptions rethrown in
//                               ascending index order)
//   parallel/digest.hpp       — signal_set_digest, result_table_digest           (S2-1)
//                               (canonical-order wyhash; the determinism oracle)
//   parallel/batch_eval.hpp   — parallel_evaluate (per-worker stateful Engine)    (S2-2)
//   parallel/parallel_run.hpp — parallel_backtests, parallel_cpcv, FoldResult     (S2-3)
//
// SWITCH POINT: when the atx-core deterministic pool (concurrent/task_pool.hpp)
// lands, replace the engine-local fallback with a single alias:
//   using Pool = atx::core::concurrent::TaskPool;
// The engine consumes `Pool` through that one name, so the swap is one line.

namespace atx::engine::parallel {

// Engine-local deterministic fixed-worker pool (S2-1) — the fallback that ships S2
// until the atx-core concurrent/task_pool.hpp Pattern-B request lands.
class DetPool;

// One fold's / one backtest's result row, pre-indexed by canonical (alpha,fold) id
// for fixed-order assemble + reduce-by-sort aggregation (S2-3).
struct FoldResult;

} // namespace atx::engine::parallel

# Sprint S2 — Parallel Compute Substrate (sprint spec)

**Status:** ✅ CLOSED (2026-06-07, `feat/atx-core-stdlib @ d7a1b75`). Shipped as `parallel::` (`DetPool` +
`signal_set_digest`/`result_table_digest` + `parallel_evaluate` + `parallel_cpcv`/`parallel_backtests` +
reduce-by-sort aggregate) — 27 tests, full engine suite 882/882 green, determinism proven by the
{1,2,4,8}==single-thread digest matrix. See the close ledger [`sprint-2-progress.md`](sprint-2-progress.md)
and the user reference [`sprint-2.md`](sprint-2.md). No Phase-4 dependency — opened concurrently with S1.
**Implementation plan (the frozen *how*):** [`sprint-2-parallel-compute-implementation-plan.md`](sprint-2-parallel-compute-implementation-plan.md) — per-unit S2-0…S2-4, data structures, algorithms + pseudocode, the determinism design rules (R1–R7), and ~40 web-research references. **On conflict, the implementation plan's §0 amendment overrides this spec.**
**Backbone decision (web-researched):** a **hand-rolled fixed-worker deterministic pool** on atx-core's *existing* `concurrent/` + `arena` primitives — **NOT oneTBB** (evaluated and rejected: TBB's determinism feature is redundant for this fixed-slot embarrassingly-parallel workload, and it adds a compiled redistributable DLL against the header-only philosophy; Taskflow is the header-only fallback if a real task-graph DAG appears later). Determinism is achieved **by construction** (map pattern + fixed-order assemble + reduce-by-sort + strict-FP), not by a clever reduction.
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
§9 ("hundreds of thousands of nested backtests per day" requires vectorized fan-out) + the p0 determinism
invariant (the thing that must survive parallelism).

---

## Why this sprint

A factory that mines 10⁵–10⁹ alphas cannot run one alpha per process. p0 is **deliberately single-threaded**
for determinism — that discipline is correct, but it caps library scale. S2 introduces **deterministic
multicore** so the factory (S3), CPCV folds (S1), and learned-model fits (S5) get throughput **without
sacrificing the byte-identical-replay guarantee** that makes the engine trustworthy.

The hard part is not the threads — it is keeping the result **bit-identical to single-thread**. Floating
sums are not associative, so a naive parallel reduce changes the digest. S2's whole identity is the
**order-fixed merge**: workers compute disjoint shards, results are reassembled in canonical
instrument/alpha-id order, and the wyhash digest equals the single-thread digest exactly.

---

## Scope — units

### S2.0 — Marker + ledger
Open `sprint-2-progress.md`, freeze scope, record base SHA. Note the atx-core L4 `concurrent` dependency as
a kickoff risk.

### S2.1 — Deterministic task pool
A fixed-worker task pool. **Reconnaissance update:** atx-core *already ships* the concurrency primitives
(`concurrent/{mpmc_queue,spsc_queue,disruptor,seqlock,spinlock}`, `arena`, `object_pool`, `random`), so the
Pattern-B request narrows to **one new header** — a deterministic `concurrent/task_pool.hpp` built on the
existing `MpmcQueue`/`Arena` (~300 LOC, not a from-scratch concurrency layer). The engine consumes
`atx::core::concurrent::TaskPool`; an **engine-local `DetPool` fallback** (same API) ships S2 if the upstream
pool slips. Determinism constraints: **fixed worker count** (default = cores−2), an **atomic work-index
dispenser** (pull-style load balance that never affects results — worker→index mapping is irrelevant because
each task writes only its own pre-indexed slot), clean shutdown, **exceptions rethrown in ascending index
order**. Per-worker **`Arena`** + per-worker **`Engine`** (the Engine is *stateful* — one per worker, never
shared) satisfy the no-hot-path-alloc invariant.

### S2.2 — Parallel batch-eval
Shard a compiled `Program`'s **root set** (the N alphas in a `compile_batch`) across workers; each worker
evaluates its roots over the shared **read-only** panel; results merge into one `SignalSet` in **canonical
root order**. The shared sub-expression DAG (CSE) is read-only across workers — a thread-safe slot/cache
strategy is the crux (either per-worker slot pools, or a compute-once-publish barrier for shared nodes;
decide at kickoff and prove determinism). Alternative shard axis (date-blocks) is a documented option for
very deep single alphas. **The merged digest must equal the single-thread `Engine::evaluate` digest.**

### S2.3 — Parallel backtests / parallel CPCV folds
Run independent backtests (different alphas / different parameter points) and independent **S1 CPCV folds**
concurrently, each in its own isolated `Portfolio`/`ExecutionSimulator`/`RollingPanel` instance (no shared
mutable state), reducing into a result table in fixed (alpha-id, fold-id) order. This is what makes S1's
CPCV and S3's population evaluation fast.

### S2.4 — Determinism-under-parallelism proof + bench + close
The load-bearing test: the parallel digest **==** the single-thread digest for batch-eval, backtests, and
CPCV; **thread-count invariance** (1, 2, 4, 8 workers → identical digest); a stress fixture (many small
alphas) shows no data race (TSan on Linux/clang — a tracked residual on clang-cl/Windows, race-clean by
construction per the p0 precedent). Throughput bench (alphas/sec vs worker count; record the speedup curve).
Close ceremony.

---

## Exit criteria

- Parallel batch-eval of an Alpha101-style battery produces a `SignalSet` **byte-identical** to single-thread,
  for worker counts {1,2,4,8} (thread-count invariance).
- Parallel backtests / CPCV folds produce a result table byte-identical to the sequential run.
- No data race under a stress fixture (TSan-on-Linux green; clang-cl residual documented).
- Steady-state per-worker zero heap allocation (arena pre-sized).
- Throughput bench recorded: near-linear speedup up to physical cores on the mining workload (record actual
  curve, don't claim ideal).
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

**Determinism survives parallelism** — the entire point. Order-fixed merge in canonical id order; no RNG; no
result-affecting timing dependence. No hot-path allocation (per-worker arenas). Differential correctness
(parallel == single-thread, bit-for-bit).

## Dependencies

- **Upstream:** p0 Phase-3 `Engine`/`Program`/`SignalSet`, Phase-2 loop types.
- **atx-core (Pattern B edge):** **`concurrent/task_pool.hpp`** — the deterministic pool (S2.1), built on
  atx-core's *existing* `MpmcQueue`/`Arena` (the other `concurrent/` primitives, `arena`, `random` already
  ship — verified). **Not a hard blocker:** an engine-local `DetPool` (identical API) ships S2 if the upstream
  pool slips, lifted to atx-core L4 as a close residual (the S1 `stats_ext` precedent). Flag at kickoff.

## Explicitly NOT in this sprint

**No distributed / multi-machine compute** (v2 anti-roadmap #4 — single-box only). No JIT/computed-goto
(pure per-op micro-optimization stays future-work). No GPU. **No oneTBB/Taskflow** (evaluated as the backbone
and rejected — implementation plan §0.2; revisit only if nested/irregular loop-heavy numeric kernels emerge).
No lock-free heroics beyond the atomic work-index dispenser — correctness and determinism over cleverness.
No NUMA tuning in v1 (profile-gated future work). No RNG (S3's concern).

## Baton → next

S2 hands S3 the throughput to evaluate a population per generation across cores, and S1 the parallelism to
run CPCV folds concurrently — both **without** changing a single result digit.

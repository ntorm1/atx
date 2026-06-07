# Sprint 2 — Parallel Compute Substrate — Implementation Progress

**Status:** ✅ CLOSED (2026-06-07, `feat/atx-core-stdlib @ d7a1b75` + close docs)
**Worktree:** in-place (p0 precedent; shared branch — explicit pathspecs only, no push)
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `aebcb72` (S1 CLOSED at 2158a17; aebcb72 = Phase-3d state-space DSL merge atop the S1 close)
**Started:** 2026-06-07
**Source plan:** [`sprint-2-parallel-compute-implementation-plan.md`](sprint-2-parallel-compute-implementation-plan.md)
**Spec:** [`sprint-2-parallel-compute.md`](sprint-2-parallel-compute.md)

---

## Plan adjustments vs. the source plan (the §0 as-built amendment)

The implementation plan's §0 reconciles the spec against reconnaissance + web research; the load-bearing decisions:

1. **atx-core already ships the concurrency primitives** (`concurrent/{mpmc_queue,spsc_queue,disruptor,seqlock,spinlock}`, `arena`, `object_pool`, `random` with `jump()`). The Pattern-B request narrows to **one new header** — a deterministic `concurrent/task_pool.hpp` on the existing `MpmcQueue`/`Arena` — not a from-scratch concurrency layer.
2. **Backbone = a hand-rolled fixed-worker deterministic pool, NOT oneTBB.** TBB evaluated and rejected (impl-plan §0.2): its determinism feature (`parallel_deterministic_reduce`) is redundant for this fixed-slot embarrassingly-parallel workload and "may differ from sequential" anyway, and it adds a compiled redistributable DLL against the header-only philosophy. Taskflow is the header-only fallback if a real task-graph DAG appears later.
3. **Determinism is achieved BY CONSTRUCTION, not a clever reduction:** map pattern (one worker computes each item fully into a pre-indexed slot) + fixed-order assemble (canonical id, never completion order) + reduce-by-sort (per-fold scalars) + strict-FP flags. No cross-worker FP accumulation ⇒ scheduling order can never touch the bits.
4. **`alpha::Engine` is STATEFUL** (a monotonically-growing slot pool) → NOT thread-safe → **each worker owns its own warm `Engine`**; the shared data panel is read-only. This is the #1 correctness fact for S2-2.
5. New `parallel/` subsystem under `atx-engine/include/atx/engine/`, namespace **`atx::engine::parallel`**; tests flat in `atx-engine/tests/*_test.cpp` (CONFIGURE_DEPENDS glob — no CMake edit per unit).

**Kickoff risk:** the atx-core `concurrent/task_pool.hpp` (the deterministic `TaskPool`) is a **Pattern-B upstream request** the engine consumes. Since atx-core is a separate, off-limits effort here, S2 ships an **engine-local `DetPool`** (identical API, `atx/engine/parallel/det_pool.hpp`) so the engine units are not blocked; the eventual atx-core `TaskPool` becomes a one-line `using Pool = atx::core::concurrent::TaskPool` swap, and the lift is recorded as a close residual (the S1 `stats_ext` precedent).

---

## Per-unit status

| Unit  | Title                                         | Status | Commit SHA(s) | Tests | Notes |
|-------|-----------------------------------------------|--------|---------------|-------|-------|
| S2-0  | Marker + ledger + parallel scaffold           | ✅ done | `bef28ad`                       | —     | `parallel/fwd.hpp` (DetPool/FoldResult fwd + determinism-contract doc) + this ledger. |
| S2-1  | Deterministic pool (DetPool fallback) + digest | ✅ done | `078749e` + `5eaea08`           | 14    | Hand-rolled `DetPool`: persistent threads, atomic work-index dispenser, generation-guarded wakes, **lowest-index deterministic exception rethrow**, `JobKind` ParallelFor/EachWorker modes, Rule-of-Five pinned. `signal_set_digest` = canonical-order wyhash over **raw f64 bytes** (1-ULP flips it). *(Spec review caught a broken `for_each_worker` — old impl routed through the shared dispenser so 3/4 worker ids were skipped; fixed in `5eaea08` with a per-worker fan-out + 3 tests, non-vacuity confirmed by an old-logic probe 2000/2000 bad.)* |
| S2-2  | Parallel batch-eval (digest == single-thread)  | ✅ done | `fa6ae31`                       | 5     | `parallel_evaluate(span<Program>, panel, DetPool)`: one program/work-item on a per-worker stateful `Engine` (`unique_ptr`, warmed via `for_each_worker`), pre-indexed disjoint output slots, lowest-index `Err` reduce. Strategy A (per-worker recompute; no `Engine::evaluate_root`). **Load-bearing: `digest(parallel fan) == digest(Engine::evaluate(batch))`** over a 34-alpha shared-subexpr battery + {1,2,4,8} invariance — a genuine two-path (compile_batch vs per-program compile) bit-identity proof, backed by the existing `alpha_batch_test` batch==singly result. |
| S2-3  | Parallel backtests + CPCV folds                | ✅ done | `fc51747` + `92d9f2f`           | 5     | `parallel_cpcv` (folds of one alpha) + `parallel_backtests` (one full-sample metric per alpha) → `FoldResult` table in `(alpha_id, fold_id)` order; per-item metric via `combine::compute_metrics` over const `AlphaStreams` (pure map). `cpcv_aggregate_mean_sharpe` = **reduce-by-sort (R3)**: copy → stable-sort → sequential Neumaier fold (order-independent, bit-exact). `result_table_digest` over raw f64 bytes. **`result_table_digest(parallel) == sequential` + {1,2,4,8} invariance**; backtests cross-checked vs an independent inline reference. *(Quality pass: dropped a `noexcept` on the allocating aggregate — `92d9f2f`.)* |
| S2-4  | Determinism matrix + bench + close             | ✅ done | `d7a1b75` (+ close docs)         | 3     | `parallel_determinism_test.cpp`: the cross-cutting matrix — batch-eval **AND** CPCV digest identical across {1,2,4,8} **and** equal to the single-thread reference, on a 128-alpha stress fixture (worst case for races) + run-to-run stability. TSan-on-Linux residual documented (clang-cl has no TSan; race-freedom by construction + validated by the matrix). `parallel_bench.cpp` (Google Benchmark) records the actual `parallel_evaluate` speedup curve (see below). |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `bef28ad` | S2-0 | docs(s2-0): open sprint-2 parallel-compute ledger + scaffold |
| `078749e` | S2-1 | feat(s2-1): deterministic task pool (fallback) + digest oracle |
| `5eaea08` | S2-1 | fix(s2-1): correct for_each_worker (per-worker fan-out, not dispenser); +tests; drop dead includes |
| `fa6ae31` | S2-2 | feat(s2-2): parallel batch-eval (per-worker Engine; digest == single-thread) |
| `fc51747` | S2-3 | feat(s2-3): parallel CPCV folds + backtests (digest == sequential; reduce-by-sort) |
| `92d9f2f` | S2-3 | fix(s2-3): drop noexcept from cpcv_aggregate_mean_sharpe (it allocates) |
| `d7a1b75` | S2-4 | test(s2-4): determinism matrix ({1,2,4,8} == single-thread) + parallel throughput bench |

---

**Totals:** 5 units · **27 new tests** (ParallelDetPool 9 · ParallelDigest 5 · ParallelBatchEval 5 · ParallelRun 5 · ParallelDeterminism 3) · full engine suite **882/882 green** under clang-cl `/W4 /permissive- /WX` + strict-FP · **atx-core net diff EMPTY** across the whole sprint range (`git diff bef28ad^..d7a1b75 -- atx-core/` empty). Final whole-sprint review = **GO**.

---

## What S2 proves

Determinism **survives parallelism** — the entire point of the sprint, proven non-vacuously:

- **By construction, not by a clever reduction.** The `DetPool` does NO floating-point math: it only dispenses unique indices (atomic `next_index_.fetch_add`), barriers (`busy_` acq_rel → `cv_done_`), and rethrows the lowest-index exception deterministically. Every result is a pure per-item **map** over **const** inputs (Panel / AlphaStreams / fold index sets) writing its **own pre-indexed disjoint output slot** on a **per-worker stateful `Engine`** (the Engine-is-stateful → one-per-worker rule, honored everywhere). The only cross-item reduction (`cpcv_aggregate_mean_sharpe`) is **reduce-by-sort** (copy → stable-sort by `(alpha_id, fold_id)` → sequential Neumaier fold). No cross-worker FP accumulation ⇒ worker count and scheduling order cannot touch a single result bit.
- **Validated empirically.** Raw-f64-byte digest oracles (`signal_set_digest`, `result_table_digest`) — a 1-ULP drift flips the digest. The capstone matrices prove `digest({1,2,4,8} workers) == digest(single-thread)` for **batch-eval** (vs `Engine::evaluate` of the equivalent batch program — a genuine two-path proof) **AND** CPCV, on a 128-alpha / 28-fold worst-case (many-small-items) fixture, plus run-to-run stability.
- **Backbone = hand-rolled deterministic pool, NOT oneTBB** (evaluated + rejected, impl-plan §0.2). `parallel::Pool` aliases the eventual atx-core `concurrent::TaskPool`; the engine-local `DetPool` ships S2 as the Pattern-B fallback.

## Throughput (S2-4 bench — `BM_ParallelEval`, 128-alpha battery, Debug/clang-cl, 16-core host)

Actual `parallel_evaluate` speedup vs 1 worker (Debug build ⇒ upper-bound latencies, **not** the optimized number; recorded as the real curve, no ideal-speedup claim):

| workers | items/s | speedup |
|---------|---------|---------|
| 1       | ~620    | 1.00×   |
| 2       | ~1.18k  | ~1.9×   |
| 4       | ~1.97k  | ~3.2×   |
| 8       | ~2.55k  | ~4.1×   |
| 0 (=cores−2=14) | ~2.3k | ~3.7× |

Near-linear to 4 workers, sub-linear beyond — the 128-alpha battery saturates around 8 workers (Debug overhead + strategy-A redundant-CSE recompute + dispenser contention; a contended re-run can show 8w dipping below 4w). The curve is the honest single-box ceiling for this workload, not a claim of ideal scaling.

## Close residuals → p1 ROADMAP future-work backlog

1. **atx-core `concurrent/task_pool.hpp` Pattern-B lift** — promote the engine-local `parallel::DetPool` into atx-core L4 as `atx::core::concurrent::TaskPool`; the engine then swaps to it via the one-line `using Pool = atx::core::concurrent::TaskPool` switch point already documented in `fwd.hpp` (the S1 `stats_ext` precedent).
2. **Strategy-B CSE-preserving batch-eval** — S2-2 ships strategy A (per-worker recompute; cross-program CSE lost across the shard boundary). Strategy B (compute shared sub-nodes once, then parallelize roots) recovers the CSE win but needs an `Engine::evaluate_root(program, r)` entry that does not exist yet. Profile-gated: only if the bench shows the redundant-recompute cost is material.
3. **Linux-CI ThreadSanitizer job** — clang-cl/MSVC has no TSan; race-freedom is argued by construction + validated by the digest-invariance matrix. An optional Linux CI job running the 5 S2 suites under TSan is the upstream cross-check.
4. **Lowest-index-error tie-break is reasoned, not directly asserted** — `parallel_evaluate`/`parallel_cpcv` return the lowest-index failure (deterministic); the tests assert that an error surfaces (and the pool's own `ExceptionRethrownLowestIndexFirst`), but no test asserts *which* of two distinct errors wins. A directed test (two distinguishable errors at known indices) would close it. Minor.
5. **NUMA panel partitioning** (profile-gated) — skipped in v1 (L3-resident panel; the win is per-worker write locality, already first-touched). Record only if a profile shows cross-socket traffic.

## Baton → next

S2 hands **S3** (formulaic factory) the throughput to evaluate a mined population per generation across cores (`parallel_evaluate` + the digest oracle), and **S1** the parallelism to run CPCV folds concurrently (`parallel_cpcv`) — both **without changing a single result digit**. The `parallel::DetPool` + `signal_set_digest`/`result_table_digest` are reused verbatim by S3/S5; the `ParallelDeterminism` matrix is the template every later parallel feature re-proves against. Per the p1 ROADMAP DAG, S2 opened concurrently with S1/S6 (no P4 blocking dependency) and is now closed.

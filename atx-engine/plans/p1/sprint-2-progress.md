# Sprint 2 — Parallel Compute Substrate — Implementation Progress

**Status:** ⏳ in progress (opened 2026-06-07)
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
| S2-3  | Parallel backtests + CPCV folds                | ⏳     |               |       |       |
| S2-4  | Determinism matrix + bench + close             | ⏳     |               |       |       |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `bef28ad` | S2-0 | docs(s2-0): open sprint-2 parallel-compute ledger + scaffold |
| `078749e` | S2-1 | feat(s2-1): deterministic task pool (fallback) + digest oracle |
| `5eaea08` | S2-1 | fix(s2-1): correct for_each_worker (per-worker fan-out, not dispenser); +tests; drop dead includes |
| `fa6ae31` | S2-2 | feat(s2-2): parallel batch-eval (per-worker Engine; digest == single-thread) |

---

## Baton → next

(filled at close)

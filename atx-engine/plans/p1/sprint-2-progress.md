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
| S2-0  | Marker + ledger + parallel scaffold           | ⏳     |               | —     | `parallel/fwd.hpp` + this ledger. |
| S2-1  | Deterministic pool (DetPool fallback) + digest | ⏳     |               |       |       |
| S2-2  | Parallel batch-eval (digest == single-thread)  | ⏳     |               |       |       |
| S2-3  | Parallel backtests + CPCV folds                | ⏳     |               |       |       |
| S2-4  | Determinism matrix + bench + close             | ⏳     |               |       |       |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
|     | S2-0 | docs(s2-0): open sprint-2 parallel-compute ledger + scaffold |

---

## Baton → next

(filled at close)

# Sprint S2 — Parallel Compute Substrate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the FROZEN *how*; the sprint spec [`sprint-2-parallel-compute.md`](sprint-2-parallel-compute.md) is the *what*. On conflict, **§0 (this plan's as-built amendment) overrides** the spec.

**Goal:** Give the engine **deterministic multicore** — a fixed-worker task pool plus parallel batch-evaluation, parallel backtests, and parallel CPCV folds — so the factory (S3), CV (S1), and learned-model fits (S5) get near-linear throughput while every result digest stays **byte-identical to single-thread and invariant across worker counts {1,2,4,8}**.

**Architecture:** A hand-rolled **fixed-worker deterministic pool** (atx-core L4 Pattern-B request, built on atx-core's *existing* `concurrent/mpmc_queue` + `arena`), consumed by a thin engine `parallel/` layer. Determinism is achieved **by construction, not by a clever reduction**: every work item (alpha root, backtest, fold) is computed *entirely by one worker* into a **pre-indexed output slot**, results are **assembled in canonical id order**, and the only cross-item reduction (per-fold scalars) is a **sort-by-id-then-sequential-fold**. No cross-worker floating-point accumulation ⇒ scheduling order can never touch the bits. The `Engine` is **stateful** (a growing slot pool), so each worker owns its own warm `Engine`; the shared data panel is **read-only**.

**Tech Stack:** C++20, header-only inline (`#pragma once`), `std::thread` + atx-core `MpmcQueue`/`Arena`/`hash_*` (NO third-party runtime; **TBB explicitly evaluated and rejected — see §0.2**), GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS), CMake + Ninja, clang-cl `/W4 /permissive- /WX` **plus strict-FP flags** (`/fp:precise`, no fast-math, no FMA contraction — §1 R5). No RNG in S2.

---

## §0 — As-built reconciliation amendment (the "up to date" + research fixes)

The spec was drafted before this sprint's reconnaissance + web research. Six load-bearing corrections:

### 0.1 atx-core ALREADY has the concurrency primitives — only a *pool* is missing
The spec/ROADMAP frame S2.1 as "atx-core **L4 `concurrent`** request — built upstream" as if nothing exists. **Reconnaissance shows atx-core already ships** `atx/core/concurrent/{mpmc_queue,spsc_queue,disruptor,seqlock,spinlock}.hpp`, plus `atx/core/arena.hpp` (non-owning monotonic bump allocator, `reset()`), `atx/core/object_pool.hpp`, and `atx/core/random.hpp` (`Xoshiro256pp` with `jump()` for non-overlapping streams). The Pattern-B request narrows to **one new header**: a deterministic **task pool** (`atx/core/concurrent/task_pool.hpp`) built *on top of* `MpmcQueue` + `Arena`. This is a ~300-line library, not a from-scratch concurrency layer.

### 0.2 Backbone = hand-rolled deterministic pool, NOT TBB (decision recorded, evidence-based)
The user asked whether to bring in **oneTBB**. After web research (§7 citations) the decision is **a hand-rolled fixed-worker pool**, because:
- **Determinism is the #1 constraint and this workload is the *easy* case.** Independent alphas/backtests/folds written to pre-indexed slots, assembled in id order ⇒ byte-identical *by construction*. TBB's headline determinism feature (`parallel_deterministic_reduce`) is **redundant** here, and its result "might differ from the equivalent sequential algorithm" anyway (oneTBB spec [1]).
- **TBB is the only candidate that adds a compiled, redistributable runtime** (`tbb.dll` + `tbbmalloc.dll`) against this project's header-only/vendored-deps philosophy; static linking is officially discouraged (scheduler-singleton hazard [10]).
- TBB's real advantages — nested composability, `tbbmalloc`, range partitioners — **do not apply** to a flat, zero-steady-state-alloc batch with per-worker arenas.
- **Fallback recorded:** **Taskflow** (header-only, MIT, C++20) the day a real task-graph DAG is needed (pipelines/subflows); reach for **oneTBB** only if nested/irregular loop-heavy numeric kernels emerge. Both still require *you* to own the fixed-order assemble (their schedulers are non-deterministic), so they buy nothing for S2's flat workload.

### 0.3 The pool lives in atx-core L4 (Pattern-B), the engine consumes it
Per the project rule (engine adds **no** general-purpose primitive; parallelism is an atx-core edge), the `TaskPool` is specified here as a **Pattern-B upstream request** (§2.1 — full API + tests so the atx-core effort can build it) and the engine's `parallel/` layer **consumes** `atx::core::concurrent::TaskPool`. Sequencing: the engine S2.2–S2.4 units need the atx-core pool landed; **if it slips**, a documented engine-local fallback executor (`atx/engine/parallel/det_pool.hpp`, same API, the S1 `stats_ext` precedent) unblocks the engine and is lifted upstream later. Flag the dependency at kickoff.

### 0.4 `Engine` is STATEFUL → one Engine per worker (the determinism-critical finding)
`alpha::Engine` holds a **mutable slot pool** that grows monotonically across `evaluate()` calls (a warm `evaluate()` allocates nothing — `vm.hpp` lines ~200–230). It is therefore **NOT thread-safe**. Parallel batch-eval **must give each worker its own warm `Engine`** (one per worker, reused across items — preserves the zero-steady-state-alloc invariant per worker). Sharing one `Engine` across threads is a data race and a determinism break. This is the single most important correctness fact in S2.2.

### 0.5 Determinism strategy is concrete (not "figure it out at kickoff")
The research distills to **encodable rules R1–R7 (§1)**. The strategy: (a) **map pattern** — one worker computes each item fully into `out[id]`, no writable shared state between items; (b) **fixed-order assemble** by canonical id (alpha index / `(alpha_id, fold_id)` lexicographic), never completion order; (c) **reduce-by-sort** for per-fold scalars (sort by id, sequential Neumaier fold), never combine partials in completion order; (d) **strict FP flags** identical across all worker-count builds; (e) **partition by fixed grain / atomic work-index dispenser**, never `N/num_workers` and never branch on `hardware_concurrency()` in result math. The shared panel is `const`.

### 0.6 New `parallel/` subsystem; tests flat; no CMake-per-unit edit
No `parallel/` directory exists under `atx-engine/include/atx/engine/`. S2 creates `atx/engine/parallel/`, header-only inline, namespace **`atx::engine::parallel`**. Tests live in `atx-engine/tests/*_test.cpp` (CONFIGURE_DEPENDS glob — no `tests/CMakeLists.txt` edit per unit). The digest helper reuses `atx::core::hash_bytes`/`hash_combine` (wyhash) — no new hash.

---

## §1 — Research foundation: the determinism design rules (with citations)

These rules are derived from the web research (§7) and are **non-negotiable**; every S2 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **R1** | Any FP `+`/`*` whose operand order depends on scheduling is a determinism **bug**. The digest only protects you if the math feeding it is order-fixed. | IEEE-754 addition is non-associative; reorder ⇒ different rounding ⇒ different bits. Goldberg 1991 [G]; Intel FP-consistency [I1]. |
| **R2** | **Map pattern.** One worker computes each item fully; assemble outputs **sorted by canonical item-id**, never completion order; the shared panel is **read-only** (no synchronization, no races). | "Embarrassingly parallel" / "eliminate shared mutable state" deterministic-by-construction pattern [EP]. Sidesteps FP non-associativity entirely — there is no cross-worker accumulation. |
| **R3** | Reduce per-item scalars (e.g. per-fold metrics) by **"sort by id, then sequential fold"** (optionally Neumaier-compensated for accuracy). Never combine partials in completion order. | Reproducible-summation literature [RB][DN]; fixed partition + fixed combine order = bit-identical regardless of worker count. |
| **R4** | Partition by **fixed grain / atomic work-index dispenser**, NOT `N/num_workers`. No `std::thread::hardware_concurrency()` in result math. Worker→item assignment is irrelevant to the result. | Thread-count invariance: chunk layout must depend only on input size + fixed grain, not on how many workers ran [I2 MKL CNR]. |
| **R5** | **Strict FP everywhere**: `/fp:precise` (clang-cl), `-ffp-contract=off`, NO `/fp:fast`/`-ffast-math`. Identical flags across the {1,2,4,8} builds. | Fast-math lets the *compiler* reassociate FP before threads even enter — breaks R1 at compile time [I1]. |
| **R6** | **Test determinism** with a digest-equality matrix across worker counts {1,2,4,8} + a single-thread reference, on fixed input; plus run-to-run stability. Prove race-freedom **by construction** (workers get `const` panel + an exclusive output span); run **TSan in a Linux CI job** since clang-cl has no TSan. | Reproducibility validation methodology [BR]; TSan unsupported on Windows/clang-cl [TS]. |
| **R7** | (Future S3 RNG, not S2.) Seed any RNG by **item-id, never worker/thread-id or time**. atx-core `Xoshiro256pp::jump()` gives non-overlapping streams keyed per item-id; or a counter-based RNG (Philox) for stateless draws. | Worker-keyed streams change with worker count; item-keyed streams don't [R123]. |

**The one-sentence thesis:** *non-associativity of FP addition under nondeterministic reduction order is the only thing that breaks bit-reproducibility on a CPU map/reduce engine — eliminate cross-worker FP accumulation and the problem is gone by construction.* S2's three workloads (independent alphas, independent backtests, independent folds) are **all pure map patterns**; the only reduction (per-fold metric table) is R3.

---

## §2 — File structure

### 2.1 atx-core Pattern-B request (built UPSTREAM; specified here so the engine can consume it)

> The engine adds no general-purpose primitive (project rule). This is the **exact API the engine needs**; the atx-core effort implements it on the existing `MpmcQueue` + `Arena`. If atx-core slips, the identical-API engine-local fallback (`atx/engine/parallel/det_pool.hpp`) ships S2 and is lifted upstream as a residual.

**`atx/core/concurrent/task_pool.hpp`** — `atx::core::concurrent::TaskPool` (deterministic fixed-worker pool):

```cpp
namespace atx::core::concurrent {

// A fixed-worker, deterministic task pool. Determinism is the CALLER's contract
// (write to pre-indexed slots, assemble in id order); the pool only guarantees:
// every index in [0,n) is processed exactly once by exactly one worker, and
// parallel_for does not return until all are done. No work-stealing deque — a
// single shared atomic work-index dispenser load-balances pull-style.
class TaskPool {
public:
  // n_workers == 0 -> default = max(1, hardware_concurrency - 2) (p0 style).
  explicit TaskPool(atx::usize n_workers = 0);
  ~TaskPool();                                 // joins all workers
  TaskPool(const TaskPool&) = delete;          // Rule of Five: move-only or pinned
  TaskPool& operator=(const TaskPool&) = delete;

  [[nodiscard]] atx::usize n_workers() const noexcept;

  // Parallel index loop. `body(usize i, usize worker_id)` is invoked once per
  // i in [0,n), worker_id in [0,n_workers). Blocks until all complete.
  // body MUST be thread-safe w.r.t. other indices (write only to out[i]).
  // Exceptions thrown by body are captured and rethrown AFTER the barrier, in
  // ASCENDING index order (deterministic failure), as a TaskPoolError.
  template <class Body>
  void parallel_for(atx::usize n, Body&& body);

  // Convenience: run exactly n_workers() copies of `fn(worker_id)` once each
  // (for per-worker setup/teardown, e.g. warming a per-worker Engine).
  template <class Fn>
  void for_each_worker(Fn&& fn);
};

} // namespace atx::core::concurrent
```

Internals (atx-core implements): `n_workers` persistent `std::thread`s parked on a condition variable; `parallel_for` publishes `(n, body-thunk)` and an `std::atomic<usize> next_index{0}`; each worker loops `i = next_index.fetch_add(1, relaxed); if (i>=n) break; body(i, my_worker_id);` (pull-style dispenser = R4 load balance without breaking determinism — worker→index mapping never affects results). A `std::latch`/counter signals completion. Per-task `try/catch` stores `(index, exception_ptr)`; after the barrier the lowest-index exception is rethrown. **No `MpmcQueue` strictly required for the index-dispenser design** (an atomic counter suffices); `MpmcQueue` is the alternative if heterogeneous task objects are enqueued — the dispenser is simpler and is the recommended v1.

### 2.2 Engine `parallel/` layer (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/parallel/fwd.hpp` | forward decls for the parallel layer | S2-0 |
| `include/atx/engine/parallel/det_pool.hpp` | **fallback only** — engine-local `DetPool` with the §2.1 API, used iff atx-core `TaskPool` is unavailable; `using Pool = atx::core::concurrent::TaskPool` otherwise (one alias, single switch point) | S2-1 |
| `include/atx/engine/parallel/digest.hpp` | `signal_set_digest(const SignalSet&)`, `result_table_digest(...)` — canonical-order wyhash digests (the determinism oracle) | S2-1 |
| `include/atx/engine/parallel/batch_eval.hpp` | `parallel_evaluate(program, panel, pool)` → `SignalSet` byte-identical to `Engine::evaluate`; per-worker `Engine`, sharded roots, fixed-order assemble | S2-2 |
| `include/atx/engine/parallel/parallel_run.hpp` | `parallel_backtests(...)`, `parallel_cpcv(folds, ...)` → result table in fixed `(id, fold)` order; per-item isolated `Portfolio`/`ExecutionSimulator`/`RollingPanel` | S2-3 |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`)
`parallel_det_pool_test.cpp` + `parallel_digest_test.cpp` (S2-1), `parallel_batch_eval_test.cpp` (S2-2), `parallel_run_test.cpp` (S2-3), `parallel_determinism_test.cpp` (S2-4, the thread-count-invariance matrix). Bench: `bench/parallel_bench.cpp` (S2-4).

### 2.4 Ledger
`sprint-2-progress.md` (S2-0), updated per unit (copy the `sprint-1-progress.md` shape).

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Subject_Condition_ExpectedResult`; cover happy path, **boundaries** (n=0, n=1, n<n_workers, 1 worker == sequential, empty SignalSet), and **the determinism matrix** (digest equal for {1,2,4,8} workers AND equal to the single-thread reference).
- **Determinism (R1–R6):** NO cross-worker FP accumulation; fixed-order assemble; reduce-by-sort; strict-FP flags; no `hardware_concurrency()` in result math; `const` shared panel. The **digest-equality matrix is the load-bearing test** of the whole sprint.
- **No hot-path alloc:** per-worker `Engine` + per-worker `Arena` pre-warmed; steady-state batch allocates zero per worker (prove by a warm-run allocation check / documented arena sizing).
- **False sharing:** the result table is written by many workers — each writes a **disjoint, cache-line-distinct** region (pre-indexed slots; `alignas(std::hardware_destructive_interference_size)` on any per-worker shared counter struct). §4.4.
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). clang-tidy disabled — the strict build is the gate.
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures; weakest sufficient types (`std::span`, `const&`); functions ≤ ~60 lines; `// SAFETY:` on every concurrency deviation (memory order, lifetime, aliasing); Rule of Five on the pool (it owns threads). Reuse atx-core (`MpmcQueue`/`Arena`/`hash_*`/`Xoshiro256pp`) — no new general-purpose primitive in the engine.
- **clangd noise:** ignore squiggles; only a real `cmake --build` + the determinism matrix are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree: combine/metrics.hpp,
alpha/vm.hpp (the STATEFUL Engine — one per worker!), alpha/panel.hpp (SignalSet), loop/backtest_loop.hpp,
and atx-core concurrent/mpmc_queue.hpp + arena.hpp + hash.hpp (the primitives you build on).

THIS SPRINT'S DOMINANT RISK IS A DETERMINISM BUG that hides until a different worker count or a different run.
Determinism is achieved BY CONSTRUCTION, not by a clever reduction:
  - MAP PATTERN: one worker computes each item FULLY into a pre-indexed output slot. NO writable shared state
    between items. The shared panel is `const`. (Research rule R2.)
  - FIXED-ORDER ASSEMBLE: walk outputs in canonical id order (alpha index / (alpha,fold) lexicographic),
    NEVER completion order. (R2.)
  - REDUCE-BY-SORT: combine per-item scalars by sorting on id then a sequential fold; never accumulate
    partials in completion order; no atomic FP add. (R3.)
  - NO `hardware_concurrency()` IN RESULT MATH; partition via an atomic work-index dispenser. (R4.)
  - STRICT FP: /fp:precise, -ffp-contract=off, no fast-math, identical across all builds. (R5.)
  - The `alpha::Engine` is STATEFUL (growing slot pool) — give each worker its OWN warm Engine. Sharing one
    across threads is a data race AND a determinism break.

The acceptance test is a DIGEST-EQUALITY MATRIX: the wyhash digest of the parallel output must equal the
single-thread digest AND be identical for worker counts {1,2,4,8}. A test that only checks "it ran" is vacuous.

No UB, no data races (prove race-freedom by construction: each task gets a `const` panel view + an exclusive
output span; justify every std::atomic memory order with a `// SAFETY:` comment). Header-only inline. Rule of
Five on anything owning threads. Functions ≤ ~60 lines. Build gate: cmake --build build --config Debug --target
atx-engine-tests (/W4 /permissive- /WX + /fp:precise) + ctest -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER
git add -A; after committing run `git show HEAD --stat` (only your files) + `git merge-base --is-ancestor HEAD
HEAD`; never touch atx-core/*; do not push.
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 The deterministic pool (S2-1) — atomic work-index dispenser

Data structures (atx-core `TaskPool`; engine fallback `DetPool` mirrors it):
```
TaskPool:
  workers      : array<std::thread, N>          // N persistent threads
  job          : { fn: (usize i, usize wid)->void ; n: usize }   // current parallel_for
  next_index   : std::atomic<usize>             // the dispenser (one cache line, alignas)
  remaining    : std::atomic<usize>             // completion counter
  exceptions   : array<exception_ptr, N>        // per-worker first-failure capture
  start/done   : condition_variable + mutex     // park/wake
```
`parallel_for(n, body)` algorithm (deterministic load balance, R4):
```
publish(job = {body, n}); next_index = 0; remaining = n
wake all workers
each worker wid loops:
    i = next_index.fetch_add(1, memory_order_relaxed)   // SAFETY: relaxed — index claim only;
                                                          // no data published through this counter
    if i >= n: park
    try   body(i, wid)            // body writes ONLY out[i]; reads panel as const
    catch capture (i) into exceptions[wid]   // keep lowest index
    if remaining.fetch_sub(1, acq_rel) == 1: signal done
main: wait(done); rethrow lowest-index exception if any
```
Why this is deterministic: which `i` a worker grabs is timing-dependent, but **`body(i)` writes only `out[i]`** and reads a `const` panel — so the *output* is independent of the grab order (R2/R4). 1 worker reduces to a sequential `for`.

### 4.2 Parallel batch-eval (S2-2) — the core

Single-thread baseline: `alpha::Engine eng(panel); SignalSet ss = eng.evaluate(program);` → `ss.alphas[r]` is root `r`'s date-major column.

Parallel design (map pattern + per-worker stateful Engine, §0.4):
```
parallel_evaluate(program, panel, pool) -> SignalSet:
  R = program.num_roots()
  SignalSet out; out.alphas.resize(R); out.dates=panel.dates; out.instruments=panel.instruments
  // one WARM Engine per worker (stateful slot pool; reused across items -> zero steady-state alloc)
  per_worker Engine engines[pool.n_workers()](panel)   // constructed once, pre-warmed
  pool.parallel_for(R, [&](usize r, usize wid):
      Engine& e = engines[wid]                          // SAFETY: each wid touches only its own Engine
      out.alphas[r] = e.evaluate_root(program, r)       // computes ROOT r's column into out.alphas[r]
  )                                                      // barrier
  return out   // out.alphas already in root order -> fixed-order assemble is free
```
**CSE / shared-DAG strategy (the spec's crux).** `compile_batch` produces ONE program with a shared sub-expression DAG (cross-alpha CSE). Evaluating a *single root* needs the sub-DAG reachable from it. Two strategies:
- **(A) Per-worker recompute (S2-2 v1 — ship this).** Each worker evaluates its assigned roots independently; shared sub-nodes are **recomputed** into that worker's own slot pool. Simplest, **trivially deterministic** (each root fully owned by one worker, no shared writable slots), correct. Cost: bounded redundant compute of shared nodes (the CSE benefit is partially lost across the shard boundary). Requires an `Engine::evaluate_root(program, r)` entry point that runs only the sub-DAG of root `r` (or, simplest of all, fall back to per-root `compile` if `evaluate_root` is too invasive — measure first).
- **(B) Two-phase compute-shared-once (documented optimization, NOT v1).** Phase 1: one pass computes every *shared* sub-node (in-degree>1 across roots) into a **read-only** published buffer. Barrier. Phase 2: `parallel_for` over roots, each reading the shared buffer (const) + computing its private tail. Recovers the CSE win; needs a publish barrier and a re-proof that the shared buffer is written before any reader starts (it is — the barrier guarantees it). Determinism still holds (shared nodes computed by a single deterministic pass; roots are pure maps over const data). **Defer to an S2-2b only if the bench shows the recompute cost is material.**

> v1 = strategy (A). Prove determinism first; optimize CSE second. The plan's exit criteria are met by (A).

### 4.3 Parallel backtests / CPCV folds (S2-3)

Each backtest / fold is **fully independent** (its own `Portfolio`, `ExecutionSimulator`, `RollingPanel` — no shared mutable state). Pure map pattern:
```
parallel_cpcv(folds, alpha_streams, cfg, pool) -> vector<FoldResult>:
  vector<FoldResult> out(folds.size())          // pre-indexed by fold id
  pool.parallel_for(folds.size(), [&](usize f, usize wid):
      // each fold: isolated sim state, reads alpha_streams (const) + folds[f] (const)
      out[f] = run_one_fold(folds[f], alpha_streams, cfg)   // writes only out[f]
  )
  return out                                    // already fold-id order
```
The only reduction is aggregating per-fold metrics into a summary (R3): **sort fold results by fold id (already ordered), then a sequential Neumaier fold** — never an atomic running sum. Identical bits for any worker count.

### 4.4 The determinism digest (S2-1) — the oracle

```
signal_set_digest(ss) -> u64:
  h = 0
  h = hash_combine(h, ss.dates, ss.instruments, ss.alphas.size())
  for a in 0..ss.alphas.size():                 // FIXED root order
      h = hash_combine(h, hash_bytes(ss.alphas[a].name.data(), name.size()))
      h = hash_bytes_into(h, ss.alphas[a].values)   // raw f64 bytes, fixed order
  return h
```
Reuses `atx::core::hash_bytes` (wyhash) + `hash_combine`. Hashing **raw f64 bytes** makes the test bit-exact (a 1-ULP difference flips the digest). `result_table_digest` is the analogue over `(alpha_id, fold_id)`-ordered `FoldResult` rows. **This digest is the single source of truth for "parallel == sequential."**

### 4.5 Memory & locality (research §Q2–Q5, applied)
- **Per-worker `Engine` + `Arena`** (atx-core `Arena`, one per worker, `reset()` between items) ⇒ zero steady-state heap alloc per worker. Pre-size the arena from a serial peak-usage pass + 20% headroom.
- **False sharing:** workers write **disjoint** `out.alphas[r]` vectors (separate heap allocations — no sharing). Any per-worker shared scalar (e.g. a progress counter) is `struct alignas(std::hardware_destructive_interference_size) { ... };` per worker. Reserve `out.alphas` to final size **before** the parallel region (no concurrent vector growth).
- **NUMA:** the shared panel is read-only. **Skip NUMA tuning in v1** — for an L3-resident panel the win is negligible; the real win is per-worker write locality (each worker's `out.alphas[r]` + arena are first-touched by the worker). Record "NUMA panel partitioning" as future work, gated on a profile showing cross-socket traffic.
- **Data layout:** the panel is already column-major (`PanelView`); SignalSet columns are date-major contiguous — SIMD-friendly, no change needed.

---

## §5 — Per-unit plan

> Sequential dispatch. S2-1 (pool + digest) is the substrate S2-2/S2-3 consume; S2-4 is the proof+bench+close. Each unit: fresh implementer → spec-compliance review → code-quality review → fix loop → ledger SHA, per `superpowers:subagent-driven-development`. **The shared branch `feat/atx-core-stdlib` requires explicit-pathspec commits (see handoff block).**

### Task S2-0: Marker + ledger + parallel scaffold
**Files:** Create `atx-engine/plans/p1/sprint-2-progress.md`, `atx-engine/include/atx/engine/parallel/fwd.hpp`.
- [ ] **Step 1:** Write the ledger from the `sprint-1-progress.md` shape: header (`Base: feat/atx-core-stdlib @ <HEAD>`, in-place, shared-branch note), a "Plan adjustments vs source" paragraph quoting §0 (backbone=hand-rolled, atx-core primitives exist, Engine-stateful, determinism-by-construction), empty per-unit table S2-0…S2-4, empty commits table. **Record the atx-core `TaskPool` Pattern-B dependency + the engine-local fallback as the kickoff risk.**
- [ ] **Step 2:** `parallel/fwd.hpp` — forward decls (the `combine/fwd.hpp` pattern): namespace `atx::engine::parallel`; `class DetPool; struct FoldResult;` + doc block listing the per-unit headers + the `using Pool = atx::core::concurrent::TaskPool` switch point.
- [ ] **Step 3:** Commit (marker): `git add -- <the two files>; git commit -- <them> -m "docs(s2-0): open sprint-2 parallel-compute ledger + scaffold" -m "Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"; git show HEAD --stat`.

### Task S2-1: Deterministic pool (fallback) + digest oracle
**Files:** Create `parallel/det_pool.hpp`, `parallel/digest.hpp`; Test `tests/parallel_det_pool_test.cpp`, `tests/parallel_digest_test.cpp`.
**Scope:** the §2.1 `TaskPool` API as an engine-local `DetPool` (fallback per §0.3 — same API, so the eventual atx-core pool is a one-line `using` swap), built on `std::thread` + an atomic work-index dispenser (§4.1); plus the §4.4 digest. (Do NOT modify atx-core; the upstream `TaskPool` request is recorded in the ledger.)
- [ ] **Step 1 (det_pool tests):** suite `ParallelDetPool` —
```cpp
TEST(ParallelDetPool, ProcessesEveryIndexExactlyOnce) {
  DetPool pool{4};
  std::vector<std::atomic<int>> hits(1000);
  pool.parallel_for(1000, [&](std::size_t i, std::size_t){ hits[i].fetch_add(1); });
  for (auto& h : hits) EXPECT_EQ(h.load(), 1);
}
TEST(ParallelDetPool, OneWorkerEqualsSequential) {
  DetPool p1{1}, p4{4};
  std::vector<double> a(500), b(500);
  auto fill = [](std::size_t i){ return std::sin(0.1*double(i)); };
  p1.parallel_for(500, [&](std::size_t i, std::size_t){ a[i]=fill(i); });
  p4.parallel_for(500, [&](std::size_t i, std::size_t){ b[i]=fill(i); });
  EXPECT_EQ(a, b);                                  // fixed-slot write -> identical
}
TEST(ParallelDetPool, EmptyAndSingle) {
  DetPool pool{4};
  pool.parallel_for(0, [&](std::size_t, std::size_t){ FAIL(); });   // n=0 -> no calls
  int x=0; pool.parallel_for(1, [&](std::size_t, std::size_t){ x=42; }); EXPECT_EQ(x,42);
}
TEST(ParallelDetPool, ExceptionRethrownLowestIndexFirst) {
  DetPool pool{4};
  EXPECT_THROW(pool.parallel_for(100, [&](std::size_t i, std::size_t){
    if (i==7 || i==30) throw std::runtime_error("boom"); }), std::exception);
}
TEST(ParallelDetPool, WorkerIdInRange) {
  DetPool pool{4};
  std::vector<std::atomic<int>> seen(4);
  pool.parallel_for(4000, [&](std::size_t, std::size_t wid){ ASSERT_LT(wid,4U); seen[wid].fetch_add(1); });
}
```
- [ ] **Step 2:** Build → FAIL.
- [ ] **Step 3:** Implement `det_pool.hpp` (§4.1). Rule of Five (owns threads); `// SAFETY:` on every memory order (relaxed dispenser claim, acq_rel completion); clean shutdown joins all workers in the destructor; exceptions captured per-worker, lowest-index rethrown after the barrier. n_workers default = `max(1, hardware_concurrency()-2)` — note: `hardware_concurrency` may appear in *pool sizing* but **never in result math** (R4).
- [ ] **Step 4:** `ctest -R ParallelDetPool` → pass.
- [ ] **Step 5 (digest tests):** suite `ParallelDigest` —
```cpp
TEST(ParallelDigest, IdenticalSignalSetsHashEqual) {
  SignalSet a = make_fixture(); SignalSet b = a;
  EXPECT_EQ(signal_set_digest(a), signal_set_digest(b));
}
TEST(ParallelDigest, OneUlpDifferenceFlipsDigest) {
  SignalSet a = make_fixture(); SignalSet b = a;
  b.alphas[0].values[3] = std::nextafter(b.alphas[0].values[3], 1e9);  // 1 ULP
  EXPECT_NE(signal_set_digest(a), signal_set_digest(b));               // bit-exact oracle
}
TEST(ParallelDigest, RootOrderMatters) {
  SignalSet a = make_two_alpha_fixture(); SignalSet b = a;
  std::swap(b.alphas[0], b.alphas[1]);
  EXPECT_NE(signal_set_digest(a), signal_set_digest(b));               // order-fixed
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement `digest.hpp` (§4.4) over `atx::core::hash_bytes`/`hash_combine`. **Step 8:** `ctest -R "ParallelDetPool|ParallelDigest"` → pass; full suite green.
- [ ] **Step 9:** Commit + ledger row (`feat(s2-1): deterministic pool (fallback) + digest oracle`).

### Task S2-2: Parallel batch-eval
**Files:** Create `parallel/batch_eval.hpp`; Test `tests/parallel_batch_eval_test.cpp`.
**Scope:** §4.2 — `parallel_evaluate(program, panel, pool) -> Result<SignalSet>`, per-worker warm `Engine`, sharded roots (strategy A), fixed-order assemble. **The digest MUST equal `Engine::evaluate`'s.** First verify the exact `Engine`/`evaluate` API + whether an `evaluate_root` exists (if not, the simplest correct v1 evaluates the *whole* program per worker but keeps only its assigned roots, OR compiles per-root sub-programs — measure; document the choice).
- [ ] **Step 1:** failing tests, suite `ParallelBatchEval` —
```cpp
TEST(ParallelBatchEval, MatchesSingleThreadDigest) {
  auto [prog, panel] = make_alpha_battery(/*n_alphas=*/32);
  alpha::Engine eng(panel); auto seq = eng.evaluate(prog).value();
  DetPool pool{4};
  auto par = parallel_evaluate(prog, panel, pool).value();
  EXPECT_EQ(signal_set_digest(par), signal_set_digest(seq));   // byte-identical
}
TEST(ParallelBatchEval, ThreadCountInvariant) {
  auto [prog, panel] = make_alpha_battery(32);
  std::uint64_t ref = 0;
  for (std::size_t w : {1U,2U,4U,8U}) {
    DetPool pool{w};
    auto d = signal_set_digest(parallel_evaluate(prog, panel, pool).value());
    if (w==1U) ref = d; else EXPECT_EQ(d, ref);                // identical for all worker counts
  }
}
TEST(ParallelBatchEval, EmptyProgramAndSingleRoot) { /* R=0 -> empty SignalSet; R=1 -> one column */ }
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `batch_eval.hpp` (§4.2 strategy A). Per-worker `Engine` array sized to `pool.n_workers()`, each constructed on the panel and warmed once (e.g. a `for_each_worker` warm-up eval) so the steady-state loop allocates nothing. `out.alphas.resize(R)` BEFORE the parallel region (no concurrent growth). `// SAFETY:` each `wid` touches only `engines[wid]` and writes only `out.alphas[r]`.
- [ ] **Step 4:** `ctest -R ParallelBatchEval` → pass (incl. the {1,2,4,8} matrix). Full suite green.
- [ ] **Step 5:** Commit + ledger row (`feat(s2-2): parallel batch-eval (digest == single-thread)`). If strategy (A)'s redundant-CSE cost looks material, note an **S2-2b two-phase** follow-up (§4.2 B) as a residual.

### Task S2-3: Parallel backtests / CPCV folds
**Files:** Create `parallel/parallel_run.hpp`; Test `tests/parallel_run_test.cpp`.
**Scope:** §4.3 — `parallel_cpcv(folds, streams, cfg, pool)` + `parallel_backtests(...)`, each item an isolated sim, results in fixed `(id, fold)` order; per-fold scalar aggregation by reduce-by-sort (R3). Reuses S1 `eval::cpcv_folds` output + the loop/backtest types. Verify the exact `BacktestLoop`/`BacktestResult`/`Portfolio` ctors are copyable-into-per-item or constructible-per-item (no shared mutable state).
- [ ] **Step 1:** failing tests, suite `ParallelRun` —
```cpp
TEST(ParallelRun, CpcvDigestMatchesSequential) {
  auto folds = eval::cpcv_folds(make_spans(60), {6,2,0.0});
  auto streams = make_streams();
  auto seq = sequential_cpcv(folds, streams, cfg);          // reference
  DetPool pool{4};
  auto par = parallel_cpcv(folds, streams, cfg, pool);
  EXPECT_EQ(result_table_digest(par), result_table_digest(seq));
}
TEST(ParallelRun, ThreadCountInvariant) { /* {1,2,4,8} -> identical result_table_digest */ }
TEST(ParallelRun, AggregateIsReduceBySort) {
  // per-fold mean computed by sort-by-id sequential fold == sequential reference (bit-exact)
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `parallel_run.hpp` (§4.3). Each fold gets its own isolated sim state (constructed inside the task or from a per-worker pool of sim objects). Aggregation: collect `out[f]` (already fold-id ordered), then a **sequential** Neumaier fold — no atomic FP. `// SAFETY:` per-item isolation (no shared `Portfolio`/`Panel` mutation).
- [ ] **Step 4:** `ctest -R ParallelRun` → pass (incl. matrix). Full suite green. **Step 5:** Commit + ledger row (`feat(s2-3): parallel backtests + CPCV folds (digest == sequential)`).

### Task S2-4: Determinism-under-parallelism proof + bench + close
**Files:** Create `tests/parallel_determinism_test.cpp`, `bench/parallel_bench.cpp`; Modify ledger + `ROADMAP.md` (S2 row) + spec (closed) + create `sprint-2.md` user ref.
**Scope:** §S2.4 — the load-bearing cross-cutting proof, the throughput bench, the close.
- [ ] **Step 1:** `parallel_determinism_test.cpp`, suite `ParallelDeterminism` — the **full matrix** as one stress test: for batch-eval AND CPCV, assert digest is identical for workers {1,2,4,8} AND equal to the pure single-thread path, on a large fixture (many small alphas — the worst case for races). Add a **run-to-run stability** loop (same worker count, K repeats → identical digest). Document the **TSan-on-Linux residual** (clang-cl has no TSan; race-freedom is argued by construction — `const` panel + exclusive output spans — and validated by the matrix; an optional Linux CI TSan job is the upstream check).
- [ ] **Step 2:** Build → run → all green.
- [ ] **Step 3:** `bench/parallel_bench.cpp` — throughput (alphas/sec) vs worker count {1,2,4,8,physical}; record the **actual speedup curve** (do NOT claim ideal — note Amdahl + the per-worker Engine warm cost + strategy-A redundant CSE). Wire into `atx-engine-bench` (CONFIGURE_DEPENDS).
- [ ] **Step 4:** Build the bench; capture the curve into the ledger.
- [ ] **Step 5: Sprint close** (per `../docs/sprint.md`): fill the ledger (per-unit rows, commits table, the speedup curve, "What S2 proves / baton"); lift residuals to the ROADMAP backlog — **(a) the atx-core `TaskPool` Pattern-B lift** (engine-local `DetPool` → atx-core L4), **(b) S2-2b two-phase CSE** if deferred, **(c) NUMA panel partitioning** (profile-gated), **(d) Linux-CI TSan job**; flip `p1/ROADMAP.md` S2 row `⏳ → ✅ <sha>` + bump Last-reviewed; mark `sprint-2-parallel-compute.md` `Status: ✅ closed`; create `sprint-2.md` user reference (the `parallel::` public API + the determinism guarantees + the backbone decision).
- [ ] **Step 6:** Commit close (explicit pathspecs; `git show HEAD --stat`).

---

## §6 — Exit criteria · invariants · dependencies · NOT-in-scope · baton

**Exit criteria:**
- Parallel batch-eval of a ≥32-alpha battery → `SignalSet` **byte-identical** to `Engine::evaluate` for workers {1,2,4,8} (digest equal — thread-count invariance).
- Parallel backtests / CPCV folds → result table byte-identical to sequential, {1,2,4,8}.
- Per-fold scalar aggregation is reduce-by-sort, bit-identical to the sequential Neumaier fold.
- Steady-state per-worker zero heap alloc (per-worker `Engine` + `Arena` warm; documented sizing).
- Race-free **by construction** (`const` panel + exclusive output spans); run-to-run stable; TSan-on-Linux recorded as a residual.
- Throughput bench recorded — **actual** speedup curve up to physical cores (no ideal-speedup claim).
- `/W4 /permissive- /WX` **+ /fp:precise** clean; new `*_test.cpp` per unit; full suite stays green.

**Invariants proven:** determinism survives parallelism (digest == single-thread, {1,2,4,8} invariant); no cross-worker FP accumulation (R1/R2); reduce-by-sort (R3); no hot-path alloc (per-worker arenas); race-freedom by construction (R6).

**Dependencies:** Upstream p0 — `alpha::Engine`/`Program`/`SignalSet`/`compile_batch` (P3), `BacktestLoop`/`BacktestResult`/`Portfolio`/`ExecutionSimulator` (P2), S1 `eval::cpcv_folds`. atx-core — **existing** `concurrent/mpmc_queue`, `arena`, `hash`, `random`; **Pattern-B request:** `concurrent/task_pool.hpp` (the deterministic pool — engine-local `DetPool` fallback ships S2 if it slips, lifted upstream as a residual). **No P4 dependency** — S2 opens concurrently with S1/S6.

**Explicitly NOT in this sprint:** no distributed/multi-machine (anti-roadmap #4); no GPU; no JIT/computed-goto; no oneTBB/Taskflow (evaluated, rejected §0.2 — revisit only if nested/irregular numeric kernels emerge); no lock-free heroics beyond the atomic dispenser; no NUMA tuning v1 (profile-gated future work); no RNG (S3's concern — R7 records the rule).

**Baton → next:** S2 hands **S3** the throughput to evaluate a mined population per generation across cores, and **S1** the parallelism to run CPCV folds concurrently — both **without changing a single result digit**. The `parallel::Pool` + digest oracle are reused by S3/S5; the determinism matrix is the template every later parallel feature re-proves against.

---

## §7 — References (open-source web research)

**Floating-point determinism / reproducibility**
- [G] Goldberg, *What Every Computer Scientist Should Know About Floating-Point Arithmetic* (ACM 1991) — FP non-associativity. https://docs.oracle.com/cd/E19957-01/806-3568/ncg_goldberg.html
- [I1] Intel, *Consistency of Floating-Point Results* — changing compute-unit count reorders additions → different result; strict-FP flags. https://www.intel.com/content/dam/develop/external/us/en/documents/pdf/fp-consistency-121918.pdf
- [I2] Intel oneMKL CNR — conditional numerical reproducibility (fixed thread count, deterministic algorithms). https://www.intel.com/content/www/us/en/docs/onemkl/developer-guide-linux/2023-0/obtaining-numerically-reproducible-results.html
- [RB] ReproBLAS (Berkeley) — reproducible independent of processor count/partition/scheduling; binned accumulator. https://bebop.cs.berkeley.edu/reproblas/
- [DN] Ahrens, Demmel, Nguyen, *Algorithms for Efficient Reproducible Floating Point Summation* (ACM TOMS 2020). https://people.eecs.berkeley.edu/~demmel/ma221_Fall23/J115_Efficient_Reproducible_Summation_TOMS_2020.pdf
- Kahan/Neumaier compensated summation. https://en.wikipedia.org/wiki/Kahan_summation_algorithm · Pairwise summation. https://en.wikipedia.org/wiki/Pairwise_summation
- [BR] *Bit-Reproducible Phylogenetic Inference under Varying Core-Counts* (2025) — validate bit-identity across core counts. https://www.biorxiv.org/content/10.1101/2025.06.02.656320.full.pdf
- *Impacts of FP non-associativity on reproducibility for HPC/DL* (arXiv 2408.05148). https://arxiv.org/pdf/2408.05148v1

**Patterns / scheduling**
- [EP] Embarrassingly-parallel pattern (UF Parallel Patterns). https://www.cise.ufl.edu/research/ParallelPatterns/PatternLanguage/AlgorithmStructure/EmbParallel.htm
- Almost Deterministic Work Stealing (ADWS). https://www.researchgate.net/publication/337109439_Almost_deterministic_work_stealing
- OpenMP static vs dynamic schedule (determinism). https://saurabhsoodweb.wordpress.com/2017/03/05/static-vs-dynamic-scheduling-openmp/
- Grain-size trade-off (POP CoE). https://co-design.pop-coe.eu/best-practices/chunk-task-grain-size-trade-off.html

**Memory / locality**
- False sharing + MESI + `hardware_destructive_interference_size`. https://www.studyplan.dev/concurrency-vectorization/cache-coherency-false-sharing
- `std::pmr::monotonic_buffer_resource` deep dive (null upstream, pre-size, reserve). https://badlydrawnrod.github.io/posts/2021/12/30/monotonic_buffer_resource/ · cppreference. https://en.cppreference.com/w/cpp/memory/monotonic_buffer_resource.html
- PMR for quants (per-tick arena reset, zero steady-state alloc). https://cppforquants.com/pmr-containers-clean-memory-management-in-c/
- Thread affinity (4× speedup, L1 miss 51%→2.8%). https://coffeebeforearch.github.io/2020/05/27/thread-affinity.html
- NUMA first-touch (HPC Wiki). https://hpc-wiki.info/hpc/NUMA · NUMA deep dive (read-only cross-socket snoops). http://staroceans.org/cache_coherency.htm · Windows NUMA API. https://learn.microsoft.com/en-us/windows/win32/procthread/numa-support
- SoA vs AoS for analytical scans (3–4×). https://cedardb.com/blog/optimizing_data_layouts/ · https://en.wikipedia.org/wiki/AoS_and_SoA

**Backbone candidates (decision §0.2)**
- [1] oneTBB `parallel_deterministic_reduce` spec (same splits/joins regardless of thread count; "may differ from sequential"). https://oneapi-spec.uxlfoundation.org/specifications/oneapi/v1.4-rev-1/elements/onetbb/source/algorithms/functions/parallel_deterministic_reduce_func
- oneTBB `static_partitioner` (deterministic chunking). https://oneapi-spec.uxlfoundation.org/specifications/oneapi/v1.1-rev-1/elements/onetbb/source/algorithms/partitioners/static_partitioner · Partitioner summary. https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/Partitioner_Summary.html
- oneTBB memory (`cache_aligned_allocator`, `scalable_allocator`). https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/Memory_Allocation.html
- [10] oneTBB static-linking discouraged (scheduler singleton). https://github.com/oneapi-src/oneTBB/issues/297 · Issue #1770 static_partitioner vs global_control. https://github.com/uxlfoundation/oneTBB/issues/1770
- Taskflow (header-only, MIT, C++20). https://github.com/taskflow/taskflow · iterations/partitioners. https://taskflow.github.io/taskflow/ParallelIterations.html · non-deterministic order. https://taskflow.github.io/taskflow/ExecuteTaskflow.html
- OpenMP reduction order unspecified. https://www.openmp.org/spec-html/5.0/openmpsu107.html
- `std::reduce`/PSTL arbitrary order. https://www.cppstories.com/2018/06/parstl-tests/
- HPX `par` non-deterministic order. https://hpx-docs.stellar-group.org/latest/html/manual/writing_single_node_hpx_applications.html · build weight. https://hpx-docs.stellar-group.org/latest/html/manual/building_hpx.html
- NVIDIA ISO C++ parallelism (quant finance pre-indexed output pattern). https://developer.nvidia.com/blog/how-to-accelerate-quantitative-finance-with-iso-c-standard-parallelism/

**Testing / RNG**
- [TS] Clang ThreadSanitizer (Linux/macOS only; no Windows). https://clang.llvm.org/docs/ThreadSanitizer.html · https://discourse.llvm.org/t/threadsanitizer-support-on-windows/67352
- [R123] Random123 counter-based RNG (Philox/Threefry; stateless, parallel-stream-independent). https://www.thesalmons.org/john/random123/releases/latest/docs/index.html · C++20 `std::barrier`. https://en.cppreference.com/cpp/thread/barrier

---

## §8 — Self-review (against the spec)

- **Spec coverage:** S2.0→S2-0, S2.1→S2-1 (pool+digest), S2.2→S2-2, S2.3→S2-3, S2.4→S2-4. Every spec unit + exit criterion maps to a task. ✅
- **User asks satisfied:** data structures (`TaskPool`/`DetPool`, `WorkerArena`, the digest, `FoldResult`) ✅; algorithms + pseudocode (§4.1–§4.5) ✅; web-research references (§7, ~40 sources) ✅; HPC design (false sharing, arenas, NUMA, SoA, work-index dispenser) ✅; TBB evaluated as backbone — **decided AGAINST** with evidence (§0.2), per "prioritize research not guessing" ✅.
- **Determinism rigor:** the load-bearing acceptance is the {1,2,4,8} digest matrix == single-thread; map pattern + fixed-order assemble + reduce-by-sort + strict-FP encode R1–R6; the `Engine`-is-stateful finding (§0.4) is called out as the #1 correctness risk. ✅
- **Stale-spec fixes applied:** atx-core primitives already exist (not a from-scratch L4) §0.1; backbone hand-rolled not TBB §0.2; pool in atx-core Pattern-B + engine fallback §0.3; Engine-per-worker §0.4; determinism strategy concrete §0.5; `parallel::` namespace + flat tests §0.6. ✅
- **Type consistency:** `TaskPool`/`DetPool::parallel_for(n, body(i,wid))`, `signal_set_digest`/`result_table_digest`, `parallel_evaluate`, `parallel_cpcv`/`parallel_backtests`, `FoldResult` — consistent across §2/§4/§5. ✅

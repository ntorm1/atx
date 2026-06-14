# Sprint S7 (p2) — Deterministic Multi-Core Execution Substrate (Scale-Out-Ready) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the
> FROZEN *how*; the **what** is the S7 section of [`ROADMAP.md`](ROADMAP.md#s7--distributed--scale-out-compute--proposed).
> On conflict, **§0 (this plan's as-built amendment) overrides** the ROADMAP sketch.

**Goal:** Lift `p1` S2's single-substrate `parallel::DetPool` into a **substrate-agnostic `IExecutor` abstraction** whose
**primary implementation is a deterministic multi-process shared-memory pool** that saturates every core on one machine,
generalizing the proven *pure-map → pre-indexed-slot → reduce-by-sort* determinism recipe from threads to **processes**
without weakening a single bit of the digest contract. The whole pipeline — batch-eval, CPCV, per-alpha backtests, **and
the GA mine→admit→replay loop + library** — runs over the one seam. The abstraction is built so that the leap to **N
machines** changes **only the placement handle** (the Ray-`init` / Dask-`Client` / Spark-`master` / HPX-AGAS / Legion-
mapper pattern): the `WorkUnit` type, the `submit`/`gather` API, the slot layout, the reduce-by-sort order, and the
digest are all transport-agnostic. This sprint ships **threads (degenerate 1-process) + multi-process shared-memory on
both Windows and POSIX**; the **network transport is interface-only + a recorded atx-core L4 lift**. This is the `p2`
infra spine — it needs only `p1` S2 and runs independently of the S1→S2 operate-the-book track.

**Architecture:** One seam, three substrates, behind `parallel/fwd.hpp`'s existing `using Pool = …;` **SWITCH POINT**.
New headers in the existing `parallel/` layer (namespace `atx::engine::parallel`):
`parallel/executor.hpp` (the `IExecutor` interface + the `WorkUnit`/`ShardId`/`OutputSlots` model + the deterministic
`gather` contract), `parallel/thread_executor.hpp` (the `ThreadExecutor` — a thin adapter wrapping the as-built
`DetPool`; the in-process, 1-process degenerate case), `parallel/shm_segment.hpp` (+ `.cpp`) (the cross-platform
read-only-input + pre-indexed-output shared-memory segment — `CreateFileMapping`/`MapViewOfFile` and
`shm_open`/`mmap` behind one seam), `parallel/process_executor.hpp` (+ `.cpp`) (the `ProcessExecutor` — the **primary**
multi-process pool over the SHM substrate), and `parallel/scheduler.hpp` (the deterministic heterogeneous-cost dispatcher
+ NUMA/affinity/topology). The **boundary pin** that holds the sprint honest: the run **digest** must be **byte-identical
across `{ThreadExecutor@1 == ThreadExecutor@N == ProcessExecutor@1 == ProcessExecutor@N}` and equal to the as-built
single-thread sequential path** — the regression anchor the `p1` S2 `signal_set_digest`/`result_table_digest` oracles
already enforce, now extended across the process boundary.

**Tech Stack:** C++20, header-only inline where the as-built `parallel/` layer is (`#pragma once`), with compiled `.cpp`
for the OS-touching units (SHM segment, process spawn) per the recent header→source refactor (S449/S452). Namespace
`atx::engine::parallel`. Reuses `parallel::{DetPool, parallel_evaluate, parallel_cpcv, parallel_backtests, FoldResult,
result_table_digest, cpcv_aggregate_mean_sharpe, signal_set_digest}`, `data::{ShmBarFeed}` + `tsdb::SegmentReader` (the
existing zero-copy SHM read substrate), `factory::{Factory, ResearchDriver, detail::seed_for_run}`,
`library::Library`, `alpha::{Program, Panel, SignalSet, Engine}`, `eval::CpcvFold`, `atx::core::{hash_bytes,
hash_combine, Result, Status, types}`. GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake
edit). clang-cl `/W4 /permissive- /WX` **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). Build + ctest are the
gates; clang-tidy disabled (noise). **Cross-platform:** the SHM + process units carry both a Win32 and a POSIX backend,
selected at compile time; both are CI-built.

---

## §0 — As-built reconciliation amendment (the recon fixes)

> **Recon target (kickoff):** the merged `p1` S1–S8 engine + the recent `parallel/` header→source refactor (observations
> S452, `parallel_run.cpp`/`batch_eval.cpp` extracted to compiled impls; commit `f85f3d3` line). This sprint is cut from
> `main`. Reconnaissance against the as-built `parallel/` + `factory/` + `library/` layers surfaces the load-bearing
> corrections below; each changes a unit's scope. **Run the recon as the first act of S7-0** and amend these notes
> against the *actual* merged SHAs before dispatching S7.1.

### 0.1 `DetPool` is a single-substrate, single-level, atomic-dispenser pool — S7 lifts it behind a seam, never rewrites it
The as-built `parallel::DetPool` (`parallel/det_pool.hpp`) is N persistent `std::thread`s parking on a condition
variable; `parallel_for(n, body)` dispenses indices via a single atomic `next_index_.fetch_add` (no queue, **no
work-stealing**), barriers on a `busy_` counter, and rethrows the lowest-index exception deterministically. Its
determinism contract is explicit and **substrate-agnostic in spirit**: *"which worker grabs which index is
timing-dependent and MUST NOT affect any result… the caller writes only to its own `out[i]` slot… byte-identical across
`{1,2,4,8}` workers BY CONSTRUCTION."* The header itself names the lift: *"the engine will later consume the SAME API
through a single `using Pool = …;` alias, so the swap is one line (see `fwd.hpp` SWITCH POINT)."* **Decision:** S7.1
defines `IExecutor` as that API surface, makes `DetPool` the body of a thin `ThreadExecutor`, and points the `fwd.hpp`
SWITCH POINT at `IExecutor`. `DetPool` is **kept verbatim** as the `ThreadExecutor` engine and the in-process oracle;
S7 adds substrates beside it, it does not replace it. The dispenser is single-level (a flat `[0,n)`); the heterogeneous
+ NUMA-aware scheduler (S7.4) is additive.

### 0.2 The determinism recipe is already substrate-agnostic — S7 generalizes it to processes, it does not reinvent it
The proven recipe across `parallel_run.cpp` / `batch_eval.cpp` is three rules: **(a) pure map** — each fold/backtest/
program is computed fully by one worker into a **pre-indexed `out[i]` slot**, const reads, **no writable shared state**;
**(b) fixed-order assemble** — `out` is sized *before* the parallel region and item `i` writes *only* `out[i]`, so the
table is in canonical slot order, never completion order; **(c) reduce-by-sort** — the one cross-item reduction
(`cpcv_aggregate_mean_sharpe`) stable-sorts rows by `(alpha_id, fold_id)` then does a **sequential Neumaier
(compensated) fold**, *"NEVER an atomic running sum and NEVER a partial combine in completion order."* **This is exactly
the pattern the determinism literature prescribes** (§1A B-track): decouple result *placement* from execution *order*
(TBB `parallel_deterministic_reduce`), never atomic-add floats (PyTorch reproducibility note), fix the float-combination
order because IEEE-754 `+` is non-associative (Goldberg). **Decision:** S7 lifts (a)/(b)/(c) **verbatim** to the process
substrate — pre-indexed slots become pre-indexed **shared-memory** slots; the reduce-by-sort runs in the **parent** over
the gathered slots. `result_table_digest` / `signal_set_digest` are reused unchanged as the cross-substrate oracle.
**Nothing about the math changes; only the slots' address space does.**

### 0.3 `signal_set_digest` is intra-process — but same-binary/same-machine ⇒ cross-process stable; the merge always digests in ONE process
`parallel/digest.hpp` warns: *"`hash_bytes` is deterministic only WITHIN a process (its wyhash seeds are compile-time
constants and endianness is not normalized)."* The seeds being **compile-time constants** means the **same binary** on
the **same machine** produces the **same digest for the same bytes** in *different processes* (the constants and
endianness are identical across instances of one binary). **Decision:** the digest-invariance proof (S7.6) is sound on
one machine **because the merge gathers raw `f64` result *bytes* from the worker slots into the parent and the parent
computes the digest** — the digest is always evaluated in a single process over the assembled canonical-order table, so
the cross-process question never arises. A worker process never hashes; it only writes raw `f64` bytes into its slot.
**The cross-*machine* digest** (a different binary / a different endianness) needs a canonical-byte-order, seed-fixed
digest — a recorded **lift** carried with the network transport (§2.1), out of scope this sprint.

### 0.4 `library::admit` is the lone process hazard — partition-per-worker + canonical manifest merge is the fix; the mine-loop seed is already process-safe
The factory/library hazard map (recon): `factory::detail::seed_for_run(master, run)` (`research_driver.cpp`) is a
**pure SplitMix mix of `(master_seed, run)`** — no atomics, no wall-clock, no worker-id — so the **mining axis is already
process-safe** (any worker count yields the same per-run seed; composes with the search driver's `(gen, idx)` axis).
The hazard is **`library::Library::admit`** (`library/library.hpp`): it mutates an in-process `memtable_pending_` counter,
calls `store_.stage(...)` which **advances a global `AlphaId`**, and the correlation index (`ensure_corr` /
`rebuild_corr_index`) **scans candidates in `AlphaId` order** seeded from `master_seeds.front()`. Across processes these
are not safe. **Decision:** S7.5 adds `library::partition` — each worker process admits into a **partition-local
`Library`** over a **disjoint `AlphaId` range** (worker `w` owns `[w·stride, (w+1)·stride)`), the dedup gate runs
partition-local, and the parent **merges the partition manifests in canonical global-`AlphaId` order** post-run
(content-addressed `version_id` finalized over the merged, sorted entries — exactly `snapshot()`'s existing form). The
correlation-neighbor index is rebuilt **once on the merged library** in `AlphaId` order (the deterministic path), with a
per-partition local index + query-time neighbor merge recorded as the scale lift. **The mine loop submits via
`IExecutor`; the library merge is the only genuinely-new orchestration.**

### 0.5 The boundary pin is digest-invariance across substrate × worker-count — the load-bearing regression
**Decision:** the single most important test of S7 is the **substrate-invariance pin**: for a fixed input (panel +
program set, or a fixed mine config + seed), the run **digest** (`signal_set_digest` for eval, `result_table_digest` for
folds/backtests, the merged `library` `version_id` for mining) must be **byte-identical** across **all five paths**:
`{sequential as-built, ThreadExecutor@1, ThreadExecutor@N, ProcessExecutor@1, ProcessExecutor@N}`. This pins the new
substrates to the proven layer (`p1` S2 already proves `digest(parallel)==digest(sequential)` invariant across
`{1,2,4,8}` threads; S7 extends the equality across the process boundary). It is mandatory for S7.3 (the
`ProcessExecutor` matches the thread path) **and** S7.6 (the full pipeline, including the mine→admit→merge loop, matches).
If a workload cannot be made bit-identical across the boundary (e.g. a third-party kernel that reduces in steal-order),
it is **dispatched to the sequential reduce-by-sort path** and the divergence is recorded as a §0 decision, never a
silent drift.

### 0.6 ProcessExecutor is primary; threads are the degenerate 1-process case — ship BOTH Win32 and POSIX SHM backends
Per the kickoff decision (multi-process IPC first), the **`ProcessExecutor` over shared memory is the shipping primary
substrate**; `ThreadExecutor` is the **degenerate 1-process** case (and the in-process oracle the process path is checked
against). **Decision:** the SHM substrate (S7.2) carries **both** a **Win32** backend (`CreateFileMapping` /
`MapViewOfFile` / `OpenFileMapping` named segments, paging-file-backed via `INVALID_HANDLE_VALUE`) **and** a **POSIX**
backend (`shm_open` / `ftruncate` / `mmap` over `/dev/shm`), selected at compile time behind one `ShmSegment` seam; both
are CI-built and pass `shm_segment_test`. Read-only inputs are mapped `PROT_READ` / `FILE_MAP_READ` so an accidental
worker write **faults** rather than silently diverging (the OS-enforced read-only invariant). The **network transport**
(cross-machine) is **interface-only** this sprint — the `IExecutor` seam admits a future `DistributedExecutor`, recorded
as the atx-core L4 `dist_pool` lift (§2.1).

### 0.7 Shard cost is heterogeneous — dynamic dispatch for load balance, pre-indexed placement for determinism (decouple the two)
Backtests/alphas/folds differ in runtime by **orders of magnitude** (a deep GA expression vs a trivial one; a long fold
vs a short one). A static partition stalls on the slowest shard's worker (the OpenMP-`static` / TBB-`static_partitioner`
"without load balancing" cost, §1A §2); the literature's answer is **dynamic work-stealing for scheduling** (Blumofe-
Leiserson `T₁/P + O(T∞)`) **with result placement decoupled from steal order** (TBB `parallel_deterministic_reduce`).
**Consequence:** the as-built `DetPool` atomic dispenser **already** does dynamic dispatch (work is *claimed* by
`fetch_add`, so a fast worker grabs more items) **with** pre-indexed placement (`out[i]` is data-defined) — it is
**already the correct shape**. S7.4 keeps that shape and adds **heterogeneous-cost ordering** (dispatch the
known-expensive shards first to shorten the tail — a *scheduling* hint that **cannot** touch the bits) + **NUMA/affinity
pinning + first-touch + cache-line-padded slots** (the §1A §4 scaling realities). The scheduler is a *performance* layer;
its only correctness contract is that it **must not appear in any result bit** (the `DetPool`
`hardware_concurrency`-sizes-but-never-computes rule, generalized).

### 0.8 The worker process is a DEDICATED executable dispatched by a closed `WorkloadId` — re-exec-self is not viable (S7-0 amendment)
**Recon finding (as-built test/build wiring):** every `*_test.cpp` is auto-globbed into a **single** `atx-engine-tests`
executable that links `GTest::gtest_main` — there is **no per-test `main()`** to intercept. A `std::function` body cannot
cross a process boundary, and Windows has **no `fork()`** (`CreateProcess` starts a fresh address space), so the
"re-exec the test binary into a worker entrypoint" pattern is **not viable** here (gtest owns `main`, and a second `main`
would be a link error). **Decision (user-confirmed at S7-0 kickoff):** the `ProcessExecutor` spawns a **dedicated worker
executable** — a new `atx-shm-worker` target (`src/parallel/shm_worker_main.cpp`) — on **both** platforms
(`posix_spawn`/`CreateProcess`), giving **one** portable seam (no `fork`/`exec` backend divergence). The worker is handed
the control + input + output segment names, its worker id, and `n` on argv; it maps the segments, reads a **closed
`WorkloadId` enum** from the control segment, reconstructs the workload's inputs **from the shared-memory input segment**
(zero-copy where the input is already an mmap'd `tsdb` segment — the `ShmBarFeed` path; a serialized POD blob otherwise),
drains the atomic claim cursor, runs the registered **pure** body per shard into its output slots, and exits 0 (or a
non-zero code carrying its lowest failed shard-id). The set of workloads is **closed** (`Test` for S7.3's self-contained
kernel; `Eval`/`Cpcv`/`Backtests`/`Mine` added in S7.5) so a `switch` on `WorkloadId` reconstructs the right body — this
is the concrete form of §4.1's "type-erased descriptor = a small index into the workload's shard table." **Build edge:**
this adds **one** new executable target to `atx-engine/CMakeLists.txt` (the only hand-edit this sprint; the auto-glob
covers everything else) and a `WorkloadId`-dispatch registry the worker and parent share. **Determinism is unchanged:**
the parent still gathers raw slot **bytes** and digests in **one** process (§0.3); the worker never hashes; the slot
layout, reduce-by-sort, and digest are identical to the thread path (§0.5/R1/R7). For S7.3's unit test the `Test`
workload is a trivial deterministic arithmetic kernel (`slot[i] = pure_fn(i, seed)`), so the process pool is proven
end-to-end **before** the real engine workloads are ported in S7.5.

> **Net scope shift vs the ROADMAP sketch:** the ROADMAP S7 sketch is framed as full N-node distributed compute; per the
> kickoff directive this sprint is reframed to **a robust single-machine multi-process execution substrate with the
> scale-out abstraction in place** — the determinism recipe (§0.2), the digest oracles (§0.3), the SHM read substrate
> (`ShmBarFeed`/`SegmentReader`), and the dynamic dispatcher (§0.7) **all already exist**; S7 (a) wraps them behind a
> **substrate-agnostic `IExecutor` seam** (§0.1), (b) adds the **cross-platform multi-process SHM substrate** (§0.6), (c)
> adds **heterogeneous + NUMA-aware scheduling** (§0.7), and (d) **ports the mine-loop + library** onto the seam via
> partition-and-merge (§0.4). The genuinely-new code is the `IExecutor` abstraction + `WorkUnit` model, the
> cross-platform `ShmSegment`, the `ProcessExecutor`, and the deterministic scheduler. The dominant correctness risk is a
> **silently-non-deterministic executor** — one that loses digest-invariance across substrate/worker-count, leaks shared
> mutable state across the process boundary, folds a reduction in completion order, or fails to reduce to the sequential
> oracle. Each is a named gate in §3.

### 0.9 `mine_into` over the process boundary is a PARALLEL-EVAL map + a SEQUENTIAL parent admit — §0.4 partition-and-merge is UNSOUND (S7-5d amendment, user-confirmed)

**Recon finding (the admit fold is stateful and order-dependent):** `library::admit` (library.hpp) is a **stateful fold**, not a
pure map. It runs library-wide **F6 dedup** (`dedup_.contains(canon_hash)`), a **pool-wide MAX-|corr|** screen against every
already-admitted alpha, and — on Accept — `store.stage()` which assigns the global **`AlphaId` in ADMISSION ORDER**; the
manifest's `segment_crc`/`integrity_crc` then **fold `base_alpha_id`** (the segment's first AlphaId) into the content-address,
and `version_id = crc32(entries ++ master_seeds)` over those AlphaId-ordered entries. Every one of these is **path-dependent**:
admit candidate A then B, and B is screened against a pool that already contains A (and lands at a different AlphaId / segment
offset) than if B were admitted first or in a sibling partition. Therefore the frozen plan **§0.4 "N partition libraries →
merge manifests" is UNSOUND** — partitioning the admit fold across workers and merging the manifests **cannot** reproduce the
single-process `report.digest` or `version_id` byte-for-byte (the merge would have to re-derive the global admission order and
re-fold every `base_alpha_id`, i.e. re-run the sequential admit anyway). This is the **same hazard class as Spark's SPARK-23207**
(a non-deterministic repartition silently corrupting a downstream order-dependent result): a parallel reorder of an
order-sensitive fold is a correctness bug, not a throughput knob. **§0.4 is therefore NOT implemented.**

**The SOUND design (implemented in S7-5d):** parallelize **only** the PURE expensive per-genome **map**, and keep the stateful
fold **sequential in the parent**:
- **Map (over the IExecutor seam):** per genome, `compile + Engine::evaluate + extract_streams` → the realized
  `alpha::AlphaStreams`, **plus** `pool_aware_fitness` → `(dsr, raw)` scored against the **RUN-START pool snapshot** (a `const`
  captured before any admit grows the library). This map has **no cross-item shared mutable state** — exactly the eval/cpcv/
  backtests maps S7.5a–c already lifted. Crucially `dsr` is **pool-INDEPENDENT** (`detail::FitnessCore.dsr` — the deflated
  Sharpe of the candidate's OWN OOS stream at the fixed trial count `N = res.trial_count`), so the worker reproduces it
  bit-identically; `raw`'s only pool-dependent term is the MAX-|corr| redundancy against the run-start snapshot, which the
  worker rebuilds from the serialized admitted-pnl snapshot using the **same** SimHash seed/T/K as `library::worst_corr_to_pool`
  (→ the same value).
- **Fold (in the parent):** the parent runs the EXISTING deterministic `rank_by_deflated_fitness` (DESC `dsr`, then `raw`, then
  `canon_hash`, then `idx` — F1) on the gathered scores, then the EXISTING **sequential** `library::admit` loop fed the gathered
  streams. The loop's per-candidate growing-pool re-score (step 3c) updates only `dsr`, which is pool-independent and therefore
  **equals** the gathered run-start `dsr` — so feeding the gathered value is byte-identical to re-scoring. Because rank + admit
  is the **identical single-process code path** in the parent, `report.digest` (the search digest folded with `(canon_hash,
  AdmitKind)` per screened candidate) **and** the library `version_id` are **byte-identical BY CONSTRUCTION** across the
  sequential path, `ThreadExecutor@{1,N}`, and `ProcessExecutor@{1,N}`. The new transport (`workload_mine.{hpp,cpp}` +
  `WorkloadId::Mine` + a substrate-aware `Factory::mine_into(cfg, lib, gate, IExecutor&)` overload) moves **only bytes**, never
  a result bit. **The user explicitly confirmed the full capstone depth** (the real `mine_into` over the boundary with a
  byte-identical `FactoryReport::digest`, not a lighter eval-only subset).

---

## §1 — Research foundation: the deterministic-executor design rules (with citations)

Derived from the research north-stars (`worldquant-systems-deep-dive.md` §4.1/§8 global fan-out,
`renaissance-technologies-systems-deep-dive.md` §6.1/§6.2 continuous re-optimization at scale,
`backtest-loop-execution-sim-deep-dive.md`) + the verified parallel-computing literature (§1A) + the carried-forward
`p0`/`p1` invariants. **Non-negotiable**; every S7 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **R1** | **Digest-invariance across substrate AND worker count — the whole identity.** Same input ⇒ byte-identical run digest across `{sequential, ThreadExecutor@{1,N}, ProcessExecutor@{1,N}}`. The substrate, the worker count, the scheduling, and the steal order **must not touch a single result bit.** | `p1` invariant #1; the as-built `DetPool` / `digest.hpp` "byte-identical across `{1,2,4,8}` BY CONSTRUCTION" contract; TBB `parallel_deterministic_reduce` "same split/join regardless of thread count or task→thread mapping" [oneTBB spec]. |
| **R2** | **Substrate-agnostic submit API — local→distributed swaps only the placement handle.** The `WorkUnit` type, `submit`/`gather`, the slot layout, the reduce-by-sort order, and the digest are **transport-agnostic**; threads, processes, and (future) nodes differ **only** in the `IExecutor` impl constructed. | Ray (`@ray.remote` unchanged; only `ray.init(address)` varies) [OSDI 2018]; Dask (same graph; only `Client(...)`); Spark (same RDD ops; only `master`); HPX AGAS (same `hpx::async`; locality resolved at runtime); Legion (placement isolated in a *mapper*, orthogonal to correctness); P2300 sender/scheduler split. |
| **R3** | **Pure, deterministic, idempotent shards.** Every `WorkUnit` is a pure function of `(read-only inputs, shard-id, recorded seed)` — **no wall-clock, no unseeded RNG, no shared mutable state**. This is what makes a failed-shard re-run **bit-identical** (not merely "same-value"). | `p1` invariants #1/#2; Ray "outputs determined solely by inputs ⇒ idempotent ⇒ safe to re-execute" [OSDI 2018]; Spark lineage recompute correctness, and its negative proof SPARK-23207 (a non-deterministic shuffle ⇒ wrong recomputed counts). |
| **R4** | **Result placement decoupled from execution order.** Each shard writes a **pre-indexed slot** keyed by shard-id; the reduction reads slots in **canonical `(shard_id, item_id)` order** (reduce-by-sort) with a **sequential compensated (Neumaier) fold** — **never** an atomic float accumulation, **never** a completion-order combine. | `p1` invariant #1 + the as-built `cpcv_aggregate_mean_sharpe`; IEEE-754 `+` non-associativity [Goldberg, ACM CSur 1991]; float-`atomicAdd` non-determinism [PyTorch reproducibility]; `ray.wait`/`dask.as_completed` return **completion order** (a trap). |
| **R5** | **Zero-copy read-only inputs, mapped once.** The panel / universe / fold-index inputs are mapped into every worker `PROT_READ` (one physical copy, N viewers) — `mmap MAP_SHARED` / `CreateFileMapping`+`MapViewOfFile`; the OS read-only mapping makes an accidental write **fault**, not diverge. Reduces bytes-per-result ⇒ pushes the roofline knee out. | `data::ShmBarFeed` + `tsdb::SegmentReader` (the existing zero-copy SHM read); Linux `mmap(2)`, Win32 `CreateFileMapping`; Ray Plasma / Arrow zero-copy columnar IPC (one copy, N read-only workers). |
| **R6** | **No hot-path alloc + NUMA / false-sharing discipline.** Workers map inputs + slots **once** before the timed region (the `DetPool` "warm engines before the timed region" rule); per-worker counters & result slots are **padded to `std::hardware_destructive_interference_size`**; working buffers are **first-touched local** and workers are **pinned**; one global pool (no nested-runtime oversubscription). | `p1` invariant #6; false-sharing super-linear blow-up [Drepper §6.4]; roofline bandwidth ceiling [Williams CACM 2009]; NUMA first-touch [Drepper §5; `numa(7)`]; affinity [OpenMP `OMP_PROC_BIND`/hwloc]; sub-linear-at-all-cores measured [NuCore: 6 of 48 cores 3.34× faster on a bandwidth-bound kernel]. |
| **R7** | **Reduce to the sequential oracle on the boundary.** At 1 worker / 1 process the executor's output is **byte-identical** to the as-built single-thread sequential path (`Engine::evaluate` of the batch program; the sequential fold). The generalization is pinned to the proven layer. | `module.md` carry-forward discipline; the `p1` S2 `signal_set_digest` "batch == singly" precedent (`alpha_batch_test`). |
| **R8** | **Differential correctness of the merge and dispatch.** The executor result vs the sequential oracle (bit-identical), and a **failed-shard replay vs its original** (bit-identical), each ship an obviously-correct reference + a bit-exact differential test. | `p1` invariant #7; the `p0`/`p1` differential-test precedent for every new compute/infra kernel; Spark/Ray lineage-recompute-equals-original contract. |

**One-sentence thesis:** *`p1` S2 already proves the pure-map → pre-indexed-slot → reduce-by-sort recipe is
byte-identical across thread counts BY CONSTRUCTION — so S7 is the layer that (a) names that recipe as a
**substrate-agnostic `IExecutor` seam** (R2), (b) carries it across the **process boundary** on a cross-platform
**shared-memory** substrate where the determinism story is strictly cleaner because there is no shared mutable state
(R1/R3/R5), (c) keeps **dynamic load-balancing scheduling with placement decoupled from steal order** and adds NUMA/
false-sharing discipline (R4/R6), and (d) is pinned to the sequential oracle on the boundary (R7); the only
genuinely-new correctness risks are digest-invariance across the substrate boundary, shard purity across processes, and
the library partition-and-merge.*

---

## §1A — State-of-the-art research grounding (verified literature)

> Sourced via a fan-out web-research pass (2026-06-13) over six tracks (framework executors, work distribution,
> determinism-under-parallelism, single-box→multi-node scale-out APIs, fault-tolerance/replay, shared-memory IPC) with a
> direct-fetch verification pass for the load-bearing primaries. **Every reference below is verified primary-source**
> (project docs / papers w/ arXiv·DOI / official standards); full citations + links in the **References** section.
> Where a claim could not be pinned to a live primary it is flagged so the doc does not propagate it.

### Track A — Executor abstraction + single-box→multi-node scale-out (the *what* and the *seam*)

**A1 — Mature backtest engines bolt parallelism *one level up*; the single-backtest engine stays sequential+deterministic.** [grounds R1/R2, §4.1]
QuantConnect **LEAN** processes a backtest as a sequential per-*timeslice* event loop (algorithm events fire
synchronously to prevent look-ahead); parallelism is **external** — the Optimizer dispatches **one full backtest per
parameter combination** across 1–12 cloud nodes. **Zipline** is event-driven (`handle_data()` once per bar), with **no
built-in within-backtest parallelism** — parameter sweeps run as separate processes. **Nautilus Trader** is an explicitly
**single-threaded, deterministic** `BacktestEngine`; the high-level `BacktestNode` runs multiple `BacktestRunConfig`s as
**sequential, independent** executions. **backtrader** is single-threaded `Cerebro`; `optstrategy` spreads parameter
combinations across cores via Python `multiprocessing` (with `optreturn`/`optdatas` to cut IPC). **Microsoft Qlib**
scales *across tasks/machines* via a **MongoDB-backed shared task queue** (`TaskManager`, states
`WAITING/RUNNING/PART_DONE/DONE`) consumed by multiple workers. **vectorbt** is the exception — it pushes parallelism
*down* into NumPy/Numba column-broadcast (a strategy = a column), with vectorbtPRO adding explicit chunking. *The
recurring pattern S7 adopts: the unit of work is a **whole independent backtest/eval/alpha** (a `WorkUnit`), the
single-unit compute is **sequential + deterministic** (the as-built `run_one_fold`/`run_full_backtest`/`Engine::evaluate`),
and the parallelism + scale-out lives in a **dispatch layer above** (the `IExecutor`).*

**A2 — The "swap only the placement handle" pattern — the load-bearing scale-out convention.** [grounds R2, §4.1/§4.3]
Every mature scale-out framework keeps the **work-submission API constant** and varies **only** a transport/placement
object: **Ray** — the same `@ray.remote` task/actor runs locally or on a cluster; only `ray.init()` vs
`ray.init("ray://host:10001")` changes ("*Normal Ray code follows*"). **Dask** — the same `delayed`/futures graph; "*Different
task schedulers… compute the same result*"; only which `Client` you instantiate (no-arg `LocalCluster` vs scheduler
address) changes. **Spark** — the same RDD transformations/actions; only the `master` URL (`local[*]` vs `spark://host`),
ideally externalized to `spark-submit`. **HPX** — "*The syntax of posting an action is always the same, regardless whether
the target locality is remote… or not*"; **AGAS** resolves local-vs-remote at runtime. **Legion** — placement lives in a
**mapper**; "*mapping decisions only impact performance and are orthogonal to correctness.*" **C++ P2300 `std::execution`**
— a **scheduler** is "*a lightweight handle to an execution resource*"; a **sender** "*describes asynchronous work*";
retargeting is `starts_on`/`continues_on` against a different scheduler without rewriting the pipeline (caveat: P2300
abstracts *intra-process* execution; cross-node is a custom-scheduler extension — HPX is the C++ vehicle that crosses the
node boundary). *This is the exact contract of S7's `IExecutor`: the `WorkUnit`/`submit`/`gather`/slot/digest surface is
fixed; `{ThreadExecutor, ProcessExecutor, DistributedExecutor}` are the swappable placement handles (§4.1/§4.3).*

### Track B — Deterministic parallel execution + IPC + fault tolerance (the *how*)

**B1 — Work-stealing for load balance; the Blumofe-Leiserson bound.** [grounds R4/R6, §4.4]
The provably-good randomized work-stealing scheduler executes a fully-strict computation in expected
`T₁/P + O(T∞)` time (`T₁`=work, `T∞`=span), space ≤ `S₁·P` [Blumofe-Leiserson, JACM 1999; Cilk-5, Frigo et al. PLDI 1998].
This is the formal justification for **dynamic** dispatch on **load-imbalanced** shards (some alphas/backtests
orders-of-magnitude slower): runtime is linear-speedup plus an additive span term regardless of imbalance. Production
work-stealers: **Intel oneTBB** (per-thread deque, "*steals… from the top of another **randomly chosen** deque*"),
**Rust Rayon** (Chase-Lev deque via crossbeam). *S7 keeps the as-built dynamic `fetch_add` dispenser (a simpler, queue-
free claim) and only orders the dispatch by known cost (§4.4) — it does not need a full deque steal because the slots are
pre-indexed (B2).*

**B2 — Determinism is a reduction-order property, separable from execution order.** [grounds R1/R4, §4.4/§4.6]
IEEE-754 `+` is **non-associative** — `(a+b)+c ≠ a+(b+c)` in general — so a reduction whose combine-order tracks
scheduling is non-reproducible [Goldberg, ACM Computing Surveys 23(1), 1991]. Both TBB and Rayon document the same root
cause: `parallel_reduce` "*makes the choice of body splitting nondeterministically*" / Rayon's reduce order is "*not fully
specified… not deterministic*" for floats. The fix is to **decouple result placement from execution order**: TBB's
`parallel_deterministic_reduce` "*executes the same set of split and join operations no matter how many threads
participate… and how tasks are mapped to threads*" (restricted to `simple`/`static` partitioners "*because other
partitioners react to random work stealing*"); Cilk's analog is the **reducer hyperobject**. The strongest form is an
**order-independent reproducible summation** — UC Berkeley **ReproBLAS** / Demmel-Nguyen "Parallel Reproducible
Summation" (pre-rounding ⇒ bit-identical "*regardless of… the order in which the sums are computed*"). For **accuracy**
under a fixed order, **Kahan/Neumaier** compensated summation [Higham, SIAM J. Sci. Comput. 1993]. *S7's as-built
`cpcv_aggregate_mean_sharpe` already implements the canonical recipe: stable-sort to a fixed order, then a sequential
**Neumaier** fold — a fixed-order compensated reduction. S7 preserves it verbatim and runs it in the parent (§4.6); the
order-independent ReproBLAS form is a recorded refinement if a future reduction cannot fix its order.*

**B3 — `ray.wait` / `dask.as_completed` return COMPLETION order — the determinism trap to avoid.** [grounds R4, §4.1/§4.6]
Ray's anti-pattern docs: "*Avoid processing… results in submission order using `ray.get()` since results may be ready in a
different order*," recommending `ray.wait()` (completion order). Dask's scheduler order is an explicit **heuristic**, and
its reduction-tree shape depends on `split_every`/partition count (so changing partitions changes the float-combine
order). *Lesson for S7: the `gather` step **must** fold in canonical `(shard_id, item_id)` order (R4), never in the order
shards finish — the opposite of the convenience API these frameworks expose.*

**B4 — Lineage / recompute: a failed shard re-runs to the same result ONLY if the shard is deterministic+idempotent.** [grounds R3/R8, §4.6]
**Spark** recomputes a lost RDD partition from **lineage** (the deterministic transformation graph) — recompute is
scoped to the lost partition, not the whole dataset; **but** correctness is **conditional on determinism** — the
canonical bug **SPARK-23207** (a `RoundRobinPartitioning` shuffle whose row order is non-deterministic) yields 931,532
rows instead of 1,000,000 on executor-failure retry. **Ray** reconstructs lost objects by **re-executing the task
lineage**, explicitly assuming "*Tasks are… deterministic and idempotent*" (derived from purity: "*outputs are
determined solely by their inputs… implies idempotence*"). **Dask** "*maintains a full history of how each result was
produced and… reproduce[s] those same computations.*" Recovery is inherently **at-least-once**, so "re-runs to the same
result" holds **iff** each shard is pure. *S7's R3 makes shard purity an enforced precondition; the failed-shard
bit-identical-replay test (S7.6) is then a *consequence* of purity, not an extra mechanism.*

**B5 — Shared-memory IPC: one read-only copy, N zero-copy workers.** [grounds R5, §4.2]
**`mmap`** with `MAP_SHARED` + `PROT_READ` maps one physical copy of an input file into N processes ("*Updates… visible
to other processes mapping the same region*"; `PROT_READ` ⇒ a write **faults**); `MAP_PRIVATE` never copies read-only
pages; `fork()` shares read-only parent pages **copy-on-write** for free. **POSIX** `shm_open`+`ftruncate`+`mmap` gives a
named tmpfs segment (`/dev/shm`); **Windows** `CreateFileMapping(INVALID_HANDLE_VALUE,…,name)` + `MapViewOfFile` +
`OpenFileMapping(name)` gives named paging-file-backed shared memory ("*all… processes must use the name… of the same
file mapping object*"). **Apache Arrow** IPC + `MemoryMappedFile` is "*inherently zero-copy*"; **Ray Plasma / object
store** validates the exact pattern in production — "*objects… immutable and held in shared memory… read… without
copying (zero-copy reads).*" *S7's `ShmSegment` (§4.2) is precisely this — read-only input mapped once, pre-indexed
output slots in a second segment; the existing `ShmBarFeed`/`tsdb::SegmentReader` already proves the engine can stream a
sealed segment "*straight out of shared memory, with no per-process re-parse or data copy.*"*

**B6 — Why naive thread-per-core doesn't scale linearly (the perf ceiling S7's scheduler must respect).** [grounds R6, §4.4]
**False sharing**: two cores writing distinct variables on the **same 64-byte line** force cache-line ping-pong — Drepper
measures **390% / 734% / 1,147%** overhead for 2/3/4 threads (super-linear *negative* scaling); the C++17 remedy is
`alignas(std::hardware_destructive_interference_size)` per hot slot. **Bandwidth saturation**: the **roofline**
`P = min(π, β·I)` caps a low-operational-intensity (streaming) kernel at `β·I` *independent of core count* once DRAM is
saturated [Williams, CACM 2009] — so adding cores past the knee buys nothing. **NUMA**: remote-node access is slower;
Linux **first-touch** places a page on the node of the thread that first *writes* it, so a master-init-then-worker-consume
pattern goes all-remote unless the worker first-touches its own buffers; pin with `OMP_PROC_BIND`/hwloc. **Measured
reality**: NuCore reports a bandwidth-bound PARSEC kernel runs **3.34× faster on 6 of 48 cores** than on all 48, and that
~**75.6%** of cores is the average optimum — i.e. all-cores can be *slower*. Hoogenboom shows even "embarrassingly
parallel" Monte-Carlo scales sublinearly once **synchronization/collection rendezvous** appear — *the collector, not the
per-shard math, is where scaling dies.* *S7's scheduler (§4.4): pin + first-touch, pad slots to the interference size,
size the pool to the bandwidth knee (configurable, not hard `hardware_concurrency`), and keep collection to lock-free
per-slot writes + one final ordered combine (no per-result barrier).*

> **Verification caveats carried from the research pass:** (a) the Blumofe-Leiserson space/lower bounds were confirmed
> from multiple secondary restatements (the ACM PDF 403'd); (b) cppreference's exact wording for the interference-size
> constants is from its search index (page 403'd), the C++17 `[hardware.interference]` intent confirmed; (c) the OpenMP
> "spread ≈ 2× close" magnitude is illustrative (workload-specific); (d) the oneTBB oversubscription "quadratic threads"
> quote is from Intel-indexed snippets (canonical page 403'd); (e) WorldQuant BRAIN concurrency numbers are community-
> wrapper-reported, not official. None are load-bearing for a *correctness* rule — they ground the *performance* layer
> (§4.4), which has no bit-level contract.

---

## §2 — File structure

### 2.1 atx-core / Pattern-B requests (decided at kickoff)

> The engine adds no general-purpose primitive (project rule). S7 records the cross-module edges and ships on existing
> primitives + engine-local OS shims, exactly as `p1` S1–S8 did:
>
> 1. **L4 `dist_pool` → atx-core.** Deterministic **multi-node** task distribution (order-fixed cross-machine merge; a
>    canonical-byte-order, seed-fixed digest for the cross-*machine* equality §0.3). Ship on the **engine-local
>    `ProcessExecutor`** over a deterministic shared-memory IPC shim (S7.2/S7.3); the **network transport** is the
>    recorded lift. Engine fallback is shippable now and digest-invariant on one machine.
> 2. **L-tier `shm` / `proc` (optional) → atx-core.** A cross-platform shared-memory + process-spawn primitive. Ship on
>    an **engine-local `ShmSegment`** (Win32 + POSIX behind one seam, S7.2) reusing the `tsdb::SegmentReader` mapping
>    convention; the general-purpose atx-core primitive is the recorded lift. *(Raise only if a second consumer appears;
>    otherwise the engine-local shim stays.)*
> 3. **`DetPool` + digest + reduce-by-sort → already in `p1` S2.** Reused verbatim; no request.

### 2.2 Engine files (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/parallel/executor.hpp` | `IExecutor` (the `submit(span<WorkUnit>) → gather(OutputSlots)` seam), `WorkUnit` (pure `(inputs, ShardId, seed) → slot`), `ShardId`, `OutputSlots` (pre-indexed, cache-line-padded), `ExecutorConfig`; the SWITCH POINT target (§4.1/§0.1/R2) | S7-1 |
| `include/atx/engine/parallel/thread_executor.hpp` | `ThreadExecutor` — thin `IExecutor` adapter over the as-built `DetPool`; the in-process, degenerate-1-process case + the oracle the process path is pinned to (§4.1/§0.1/R7) | S7-1 |
| `include/atx/engine/parallel/shm_segment.hpp` + `src/parallel/shm_segment.cpp` | `ShmSegment` — cross-platform read-only-input + pre-indexed-output shared memory (Win32 `CreateFileMapping`/`MapViewOfFile` **and** POSIX `shm_open`/`mmap` behind one seam; `PROT_READ` inputs); reuses the `SegmentReader` zero-copy convention (§4.2/§0.6/R5) | S7-2 |
| `include/atx/engine/parallel/process_executor.hpp` + `src/parallel/process_executor.cpp` | `ProcessExecutor` — the **primary** multi-process pool over `ShmSegment`: worker-process lifecycle, deterministic dispatch, pure-shard exec, parent gather + digest (§4.3/§0.6/R1/R3) | S7-3 |
| `include/atx/engine/parallel/scheduler.hpp` | `Scheduler` — deterministic heterogeneous-cost dispatch ordering + NUMA/affinity/first-touch/topology + nested-parallelism guard; a *performance* layer with **no** bit contact (§4.4/§0.7/R6) | S7-4 |
| `include/atx/engine/library/partition.hpp` | `library::partition` — disjoint-`AlphaId`-range partition-local `Library` + canonical-order manifest merge (the §0.4 process-safe library path) (§4.5/R1) | S7-5 |

> **Edits (not new files)** in S7-5: `parallel/parallel_run.{hpp,cpp}`, `parallel/batch_eval.{hpp,cpp}`, and
> `factory/research_driver.cpp` gain an `IExecutor&` overload (the existing `DetPool&` overload is kept and becomes
> `ThreadExecutor`'s body) so every workload submits through the seam without duplicating the map logic. `parallel/fwd.hpp`
> SWITCH POINT re-points `using Pool = …;` at `IExecutor`. `parallel/digest.hpp` is **unchanged** (reused).

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)
`parallel_executor_test.cpp` (S7-1: the seam + `WorkUnit` purity contract + `ThreadExecutor`==`DetPool`),
`parallel_shm_segment_test.cpp` (S7-2: round-trip a read-only input + pre-indexed output across a mapped segment; the
read-only-write-faults check; **both** Win32 + POSIX backends), `parallel_process_executor_test.cpp` (S7-3:
`ProcessExecutor`@{1,N} digest == `ThreadExecutor` digest; pure-shard exec; lowest-shard error determinism across the
process boundary), `parallel_scheduler_test.cpp` (S7-4: heterogeneous-cost ordering changes wall-clock but **not** the
digest; pinning/first-touch don't touch bits), `parallel_executor_port_test.cpp` (S7-5: batch-eval/CPCV/backtests **and**
the mine→admit→merge loop produce the same digest/`version_id` over `IExecutor` as the as-built `DetPool` path; the
library partition-and-merge in canonical `AlphaId` order), `parallel_digest_invariance_test.cpp` (S7-6: **the capstone** —
the five-path substrate-invariance pin §0.5), `parallel_fault_tolerance_test.cpp` (S7-6: a killed/failed shard re-runs
**bit-identical**; the merge skips/replays deterministically).
Bench: `bench/executor_bench.cpp` (S7-6: speedup vs 1 worker across `(workload ∈ {eval, cpcv, backtests, mine}, P ∈
{1,2,4,8,all}, substrate ∈ {thread, process})`; the bandwidth/NUMA knee; digest-invariance has zero wall-clock cost
because it is the *same* slots).

### 2.4 Ledger
`sprint-7-progress.md` (S7-0), updated per unit (copy `p1`'s `sprint-2-progress.md` shape — the determinism-substrate
precedent). S7 is **7 units incl. marker** — at the 4–7 ceiling; **split contingency**: if S7.3 (`ProcessExecutor`) or
S7.4 (scheduler) over-run, split into **S7-a (S7.0–S7.3: seam + SHM substrate + process pool)** / **S7-b (S7.4–S7.6:
scheduler + workload port + digest-invariance proof)** with two ledgers, exactly as `p1` S4/S7 did. Default: one plan.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Suite_Condition_ExpectedResult`; cover happy path, **boundaries** (the sequential
  reduction pin §0.5/R7; a single-shard run; an empty `WorkUnit` span → no-op, no spawn; a worker that throws → the
  **lowest shard-id** error rethrown deterministically across the process boundary §0.1; a heterogeneous-cost mix where
  one shard dominates the tail; 1 worker vs N workers vs 1 process vs N processes; a read-only input write → **fault**;
  a deliberately-killed worker → bit-identical replay), and the **invariant proofs** (R1 five-path digest-invariance, R3
  shard purity / no shared mutable state across processes, R4 reduce-by-sort-not-completion-order, R7 reduce to
  sequential, R8 failed-shard replay bit-identical).
- **Digest-invariance (R1) — the whole identity:** the run digest (`signal_set_digest` / `result_table_digest` / merged
  `version_id`) must be **byte-identical** across `{sequential, ThreadExecutor@{1,N}, ProcessExecutor@{1,N}}`; this is a
  mandatory test for **S7-3** and **S7-6**. The substrate, worker count, dispatch order, and scheduler **never** appear
  in a result bit (the `DetPool` "`hardware_concurrency` sizes but never computes" rule, generalized to processes).
- **Pure shards / no shared mutable state (R3):** every `WorkUnit` is a pure function of `(read-only inputs, shard-id,
  recorded seed)` — **no wall-clock**, **no unseeded RNG** (seeds derive from `seed_for_run`-style pure mixes), **no
  writable shared state** across workers. A test asserts a worker that touches shared state is rejected at the API
  boundary; the SHM input mapping is `PROT_READ` so a write **faults**.
- **Result placement (R4):** each shard writes a **pre-indexed slot** keyed by shard-id; the reduction reads slots in
  **canonical `(shard_id, item_id)` order** with the as-built **Neumaier** fold — **never** an atomic float add, **never**
  a completion-order combine. A test perturbs completion order (sleep-jitter a worker) and asserts the digest is
  unchanged.
- **Zero-copy read-only inputs (R5):** the panel / fold-index / program inputs are mapped **once** `PROT_READ` /
  `FILE_MAP_READ` and shared across workers (one physical copy); reuse the `tsdb::SegmentReader` mapping convention. A
  test confirms no per-worker input copy (the memory profile is one input + N slots, not N inputs).
- **NUMA / false-sharing / no hot-path alloc (R6):** per-worker counters & result slots are
  `alignas(std::hardware_destructive_interference_size)`; workers first-touch their own buffers and are pinned; the
  steady-state dispatch loop allocates **zero** (segments + slots mapped once before the timed region); one global pool
  (no nested-runtime oversubscription — guard against a BLAS/OpenMP pool multiplying against the executor). Cold paths
  (segment creation, process spawn, topology query) may allocate (documented).
- **Cross-platform (§0.6):** the SHM + process units ship **both** a Win32 and a POSIX backend behind one seam; **both**
  are CI-built and pass their test. No `#ifdef` leaks past the `ShmSegment` / `ProcessExecutor` seam into the workload
  code.
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures (spawn
  failure, segment-map failure, infeasible partition, a worker non-zero exit, dim mismatch); weakest sufficient types
  (`std::span`, `const&`, `std::string_view`); functions ≤ ~60 lines; **reuse `parallel::{DetPool, FoldResult,
  result_table_digest, signal_set_digest, cpcv_aggregate_mean_sharpe}`, `tsdb::SegmentReader`, `factory::{seed_for_run}`,
  `library::Library` — do NOT reinvent the pool, the digest, the reduce-by-sort, the SHM read, or the seed mix**; the
  only new code is the `IExecutor` seam, the cross-platform `ShmSegment`, the `ProcessExecutor`, the scheduler, and the
  library partition/merge.
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) **+ strict FP** (`/fp:precise`, `-ffp-contract=off`).
  clang-tidy disabled — the strict build + ctest are the gate.
- **clangd noise:** ignore squiggles; only a real `cmake --build` + ctest are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree (REUSE, do NOT rebuild):
parallel/det_pool.hpp (DetPool — the ALREADY-deterministic fixed-worker pool: parallel_for dispenses indices via an
atomic fetch_add, barriers on busy_, rethrows the LOWEST-index exception; "byte-identical across {1,2,4,8} BY
CONSTRUCTION"; this is your ThreadExecutor body AND your in-process oracle — you WRAP it, you do NOT rewrite it);
parallel/parallel_run.{hpp,cpp} (FoldResult, run_one_fold/run_full_backtest pure maps, parallel_cpcv/parallel_backtests
into pre-indexed out[f]/out[a], cpcv_aggregate_mean_sharpe = stable-sort by (alpha_id,fold_id) then SEQUENTIAL NEUMAIER
fold — THIS is reduce-by-sort, reuse it verbatim, NEVER an atomic float sum, NEVER completion order); parallel/batch_eval
.{hpp,cpp} (parallel_evaluate: per-worker stateful Engine, disjoint output slots offsets[k], byte-identical to the batch
single-thread path); parallel/digest.hpp (signal_set_digest / result_table_digest — RAW f64 bytes, canonical order, 1-ULP
flips it; intra-process only, so the PARENT gathers worker result BYTES and digests in ONE process); data/shm_bar_feed.hpp
+ tsdb::SegmentReader (the EXISTING zero-copy SHM read substrate — stream a sealed segment straight out of shared memory,
no per-process copy; reuse its mapping convention for ShmSegment); factory/research_driver.cpp (detail::seed_for_run(
master,run) = PURE SplitMix mix, no atomics/no wall-clock/no worker-id — the mining axis is ALREADY process-safe);
library/library.hpp (Library::admit — NOT process-safe: mutates memtable_pending_, store_.stage() advances a global
AlphaId, corr index rebuilds in AlphaId order; partition per worker over a disjoint AlphaId range and merge manifests in
canonical AlphaId order — that is the ONLY new library work).

THIS SPRINT'S DOMINANT RISK IS A SILENTLY-NON-DETERMINISTIC EXECUTOR: one that loses digest-invariance across substrate
or worker count; leaks shared mutable state across the process boundary; folds a reduction in COMPLETION order; or fails
to reduce to the sequential oracle on the boundary. The gates:
  - DIGEST-INVARIANCE (R1): run digest BYTE-IDENTICAL across {sequential, ThreadExecutor@{1,N}, ProcessExecutor@{1,N}}.
    Substrate/worker-count/dispatch-order/scheduler NEVER appear in a result bit. Mandatory test for S7-3 and S7-6.
  - PURE SHARDS (R3): every WorkUnit is a pure fn of (read-only inputs, shard-id, recorded seed). NO wall-clock, NO
    unseeded RNG, NO writable shared state. Read-only inputs mapped PROT_READ so a write FAULTS. => failed shard re-runs
    BIT-IDENTICAL.
  - RESULT PLACEMENT (R4): pre-indexed slot per shard; reduce-by-sort in canonical (shard_id,item_id) order with the
    as-built NEUMAIER fold. NEVER atomic float add. NEVER completion order. Perturb completion order => digest unchanged.
  - ZERO-COPY INPUTS (R5): inputs mapped ONCE PROT_READ, N viewers (reuse SegmentReader). One input copy + N slots.
  - NUMA/FALSE-SHARING (R6): slots/counters alignas(hardware_destructive_interference_size); first-touch local; pin; one
    global pool; steady-state loop allocates ZERO.
  - REDUCE TO SEQUENTIAL (R7): 1 worker / 1 process => BYTE-IDENTICAL to the as-built single-thread path. The regression.
  - CROSS-PLATFORM: ship + CI-build BOTH Win32 and POSIX SHM/process backends behind one seam.

PIT/no-survivorship/no-look-ahead are inherited from the workloads (the shards are the existing pure maps); S7 must not
introduce wall-clock or future reads. Reuse ONE pool (DetPool), ONE digest, ONE reduce-by-sort, ONE SHM read convention.
Header-only inline where parallel/ is; compiled .cpp for the OS-touching units (shm_segment, process_executor).
Functions <= ~60 lines. Build gate: cmake --build build --config Debug --target atx-engine-tests (/W4 /permissive- /WX +
/fp:precise) + ctest -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER git add -A;
after committing run `git show HEAD --stat` (only your files); never touch atx-core/* or atx-tsdb/*; do not push.
End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 `IExecutor` seam + `WorkUnit`/shard model + deterministic gather (S7-1)

The substrate-agnostic surface. A `WorkUnit` is a pure description of work + the slot it owns; an `IExecutor` runs a span
of them and the caller `gather`s the slots in canonical order. The `WorkUnit`/slot/reduce surface is the **transport
boundary** — `{Thread, Process, Distributed}Executor` differ only below it (R2).

```cpp
// section: parallel/executor.hpp  (namespace atx::engine::parallel)
using ShardId = atx::usize;                       // canonical, dense, [0, n) — the slot key (R4)

// A pure unit of work. Reads ONLY (inputs, id, seed); writes ONLY its own pre-indexed slot (R3/R4).
struct WorkUnit {
  ShardId    id;                                  // canonical slot index — the ONLY thing that orders the reduction
  atx::u64   seed;                                // derived from a pure mix (factory::seed_for_run analogue); recorded
  // The body is a pure callable: (const Inputs& ro, ShardId) -> void, writing into the worker's slot view.
  // Carried as a type-erased descriptor (a small index into the workload's shard table), NOT a std::function with
  // captured mutable state — so it is trivially shippable to another process (it captures NOTHING but `id`).
};

// Pre-indexed, cache-line-padded output region. Slot s is written ONLY by shard s (no false sharing — R6).
template <class Slot>
struct OutputSlots {
  std::span<Slot> slots;                          // size n; slots[s] padded to hardware_destructive_interference_size
  // gather(): read slots in ASCENDING id order and reduce-by-sort (the as-built Neumaier fold), in the PARENT (R4/§0.3).
};

struct ExecutorConfig {
  atx::usize workers = 0;                          // 0 => substrate default (DetPool's max(1,hw-2)); the BANDWIDTH KNEE,
                                                  //      configurable — NOT a hard hardware_concurrency (R6/§1A B6)
  bool       pin_workers = true;                  // affinity + first-touch (R6); a perf knob, never a bit knob
};

// The seam. Threads, processes, and (future) nodes implement THIS — nothing above it changes (R2).
class IExecutor {
public:
  virtual ~IExecutor() = default;
  // Run every WorkUnit exactly once; each writes its own slot; block until all done. The LOWEST-id error is surfaced
  // deterministically (the DetPool rethrow rule, lifted across the substrate). NO cross-shard state.
  template <class Slot, class Body>             // (non-virtual sugar; the virtual core is run_erased below)
  void submit(std::span<const WorkUnit> units, OutputSlots<Slot> out, Body&& per_shard);
  [[nodiscard]] virtual atx::usize workers() const noexcept = 0;
protected:
  virtual void run_erased(atx::usize n, const std::function<void(ShardId, atx::usize)>& body) = 0;  // DetPool::parallel_for shape
};
```
```cpp
// section: parallel/thread_executor.hpp  (namespace atx::engine::parallel)
// The degenerate 1-process case + the oracle the ProcessExecutor is pinned to (R7). A THIN adapter over DetPool — the
// run_erased body IS DetPool::parallel_for. Zero new scheduling logic; DetPool already satisfies R1/R4 by construction.
class ThreadExecutor final : public IExecutor {
public:
  explicit ThreadExecutor(ExecutorConfig c = {}) : pool_{c.workers} {}
  [[nodiscard]] atx::usize workers() const noexcept override { return pool_.n_workers(); }
protected:
  void run_erased(atx::usize n, const std::function<void(ShardId, atx::usize)>& body) override {
    pool_.parallel_for(n, [&](atx::usize i, atx::usize wid) { body(i, wid); });   // DetPool verbatim
  }
private:
  DetPool pool_;
};
```
**Determinism (R1/R4):** `submit` writes pre-indexed slots; `gather` reduces in ascending `ShardId` order with the
as-built Neumaier fold — **never** completion order. **Boundary (R7):** `ThreadExecutor` *is* `DetPool`, so it inherits
"byte-identical across `{1,2,4,8}` BY CONSTRUCTION" for free; the seam adds no math. **SWITCH POINT (§0.1):**
`parallel/fwd.hpp`'s `using Pool = …;` re-points at `IExecutor`; existing `DetPool&` call-sites take an `IExecutor&`.

### 4.2 Cross-platform shared-memory substrate (S7-2)

One read-only **input** segment (mapped `PROT_READ` into every worker — the zero-copy panel/fold-index/program inputs,
R5) and one **output** segment (the pre-indexed, cache-line-padded slots, written by exactly one worker each, R4/R6).
Win32 + POSIX behind one seam (§0.6); reuses the `tsdb::SegmentReader` mapping convention.

```cpp
// section: parallel/shm_segment.hpp  (namespace atx::engine::parallel)
enum class ShmAccess { ReadOnly, ReadWrite };     // inputs => ReadOnly (a write FAULTS — the OS-enforced invariant, R5)

class ShmSegment {                                // RAII; pinned (owns an OS handle) — copy/move deleted like DetPool
public:
  // Create a named segment of `bytes` (parent) or open an existing one (worker). One seam, two backends:
  //   POSIX:  shm_open(name, O_CREAT|O_RDWR | (worker? O_RDONLY)) -> ftruncate -> mmap(PROT_READ[|WRITE], MAP_SHARED)
  //   Win32:  CreateFileMapping(INVALID_HANDLE_VALUE, PAGE_READWRITE, bytes, name) | OpenFileMapping(name)
  //           -> MapViewOfFile(FILE_MAP_READ | FILE_MAP_WRITE)
  [[nodiscard]] static atx::core::Result<ShmSegment>
  create(std::string_view name, atx::usize bytes);                 // parent: RW
  [[nodiscard]] static atx::core::Result<ShmSegment>
  open(std::string_view name, ShmAccess access);                   // worker: inputs RO, slots RW

  [[nodiscard]] std::span<const std::byte> view_ro() const noexcept;   // PROT_READ map — zero-copy input
  [[nodiscard]] std::span<std::byte>       view_rw() noexcept;         // the output-slot region (this worker's slots)
  ~ShmSegment();                                  // munmap/UnmapViewOfFile + shm_unlink/CloseHandle (parent unlinks)
private:
  void*       base_{nullptr};
  atx::usize  bytes_{0};
#if defined(_WIN32)
  void* handle_{nullptr};                         // HANDLE from CreateFileMapping/OpenFileMapping
#else
  int   fd_{-1};                                  // shm_open fd
  bool  owner_{false};                            // parent unlinks on dtor (worker just closes)
#endif
};
```
**Zero-copy (R5):** the input segment is mapped once by the parent (e.g. a sealed `tsdb` OHLCV segment via the existing
`SegmentReader`, or a serialized `Panel`/`Program` blob) and `open(…, ReadOnly)`'d by every worker — one physical copy,
N viewers; a worker write to it **faults**. **Slots (R4/R6):** the output segment is a flat array of
`alignas(std::hardware_destructive_interference_size)` slots; worker `w` writes only its shards' slots (no false
sharing, no locks). **Cross-platform (§0.6):** the `#if defined(_WIN32)` split lives **only** here; both backends pass
`parallel_shm_segment_test` (round-trip + read-only-write-faults). **Determinism (R1):** the parent gathers raw slot
**bytes** and digests them in one process (§0.3) — workers never hash.

### 4.3 `ProcessExecutor` — the primary multi-process pool (S7-3)

The shipping primary substrate. The parent maps inputs + slots into a `ShmSegment`, spawns N worker processes, each
maps the same segments, runs its claimed shards (pure, into its slots), and exits; the parent barriers on exit, gathers
the slots, and digests. Determinism is **stronger** than threads here — separate address spaces mean **no shared mutable
state at all** (the largest source of thread non-determinism is structurally absent).

```cpp
// section: parallel/process_executor.hpp  (namespace atx::engine::parallel)
class ProcessExecutor final : public IExecutor {
public:
  explicit ProcessExecutor(ExecutorConfig c = {});
  [[nodiscard]] atx::usize workers() const noexcept override { return n_workers_; }
protected:
  void run_erased(atx::usize n, const std::function<void(ShardId, atx::usize)>& body) override {
    // 1. Parent maps the read-only INPUT segment (zero-copy, R5) + the pre-indexed OUTPUT slot segment (R4).
    // 2. Spawn n_workers_ worker PROCESSES (CreateProcess / posix_spawn|fork), passing: segment names, worker id,
    //    n, and the DETERMINISTIC dispatch plan (the shard ranges / the atomic-claim cursor in a small control segment).
    // 3. Each worker: open() the segments (inputs RO, its slots RW), then DRAIN the shared atomic claim cursor
    //    (the DetPool fetch_add dispenser, lifted into a control-segment atomic) — claim shard `id`, run body(id),
    //    write slot[id]. Dynamic dispatch => load balance (R4/B1); pre-indexed slot => placement is data-defined.
    // 4. Worker exits with code 0, or a NON-ZERO code carrying the LOWEST shard-id it failed (deterministic surface).
    // 5. Parent waits on ALL workers (the barrier), reads the per-worker error slot, and rethrows the GLOBAL lowest-id
    //    error (the DetPool rethrow rule, across the process boundary). Then gather() runs in the PARENT (R4/§0.3).
  }
private:
  atx::usize n_workers_;
  // Pre-sized scratch: segment handles, the spawn argv, the control segment (claim cursor + per-worker error slot).
  // Mapped/sized ONCE per submit shape; the steady-state claim loop allocates ZERO (R6).
};
```
**Boundary pin (R1/R7/§0.5):** `ProcessExecutor@{1,N}`'s gathered slots must digest **byte-identical** to
`ThreadExecutor`'s and to the sequential path — the same shards write the same bytes into the same canonical slots; only
the address space differs. **Pure shards (R3):** a worker captures **nothing but its `ShardId`** and the read-only
segment view; the seed is the pure `seed_for_run`-style mix carried in the control segment — so a re-spawned worker
re-runs **bit-identical** (R8). **No shared mutable state:** the only shared writable memory is the disjoint slot region
(each slot single-writer) + the atomic claim cursor (an *integer* dispenser that publishes no result data — the same
"the counter does not enter the bits" guarantee `DetPool` documents). **Error determinism:** a non-zero worker exit
carries its lowest failed shard-id; the parent reduces to the global lowest — matching `DetPool::rethrow_lowest` across
processes.

### 4.4 Deterministic heterogeneous-cost scheduler + NUMA discipline (S7-4)

A **performance** layer with **no bit contact** (the §0.7 contract). It (a) orders dispatch by known shard cost to
shorten the tail, (b) pins workers + first-touches their buffers, (c) sizes the pool to the bandwidth knee, (d) guards
against nested-runtime oversubscription. It **cannot** change a result bit — proven by the digest being identical with
the scheduler on vs off.

```cpp
// section: parallel/scheduler.hpp  (namespace atx::engine::parallel)
struct Topology {                                 // queried once (hwloc-style; cold path); cached on the executor
  atx::usize n_cores, n_numa_nodes, n_pus;
  std::vector<atx::usize> pu_to_node;             // PU -> NUMA node (for first-touch + spread pinning)
  atx::usize line_bytes;                          // == std::hardware_destructive_interference_size (slot padding)
};

struct Scheduler {
  Topology topo;
  // (a) DISPATCH ORDER — a permutation of shard ids by DESCENDING known cost (longest-processing-time-first), so the
  //     expensive shards start first and the tail shortens (a SCHEDULING hint). The slots are STILL written by shard
  //     id, so the reduction order is unchanged => the digest is UNCHANGED. The cost estimate is a hint (e.g. program
  //     node count / fold length); a wrong hint costs wall-clock, never bits.
  [[nodiscard]] std::vector<ShardId> dispatch_order(std::span<const atx::f64> cost_hint) const;  // pure, deterministic
  // (b) AFFINITY + FIRST-TOUCH — pin worker w to a PU (spread across NUMA nodes for bandwidth, OMP_PROC_BIND=spread
  //     analogue, §1A B6), and have w FIRST-TOUCH its own slot pages so they land on its local node (avoid all-remote).
  void pin_and_first_touch(atx::usize worker_id, std::span<std::byte> my_slots) const;
  // (c) POOL SIZE — default to min(workers, bandwidth_knee); the knee is configurable (NuCore: all-cores can be SLOWER).
  // (d) OVERSUBSCRIPTION GUARD — assert exactly one global pool; a nested BLAS/OpenMP pool is pinned to 1 thread.
};
```
**No bit contact (R1/§0.7):** `dispatch_order` permutes *which shard runs when*, not *which slot a shard writes* — the
reduction reads slots in canonical id order regardless, so the digest is invariant to the schedule (the test runs the
same job with `dispatch_order` = identity vs cost-sorted vs reversed and asserts **one** digest). **NUMA/false-sharing
(R6):** slots are `alignas(line_bytes)`; `pin_and_first_touch` keeps each worker's slots local; pool sized to the knee
(§1A B6 — adding cores past the bandwidth saturation point buys nothing and can hurt). **Reuse:** the `DetPool` atomic
dispenser is the claim mechanism; the scheduler only orders the *initial* shard list and pins — it does **not** replace
the pool.

### 4.5 Workload port + library partition/merge (S7-5)

Every workload submits through the seam — batch-eval, CPCV, backtests (already pure maps), **and** the GA
mine→admit→replay loop (the new orchestration). The library is made process-safe by partition-and-merge (§0.4).

```cpp
// section: parallel/parallel_run.hpp / batch_eval.hpp  (EDIT — add an IExecutor& overload beside the DetPool& one)
// The map bodies (run_one_fold / run_full_backtest / Engine::evaluate-per-program) are UNCHANGED — they were already
// pure. Only the dispatch changes: pool.parallel_for(...) becomes exec.submit(units, slots, body). The DetPool& overload
// stays and forwards to a ThreadExecutor (zero behavior change for existing callers).
[[nodiscard]] std::vector<FoldResult>
parallel_backtests(const alpha::AlphaStreams& streams, atx::f64 book_size, IExecutor& exec);   // + DetPool& kept

// section: library/partition.hpp  (namespace atx::engine::library)
// The §0.4 process-safe library path. Each worker admits into a partition-local Library over a DISJOINT AlphaId range;
// the parent merges manifests in canonical (global AlphaId) order. The mine-loop seed (seed_for_run) is ALREADY safe.
struct LibraryPartition {
  atx::usize worker_id, n_workers;
  atx::usize alpha_id_stride;                     // worker w owns AlphaIds [w*stride, (w+1)*stride) — disjoint, dense
  Library    local;                               // partition-local store/dedup/journal; NO shared memtable_pending_
};

// Merge partition manifests into one library snapshot, in canonical global-AlphaId order (matches Library::snapshot's
// content-addressed version_id form). Determinism (R1): entries sorted by global AlphaId, version_id = crc32 over
// (merged entries ++ master_seeds) — IDENTICAL to a single-process mine of the same total budget + seed.
[[nodiscard]] atx::core::Result<Manifest>
merge_partitions(std::span<LibraryPartition> parts, std::span<const atx::u64> master_seeds);
// Correlation index: rebuilt ONCE on the merged library in AlphaId order (the deterministic path); per-partition local
// corr + query-time neighbor merge is the recorded scale lift.
```
**Determinism (R1/R7):** an N-worker mine of budget `B` with master seed `s` produces a merged `version_id`
**byte-identical** to a 1-worker (or sequential) mine of budget `B` with seed `s` — because `seed_for_run(s, run)` is
pure (run `r` gets the same seed regardless of which worker runs it) and the merge sorts to canonical global-`AlphaId`
order. **No shared mutable state (R3):** each worker's `Library` is private; the only cross-worker step is the parent's
post-run manifest merge (a pure sort+fold). **Boundary:** the existing `library_integration_test` digest is the oracle.

### 4.6 Digest-invariance proof + fault tolerance + bench + close (S7-6)

- **The capstone digest-invariance test** (`parallel_digest_invariance_test.cpp`, §0.5/R1): for a fixed input, assert
  the run digest is **byte-identical** across **all five paths** — `{sequential as-built, ThreadExecutor@1,
  ThreadExecutor@8, ProcessExecutor@1, ProcessExecutor@8}` — for **each** workload (eval `signal_set_digest`, cpcv/
  backtests `result_table_digest`, mine merged `version_id`). This is the whole identity of the sprint; it subsumes R7
  (the `@1` paths) and R4 (perturbing completion order leaves it unchanged).
- **Fault-tolerance test** (`parallel_fault_tolerance_test.cpp`, R3/R8/§1A B4): kill a worker mid-shard (or inject a
  shard that returns a non-zero exit), assert the parent (a) surfaces the deterministic lowest-id error **or** (b)
  re-runs the failed shard and the re-run slot is **bit-identical** to the original — the lineage-recompute contract,
  which holds **because** the shard is pure (R3). A non-deterministic shard would break it (the SPARK-23207 negative
  proof); the test asserts purity is enforced.
- **Bench** (`executor_bench.cpp`): speedup vs 1 worker across `(workload ∈ {eval, cpcv, backtests, mine}, P ∈
  {1,2,4,8,all}, substrate ∈ {thread, process})`; the **bandwidth/NUMA knee** (where adding cores stops helping — the
  pool-size default); the process-spawn + segment-map fixed cost (amortized across shards); confirmation that
  digest-invariance has **zero** wall-clock cost (it is the *same* slots). Report the speedup curve + the knee, not a
  single "linear" claim (§1A B6).
- **Close ceremony:** residuals → ROADMAP backlog (the **network transport** / atx-core L4 `dist_pool` lift §2.1; the
  **cross-machine digest** canonical-byte-order form §0.3; the **per-partition corr index** + query-time merge §0.4/§4.5;
  the atx-core `shm`/`proc` primitive §2.1 if a second consumer appears); ROADMAP status table `⏳ → ✅ <sha>`; `Last
  reviewed` bump; "What S7 proves" + "Next sprint priorities" baton; user reference `sprint7.md`; merge (`--no-ff`).

---

## Exit criteria

- The `IExecutor` seam exists with `WorkUnit`/`ShardId`/`OutputSlots`/`ExecutorConfig`; `parallel/fwd.hpp`'s SWITCH POINT
  points at it; `ThreadExecutor` is a thin `DetPool` adapter and existing `DetPool&` call-sites take an `IExecutor&` with
  **zero** behavior change (R2/R7).
- The `ShmSegment` maps read-only inputs (`PROT_READ`, a write **faults**) zero-copy and pre-indexed, cache-line-padded
  output slots, on **both** Win32 and POSIX, both CI-built (R5/R6/§0.6).
- The `ProcessExecutor`@{1,N} gathered slots digest **byte-identical** to `ThreadExecutor`'s and to the sequential path;
  a worker error surfaces as the deterministic global lowest shard-id across the process boundary (R1/R3).
- The scheduler orders heterogeneous-cost dispatch + pins + first-touches + sizes to the bandwidth knee, and the run
  digest is **identical** with it on vs off (no bit contact — R1/R6/§0.7).
- Every workload (eval, cpcv, backtests, **and** the mine→admit→merge loop with the partition-and-merge library)
  produces the same digest/`version_id` over `IExecutor` as the as-built `DetPool`/sequential path (R1/R7).
- **The capstone holds:** the run digest is byte-identical across `{sequential, ThreadExecutor@{1,8},
  ProcessExecutor@{1,8}}` for every workload; a failed shard re-runs bit-identical (R1/R8).
- The bench reports the speedup curve + the bandwidth/NUMA knee across substrates; digest-invariance has zero wall-clock
  cost.
- `/W4 /permissive- /WX` + `/fp:precise` clean on both platforms; one test file per unit; the full engine suite stays
  green per unit.

## Invariants this sprint must prove

All seven carried-forward invariants (ROADMAP "Carried-forward invariants"), with three explicit stress points (the
ROADMAP names S7 as one of the three places determinism is easiest to break):
1. **Determinism (R1)** — the process boundary is the new stress point; the run digest must be byte-identical across
   `{1 thread == N threads == 1 process == N processes}` and equal the sequential path. The substrate/worker-count/
   scheduler **never** appear in a result bit. No RNG in S7 except the inherited, recorded mining seed.
2. **No look-ahead / PIT / no-survivorship (R3)** — inherited from the workloads (the shards are the existing pure
   maps); S7 must **not** introduce a wall-clock read or a future read; the read-only input mapping enforces it
   structurally.
3. **Reduction to the sequential oracle (R7)** — the multi-substrate executor is pinned bit-for-bit to the proven
   single-thread path on the 1-worker / 1-process boundary; without this, S7 is not demonstrably a superset of the
   `DetPool` layer it lifts.

## Dependencies

- **Upstream (`p1`, assumed merged):** S2 (`parallel::{DetPool, parallel_evaluate, parallel_cpcv, parallel_backtests,
  FoldResult, result_table_digest, cpcv_aggregate_mean_sharpe, signal_set_digest}` — reused verbatim), S3/S4
  (`factory::{Factory, ResearchDriver, detail::seed_for_run}`, `library::Library` — the mine-loop the executor ports),
  the `data::ShmBarFeed` + `tsdb::SegmentReader` zero-copy SHM read substrate.
- **atx-core (already available — reuse, no edge):** `hash_bytes`/`hash_combine` (`hash.hpp`), `Result`/`Status`
  (`error.hpp`), `types.hpp`.
- **atx-core (Pattern B — new edges raised by this sprint):**

| S7 unit | Needs from `atx-core` | Engine-side fallback (shippable now) |
|---|---|---|
| S7.3 | **L4 `dist_pool`** — deterministic **multi-node** task distribution (order-fixed cross-machine merge; canonical-byte-order cross-machine digest) | engine-local `ProcessExecutor` over a deterministic shared-memory IPC shim; the **network transport** is the recorded lift |
| S7.2 | **L-tier `shm`/`proc`** (optional) — cross-platform shared memory + process spawn | engine-local `ShmSegment` (Win32 + POSIX behind one seam) reusing the `SegmentReader` mapping convention |

## Explicitly NOT in this sprint

- **No network / cross-machine transport** — the `IExecutor` seam admits a future `DistributedExecutor`; the actual
  network layer + the cross-machine canonical digest are the recorded atx-core L4 `dist_pool` lift (§2.1/§0.3). This
  sprint proves digest-invariance on **one machine**, across threads and processes.
- **No general job-orchestration framework** (anti-roadmap #4) — `ProcessExecutor` is a deterministic *eval/mine*
  distributor that does one thing (fan the digest-invariant workload + gather), **not** a Kubernetes/Spark/Ray-style
  scheduler. No dynamic cluster membership, no autoscaling, no DAG engine.
- **No new general-purpose primitive in the engine** (anti-roadmap #6) — the multi-node pool + the cross-platform SHM
  are recorded `atx-core` requests (Pattern B), engine-side shims only until the L-tier kernels land.
- **No change to the workload math** — S7 is pure infra; `run_one_fold`/`run_full_backtest`/`Engine::evaluate`/the GA
  mine bodies are **unchanged** (they were already pure maps). S7 changes *where* they run, never *what* they compute.
- **No per-partition correlation index** as the production path — the merged library rebuilds corr **once** in `AlphaId`
  order (deterministic); per-partition local corr + query-time neighbor merge is a recorded scale lift (§4.5).
- **No GPU / accelerator substrate** — the seam admits one in principle, but S7 targets CPU cores across processes only.

## Baton → next

S7 hands the whole `p2` factory a **substrate-agnostic execution seam**: any workload that is a pure map + a
reduce-by-sort (eval, cpcv, backtests, GA mining — and, later, S5 NN training fans and S6 alt-data ingestion) runs over
`IExecutor` and saturates all cores deterministically today, on either substrate, with one swappable handle. S7 hands
**S8** (the unified-fund capstone) the "distributed at scale" leg it routes through, and hands the **future N-node lift**
a finished abstraction where scale-out is *only* a new `IExecutor` impl + the recorded atx-core L4 transport — the
determinism contract, the slot layout, the reduce-by-sort, and the digest are already proven and do not change. With S7
closed, the `p2` scale ceiling is lifted from one core to all cores on one box, with the road to N boxes paved.

---

## References

> All primary sources, verified during the 2026-06-13 research pass (six-track fan-out web search + direct-fetch
> verification for the load-bearing primaries). Bibliographic anchors (DOI / arXiv / venue / official docs) confirmed
> against publisher + project pages. **[LB]** = load-bearing (a unit's design rests on it); **[S]** = supporting;
> **[X]** = a circulated claim that **failed verification** — recorded so the doc does not propagate it. Where a canonical
> page returned HTTP 403/TLS-failed, the claim was confirmed via an official mirror or the project's own docs and is
> flagged in the §1A caveat note.

### Track A — executor abstraction + single-box→multi-node scale-out

1. **[LB]** P. Moritz, R. Nishihara, et al. *Ray: A Distributed Framework for Emerging AI Applications.* OSDI 2018.
   arXiv:1712.05889 — <https://arxiv.org/abs/1712.05889>. Docs: <https://docs.ray.io/>. *(Tasks/actors unchanged
   local↔cluster; only `ray.init(address)` varies — the "swap the placement handle" pattern, R2/§4.1. Lineage
   re-execution assumes deterministic+idempotent tasks — R3/§4.6. Plasma/object-store zero-copy shared-memory reads —
   R5/§4.2. `ray.wait` completion-order trap — R4/§4.6.)*
2. **[LB]** M. Zaharia, et al. *Resilient Distributed Datasets: A Fault-Tolerant Abstraction for In-Memory Cluster
   Computing.* NSDI 2012 — <https://www.usenix.org/conference/nsdi12/technical-sessions/presentation/zaharia>. Spark RDD
   Programming Guide — <https://spark.apache.org/docs/latest/rdd-programming-guide.html>. *(Lineage recompute of a lost
   partition; `master` URL = the only local↔cluster change — R2/B4. The non-determinism-breaks-recompute negative proof
   below.)*
3. **[LB]** Apache Spark JIRA **SPARK-23207** *Shuffle+Repartition on a DataFrame could lead to incorrect answers* —
   <https://issues.apache.org/jira/browse/SPARK-23207> (and SPARK-51756). *(The canonical proof that a non-deterministic
   transformation breaks lineage-recompute correctness — grounds R3's purity precondition, §4.6/B4.)*
4. **[S]** Dask documentation — *Scheduling* <https://docs.dask.org/en/stable/scheduling.html>, *Resilience*
   <https://distributed.dask.org/en/stable/resilience.html>, *Client* <https://distributed.dask.org/en/stable/client.html>.
   *("Different schedulers compute the same result"; only the `Client` changes — R2. Recompute from the task graph — B4.
   Reduction-tree shape depends on `split_every`/partitions — the completion/partition-order trap, R4/§4.6.)*
5. **[LB]** E. Niebler, et al. *P2300R10 — `std::execution`.* WG21, 2024 —
   <https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html>. *(Scheduler = "a lightweight handle to an
   execution resource"; sender describes work; retarget by swapping the scheduler — the C++ sender/scheduler split
   grounding the `IExecutor` seam, R2/§4.1. Caveat: intra-process only; cross-node is a custom-scheduler extension.)*
6. **[S]** H. Kaiser, et al. *HPX — A Task Based Programming Model in a Global Address Space.* PGAS 2014. DOI:
   10.1145/2676870.2676883 — <https://dl.acm.org/doi/abs/10.1145/2676870.2676883>. Docs:
   <https://hpx-docs.stellar-group.org/>. *(AGAS makes `hpx::async` syntax identical local vs remote — the C++ vehicle
   that crosses the node boundary the future `DistributedExecutor` mirrors, R2.)*
7. **[S]** M. Bauer, S. Treichler, E. Slaughter, A. Aiken. *Legion: Expressing Locality and Independence with Logical
   Regions.* SC 2012. DOI: 10.1109/SC.2012.71 — <https://legion.stanford.edu/overview/>. *(Placement isolated in a
   **mapper**; "mapping decisions only impact performance and are orthogonal to correctness" — the exact contract of
   S7's scheduler-has-no-bit-contact rule, R6/§0.7/§4.4.)*
8. **[S]** QuantConnect LEAN *Algorithm Engine* <https://www.quantconnect.com/docs/v2/writing-algorithms/key-concepts/algorithm-engine>;
   Zipline (ML4T) <https://stefan-jansen.github.io/machine-learning-for-trading/08_ml4t_workflow/04_ml4t_workflow_with_zipline/>;
   NautilusTrader *Backtesting* <https://nautilustrader.io/docs/latest/concepts/backtesting/>; backtrader *Optimization*
   <https://www.backtrader.com/docu/optimization-improvements/>; Microsoft Qlib *Task Management*
   <https://qlib.readthedocs.io/en/latest/advanced/task_management.html>; vectorbt
   <https://vectorbt.dev/getting-started/features/>. *(The "single-backtest engine sequential+deterministic; parallelism
   one level up" pattern — A1/§4.1. Qlib's MongoDB shared task queue across machines; backtrader `multiprocessing`
   optstrategy.)*

### Track B — deterministic parallel execution, work distribution, IPC, fault tolerance

9. **[LB]** R. D. Blumofe, C. E. Leiserson. *Scheduling Multithreaded Computations by Work Stealing.* JACM 46(5):720–748,
   1999. DOI: 10.1145/324133.324234 — <https://dl.acm.org/doi/10.1145/324133.324234>. — M. Frigo, C. E. Leiserson, K. H.
   Randall. *The Implementation of the Cilk-5 Multithreaded Language.* PLDI 1998. DOI: 10.1145/277650.277725. *(The
   `T₁/P + O(T∞)` work-stealing bound — the justification for dynamic dispatch on load-imbalanced shards, R4/B1/§4.4.)*
10. **[LB]** oneAPI/oneTBB specification — *`parallel_deterministic_reduce`* and *`parallel_reduce`* —
    <https://oneapi-spec.uxlfoundation.org/specifications/oneapi/v1.3-rev-1/elements/onetbb/source/algorithms/functions/parallel_deterministic_reduce_func>;
    *Partitioner Summary* / *How the Task Scheduler Works* — <https://uxlfoundation.github.io/oneTBB/>. *("Same split/join
    regardless of thread count or task→thread mapping" — the decouple-placement-from-execution-order rule, R1/R4/§4.4. The
    `static_partitioner` "without load balancing" tradeoff. Random-deque steal = nondeterministic execution order.)*
11. **[LB]** D. Goldberg. *What Every Computer Scientist Should Know About Floating-Point Arithmetic.* ACM Computing
    Surveys 23(1):5–48, 1991. DOI: 10.1145/103162.103163. *(IEEE-754 `+` non-associativity — why reduction order must be
    fixed, R4/B2/§4.6.)*
12. **[LB]** J. Demmel, H. D. Nguyen. *Parallel Reproducible Summation.* IEEE Trans. Computers 64(7):2060–2070, 2015. —
    P. Ahrens, J. Demmel, H. D. Nguyen. *Efficient Reproducible Floating Point Summation and BLAS.* UCB/EECS-2016-121.
    **ReproBLAS** — <https://bebop.cs.berkeley.edu/reproblas/>. *(Order-INDEPENDENT reproducible summation — the recorded
    refinement if a future reduction cannot fix its order, R4/§4.6.)*
13. **[S]** N. J. Higham. *The Accuracy of Floating-Point Summation.* SIAM J. Sci. Comput. 14(4):783–799, 1993. DOI:
    10.1137/0914050. — A. Neumaier. *Rundungsfehleranalyse einiger Verfahren zur Summation endlicher Summen.* ZAMM
    54(1):39–51, 1974. DOI: 10.1002/zamm.19740540106. *(Kahan/Neumaier compensated summation — the as-built
    `cpcv_aggregate_mean_sharpe` fold; accuracy under a fixed order, R4.)*
14. **[S]** PyTorch *Reproducibility* notes — <https://pytorch.org/docs/stable/notes/randomness.html>. *(Float
    `atomicAdd` is non-deterministic in order ⇒ never atomic-accumulate floats in result math, R4/§4.6.)*
15. **[LB]** Linux `mmap(2)` — <https://man7.org/linux/man-pages/man2/mmap.2.html>; `shm_overview(7)` —
    <https://man7.org/linux/man-pages/man7/shm_overview.7.html>. Microsoft *Creating Named Shared Memory* / *Sharing Files
    and Memory* — <https://learn.microsoft.com/en-us/windows/win32/memory/creating-named-shared-memory>. *(The
    cross-platform read-only zero-copy SHM substrate — `MAP_SHARED`/`PROT_READ` and `CreateFileMapping`/`MapViewOfFile`,
    R5/§4.2/§0.6.)*
16. **[S]** Apache Arrow *IPC* <https://arrow.apache.org/docs/cpp/ipc.html> + *C Data Interface*
    <https://arrow.apache.org/docs/format/CDataInterface.html>; Ray *object serialization*
    <https://docs.ray.io/en/latest/ray-core/objects/serialization.html>; Plasma blog (2017)
    <https://arrow.apache.org/blog/2017/08/08/plasma-in-memory-object-store/>. *(Zero-copy memory-mapped IPC; "one
    shared-memory copy, N read-only workers" validated in production — R5/§4.2.)*
17. **[LB]** U. Drepper. *What Every Programmer Should Know About Memory.* 2007 —
    <https://akkadia.org/drepper/cpumemory.pdf>. *(False-sharing super-linear blow-up (390/734/1147% for 2/3/4 threads,
    §6.4); NUMA first-touch (§5); affinity (`sched_setaffinity`, §6.4.3) — R6/§4.4/§1A B6.)*
18. **[LB]** S. Williams, A. Waterman, D. Patterson. *Roofline: An Insightful Visual Performance Model for Multicore
    Architectures.* Communications of the ACM 52(4):65–76, 2009. DOI: 10.1145/1498765.1498785. *(`P = min(π, β·I)` — the
    bandwidth ceiling: adding cores past the knee buys nothing for streaming kernels — the pool-size-to-the-knee rule,
    R6/§4.4.)*
19. **[S]** cppreference — *`std::hardware_destructive_interference_size`* —
    <https://en.cppreference.com/w/cpp/thread/hardware_destructive_interference_size> (C++17 `[hardware.interference]`).
    OpenMP 5.0 — *`OMP_PROC_BIND`* <https://www.openmp.org/spec-html/5.0/openmpse52.html> / *`OMP_PLACES`*. **hwloc** —
    <https://www.open-mpi.org/projects/hwloc/>. *(Cache-line slot padding + thread pinning/topology — R6/§4.4.)*
20. **[S]** *NuCore* (W. Wang, et al.) — <https://wwang.github.io/papers/NuCore.pdf>. — J. E. Hoogenboom. *Is Monte Carlo
    Embarrassingly Parallel?* PHYSOR/ANS 2012 — <https://www.osti.gov/biblio/22105636>. *(Measured: a bandwidth-bound
    kernel runs 3.34× faster on 6 of 48 cores; ~75.6% of cores is the avg optimum — all-cores can be SLOWER. Even
    "embarrassingly parallel" work scales sublinearly once collection/synchronization rendezvous appear — the collector,
    not the per-shard math, is where scaling dies. Grounds the §4.4 perf contract: "near-linear until the knee", not
    "linear".)*

> **Verification caveats carried from the research pass:** (a) the Blumofe-Leiserson space/lower bounds and the exact
> Cilk-5 time-bound wording were confirmed via secondary restatements (ACM PDF 403'd); (b) cppreference's verbatim
> interference-size wording is from its search index (page 403'd), the C++17 `[hardware.interference]` intent confirmed;
> (c) the OpenMP "spread ≈ 2× close" magnitude is illustrative (workload-specific) — the *direction* (single-socket
> bandwidth favors spread) is well-supported; (d) the oneTBB oversubscription "quadratic threads" quote is from
> Intel-indexed snippets (canonical Appendix-B page 403'd); (e) the NuCore/Hoogenboom numbers are measured but
> workload-specific; (f) WorldQuant BRAIN concurrency numbers (§1A A1) are community-wrapper-reported, not official —
> none of (a)–(f) is load-bearing for a *correctness* rule (they ground the §4.4 *performance* layer, which has no
> bit-level contract). The IPC, P2300, oneTBB-determinism, Goldberg, Roofline, Spark-lineage, and Ray primaries (the
> load-bearing rules R1–R6) were verified first-hand.

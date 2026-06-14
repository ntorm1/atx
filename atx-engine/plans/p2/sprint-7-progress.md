# Sprint 7 (p2) — Deterministic Multi-Core Execution Substrate (Scale-Out-Ready) — Implementation Progress

**Status:** ✅ COMPLETE — **S7-b done** — all units S7.0–S7.6 implemented, reviewed (spec + quality), and verified. Scheduler + all-four-workloads-over-process (eval/cpcv/backtests/mine) + five-path digest capstone + fault tolerance + benchmark. Full `atx-engine` suite **1262/1262 green** under clang-cl `/W4 /permissive- /WX /fp:precise`. Awaiting user merge (`--no-ff`); branch not pushed. (2026-06-14)
**Worktree:** `C:\Users\natha\atx-wt\p2-s7` (isolated; one branch per worktree)
**Branch:** `feat/p2-s7-scale-out`
**Base:** `main` @ `f85f3d3` (p1 S1–S8 merged + the `parallel/` header→source refactor)
**Started:** 2026-06-13
**Source plan:** [`sprint-7-distributed-scale-out-compute-implementation-plan.md`](sprint-7-distributed-scale-out-compute-implementation-plan.md)
**Spec (the *what*):** [`ROADMAP.md`](ROADMAP.md) §S7

> **Execution mode:** subagent-driven development (fresh implementer per unit + two-stage review:
> spec-compliance then code-quality). **Cadence (user-set):** gate at **S7-a** — implement S7.0–S7.3
> (seam + SHM substrate + process pool), then stop and report before S7.4–S7.6.

---

## Plan adjustments vs. the source plan (the §0 as-built amendment)

The implementation plan's §0 reconciles the frozen *how* against reconnaissance of the as-built `parallel/` +
`factory/` + `library/` layers + the test/build wiring. The load-bearing decisions (full text in the plan §0.1–§0.8):

1. **§0.1** `DetPool` is lifted **behind a seam**, never rewritten — it becomes the body of `ThreadExecutor` and the
   in-process oracle the process path is pinned to.
2. **§0.2** The pure-map → pre-indexed-slot → reduce-by-sort recipe is already substrate-agnostic; S7 carries it across
   the **process** boundary verbatim (slots become shared-memory slots; the Neumaier reduce runs in the parent).
3. **§0.3** `digest.hpp` is intra-process, but the parent always gathers raw `f64` result **bytes** and digests in **one**
   process — so on-machine digest-invariance holds; the cross-*machine* canonical digest is a recorded lift.
4. **§0.4** `library::admit` is the lone process hazard → **partition-per-worker (disjoint AlphaId range) + canonical
   manifest merge**; `seed_for_run` is already process-safe.
5. **§0.5** The boundary pin = **five-path digest-invariance** `{sequential, ThreadExecutor@{1,N}, ProcessExecutor@{1,N}}`.
6. **§0.6** `ProcessExecutor` is the shipping primary; threads are the degenerate 1-process case; **both** Win32 + POSIX
   SHM backends behind one seam.
7. **§0.7** Heterogeneous shard cost → dynamic dispatch for load balance with placement decoupled from steal order; the
   scheduler is a *performance* layer with **no** bit contact.
8. **§0.8 (NEW, S7-0 amendment, user-confirmed):** the worker process is a **dedicated `atx-shm-worker` executable**
   dispatched by a closed **`WorkloadId`** enum — re-exec-self is **not viable** (every `*_test.cpp` links one
   `gtest_main` exe; no per-test `main` to intercept; Windows has no `fork`). One portable seam
   (`posix_spawn`/`CreateProcess`), no backend divergence. Adds **one** hand-edit to `atx-engine/CMakeLists.txt` (the new
   exe target — the only CMake edit this sprint). S7.3 proves the pool end-to-end with a trivial `Test` arithmetic
   workload; the real `Eval`/`Cpcv`/`Backtests`/`Mine` workloads are registered in S7.5.

**Build environment note:** the worktree builds via the `ninja` CMake preset (clang-cl + bundled Ninja + vcpkg at
`C:\Users\natha\vcpkg`) sourced through a local uncommitted `s7env.cmd` wrapper (vcvars64 + `VCPKG_ROOT`); the `dev`
preset's sccache/`ATX_DEPS_DIR` are absent in this shell. Submodule `atx-core/third-party/databento-cpp` was
`submodule update --init`'d into the worktree (worktree-add does not populate submodules).

---

## Per-unit status

| Unit  | Title                                                        | Status   | Commit | Tests | Notes |
|-------|--------------------------------------------------------------|----------|--------|-------|-------|
| S7-0  | Marker + ledger + plan §0.8 amendment                        | ✅ done   | `f53de17` | —     | Brought the frozen plan + ROADMAP onto the branch; added §0.8 (dedicated-worker-exe + `WorkloadId`); opened this ledger. |
| S7-1  | `IExecutor` seam + `ThreadExecutor` (DetPool adapter)        | ✅ done   | `1b22d86`+`bb82d1b` | 12 | Seam = `ShardId`/`WorkloadId`/`InputView`/`SlotView`(cache-line-padded)/`ShardFn`(fn-ptr, process-portable)/registry/`IExecutor`. `ThreadExecutor` wraps `DetPool`; lowest-id error = ascending scan. **digest({1,2,4,8})==sequential** proven. Quality fix: `ATX_ASSERT` slot/make_slot_view preconditions + death test; drop unused `<span>`. Both reviews ✅. |
| S7-2  | Cross-platform `ShmSegment` (Win32 + POSIX)                  | ✅ done   | `4929f4c`+`0a75ed3` | 13 | Named RO/RW segments, RAII move-only, mirrors `tsdb::Mapping`. Length-header solves Win32 size-on-open; RO-write **faults** (death test). Quality fix: validate header len vs real mapped size (`VirtualQuery`/`fstat`) → no OOB span; least-privilege `FILE_MAP_WRITE`; reject ambiguous names + `// SECURITY:` note. Both reviews ✅. **POSIX backend compiled-out on this host → Linux-CI must run it.** |
| S7-3  | `ProcessExecutor` + `atx-shm-worker` exe + `WorkloadId`      | ✅ done   | `c0a6873`+`aa9e9e4` | 10 | Primary multi-process pool, uniform `IExecutor::submit` (internal SHM copy-in/out); cross-process `atomic_ref` claim cursor; dedicated worker exe (sibling-discovery + `ATX_SHM_WORKER` override); `WaitForMultipleObjects`/`waitpid` barrier. **`digest(process@{1,4})==digest(thread@{1,4})==sequential`** holds. Quality fix: **inspect worker exit codes** (crash w/o ErrorSlot ⇒ deterministic `Err`, +`WorkerAbnormalExitSurfacesAsError` test) + cursor alignment guard + EINTR + `from_chars` worker-id. Both reviews ✅. **POSIX abnormal-exit path Linux-CI-pending.** |
| —     | **S7-a GATE → report to user**                               | ✅ reached | —      | —     | S7.0–S7.3 done (each: implement → spec-review → quality-review → fix → verify). STOP for user review before S7.4–S7.6. |
| S7-4  | Deterministic heterogeneous-cost scheduler + NUMA            | ✅ done   | `e8c066b`+`1409045` | 14 | LPT descending-cost `dispatch_order` (placement decoupled from result bits, §0.7); `Topology`/`query_topology`; `pin_and_first_touch` platform seam; single-pool oversubscription guard. Quality fix: POSIX `CPU_SET` guarded vs `CPU_SETSIZE` OOB (match Win32 `pu<64`). Both reviews ✅. **POSIX pin path Linux-CI-pending.** |
| S7-5a | In-process `IExecutor&` workload port                        | ✅ done   | `6e96376`+`2873c1b` | 9  | eval/cpcv/backtests over the substrate-agnostic seam (ThreadExecutor); ONE shared map impl behind both the `DetPool&` and `IExecutor&` overloads (no copy-paste divergence). Quality fix: `exec_dispatch` `ATX_ASSERT`→`ATX_CHECK` — a debug-only assert swallowed the `parallel_for` Status, so under NDEBUG a ProcessExecutor passed to `parallel_cpcv/backtests` returned silent zero-filled results (+ death tests). Both reviews ✅. |
| S7-5b | backtests + cpcv over the PROCESS boundary                   | ✅ done   | `0287c27` | 9  | Serialize `AlphaStreams` → `WorkloadId::Backtests`/`Cpcv` + registered `ShardFn` + byte `InputView`; cpcv test-idx + dimension products validated before any span. **`digest(process@{1,4})==thread@{1,8}==sequential`** (result_table_digest). Quality fix (self-review): `checked_mul` dimension-overflow guard (untrusted `na*np*ni` wrap). Both reviews ✅. |
| S7-5c | eval over the PROCESS boundary                               | ✅ done   | `ab8d36f`+`cd46d72` | 8  | Serialize bytecode `Program` + `Panel` → `WorkloadId::Eval`; variable per-program root count handled by a `max_nroots` uniform slot with the parent supplying root NAMES (never cross back); `signal_set_digest` five-path. Quality fix: ALWAYS-ON guard before the `eval_shard` slot memcpy (a debug-only assert an NDEBUG build elides before a memcpy = source OOB). Both reviews ✅. |
| S7-5d | **mine** over the PROCESS boundary (capstone workload)       | ✅ done   | `93232b0`+`0ca7c14` | 9  | **SOUND design (§0.9):** parallelize the pure per-genome compile+eval+pool-aware-fitness map over process; the parent runs the EXISTING deterministic rank + the SEQUENTIAL `library::admit` fold → byte-identical `FactoryReport::digest` AND library `version_id`, five-path. Genome crosses by op-NAME (deterministic, validated) + re-`analyze`; run-start pool snapshot rebuilds the same `CorrNeighborIndex`; reuses the S7-5c Panel ser. Impl found+fixed a real determinism bug (positions written with the pool-snapshot `T` instead of the realized-stream `T`). Quality fix: 4 Critical + 2 High integer-overflow / NDEBUG-memcpy hardening on the slot/offset math. Both reviews ✅. **Replaces the UNSOUND frozen §0.4 partition-merge** (stateful admit fold; `AlphaId` in admission order; `segment_crc`/`integrity_crc` fold `base_alpha_id`). |
| S7-6  | Five-path digest capstone + fault tolerance + benchmark      | ✅ done   | `4e4a8dd`+`1917849` | 9  | ONE table-driven five-path pin `{sequential, Thread@{1,4,8}, Process@{1,4}}` over ALL FOUR workloads (fresh substrate + fresh empty library per mine leg; oracle non-vacuity asserted). Fault tolerance: a worker self-exits mid-shard → deterministic `Err` (invariant across worker counts + repeats), and a clean re-run after a fault is bit-identical to the sequential reference. `bench/executor_bench.cpp` (speedup/knee table; ProcessExecutor ~2× on the bench workload; determinism self-check). Quality fix: capstone builders `ATX_CHECK`-abort so the headline test can never pass vacuously. Both reviews ✅. **POSIX external-kill leg Linux-CI-pending.** |
| —     | **S7-b GATE → report to user**                               | ✅ reached | —      | —     | S7.4–S7.6 done (each: implement → spec-review → quality-review → fix → re-verify). Full suite **1262/1262**. Branch not pushed/merged — awaiting user `--no-ff` merge. |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `f53de17` | S7-0 | docs(s7-0): open sprint-7 ledger + bring plan/ROADMAP onto branch + §0.8 worker-exe amendment |
| `1b22d86` | S7-1 | feat(s7-1): IExecutor seam + ThreadExecutor (DetPool adapter) |
| `bb82d1b` | S7-1 | fix(s7-1): assert SlotView/make_slot_view preconditions; drop unused `<span>` |
| `4929f4c` | S7-2 | feat(s7-2): cross-platform ShmSegment (Win32 + POSIX named shared memory) |
| `0a75ed3` | S7-2 | fix(s7-2): validate shm length header vs mapped size; least-privilege FILE_MAP_WRITE; reject ambiguous names |
| `c0a6873` | S7-3 | feat(s7-3): ProcessExecutor + atx-shm-worker (multi-process digest-invariant substrate) |
| `aa9e9e4` | S7-3 | fix(s7-3): detect abnormal worker exit; guard cursor atomic alignment; EINTR + worker-id validation |
| `a224320` | S7-a | test: disable 2 pre-existing LibraryIntegration failures (unblock S7 baseline) |
| `f2c6b88` | S7-a | docs(s7-a): mark S7.0–S7.3 done at the S7-a gate (seam + SHM + process pool) |
| `e8c066b` | S7-4 | feat(s7-4): deterministic heterogeneous-cost scheduler + NUMA pinning |
| `1409045` | S7-4 | fix(s7-4): guard POSIX CPU_SET against CPU_SETSIZE OOB (match Win32 pu<64) |
| `6e96376` | S7-5a | feat(s7-5a): in-process IExecutor& workload port (eval/cpcv/backtests) |
| `2873c1b` | S7-5a | fix(s7-5a): abort on out-of-process executor misuse (ATX_CHECK, not debug assert) |
| `0287c27` | S7-5b | feat(s7-5b): backtests + cpcv real workloads over the process boundary (serialized AlphaStreams) |
| `ab8d36f` | S7-5c | feat(s7-5c): eval over the process boundary (serialized Program + Panel) |
| `cd46d72` | S7-5c | fix(s7-5c): always-on guard before eval_shard slot memcpy (NDEBUG OOB) |
| `93232b0` | S7-5d | feat(s7-5d): mine_into over the process boundary (sound parallel-eval + sequential parent admit) |
| `0ca7c14` | S7-5d | fix(s7-5d): harden mine slot/offset arithmetic against integer overflow |
| `4e4a8dd` | S7-6 | feat(s7-6): five-path digest capstone + fault-tolerance test + executor benchmark |
| `1917849` | S7-6 | fix(s7-6): builders abort loudly so the five-path capstone can never pass vacuously |

**S7-a totals (gate):** 3 coding units + marker · **35 new tests** (ParallelExecutor 12 · ParallelShmSegment 13 · ParallelProcessExecutor 10) · full engine suite green under clang-cl `/W4 /permissive- /WX` + strict-FP · each unit through implement → spec-compliance review → code-quality review → fix → re-verify · `atx-core/*` + `atx-tsdb/*` untouched across the whole range.

**S7-b totals (S7.4–S7.6):** 5 coding units (S7-4 scheduler · S7-5a/b/c/d workload port · S7-6 capstone) · **~58 new tests** (Scheduler 14 · WorkloadPort 7 · backtests/cpcv-process 9 · eval-process 8 · mine-process 9 · five-path capstone 4 · fault tolerance 5 + a benchmark target) · 9 feat + 5 fix commits · **all four real workloads (eval / cpcv / backtests / mine) proven byte-identical across the five paths `{sequential, ThreadExecutor@{1,N}, ProcessExecutor@{1,N}}`** — incl. mine's `FactoryReport::digest` AND library `version_id`. Each unit through implement → spec review → code-quality review → fix → re-verify. **Full `atx-engine` suite 1262/1262** under clang-cl `/W4 /permissive- /WX /fp:precise`. `atx-core/*` + `atx-tsdb/*` untouched across the whole sprint.

**Sprint total (S7-a + S7-b):** 8 coding units + marker · ~93 new tests · 16 feat + 11 fix/marker commits · the `IExecutor` seam, cross-platform SHM substrate, dedicated `atx-shm-worker` exe, deterministic cost scheduler, all four workloads over the process boundary, and the five-path determinism capstone + fault tolerance + benchmark — shipped and green on a single Windows box.

---

## Open cross-platform residual (carry into S7-b / CI)

The host is **Windows**; both OS backends are written behind one seam, but only the **Win32** path is compiled+run here. A **Linux CI runner must build + `ParallelShmSegment`/`ParallelProcessExecutor`-test the POSIX backends** (`shm_open`/`ftruncate`/`mmap`; `posix_spawn`/`waitpid`+`EINTR`; `WIFSIGNALED` abnormal-exit; `readlink(/proc/self/exe)` discovery). Logic mirrors the validated Win32 path but is empirically unverified on POSIX.

---

## Close residuals → ROADMAP backlog

Carried to the `p2` ROADMAP backlog (out of S7 scope; recorded here at close):

1. **N-node network transport** — the multi-process pool is the single-box primitive; the cross-machine work distributor (atx-core L4 `dist_pool` over a network/RDMA transport) is the recorded lift. The `IExecutor` seam is already the insertion point (a third `Substrate`).
2. **Cross-machine canonical digest** — `hash_bytes`/`signal_set_digest` are deterministic only WITHIN a process (compile-time wyhash seeds, endianness not normalized). On-machine five-path invariance holds because the parent always gathers raw result bytes and digests in ONE process; a byte-canonical (endian-normalized) digest is required before comparing digests ACROSS heterogeneous machines.
3. **Library scale-out** — §0.9 establishes that `library::admit` is a sequential stateful fold (cannot be byte-identically partition-merged); a future per-partition corr index + query-time merge that preserves `version_id` determinism is an open design, not a drop-in.
4. **atx-core `shm`/`proc` primitives** — `ShmSegment` + the spawn/barrier live in `atx-engine` for now; promote to an atx-core L-layer primitive if/when a second consumer appears.
5. **POSIX backends are Linux-CI-pending** — see the cross-platform residual above: `ShmSegment`/`ProcessExecutor`/scheduler-pin/worker-abnormal-exit POSIX paths are written behind the seam but compiled+run only on Win32 on this host.

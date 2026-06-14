# Sprint 7 (p2) — Deterministic Multi-Core Execution Substrate (Scale-Out-Ready) — Implementation Progress

**Status:** 🚧 IN PROGRESS — **S7-a gate reached** (S7.0–S7.3 done; paused for user review before S7.4–S7.6) (2026-06-13)
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
| S7-4  | Deterministic heterogeneous-cost scheduler + NUMA            | ⏳ later   | —      | —     | (post-gate) Performance layer; no bit contact. |
| S7-5  | Workload port (`IExecutor&` overloads) + `library::partition`| ⏳ later   | —      | —     | (post-gate) eval/cpcv/backtests/mine over the seam; partition-and-merge library. |
| S7-6  | Digest-invariance capstone + fault tolerance + bench + close | ⏳ later   | —      | —     | (post-gate) Five-path pin; failed-shard bit-identical replay; speedup/knee bench. |

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

**S7-a totals (gate):** 3 coding units + marker · **35 new tests** (ParallelExecutor 12 · ParallelShmSegment 13 · ParallelProcessExecutor 10) · full engine suite green under clang-cl `/W4 /permissive- /WX` + strict-FP · each unit through implement → spec-compliance review → code-quality review → fix → re-verify · `atx-core/*` + `atx-tsdb/*` untouched across the whole range.

---

## Open cross-platform residual (carry into S7-b / CI)

The host is **Windows**; both OS backends are written behind one seam, but only the **Win32** path is compiled+run here. A **Linux CI runner must build + `ParallelShmSegment`/`ParallelProcessExecutor`-test the POSIX backends** (`shm_open`/`ftruncate`/`mmap`; `posix_spawn`/`waitpid`+`EINTR`; `WIFSIGNALED` abnormal-exit; `readlink(/proc/self/exe)` discovery). Logic mirrors the validated Win32 path but is empirically unverified on POSIX.

---

## Close residuals → ROADMAP backlog

_(populated at sprint close — network transport / atx-core L4 `dist_pool`; cross-machine canonical digest; per-partition corr
index + query-time merge; atx-core `shm`/`proc` primitive if a second consumer appears.)_

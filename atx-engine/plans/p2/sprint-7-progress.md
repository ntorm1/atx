# Sprint 7 (p2) — Deterministic Multi-Core Execution Substrate (Scale-Out-Ready) — Implementation Progress

**Status:** 🚧 IN PROGRESS (opened 2026-06-13)
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
| S7-0  | Marker + ledger + plan §0.8 amendment                        | ✅ done   | —      | —     | Brought the frozen plan + ROADMAP onto the branch; added §0.8 (dedicated-worker-exe + `WorkloadId`); opened this ledger. |
| S7-1  | `IExecutor` seam + `ThreadExecutor` (DetPool adapter)        | ⏳ pending | —      | —     | The SWITCH POINT target; `WorkUnit`/`ShardId`/`OutputSlots`/`ExecutorConfig`; `ThreadExecutor` == `DetPool` by construction. |
| S7-2  | Cross-platform `ShmSegment` (Win32 + POSIX)                  | ⏳ pending | —      | —     | RO-input (`PROT_READ` faults on write) + pre-indexed cache-line-padded output slots; reuse `SegmentReader` mapping convention. |
| S7-3  | `ProcessExecutor` + `atx-shm-worker` exe + `WorkloadId`      | ⏳ pending | —      | —     | Primary multi-process pool; `Test` workload; 5-path digest-invariance + lowest-id error across the process boundary. |
| —     | **S7-a GATE → report to user**                               | ⏳ pending | —      | —     | Stop after S7-3; user reviews before S7.4–S7.6. |
| S7-4  | Deterministic heterogeneous-cost scheduler + NUMA            | ⏳ later   | —      | —     | (post-gate) Performance layer; no bit contact. |
| S7-5  | Workload port (`IExecutor&` overloads) + `library::partition`| ⏳ later   | —      | —     | (post-gate) eval/cpcv/backtests/mine over the seam; partition-and-merge library. |
| S7-6  | Digest-invariance capstone + fault tolerance + bench + close | ⏳ later   | —      | —     | (post-gate) Five-path pin; failed-shard bit-identical replay; speedup/knee bench. |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| _pending_ | S7-0 | docs(s7-0): open sprint-7 ledger + bring plan/ROADMAP onto branch + §0.8 worker-exe amendment |

---

## Close residuals → ROADMAP backlog

_(populated at close — network transport / atx-core L4 `dist_pool`; cross-machine canonical digest; per-partition corr
index + query-time merge; atx-core `shm`/`proc` primitive if a second consumer appears.)_

# All-core scheduling of the graded surface fit

Lane P0 (vol-sota r6, 2026-10-23). This page covers how `fit_quote_surface` spreads its
per-expiry work over cores, and what it guarantees about the served numbers when it does.
It also gives the nested-dispatch rule, the L575 disposition and the deployment-hardware
pinning plan. Measurements are in the lane report
(`.superpowers/sdd/2026-10-18-vol-sota/lane-P0-all-core-r6-report.md`; evidence
`C:\atx-cache\quote-benchmark\P0-20261023-all-core\`). Every timing there is a
DEVELOPMENT row from a shared i7-1260P host, and none of them is a bar result.

## 1. What runs in parallel and what stays serial

`fit_quote_surface` (`src/fitting/quote_surface.cpp`, `SurfaceFitter`) fits one surface in
stages:

| Stage | Shape | Why |
|---|---|---|
| pass A (or the ladder's knot pass) | per-expiry fan-out | each expiry's first fit reads only its own chain, the configs and a prior built before the stage |
| B1, B2 channel refits | per-expiry fan-out | the same contract, over the expiries the stage selects |
| repair rungs, calendar pass, dominance pass, market-distance pass, publication | serial on the caller | each one reads more than one expiry, or mutates state that later expiries read |

Each fan-out stage goes through `SurfaceFitter::for_each_expiry`. That function:

1. Orders the stage's expiries **longest first** (`detail::longest_first`,
   `src/fitting/expiry_schedule.hpp`). It does a stable descending sort on a cost known
   before the fit, the expiry's strictly two-sided quote count. Equal costs keep their
   input order, and there is no timing feedback. Starting the densest expiry first keeps
   it off the end of the stage.
2. Runs the tasks through `detail::run_expiry_tasks` (`src/fitting/expiry_pool.hpp`).
   - At width 1 the tasks run serially on the caller, in task order. This is the
     historical serial path.
   - At any other width they run on the library's one persistent pool, `pricing_executor()`.

**Width.** `QuoteSurfaceConfig::fit_workers` is read verbatim. AUTO (0) resolves through
the thread's elastic resolver, else the `ATX_VOL_FIT_WORKERS` cap, else
`hardware_concurrency`. The result is never more than the number of tasks. The pool then
clamps the width down to `size() + 1`, its workers plus the caller. A stage with fewer
than 4 tasks (`kInlineThreshold`) runs inline on the caller. A throwing task is rethrown
on the caller once every context has stopped, and `fit_quote_surface` returns its typed
error, so no partly written surface is ever returned.

**Bounded pool, no thread per fit.** Before lane P0 every fan-out stage started
`fit_workers` fresh `std::jthread`s (`parallel_for_dynamic`), and the caller only waited
on them. Now:

- The pool is built once per process, with `atx_auto_worker_count() - 1` workers (the
  same `ATX_VOL_FIT_WORKERS` knob).
- The caller takes part as context 0.
- After the first fit, no fit creates a thread. Test:
  `FitFanoutDeterminism.FitsRunOnABoundedSetOfThreads` runs `size()/4 + 2` consecutive
  surface fits and observes at most `size() + 1` distinct task threads.

**Exclusive per-expiry scratch.** A task writes only its own expiry's slots: `out_[i]`,
`breadth_[i]`, `two_sided_[i]` and `held_point_[i]`. It also installs that expiry's rung
memo (`ScopedRungMemo`) for the task's duration. Everything else it reads is state no task
writes. Thread-local caches (the de-Am inverse memories, Eigen's internal workspaces) are
value-neutral. The serial path has always reused them across expiries and across fits on
one thread (quote-bench `--reps`, `predictions_identical_to_rep1`).
`FitFanoutDeterminism.SurfaceRowsAreByteIdenticalAcrossWidthsAndSchedules` pollutes them
before every task (a foreign American IV, a foreign blocked Eigen product, a NaN-filled
heap block) and requires the same bytes.

## 2. Determinism contract

**Served rows are byte-identical for every width, every claim order, every thread and
every core.** That covers fit_workers 1, 2, 4, P-cores and all cores, on any core type.
The proof has three parts:

- **Slot ownership.** Which context claims a task never changes what the task computes.
  Each task writes only its own slot, from inputs no task writes (above).
- **Serial cross-expiry stages.** Every stage that reads two expiries runs on the caller,
  in a fixed order, after the join.
- **No thread- or core-dependent arithmetic.** Eigen's blocking is pinned at build time.
  This was the inherited "fan-out" defect, described next.

### The inherited C2 fan-out defect (root cause and fix)

Eigen 3.4 sizes its blocked GEMM and TRSM kernels from L1, L2 and L3 sizes. It reads those
sizes with CPUID **once per process**, in the function-local static that the first blocked
product builds (`Eigen::internal::manage_caching_sizes`).

On a hybrid CPU the answer depends on the core type that runs that first product:

| Core on the i7-1260P | L1 | L2 | L3 | GEBP `kc` |
|---|---|---|---|---|
| P core | 48 KiB | 1.25 MiB | 18 MiB | 376 |
| E core | 32 KiB | 2 MiB | 18 MiB | 248 |

A different `kc` gives a different floating-point summation order in every blocked dense
product, for example the curve solve's `B'WB` on a slice with hundreds of distinct strikes.
So one process served one hash for the whole run, and another process served a different
one (SPY h0 + repair: `ba413a85…` against `99f67416…`).

The fan-out only made an E-core first product more likely: a fresh worker thread lands on
an E core more often than the foreground caller does. A serial run pinned to an E core
flipped too.

**Fix.** The root `CMakeLists.txt` adds four compile definitions to the `eigen` INTERFACE
target: `EIGEN_NO_CPUID` and `EIGEN_DEFAULT_L{1,2,3}_CACHE_SIZE`. The values are the
i7-1260P P-core sizes (49152 / 1310720 / 18874368), the ones every recorded reference hash
was produced under.

- The definitions are compile-time, ODR-consistent across every TU that includes Eigen,
  and part of the exported `eigen` target.
- Eigen never queries the CPU again.
- Served numbers no longer depend on the core, the thread or the worker count. They do not
  depend on the host's caches either: a different machine computes the same blocking.

Tests (`tests/fit_fanout_determinism_test.cpp`, suite `FitFanoutDeterminism`, label
`atx_vol_quote_slice`):

- `EigenCacheQueryIsTheSameOnEveryCore` pins to every logical CPU and requires one answer.
- `EigenBlockingUsesThePinnedCacheSizes`.
- `DenseSliceIgnoresTheCoreTypeThatRunsIt` fits a 601-strike dense repair slice in child
  processes pinned to the P-core mask and to the E-core mask, and requires one hash.
- `SurfaceRowsAreByteIdenticalAcrossWidthsAndSchedules` runs widths 1 / 2 / 4 / auto, with
  and without per-task pollution.

On the pre-fix root, the core-type test is RED: P `4546e5e6…`, E `7a96de1f…`.

**Consequence for references.** Every hash recorded on a P-core caller is unchanged. A
run that, before this fix, happened to make its first blocked product on an E core
produced one of the alternate hashes. Those alternates (`99f67416…` and `be888dc5…` among
them) are no longer reachable.

**Changing the pinned sizes.** Changing the sizes changes served numbers on dense slices,
in the last bits. It is a numerical decision and needs a new reference set. It is never a
tuning knob.

## 3. Nested-dispatch rule (no oversubscription)

A fit task is pool work. Therefore:

- A plain `pricing_executor().run_*` issued inside a fit task runs **inline** on the
  task's thread. Test: `FitFanoutDeterminism.PricingDispatchInsideAFitTaskRunsInline`
  issues a 64-block `run_blocks` from every task and observes no block on a foreign
  thread.
- A `fit_quote_surface` called from inside pool work runs its stages inline too.
- Only the `*_nested` variants may add one level, and they draw only on the idle workers
  the enclosing dispatch left parked (`nested_budget()`). Live fitting never calls them.
- The jthread fan-outs in `core/parallel_for.hpp` are not reachable from a
  `fit_quote_surface` task (audit: every `parallel_for*` call site in `src/`).
- Concurrent top-level fits, for example board fits on a populate scheduler's outer
  threads, share the pool's workers through its help-first join. The thread count is
  bounded by the callers plus the pool, never by callers × `fit_workers` as it was with
  per-stage jthreads.

### L575 (`PricingExecutor.NestedDispatch_ReachesAPoolWorkerAndWakesThePool`)

This is a test race, not an executor defect. The test asserts that a nested dispatch's
queued context is picked up by a pool worker. But the join is help-first: the dispatcher
drains the shared queue itself. On a loaded host it could finish row 0's queued context
before any parked worker woke, which is correct executor behaviour, and the assertion then
failed. The test now has the dispatcher, when it reaches row 0's caller context, wait
(bounded at 60 s) until a pool worker has started a queued row-0 context. It then asserts
the wait never timed out.

| Run | Repeats | Failures | Workers cap | Host load |
|---|---|---|---|---|
| Before the change | 80 | 20 | 3 | 59 % |
| After, idle host | 200 | 0 | 3 | 23 % |
| After, loaded host | 200 | 0 | 3 | 71 → 55 % |

The executor code is unchanged.

## 4. Measuring it

Use quote-bench with `--timing-detail`:

- `graded_stats.timing_detail.chain_wall_us` is the sum of the chain walls. Compare it
  with `surface_wall_us`, the elapsed surface wall.
- `graded_stats.slices[].timing.wall_us` is each expiry's own fit wall. The largest one is
  the longest chain.
- Elapsed time can never fall below max(longest chain, serial stages + sum of chains /
  width).

B0's `bench/run_cold_trials.ps1` is single-CPU only (it forces `ATX_VOL_FIT_WORKERS=1`).
The P0 rows at width > 1 use a runner that sets its own process affinity mask before it
launches each fresh trial, so every trial inherits the mask from creation. The script is
kept with the evidence.

## 5. Deployment hardware

Determinism no longer depends on the core type: the Eigen sizes are compiled in. So
deployment pinning is a speed decision only:

- **Hybrid (P + E) hosts.** Run `fit_workers` up to the P-core logical count for
  latency-critical fits. The E cores take extra width at a lower per-core rate, and the
  longest chain bounds the elapsed time anyway.
- **Homogeneous server parts.** Every core is equal, so AUTO width is the plan. The pinned
  Eigen sizes are not tuned for a server L2, so a re-tune would be a numerical decision
  (section 2).
- **Q0** re-measures F3 / F4 / F5 on the declared deployment hardware and the final root.
  The P0 numbers are development rows.

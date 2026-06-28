# Sprint 3 — Eval-VM Hot Path + Bench Baseline

**Goal:** remove the throughput ceiling on breadth imposed by the O(T·W) batch
time-series kernels — specifically the variance family — and prove the removal with
a recorded microbench before/after line from the existing Google Benchmark harness.
Three concrete deliverables: (1) numerically-safe online rolling kernels for
TsVar/TsStd/TsZscore/TsAvDiff using Welford / Neumaier-compensated summation that
match the batch oracle within a tight tolerance band (and are bit-exact in the
AuditExact path, or gated behind ResearchFast if not achievable bit-exact); (2)
cross-instrument parallelism for the remaining batch Ts ops, digest-invariant across
{1,2,4,8} workers; (3) a bench baseline — before/after ns/op or eval-ms/genome
recorded from `atx-engine/bench/alpha_batch_bench.cpp` and `parallel_bench.cpp`,
gated by `ATX_BUILD_BENCH=ON`.

**Owns (exclusive):**
`atx-engine/include/atx/engine/alpha/ts_ops.hpp`,
`atx-engine/include/atx/engine/alpha/vm.hpp`,
`atx-engine/bench/` (additions only — do NOT break existing bench targets),
`atx-engine/tests/alpha/` (extend existing; new `ts_online_variance_test.cpp` and
`ts_parallel_eval_test.cpp`).

**Must NOT touch:** `src/factory/factory.cpp`, `src/factory/search_driver.cpp`,
fitness/search/combine layers, gate/fitness (S1), datafields/augment (S2), CLI hub
(S7). `oracle.hpp` is the immutable differential reference — read it, never edit
it. The four atx-impl hub files are reserved for S7 only (ROADMAP.md §Disjoint
file-ownership).

---

## Implementation-quality handoff block

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear
module-level intent, grouped constants/types/APIs, explicit ownership and lifecycle
rules, named error contracts, and concise comments that explain invariants,
non-obvious control flow, or domain semantics. Do not follow weaker patterns that
expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done
until the public API, implementation, tests, docs/ledger row, and build/test gate
are complete. Do not leave TODO placeholders, fake success paths, unused APIs, or
untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership,
ordering, crash/recovery semantics, and tricky domain rules. Do not comment obvious
assignments or wrap every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## Baseline — where the cycles go

Anchors verified against HEAD (2026-06-28). Every cell below was read and confirmed
before noting it.

| Sink | File : region | Cost | Class |
|---|---|---|---|
| **Variance family batch recompute** — TsVar/TsStd/TsZscore/TsAvDiff deliberately reverted from online to batch (`ts_is_online_op` returns false; see comment block `ts_ops.hpp:303–311`): the previous rolling Σx² kernel suffered catastrophic cancellation on high-mean/low-variance columns (e.g. `ts_std(volume,d)` at 1e7–1e8 magnitude loses ~8 significant digits). Each output cell rescans its full trailing window. | `ts_ops.hpp:303–311` (revert rationale); `ts_is_online_op:328–357` (four ops stay `default:false`); `vm.hpp:833–848` (online/batch dispatch branch) | O(T·W) per instrument column per op; dominant on variance-heavy genomes with large windows | P1 — Welford / Neumaier online kernel, ResearchFast if not bit-exact |
| **No cross-instrument parallelism in Ts kernels** — `eval_time_series` (`vm.hpp:792`) iterates a single-threaded instrument loop (`vm.hpp:840`, `vm.hpp:883`); one `Engine` per search worker (`search_driver.cpp:97–98`), parallelism is at the genome level (`det_pool.parallel_for` at `search_driver.cpp:708`), not the column level within a single genome eval. A Ts-heavy genome on a 500-instrument panel spends all its column time in one thread. | `vm.hpp:840` (online instrument loop); `vm.hpp:883` (batch column-extract loop); `search_driver.cpp:85,97–98,708` | Linear in instruments for every Ts op; no intra-eval concurrency | P2 — thread-pool over instrument chunks, digest-invariant |
| **Stale doc comment at vm.hpp:56–57** — says "Cross-sectional (Cs*) and time-series (Ts*) opcodes are NOT YET implemented: they return Err(NotImplemented)." Both families are fully implemented (P3-7 / P3-8). The stale comment is a correctness liability for future readers. | `vm.hpp:56–57` | Zero perf cost; correctness liability | C1 — one-line cleanup, no behavior change |

**What is already good — do not regress:**
- Online sum/mean sweep (`ts_online_sum_family`, called from `vm.hpp:844`) is O(T)
  and already correct; do not touch it.
- Online extreme sweep (`ts_online_extreme`, called from `vm.hpp:842`) is O(T) and
  bit-exact; do not touch it.
- Column-extract transpose (S1-3, `vm.hpp:863–905`) already converts the batch
  path to cache-friendly contiguous reads; the new variance kernel slots into the
  same extracted `col` span — do not re-extract.
- `SlotPool` zero-alloc dispatch (`vm.hpp:240`), `CsScratch` reuse (`vm.hpp:624–628`),
  `ts_scratch_a_` / `ts_scratch_b_` growth-only resize (`vm.hpp:827–829`), `ts_col_`
  / `ts_col_b_` growth-only column buffers (`vm.hpp:879–881`) are all already optimal.
  None of these are touched by this sprint.
- Search-level parallelism (`det_pool.parallel_for` at `search_driver.cpp:708`) and
  per-worker `Engine` instances (`search_driver.cpp:97–98`) are not modified.

---

## Determinism contract

p7 inherits the **two-tier EvalMode** contract from p6 S1 and the p7 ROADMAP
§Shared determinism contract (B). Summary for this sprint:

- **AuditExact** = today's contract verbatim. Byte-identical output across all
  worker counts. The seq==parallel digest identity (F1) proven for the search loop
  (`search_driver.cpp:85,708`) must extend to any new intra-eval column
  parallelism: each instrument column is an independent unit of work with no
  cross-column shared mutable state, so dispatch order does not affect results as
  long as the column-level kernel is deterministic. Wins that are AuditExact-
  shippable land unconditionally.
- **ResearchFast** = an explicit `EvalMode` opt-in (engine-config field defaulting
  to `AuditExact`) for wins that change bits — e.g. a Welford kernel whose
  accumulation order differs from the batch oracle's chronological two-pass. Wins
  behind `ResearchFast` do NOT change the default path; admitted alphas discovered
  under `ResearchFast` are re-scored on the exact path before publication. A
  `ResearchFast`-gated win ships only when: (a) off-path byte-identity is tested,
  (b) the tolerance band is documented and tested, (c) a bench line proves the win.

`oracle.hpp` is **frozen** for the life of p7. The differential test suite
(`atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*`) is the
byte-identity gate for every unit.

**This sprint's AuditExact / ResearchFast split:**
- S3-0 (ledger + baseline): AuditExact, no code change.
- S3-1 (Welford/Neumaier kernel for TsVar/TsStd): **ResearchFast** — Welford's
  online accumulation order differs from the oracle's two-pass chronological
  recompute; the kernel is provably more accurate but not bit-identical. Ships
  behind `ResearchFast` mode. The AuditExact path retains the current batch kernel.
- S3-2 (extend to TsZscore/TsAvDiff): **ResearchFast** — same reasoning; zscore
  builds on var/std; av_diff builds on online mean. Both remain batch on AuditExact.
- S3-3 (cross-instrument parallelism for batch Ts): **AuditExact** — column-
  independence means parallel dispatch produces bit-identical results to serial;
  seq==parallel digest test is the gate.
- S3-4 (bench delta writeup + vm.hpp stale-comment cleanup): AuditExact, comment
  only.

---

## Wiring map

```
ts_ops.hpp:282–326  — Online-rolling-kernel comment block (Task 7 narrative).
                      S3-1 adds tsv_welford_var_family() and tsv_neumaier_mean()
                      here, below the existing ts_online_sum_family block.
ts_ops.hpp:328–357  — ts_is_online_op() dispatch table. S3-1 adds TsVar/TsStd;
                      S3-2 adds TsZscore/TsAvDiff — but ONLY when EvalMode ==
                      ResearchFast (the switch is in vm.hpp, not here; ts_ops.hpp
                      exports the kernel functions unconditionally).
vm.hpp:56–57        — Stale "NOT YET implemented" comment. S3-4 replaces with
                      accurate one-liner.
vm.hpp:783–848      — eval_time_series() header comment + online/batch dispatch.
                      S3-1 adds the ResearchFast branch: if (mode_ ==
                      EvalMode::ResearchFast && ts_is_online_variance_op(in.op))
                      dispatch to the Welford sweep; else fall through to existing
                      batch path. S3-3 wraps the batch instrument loop in a
                      parallel_for over column chunks.
vm.hpp:240 region   — Engine constructor / members. S3-1 adds EvalMode mode_
                      member (default AuditExact) + setter/accessor. S3-3 adds
                      atx::engine::parallel::DetPool* ts_pool_ (nullable; null =
                      single-threaded, set by caller before evaluate()).
atx-engine/bench/   — S3-0 records baseline; S3-4 records after numbers.
                      New file: ts_variance_bench.cpp targeting the variance-family
                      kernels specifically (before/after the Welford switch).
atx-engine/tests/alpha/
  ts_online_variance_test.cpp  — S3-1/S3-2: differential + pathological fixtures.
  ts_parallel_eval_test.cpp    — S3-3: seq==parallel digest identity.
```

---

## Tasks

### S3-0 — Open ledger; record current bench baseline

**Goal:** freeze the starting numbers so S3-4's "after" has a credible "before."
Open the sprint ledger (`phase-3-progress.md`), create the worktree, land the
marker commit. Run the existing bench targets against the HEAD binary and record
the raw output. No code changes.

**Wiring (file:line):** `atx-engine/bench/alpha_batch_bench.cpp:46–76` (mined
battery, 24 alphas, 512×256 panel). `atx-engine/bench/parallel_bench.cpp:53–55`
(128 alphas, 256×128 panel, workers ∈ {1,2,4,8}). Build flag at
`CMakeLists.txt:198` (`ATX_BUILD_BENCH=ON`).

**Determinism:** no code change; no contract impact.

**Accept:**
- `phase-3-progress.md` created, marker commit landed, `Base: master @ <SHA>` recorded.
- The following bench command runs to completion and its output is pasted verbatim
  into the ledger's "S3-0 baseline" table (host, build context, compiler, date):

```
cmake -B build-bench -DATX_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release && \
cmake --build build-bench --target atx-engine-bench -j && \
build-bench/atx-engine/bench/atx-engine-bench \
  --benchmark_filter="BM_BatchEvaluate_MinedBattery|BM_ParallelEvaluate" \
  --benchmark_format=console
```

- Recorded before line (example shape — fill with actual numbers):
  ```
  BM_BatchEvaluate_MinedBattery  <N> ns   <M> ns   <K> iters
  BM_ParallelEvaluate/workers:1  <N> ns   ...
  BM_ParallelEvaluate/workers:4  <N> ns   ...
  ```
- No test regressions (`atx-engine-tests` and `atx-engine-factory-tests` green).

---

### S3-1 — Welford / Neumaier online variance kernel for TsVar and TsStd

**Goal:** restore O(T) rolling kernels for TsVar and TsStd using Welford's
online algorithm for variance (numerically safe, no Σx² anywhere) plus
Neumaier / Kahan compensated summation for the mean accumulator used in the
variance formula. The kernel must match the batch oracle within a tight tolerance
band (atol = rtol = 1e-9 on random panels; pathological constant/near-constant
windows must produce NaN or near-zero, not spurious inf). Ships behind
`ResearchFast` — the Welford accumulation order differs from the oracle's
chronological two-pass, so bit-exact is not achievable. The AuditExact (default)
path retains the current batch kernel unchanged.

**Algorithm note (Welford):** For a trailing window [t-d+1, t], maintain online
running mean `M` and variance accumulator `S` (= Σ(xᵢ - M̄)² in Welford form)
as elements enter and leave the window. The leaving-element correction for a
sliding window uses Chan's / Pébay's formula:
```
  entering x_new:  delta = x_new - M_old; M += delta/n; S += delta*(x_new - M)
  leaving  x_old:  delta = x_old - M_new; M -= delta/(n-1); S -= delta*(x_old - M_old)
```
This avoids all subtraction of large nearly-equal numbers. Name the kernel
`tsv_welford_var_col` (instrument column sweep, single pass, O(T), no Σx²).
The mean accumulator uses Neumaier summation (carry a compensation term `c`
alongside the running sum) to bound the mean error even on high-magnitude columns
like volume.

**Wiring (file:line):**
- `ts_ops.hpp:282–326`: add `tsv_neumaier_mean_col()` and `tsv_welford_var_col()`
  inline functions immediately after the existing online-kernel comment block,
  following the style of `ts_online_sum_family` (`ts_ops.hpp:359–420`).
- `vm.hpp` (Engine class): add `EvalMode mode_` member (default `EvalMode::AuditExact`);
  add public `set_eval_mode(EvalMode)` setter and `eval_mode()` accessor (mirrors the
  p6 S1-0 plumbing pattern).
- `vm.hpp:833` (online/batch dispatch): add `else if (mode_ == EvalMode::ResearchFast
  && ts_is_online_variance_op(in.op))` branch dispatching to `tsv_welford_var_col`
  for TsVar/TsStd — interleaved after the existing `ts_is_online_op` branch and
  before the batch fallthrough.
- New helper `ts_is_online_variance_op(OpCode)` in `ts_ops.hpp` (returns true for
  TsVar/TsStd only; used by the vm.hpp dispatch so it stays in ts_ops.hpp alongside
  `ts_is_online_op`).

**Determinism:** ResearchFast. Default (AuditExact) path: zero bytes change — the
`if (ts_is_online_op(in.op))` branch fires first and is unchanged; the Welford
branch only fires under `ResearchFast`. Off-path byte-identity is explicitly tested.

**Accept:**
- New test file `atx-engine/tests/alpha/ts_online_variance_test.cpp`:
  - **Pathological near-constant window** (mean ≈ 1e7, std ≈ 1.0): batch oracle
    returns a value within 1e-9 of truth; Welford kernel returns a value within
    1e-9 of truth; both agree within atol=1e-9. This is the catastrophic-
    cancellation regression the revert was triggered by — the test must cover it.
  - **Pathological constant window** (all x[i] == C): batch oracle produces
    var=0.0; Welford kernel produces var=0.0 (not a spurious negative).
  - **Randomised panel** (500 dates × 200 instruments, d=20, seed fixed):
    Welford kernel vs batch oracle within atol=rtol=1e-9 on every finite cell.
  - **Off-path byte-identity**: Engine with `EvalMode::AuditExact` evaluates a
    TsVar/TsStd program before and after S3-1's code lands → byte-identical
    `SignalSet` output.
  - **Oracle differential stays green**: `atx-engine-factory-tests
    --gtest_filter=*Oracle*:*Golden*:*Digest*` green.
- New bench target `ts_variance_bench.cpp` (added under `atx-engine/bench/`):
  ```
  BM_TsVarBatch   — Engine(AuditExact).evaluate, ts_std(close, 20), 512×256 panel
  BM_TsVarWelford — Engine(ResearchFast).evaluate, ts_std(close, 20), 512×256 panel
  ```
  Recorded after line (shape):
  ```
  BM_TsVarBatch    <N_before> ns   ...
  BM_TsVarWelford  <N_after>  ns   ...   [ratio: ~W× speedup, target ≥ 2× for d=20]
  ```
  Command:
  ```
  build-bench/atx-engine/bench/atx-engine-bench \
    --benchmark_filter="BM_TsVar" --benchmark_format=console
  ```

---

### S3-2 — Extend online variance family to TsZscore and TsAvDiff

**Goal:** extend the ResearchFast online path from S3-1 to cover TsZscore and
TsAvDiff, which both build on a rolling mean and variance. TsZscore = (x[t] -
online_mean) / online_std; TsAvDiff = x[t] - online_mean. Both avoid the
near-cancellation that the revert comment (`ts_ops.hpp:349–352`) documents for
the previous naive Σx²-based implementation.

**Algorithm note:** TsZscore re-uses `tsv_welford_var_col`'s running (M, S, n)
accumulator to compute std = sqrt(S/(n-1)) at each output cell, then zscore =
(x[t] - M) / std. TsAvDiff = x[t] - M. The risk: for TsAvDiff, the comment at
`ts_ops.hpp:349–352` notes that "its OUTPUT is itself a near-cancellation (x[t]
≈ mean on a high-mean/low-variance window) … a tiny rolling-mean drift blows up
to a large RELATIVE error." Welford + Neumaier mean reduces the mean drift by
~10⁴×, but relative error in x[t]-mean can still be large when the signal is
genuinely small. The tolerance for TsAvDiff is therefore atol-only (1e-7 absolute)
rather than relative, and the test must include the adversarial case. If the
adversarial tolerance cannot be met, TsAvDiff stays batch-only and this task
documents that explicitly rather than shipping a broken kernel.

**Wiring (file:line):**
- `ts_ops.hpp:328–357` (`ts_is_online_variance_op`): add TsZscore and TsAvDiff to
  the ResearchFast-online dispatch (the helper added in S3-1).
- `ts_ops.hpp` (after `tsv_welford_var_col`): add `tsv_welford_zscore_col()` and
  `tsv_welford_avdiff_col()` following the same style.
- `vm.hpp:833` dispatch: the `ts_is_online_variance_op` branch already covers
  TsZscore/TsAvDiff once they are added to the helper — no vm.hpp structural change
  beyond the helper expansion.

**Determinism:** ResearchFast, same contract as S3-1. AuditExact path untouched.

**Accept:**
- Extend `ts_online_variance_test.cpp` (S3-1's file) with:
  - **TsZscore pathological**: mean≈1e7, std≈1.0; Welford zscore vs batch oracle
    within atol=1e-9.
  - **TsAvDiff adversarial**: mean≈1e6, av_diff amplitude ≈ 1.0; Welford av_diff
    vs batch oracle within atol=1e-7 (absolute). If this cannot be achieved,
    document the measured error and mark TsAvDiff as "deferred — stays batch" in
    the ledger row, removing TsAvDiff from the online dispatch.
  - **Randomised panel** (same fixture as S3-1): TsZscore and TsAvDiff Welford vs
    oracle within their respective tolerances on every finite cell.
  - **Off-path byte-identity**: AuditExact path byte-identical after S3-2.
- Bench: extend `ts_variance_bench.cpp` with `BM_TsZscoreBatch` / `BM_TsZscoreWelford`
  and `BM_TsAvDiffBatch` / `BM_TsAvDiffWelford`. Record before/after pairs.
- Oracle differential and golden/digest tests stay green.

---

### S3-3 — Cross-instrument parallelism for batch Ts ops (seq == parallel digest)

**Goal:** wrap the batch instrument loop in `eval_time_series` (`vm.hpp:883`) in a
thread-pool dispatch over instrument-column chunks, so a Ts-heavy genome on a wide
panel uses multiple cores within the single Engine::evaluate call. The digest must
be byte-identical between serial (1 worker) and parallel (N workers) — column
independence guarantees this, and the test proves it.

**Design constraints:**
- The column-extract transpose (S1-3, `vm.hpp:863–905`) already extracts one
  column at a time into `ts_col_` / `ts_col_b_`. With per-thread column buffers
  this becomes trivially parallel; each thread gets its own scratch (`ts_col_`,
  `ts_col_b_`, `ts_scratch_a_`, `ts_scratch_b_`) to avoid data races.
- The pool is nullable (`DetPool* ts_pool_`). When null (the default), `eval_time_series`
  runs its existing single-threaded loop — AuditExact, zero change to any existing
  caller. A caller that wants intra-eval parallelism calls
  `engine.set_ts_pool(&pool)` before `evaluate()`.
- The `DetPool` used here must be SEPARATE from the search-level `det_pool` in
  `search_driver.cpp:85` — nesting two DetPool dispatches into each other is a
  deadlock risk. This sprint adds a new optional pool; wiring it into the search
  driver belongs to S7 (CLI hub), not here.
- Minimum chunk size: 1 instrument column per thread (no partial-column splits).
  Chunk the `j in [0, instruments)` range across `ts_pool_->n_workers()` bands;
  each band owns disjoint j-indices and writes to disjoint output slots.

**Wiring (file:line):**
- `vm.hpp` (Engine class members): add `parallel::DetPool* ts_pool_{nullptr}` and
  `std::vector<std::vector<f64>> ts_col_thr_`, `ts_col_b_thr_`, `ts_scratch_a_thr_`,
  `ts_scratch_b_thr_` (one scratch vector per worker, grown on demand).
- `vm.hpp` (new public API): `set_ts_pool(parallel::DetPool*)` setter.
- `vm.hpp:883` batch loop: replace
  ```cpp
  for (atx::usize j = 0; j < instruments; ++j) { ... extract + kernel ... }
  ```
  with a `ts_pool_`-aware dispatch:
  ```cpp
  if (ts_pool_ && instruments > 1) {
      ts_pool_->parallel_for(instruments, [&](usize j, usize wid) {
          // use ts_col_thr_[wid] / ts_scratch_a_thr_[wid] etc.
          ...
      });
  } else {
      // original single-threaded loop (unchanged bytes)
  }
  ```
- Online instrument loops (`vm.hpp:840–846`) stay single-threaded in this sprint
  (deque state is per-instrument-column already, but the deque scratch vectors are
  currently shared `ts_dq_lo_` / `ts_dq_hi_` — parallelising those safely requires
  per-thread deques, deferred to the future-work backlog).

**Determinism:** AuditExact. Column independence is the proof: each output cell
`out[t*I+j]` depends only on input cells in column j; no two threads write the
same output index; read indices are column-disjoint (except reading the same `x`
span read-only, which is safe). Seq==parallel digest is the acceptance gate, not a
proof-by-argument.

**Accept:**
- New test file `atx-engine/tests/alpha/ts_parallel_eval_test.cpp`:
  - **Seq==parallel digest identity**: evaluate a Ts-heavy program (TsStd + TsRank
    + TsCorr, d=20) on a 600×501 panel with `ts_pool_` null (serial) vs a
    2-worker DetPool vs a 4-worker DetPool. All three produce byte-identical
    `SignalSet` output (every f64 cell, NaN==NaN).
  - **Thread-local scratch no-race**: Tsan clean under two runs of the seq==parallel
    test (run with `-DCMAKE_BUILD_TYPE=Debug` and ThreadSanitizer enabled if the
    build supports it; otherwise document that the test is Tsan-pending).
  - **Null pool path unchanged**: Engine with `ts_pool_ == nullptr` produces
    byte-identical output before and after S3-3.
- Bench: `parallel_bench.cpp` already measures genome-level parallelism; add
  `ts_column_parallel_bench.cpp` targeting `eval_time_series` intra-eval column
  parallelism at {1,2,4} workers on a 256×512 panel (instruments >> dates, the
  regime where column parallelism wins). Recorded after line:
  ```
  BM_TsColumnEval/workers:1   <N> ns
  BM_TsColumnEval/workers:2   <N> ns   [target: ≥1.5× over workers:1 at 512 instruments]
  BM_TsColumnEval/workers:4   <N> ns
  ```
  Command:
  ```
  build-bench/atx-engine/bench/atx-engine-bench \
    --benchmark_filter="BM_TsColumnEval" --benchmark_format=console \
    --benchmark_enable_random_interleaving=true
  ```
- Oracle differential and golden/digest tests stay green.

---

### S3-4 — Bench delta writeup + vm.hpp stale-comment cleanup

**Goal:** close the sprint with a tidy ledger and a clean codebase. Two
sub-tasks: (a) run the full bench suite after S3-1/S3-2/S3-3 land and record
the after numbers alongside the S3-0 before numbers in the ledger — the delta
is the perf evidence for the sprint; (b) fix the stale comment at `vm.hpp:56–57`
that falsely says Cs*/Ts* ops are "NOT YET implemented."

**Wiring (file:line):**
- `vm.hpp:56–57`: replace "Cross-sectional (Cs*) and time-series (Ts*) opcodes
  are NOT YET implemented: they return Err(NotImplemented) here and land in P3-7
  / P3-8 respectively." with an accurate summary, e.g.:
  "Cross-sectional (Cs*) kernels are in cs_ops.hpp (P3-7); time-series (Ts*)
  kernels are in ts_ops.hpp (P3-8). Both families are fully implemented; the
  oracle differential (P3-9) enforces bit-exact agreement."
- `phase-3-progress.md`: add "S3-4 measured bench delta" table with before (S3-0)
  and after columns for each bench target.

**Determinism:** AuditExact (comment-only change in vm.hpp). No output bytes change.

**Accept:**
- `vm.hpp:56–57` comment is accurate; `git diff` shows only the comment lines
  changed in this unit; all tests stay green.
- Ledger table with before/after recorded bench lines (command, host, build
  context, ns/op) for:
  - `BM_BatchEvaluate_MinedBattery` (overall batch-eval throughput)
  - `BM_TsVarBatch` vs `BM_TsVarWelford` (variance kernel speedup)
  - `BM_TsColumnEval/workers:1..4` (intra-eval column parallelism curve)
- Sprint close ceremony per `docs/sprint.md`: residuals lifted into ROADMAP
  future-work backlog, ROADMAP status table updated, `Last reviewed` bumped,
  `phaseN.md` user reference stub written.
- Full bench command (after build):
  ```
  build-bench/atx-engine/bench/atx-engine-bench \
    --benchmark_filter="BM_BatchEvaluate_MinedBattery|BM_TsVar|BM_TsZscore|\
  BM_TsAvDiff|BM_TsColumnEval|BM_ParallelEvaluate" \
    --benchmark_format=console \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true
  ```

---

## New test files

All units contribute to two new test files (auto-globbed by CMake):

- **`atx-engine/tests/alpha/ts_online_variance_test.cpp`** (S3-1, S3-2):
  - Pathological near-constant window (catastrophic-cancellation regression).
  - Constant-window var=0.0 (no spurious negative).
  - Randomised panel tolerance sweep (atol=rtol=1e-9 for var/std/zscore;
    atol=1e-7 for av_diff).
  - Off-path byte-identity (AuditExact eval unchanged after S3-1/S3-2).
  - Oracle differential (`*Oracle*:*Golden*:*Digest*`) stays green.

- **`atx-engine/tests/alpha/ts_parallel_eval_test.cpp`** (S3-3):
  - Seq==parallel digest identity at {null, 2-worker, 4-worker} pool.
  - Null-pool path byte-identity.
  - (Tsan pending if build supports; documented in ledger if deferred.)

Both files use the existing `ATS_TEST(...)` framework. CMake picks them up via the
existing glob in `atx-engine/tests/CMakeLists.txt`.

---

## Sequencing

1. **S3-0** (baseline) — independent; do first. Establishes the "before" numbers
   that make S3-4's delta meaningful. One-commit unit.
2. **S3-1** (Welford TsVar/TsStd) — builds on S3-0's clean HEAD. Highest
   correctness risk (sliding-window Welford leaving-element formula must be
   verified against the pathological fixture before any performance claim).
3. **S3-2** (TsZscore/TsAvDiff) — sequential after S3-1 (reuses `tsv_welford_var_col`
   and the tolerance test fixtures). TsAvDiff has an explicit bail-out path
   (stays batch) if the adversarial tolerance cannot be met.
4. **S3-3** (column parallelism) — independent of S3-1/S3-2 (touches vm.hpp's
   batch loop structure, not the online kernel path); can begin in parallel with
   S3-2 if sub-agents are available, but sequential is safer (S3-2 touches
   `vm.hpp:833` dispatch; S3-3 touches `vm.hpp:883` batch loop — adjacent but
   non-overlapping).
5. **S3-4** (bench delta + cleanup) — must follow S3-1/S3-2/S3-3 (needs the
   after-code in place to bench it).

Expected compounding: S3-1 reduces the cost of every TsVar/TsStd evaluation from
O(T·W) to O(T) under ResearchFast (for d=20, that is a ≥20× FLOP reduction per
column; the actual ns/op win is smaller due to memory bandwidth and ILP, target
≥2×). S3-2 extends that win to TsZscore/TsAvDiff. S3-3 gives a wall-clock
reduction proportional to the number of columns dispatched in parallel — meaningful
on wide panels (≥200 instruments) even at 2 workers. Together they widen the
practical search space: a genome that uses `ts_std(volume, 20)` on 500 instruments
no longer dominates the per-genome eval budget.

---

## Risks / guardrails

- **Sliding-window Welford leaving-element error.** The Chan/Pébay leaving-element
  formula for a sliding window introduces a small bias not present in a pure
  forward-only Welford. The pathological near-constant test (S3-1 Accept) is the
  primary guard. If the leaving-element formula produces errors above 1e-9 on the
  pathological fixture, the kernel does not ship — stay batch for that op. Numerical
  analysis reference: Pébay (2008) "Formulas for Robust, One-Pass Parallel
  Computation of Covariances and Arbitrary-Order Statistical Moments."
- **TsAvDiff relative error is fundamentally large.** The existing comment at
  `ts_ops.hpp:349–352` is correct: when x[t] ≈ mean, even a tiny absolute error in
  the mean produces a large RELATIVE error in x[t]-mean. The accept criterion is
  atol-only (1e-7 absolute). If this cannot be achieved, TsAvDiff stays batch —
  the plan has an explicit bail-out and the ledger records the decision.
- **Thread-local scratch sizing.** Each worker thread in S3-3 needs its own
  `ts_col_` (dates-sized) and `ts_scratch_a_/b_` (window-d-sized). The Engine's
  per-worker vectors must be grown lazily (same pattern as the existing growth-only
  resize at `vm.hpp:827–829`). A growth race between threads is prevented by
  sizing all per-worker scratch to `[n_workers]` up front in `set_ts_pool()`.
- **Deadlock from nested DetPool dispatch.** The S3-3 column pool must never be the
  same pool instance as the search-level `det_pool` (`search_driver.cpp:85`). The
  API (`set_ts_pool(DetPool*)`) documents this constraint in the header comment.
  Wiring into the search driver (S7) must respect this by creating a separate pool.
- **Bench noise in Debug builds.** The existing benches run in Debug/clang-cl (as
  noted in `alpha_batch_bench.cpp:17` and `parallel_bench.cpp:14`). The before/after
  numbers are therefore absolute-latency upper bounds. The before/after RATIO is the
  meaningful signal (noise cancels). If the ratio is < 1.1× for S3-1/S3-3 (i.e.
  faster path not detectable above noise), add a Release build qualifier:
  `-DCMAKE_BUILD_TYPE=Release`; document which build was used per recorded line.
- **No hour-long prod run.** Per p7 ROADMAP §Validation discipline: the gate is
  (a) unit tests on tiny deterministic fixtures, (b) dev-panel smoke ≤5 min, (c)
  microbench delta. A full-panel run is V1 (operator, out-of-loop). Do not run one
  inside this sprint.

---

## Bench / acceptance summary

Every perf claim in this sprint requires a recorded bench line. No speedup claim
without bench evidence. Per `docs/implementation-quality.md`: benchmarks record
commands, timing scope, host/build context, and validation method.

| Unit | Bench target(s) | Accept metric |
|---|---|---|
| S3-0 | `BM_BatchEvaluate_MinedBattery`, `BM_ParallelEvaluate/workers:*` | Baseline recorded; no regression |
| S3-1 | `BM_TsVarBatch`, `BM_TsVarWelford` | Welford ≥2× faster than batch at d=20, 512×256; tolerance test green |
| S3-2 | `BM_TsZscoreBatch`, `BM_TsZscoreWelford`, `BM_TsAvDiffBatch`, `BM_TsAvDiffWelford` | Same ratio target; TsAvDiff ships only if atol≤1e-7 adversarial met |
| S3-3 | `BM_TsColumnEval/workers:{1,2,4}` | workers:2 ≥1.5× over workers:1 at 512 instruments; seq==parallel digest byte-identical |
| S3-4 | All of the above (after code) | Before/after delta table in ledger; vm.hpp comment accurate |

End-to-end gate (every unit):
```
atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*
```
Must stay green before and after each unit commits.

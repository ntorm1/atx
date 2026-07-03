# Sprint 1 — Eval/VM Hot-Path Performance

**Goal:** cut dominant per-genome cost (compile + VM eval + position sizing) paid
`generations×population` times, without changing a single output byte. The biggest
single lever on the 21-minute wall-clock.

**Mandate / Owns (exclusive):**
`atx-engine/include/atx/engine/alpha/{vm,streams,bytecode,dag,cs_ops,ts_ops}.hpp`,
`atx-engine/include/atx/engine/loop/weight_policy.hpp`,
`atx-engine/tests/alpha/` (extend existing) + new `alpha_eval_perf_test.cpp`.

**Must NOT touch:** `factory.cpp`, fitness/search, config, `stage_*`, `oracle.hpp`.
The four hub files (`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}`)
are **reserved for S7 only** — see [ROADMAP.md](ROADMAP.md) §Disjoint file-ownership.

---

## Baseline / where the cycles go

Anchors verified against HEAD (2026-06-27). Every cell in the "already good" list
was read and confirmed before noting it.

| Sink | File : region | Cost | Class |
|---|---|---|---|
| **`compile()` per-genome, no memoization** — `build_dag` + `linearize` called freshly every evaluation; genomes with shared structure pay the full compile cost each time | `bytecode.hpp:219` (`compile`), `dag.hpp:347` (`build_dag`), `bytecode.hpp:213` (`linearize`) | compile latency × gen × pop; scales with AST size | P1 — eliminate with compile cache |
| **`to_target_weights` allocates 3 vectors per date** — `weights(n)`, `live_idx`, `dense` allocated on every call; `fill_alpha_stream` (`streams.hpp:230–232`) loops over every date calling this, so cost scales as allocs × dates × alphas | `weight_policy.hpp:215` (`to_target_weights`); comment at `weight_policy.hpp:128` flags the deferred-residual scratch overload; caller loop at `streams.hpp:230` | ~3 heap allocs × dates per eval | P2 — scratch-buffer overload + hoist |
| **TS batch-path recomputes over full window per cell** — `ts_value_at` (`ts_ops.hpp:494`) walks the trailing window for every (t, j) cell for Var/Std/Rank/Med/AvDiff/Corr and all non-online ops; the online sweep (`ts_online_sum_family`, `ts_ops.hpp:372`) covers only Sum/Mean/Min/Max/Scale | `vm.hpp:765–776` (batch dispatch loop — instrument-outer / date-inner); `ts_ops.hpp:494` (`ts_value_at`); `ts_ops.hpp:549–554` (TsRank/TsMed sort) | O(T·W·N) per alpha on window-heavy genomes; biggest per-genome compute sink | P4 / P7 — transpose + extend online sweep |
| **`eval_cross_section` valid-set built in appearance order** — `cs_one_date` (`vm.hpp:645`) rebuilds the valid index list each date via a forward scan (`vm.hpp:649–653`); the result is ascending-instrument-index order today, but that invariant is implicit, not enforced | `vm.hpp:630`, `vm.hpp:645–653` (`cs_one_date` — valid set loop) | Correctness / determinism risk, not a throughput sink | C1 — canonicalize + test |

**What is already good — do not regress:**
- VM dispatch loop is zero-alloc per dispatch instr (`vm.hpp:240`); `SlotPool` reused
  across calls and grown only on need (`vm.hpp:307–312`, `ensure_pool`).
- `CsScratch` is an Engine member reused across all dates and `evaluate()` calls,
  capacity growing monotonically (`vm.hpp:624–628`, `cs_ops.hpp:132–150`).
- Online sweep already covers Sum/Mean/Min/Max/Scale (`ts_ops.hpp:372`/`421`) — bit-exact
  and zero per-cell allocation. Do not replace with a batch path.
- `resolve_fields` allocates only on growth, not per-call (`vm.hpp:296–303`).
- Cross-alpha batch compile (`compile_batch`, `bytecode.hpp:225`) already gives
  cross-alpha CSE for the batch-eval path; the sprint adds per-genome memoization on top.

---

## Determinism contract

p6 inherits the **two-tier EvalMode** contract from
[p5 Sprint 1 §Determinism contract](../p5/sprint-1-performance.md#determinism-contract-the-structural-change)
— see [ROADMAP.md](ROADMAP.md) §Shared determinism contract (B) for the authoritative
statement. Summary for this sprint:

- **`AuditExact`** = today's contract verbatim: chronological-order Ts reductions,
  ascending-index Cs valid set, single-thread-equivalent digest, bit-identical across
  worker counts. Unchanged — the system of record.
- **`ResearchFast`** = future unlock for online variance, SIMD horizontal reductions,
  parallel reduction, fast-math. Not reached in this sprint — all four tasks below are
  byte-identical improvements that ship in `AuditExact` with no mode flag.

**This sprint's tasks are entirely AuditExact-shippable:** compile memoization (same
`Program` bytes out, just cached), scratch-buffer reuse (`to_target_weights` with
identical output), reusable Engine API (no eval change), and Ts-kernel transpose (same
reduction order enforced, or kernel skipped). The cross-section canonicalization task
(S1-4) also stays byte-identical. No `ResearchFast` wins land here — those belong to p5
Sprint 1's S1-2/S1-3 tasks if the EvalMode plumbing is ever wired in.

---

## Tasks

### S1-0 — Compile memoization keyed by canonical structure

**Root cause:** `compile()` (`bytecode.hpp:219`, = `build_dag` + `linearize`) is the
named primary bottleneck — called per-genome with no caching. The DAG's hash-cons table
already assigns each node a unique `NodeKey` (`dag.hpp:60`) based on opcode + children
(structural hash); the resulting `Program` for a given AST structure is fully
deterministic and reusable across evaluations within a run.

**Fix:** add a process-local compile cache inside the compile layer keyed by a
structural hash of the `Ast` (or equivalently, the `Dag`'s root node ids after
hash-consing). A cache hit returns the stored `Program` directly; `compile()` output
bytes are unchanged. The cache is transparent: no call-site change. Collisions
impossible with a full-equality check on hit (store the root NodeId vector or a
SHA-derived key alongside the `Program`).

**Determinism:** `AuditExact`-shippable. The cache emits the identical `Program` bytes
as a cold compile of the same AST — the linearizer is deterministic. Add a test:
`compile(ast)` twice → byte-identical `Program`; cached vs uncached compile of the same
AST are byte-identical.

**Accept:** golden and digest tests stay green; compile-cache test passes; a
microbench (N compiles of a fixed AST corpus, before/after) shows a measurable wall-time
reduction. Report before/after numbers in the commit body.

---

### S1-1 — Scratch-buffer `to_target_weights` + hoist buffers out of the date loop

**Root cause:** `WeightPolicy::to_target_weights` (`weight_policy.hpp:215`) allocates
`weights(n, 0.0)`, `live_idx`, and `dense` on every call (`weight_policy.hpp:224–235`).
`fill_alpha_stream` (`streams.hpp:222`) calls it once per date in the inner loop
(`streams.hpp:230`): `~3 heap allocs × dates × n_alphas` over a run. The header
explicitly flags a caller-provided-scratch overload as a tracked deferred residual
(`weight_policy.hpp:128`).

**Fix:** add a caller-provided-scratch overload to `WeightPolicy::to_target_weights`
taking pre-sized `weights`, `live_idx`, `dense` buffers by reference. Hoist the buffer
allocations above the date loop in `fill_alpha_stream` (`streams.hpp:230`) and pass them
on each call. Output bytes unchanged — same arithmetic, same order, same NaN handling.

**Determinism:** `AuditExact`-shippable. No reduction-order change.

**Accept:** scratch overload passes the same outputs as the allocating overload on a
multi-date panel; golden tests stay green; alloc count per eval (tracked via a
before/after microbench or `ASAN` alloc counter) drops to O(1) per alpha.

---

### S1-2 — Reusable Engine API across same-panel evaluations

**Root cause:** `Engine` construction borrows the `Panel` for its lifetime and calls
`resolve_fields` + `ensure_pool` on the first `evaluate()` call (`vm.hpp:228–229`). The
`SlotPool` is already reused across `evaluate()` calls on the same `Engine` instance
(`vm.hpp:307–312`). However, callers in `streams.hpp`/factory paths may construct a new
`Engine` per genome rather than reusing one across the panel. A `reset_program()` or
`reuse()` entry that clears per-program state (field remap, pool size) while retaining
the allocated buffers would let the factory reuse one warm `Engine` per worker.

**Fix:** expose a `reset()` / reuse-path on `Engine` that:
1. does NOT reallocate the `SlotPool` if `num_slots` and `cells` are unchanged, and
2. clears per-program scratch (field remap, any per-eval counters) back to a clean-start
   state.

Do NOT edit `factory.cpp` (S2 owns it). Provide the API in the eval layer only; S2
will wire it.

**Determinism:** `AuditExact`-shippable. No eval logic changes.

**Accept:** a test showing an `Engine` re-used across two different `Program`
evaluations produces byte-identical results vs two freshly-constructed `Engine`
evaluations on the same programs and panel. No `SlotPool` reallocation on the second
call when shape is unchanged.

---

### S1-3 — Transpose time-series VM kernels for cache / SIMD

**Root cause:** `eval_time_series` (`vm.hpp:719`) iterates the batch path with
instrument-outer / date-inner (`vm.hpp:765–766`):
```
for j in 0..instruments:
  for t in 0..dates:
    out[t*instruments+j] = ts_value_at(..., t, j, ...)
```
The panel buffer is date-major (row = date, column = instrument), so each window gather
in `ts_value_at` (`ts_ops.hpp:494`) strides by `instruments` — cache-hostile and
defeats auto-vectorization for the batch ops (TsVar, TsStd, TsRank, TsMed, TsCorr,
TsCov, TsRegression, OU ops).

**Fix:** restructure the batch-path dispatch inside `eval_time_series`
(`vm.hpp:765–776`) to an access pattern that reads contiguous or gathered memory per
window: for each instrument column, extract a compact per-column buffer once (or
use a contiguous column-major scratch layout), then apply the kernel. The online sweep
(`ts_online_sum_family` at `ts_ops.hpp:372`, `ts_online_extreme` at `ts_ops.hpp:421`)
already has a per-instrument outer loop and is NOT affected — do not modify it.

**MUST preserve exact float results**: keep summation order and reduction order
identical to today's oracle for each kernel — windowed sums and sorts are not
order-sensitive (argextreme tie-break: same), but variance (`tsv_var`) accumulates
`Σ(x-mean)²` in a specific order. If exact-order preservation is impossible for a
kernel, **skip that kernel** and document it; do not risk digest identity for a perf win.

**Determinism:** `AuditExact`-shippable (bit-exact reduction order preserved, or kernel
skipped).

**Accept:** each transposed kernel tested against the original for bit-for-bit equality
on a randomized panel (in `alpha_eval_perf_test.cpp`); golden tests stay green; a
microbench (cells/s before/after on a Ts-heavy alpha corpus) shows measurable improvement.
Report before/after in the commit body.

---

### S1-4 — Canonicalize the cross-section valid set by instrument index

**Root cause:** `cs_one_date` (`vm.hpp:645`) rebuilds the valid (non-NaN) index set via
a forward scan over the row (`vm.hpp:648–653`), producing an ascending-instrument-index
list. Today this is implicitly safe only because a single genome evaluates on a single
worker with a fixed panel traversal order. The comment at `cs_ops.hpp:119–120` notes
the valid list is "in ascending instrument-index order (the `valid` list is
index-ascending)" — but this is a property of the scan, not an enforced invariant.
If worker dispatch or date-splitting ever reorders the scan, rank/zscore tie-break
depends on the valid-set order.

**Fix:** make the invariant explicit and tested. The valid-set scan at `vm.hpp:648–653`
already produces ascending-index order; add a comment stating the invariant is required
(not accidental), and add a test proving rank/zscore output is identical before/after a
shuffled-then-restored eval order (i.e. same valid set → same output regardless of
when within the date loop the row is evaluated). This is a correctness and
determinism-hardening task, not a throughput task.

**Determinism:** `AuditExact`-shippable. Current output is unchanged — the invariant
already holds; we are pinning and testing it.

**Accept:** a test asserting that rank/zscore row output is invariant to a shuffled
date-evaluation order on a synthetic panel; golden tests stay green; no performance
regression.

---

## New test file: `alpha_eval_perf_test.cpp`

All four tasks add cases to `atx-engine/tests/alpha/alpha_eval_perf_test.cpp`
(or extend existing alpha test files if they already cover the surface):

- **Digest-unchanged:** a fixed AST set evaluated on a synthetic panel → byte-identical
  `AlphaStreams` PnL/positions before vs after each task.
- **Compile-cache:** same AST compiled twice → byte-identical `Program`; cache hit
  returns identical bytes.
- **Scratch-reuse:** `to_target_weights` scratch overload produces identical weights to
  the allocating overload across a multi-date panel.
- **Ts-transpose:** each transposed kernel matches the original kernel bit-for-bit on a
  randomized panel.
- **Cs valid-set:** rank/zscore output identical before and after a shuffled date order.
- **Factory goldens:** the golden/digest/oracle filter
  (`atx-engine-factory-tests --gtest_filter=*Golden*:*Digest*:*Oracle*`) stays green.

---

## Sequencing & expected compounding

1. **S1-0** (compile cache) and **S1-4** (cs valid-set canonicalization) are independent
   of each other and of S1-1/S1-2/S1-3 — do these first. S1-0 is the highest-leverage
   single change for deep-search runs where genomes repeat structure across generations.
2. **S1-1** (scratch buffer) next — zero risk, large alloc-pressure reduction; unblocks
   accurate per-eval alloc measurement.
3. **S1-2** (reusable Engine API) — needed by S2 (factory.cpp reuse); provides the API
   without touching factory.
4. **S1-3** (ts-transpose) last — highest mechanical risk (must preserve bit-exact
   reduction); do after the other three are green and the test suite is locked.

Expected compounding: S1-0 eliminates the compile overhead for repeated-structure
genomes (dominant in late-generation GA); S1-1 drops heap churn by ~3× dates per eval;
S1-3 improves cache behavior on batch-path Ts ops (the remaining heavy kernel class
after the online sweep covers Sum/Mean/Min/Max). Exact magnitudes require bench lines
(see Bench / acceptance below).

---

## Risks / guardrails

- **Digest drift from transpose reordering.** Mitigated by the bit-exact requirement
  per kernel and the mandatory digest test. If a kernel can't preserve order, skip it —
  no speedup claim without a bench line AND green digest.
- **"Optimizing" the already-fast paths.** `SlotPool`, `CsScratch`, `resolve_fields`,
  and the online sweep are already zero-alloc and cache-efficient. Do not touch without
  a profiler showing they are hot. Profile, don't guess.
- **Compile cache keying error.** Two structurally different ASTs must never map to the
  same cache key. Full-equality check on hit (not hash-only lookup) is mandatory. A key
  collision produces silently wrong results — treat as a correctness bug, not a
  performance bug.
- **S1-2 interfering with S2.** S1-2 provides the Engine API; S2 wires it in
  `factory.cpp`. If S2 launches before S1-2 lands, S2 implements the reuse locally and
  notes the dependency per the ROADMAP parallelism contract.

---

## Bench / acceptance

Extend or create `atx-engine/tests/alpha/alpha_eval_perf_test.cpp` with:

- **Per-task microbench:** wall-clock of N compiles (S1-0), M `to_target_weights` calls
  (S1-1), K `eval_time_series` cells/s on a Ts-heavy corpus (S1-3). Before/after numbers
  in the commit body.
- **End-to-end smoke:** `atx-engine-factory-tests --gtest_filter=*Golden*:*Digest*:*Oracle*`
  green before and after each task.
- **Byte-identity assertion:** for every task, the same fixed AST corpus + synthetic
  panel produces byte-identical `SignalSet` outputs after the change.

No speedup claim without a bench line. Per
[implementation-quality.md](../docs/implementation-quality.md): benchmarks record
commands, timing scope, and host/build context.

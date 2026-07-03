# Sprint 1 — Alpha-Generation Performance

**Goal:** push **alphas-evaluated-per-second by 10–100×** so the search can sustain WorldQuant-class
breadth (`IR ≈ IC·√BR` — breadth is the product). Structural changes, looser byte-determinism,
better cache behavior, and vectorization are all in scope.

**Mandate (from the user):** allowed to make structural changes, loosen byte determinism, improve
cache friendliness, vectorize. The one thing we may **not** trade away is the trustworthiness of
the *published* number — see §Determinism contract.

---

## Baseline (truth, measured)

- Canonical run: gen-15 × pop-60 deep search ≈ **1600 s**, 790 distinct evals. Panel = 1.5 GB
  (2,627 dates × 23,348 instruments × 12 fields).
- Throughput today ≈ **~0.5 alphas/s** on a deep run. WorldQuant-class breadth needs **10²–10⁴×**.
- The throughput equation, two independent levers:
  ```
  alphas/sec  ≈  (effective_workers)  /  (eval_ms_per_genome + orchestration_overhead_per_genome)
  ```
  Lever **A** = drive `eval_ms_per_genome` down (VM kernels). Lever **B** = drive
  `orchestration_overhead` down and `effective_workers` up (search loop, fitness, panel marshalling).
- **Decisive finding:** the byte-determinism tax in the *orchestration* is < 1% — loosening it there
  buys almost nothing. The determinism cost lives in the *eval kernels*: online variance, SIMD
  horizontal sums, parallel reduction, and fast-math are exactly the wins that break byte-identity.
  So "loosen determinism" pays off in Lever A, and we ring-fence it (§Determinism contract).

### Where the cycles go (audited, file:line)

| Sink | Location | Cost | Class |
|---|---|---|---|
| **Ts ops recompute O(window)/cell** (Var/Std/Zscore/Rank/Med/Quantile/Corr) | `ts_ops.hpp:494-798` (batch path); online path only covers Sum/Mean/Min/Max/Scale `ts_ops.hpp:328-478` | O(T·W·N) — ~10⁹–10¹⁰ window traversals/alpha | **biggest per-genome sink** |
| **`corr_to_pool` O(pool·T)/candidate** | `fitness.cpp:21-41` | linear in pool size → unscalable past ~5k members | **biggest scaling sink** |
| **NaN-checks block SIMD on elementwise** | `vm.hpp:94,121-124,153-157`; `ts_ops.hpp:82`; `cs_ops.hpp:91` | per-element branch defeats auto-vec on Add/Sub/Mul/Div/Log/… | 2–8× on elementwise-heavy genomes |
| **CPCV fold slicing allocates per fold** | `fitness.cpp:204-235` | ~10–20 vector allocs/candidate → multi-GB churn over a run | alloc/GC pressure |
| **Panel serialized/copied for process workers** | `workload_eval.cpp:235-311` | ~100 MB+ memcpy per program batch; bounds worker count by RAM | memory-bound worker ceiling |
| **Weak-panel re-eval = 2nd full eval** | `fitness.cpp:301-309` | doubles eval cost when robustness panel is set | 2× when active |
| **No work-stealing in DetPool** | `det_pool.hpp` (atomic counter dispatch); LPT mitigates `search_driver.cpp:590-594` | slowest genome bounds makespan | tail-latency |

What is **already good** and must not regress: VM dispatch is batch-per-opcode over a contiguous
date-major buffer with zero per-cell allocation (`vm.hpp:168-283`); the SlotPool is pre-sized and
reused; cross-sectional group ops are already O(n) hash-bucketed (`cs_ops.hpp:311-350`, the prior
sprint's win). Do not "optimize" these — measure first.

---

## Determinism contract (the structural change)

Introduce an explicit evaluation mode, threaded through `Engine`/VM and the search driver:

```cpp
enum class EvalMode { AuditExact, ResearchFast };
```

- **`AuditExact`** = today's contract verbatim: chronological-order Ts reductions, ascending-index
  Cs reductions, single-thread-equivalent digest, bit-identical across worker counts. **Unchanged.**
- **`ResearchFast`** = unlocks: rolling-Σx² / Welford online variance family, SIMD horizontal
  reductions, parallel/lock-free reduction order, `-ffast-math` on isolated kernels, work-stealing
  dispatch. Results are **not** bit-identical and **not** worker-count-invariant.

**The rule that keeps it honest:**

1. Search and ranking run in `ResearchFast`.
2. **Every candidate that passes the admission gate is re-evaluated once in `AuditExact`** before its
   PnL/positions/metrics are written to `library::Library`. The deflation scorecard
   (DSR/PBO/CPCV/capacity) is always the `AuditExact` number.
3. A **tolerance-band differential test** (S1-0) asserts `ResearchFast` agrees with `AuditExact`
   within a relative-ULP band on a fixed genome corpus, so "fast" can never silently diverge into a
   different *alpha*, only a different *last-bit*.

Net: the search gets 10–100× headroom; the published number a human/PBO test reads is still the
exact, reproducible one.

---

## Tasks

### S1-0 — Mode plumbing + tolerance-band differential harness *(do first; unblocks the rest)*
- Add `EvalMode` to `Engine`/VM eval entry and to `SearchConfig`. Default `AuditExact` everywhere so
  the tree is byte-identical until a fast kernel is opted in.
- Build a differential harness: a corpus of ~200 genomes (elementwise / ts-heavy / cs-heavy / mixed),
  run both modes, assert `max relative ULP diff ≤ τ` per op family (τ tuned per op: tighter for
  monotone ops, looser for variance/regression). Wire into CI as `atx-engine-perf-diff-tests`.
- **Accept:** harness green with all kernels in `AuditExact` (no-op equality); knobs exist to flip a
  single op family to `ResearchFast` and see the band, not a different alpha.

### S1-1 — Lazy / incremental pool-correlation *(biggest scaling win — algorithmic, no determinism cost)*
- **Root cause:** `pool_aware_fitness` calls `corr_to_pool` = O(pool·T) for **every** candidate during
  search ranking (`fitness.cpp:21-41,472-473`). At 10k pool members × 500 OOS periods that is ~5M
  correlation ops *per ranked candidate*. This caps usable pool size at a few thousand — fatal for the
  breadth thesis.
- **Fix:**
  1. **Lazy diversify:** rank the search on `raw = wq · robust` only; compute `diversify` (max-corr to
     pool) **at admission time**, not for every ranked genome. (`factory.cpp` ranking path.)
  2. **Incremental max-corr:** maintain per-candidate running max-|corr|-to-pool; when the pool grows
     by one member, update only the new column (1×T), never rescan the whole pool. Back it with the
     existing `CorrNeighborIndex` SimHash neighbors so most pairs are pruned before any O(T) corr.
- **Determinism:** none affected (algorithmic dedup of identical math).
- **Accept:** admitted set byte-identical to today on a fixed seed; fitness wall-time flat as pool
  grows from 100 → 10k (was linear); ≥10× on the ranking phase at pool≥5k.

### S1-2 — Online Ts variance family (ResearchFast) *(biggest per-genome win)*
- **Root cause:** Var/Std/Zscore/AvDiff fall back to O(window) batch recompute (`ts_ops.hpp:344-352`,
  reverted from online due to catastrophic cancellation in naive rolling Σx²).
- **Fix:** implement **Welford / Youngs–Cramer compensated rolling moments** (numerically safe rolling
  variance) behind `ResearchFast`. `AuditExact` keeps the batch path. Validate via S1-0 band.
- **Determinism:** breaks byte-identity (reduction order) → ResearchFast only.
- **Accept:** 5–15× on variance-heavy genomes; S1-0 band holds (τ_var); admitted alphas re-scored in
  AuditExact match the historical batch value within τ.

### S1-3 — Incremental Ts order-statistics: Rank / Med / Quantile / Min / Max
- **Root cause:** sort-per-window O(W log W)/cell (`ts_ops.hpp:551-554,640-645,225-240`). Min/Max
  already have an O(1) monotonic-deque online path — extend the pattern.
- **Fix:** monotonic-deque for windowed Min/Max/argmin/argmax (bit-exact — can ship in `AuditExact`);
  order-statistic ring / indexable skiplist (or two-heap median) for Med/Quantile/Rank (ResearchFast).
- **Accept:** 3–5× on rank/median-heavy genomes; Min/Max stay bit-exact and need no mode flag.

### S1-4 — SIMD elementwise via NaN-mask extraction
- **Root cause:** `std::isnan` branch per element on Add/Sub/Mul/Div/Log/Sigmoid/Tanh/comparisons
  defeats auto-vectorization (`vm.hpp:480-568`).
- **Fix:** compute a NaN/validity bitmask once per column, then run a branchless vectorized kernel and
  blend NaN back. Add `__restrict` to span kernels to kill aliasing assumptions. Elementwise ops have
  **no reduction**, so the result is bit-identical — **shippable in `AuditExact`**. Reductions
  (horizontal sums) stay `ResearchFast`.
- **Accept:** 2–8× on elementwise-heavy genomes; AuditExact bit-identical (no mode flag needed for the
  elementwise kernels); confirm AVX2 4-lane (or wider) codegen via disassembly check in the bench.

### S1-5 — Kill CPCV fold-alloc churn + weak-panel double-eval
- **Root cause:** `aggregate_oos` allocates slice vectors per fold per candidate (`fitness.cpp:204-235`);
  weak-panel robustness triggers a full second eval (`fitness.cpp:301-309`).
- **Fix:** precompute fold index sets once per run; reuse scratch slice buffers across candidates (the
  CsScratch/SlotPool pattern). Share the single full-panel SignalSet between full-universe and
  weak-universe metric slicing instead of re-evaluating.
- **Determinism:** none (same math, no realloc).
- **Accept:** allocation count/candidate → O(1); 2× on the fitness phase when robustness panel active.

### S1-6 — Shared-mmap panel + persistent warm workers
- **Root cause:** `serialize_eval_input` copies the whole panel (~100 MB+) per program batch into SHM
  (`workload_eval.cpp:235-311`); worker count is RAM-bound by these copies.
- **Fix:** map the panel **once** into a single read-only shared segment for the lifetime of the run;
  workers attach and read-alias (the deserializer already does zero-copy column aliasing). Keep a
  persistent warm DetPool across generations instead of re-marshalling per batch.
- **Accept:** per-generation marshalling cost → ~0 after first; worker ceiling rises (RAM no longer
  scales with worker count); end-to-end deep-run wall-time ↓ measurably on a multi-core box.

### S1-7 — Work-stealing dispatch for variable-cost genomes
- **Root cause:** atomic-counter dispatch with no rebalancing; a single huge genome bounds makespan
  (`det_pool.hpp`; LPT sort `search_driver.cpp:590-594` only mitigates).
- **Fix:** work-stealing deque under `ResearchFast` (steal order is nondeterministic → fine in fast
  mode; AuditExact re-eval is single-genome so unaffected).
- **Accept:** tail-bound makespan improves on mixed tiny/huge populations; AuditExact path untouched.

---

## Sequencing & expected compounding

1. **S1-0** (plumbing + harness) — gates everything.
2. **S1-1** (lazy/incremental corr) and **S1-4** (SIMD elementwise) next — both are large wins with
   **zero determinism cost** (S1-1 algorithmic, S1-4 bit-exact). Land these even if ResearchFast
   slips.
3. **S1-2 + S1-3** (online Ts ops) — the headline per-genome speedups; ResearchFast-gated.
4. **S1-5 + S1-6 + S1-7** — orchestration/memory; unlock more workers and cut churn.

Rough compounding target on a deep run: S1-1 (≥10× ranking at scale) × S1-2/3 (5–15× eval) × S1-4
(2–8× elementwise) × S1-6/7 (≥2× effective workers) → **well into the 10²–10³× alphas/sec** regime,
which is the entry ticket to mega-alpha breadth.

## Risks / guardrails

- **Numerical drift masquerading as a new alpha.** Mitigated by S1-0 band + mandatory AuditExact
  re-score at admission. If a ResearchFast genome can't be reproduced in AuditExact within τ, it is
  **rejected**, not admitted.
- **"Optimizing" the parts that are already good** (VM dispatch, Cs group ops, SlotPool). Forbidden
  without a bench showing they're hot. Profile, don't guess.
- **Losing reproducibility of the published library.** The library only ever stores AuditExact
  numbers; ResearchFast is a search accelerator, never a system of record.

## Bench / acceptance

- Extend `bench/` with a per-op-family microbench (cells/s, ns/cell) and a deep-search macrobench
  (alphas/s, gen wall-time) reporting both modes. Every task lands with a before/after number from
  these benches in its commit body. No speedup claim without a bench line.

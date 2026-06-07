# Sprint S2 — Parallel Compute Substrate (user reference)

**Status:** ✅ CLOSED 2026-06-07 (`feat/atx-core-stdlib @ d7a1b75`). **Spec:** [`sprint-2-parallel-compute.md`](sprint-2-parallel-compute.md) · **Plan:** [`sprint-2-parallel-compute-implementation-plan.md`](sprint-2-parallel-compute-implementation-plan.md) · **Ledger:** [`sprint-2-progress.md`](sprint-2-progress.md)

S2 is the engine's **deterministic multicore** layer — the throughput substrate the factory (S3), CV (S1), and learned-model fits (S5) call into to evaluate at scale **without sacrificing the byte-identical-replay guarantee**. One header-only inline subsystem, no atx-core changes, no RNG.

```
atx/engine/parallel/   — atx::engine::parallel   (deterministic pool + digest oracle + parallel eval/run)
```

**The one fact that defines this sprint:** *non-associativity of FP addition under nondeterministic reduction order is the only thing that breaks bit-reproducibility on a CPU map/reduce engine — so S2 eliminates cross-worker FP accumulation entirely.* Determinism is achieved **by construction** (map pattern + fixed-order assemble + reduce-by-sort + strict-FP), **not** by a clever reduction.

---

## Public API

### `parallel/det_pool.hpp` — the deterministic pool (engine-local fallback)
```cpp
class DetPool {
  explicit DetPool(atx::usize n_workers = 0);          // 0 ⇒ max(1, hardware_concurrency()-2)
  [[nodiscard]] atx::usize n_workers() const noexcept;
  template <class Body> void parallel_for(atx::usize n, Body&& body);   // body(i, worker_id), i∈[0,n)
  template <class Fn>   void for_each_worker(Fn&& fn);                  // fn(worker_id) once per worker
};
```
- Fixed-worker pool: persistent `std::thread`s parked on a CV, an **atomic work-index dispenser** (`next_index_.fetch_add`) load-balances pull-style. **Which worker grabs which index is timing-dependent and MUST NOT affect results** — the caller's contract is to write only to its own `out[i]`. `parallel_for` blocks until all indices complete (a barrier). **n==0** is a no-op. **1 worker == sequential.** Body exceptions are captured per-worker and the **lowest index** that threw is rethrown after the barrier (deterministic failure). Rule-of-Five **pinned** (owns threads). `for_each_worker` runs `fn` exactly once on **each** worker (used to warm one stateful `Engine` per worker).
- **Engine-local fallback** for the eventual atx-core `concurrent::TaskPool` (Pattern-B request). The engine consumes it through a single `using Pool = atx::core::concurrent::TaskPool;` switch point (see `fwd.hpp`) — the upstream swap is one line.

### `parallel/digest.hpp` — the determinism oracle
```cpp
[[nodiscard]] atx::u64 signal_set_digest(const alpha::SignalSet&) noexcept;
```
- Canonical-order **wyhash over RAW f64 bytes** (shape → root count → each alpha by name bytes + raw value bytes, in stored root order). A **1-ULP** drift, a reordered root, a renamed root, or a reshape all **flip the digest** — the bit-exact "parallel == sequential" oracle. (`result_table_digest`, the `FoldResult`-table analogue, lives in `parallel_run.hpp`.) Deterministic **within a process** (wyhash seeds are compile-time constants).

### `parallel/batch_eval.hpp` — parallel batch-eval
```cpp
[[nodiscard]] atx::core::Result<alpha::SignalSet>
parallel_evaluate(std::span<const alpha::Program> progs, const alpha::Panel& panel, DetPool& pool);
```
- Fans N compiled programs across the pool — **one program per work item**, each on a **per-worker stateful `Engine`** (the `Engine` is stateful → one per worker, **never** shared) — and assembles roots into one `SignalSet` in **(program order, then root order)**. The digest is **byte-identical to the single-thread `Engine::evaluate` of the equivalent batch program** and **invariant across worker counts {1,2,4,8}**. Returns the **lowest-index** program's evaluate error, if any. **Strategy A** (per-worker recompute; cross-program CSE lost across the shard boundary — strategy B is a profile-gated residual needing an `Engine::evaluate_root` entry that does not exist yet).

### `parallel/parallel_run.hpp` — parallel CPCV folds / backtests + reduce-by-sort
```cpp
struct FoldResult { atx::usize alpha_id, fold_id; atx::f64 sharpe, returns; atx::usize n_test; };
[[nodiscard]] atx::u64 result_table_digest(std::span<const FoldResult>) noexcept;
[[nodiscard]] std::vector<FoldResult>
parallel_cpcv(std::span<const eval::CpcvFold> folds, const alpha::AlphaStreams& streams,
              atx::usize alpha_id, atx::f64 book_size, DetPool& pool);
[[nodiscard]] std::vector<FoldResult>
parallel_backtests(const alpha::AlphaStreams& streams, atx::f64 book_size, DetPool& pool);
[[nodiscard]] atx::f64 cpcv_aggregate_mean_sharpe(std::span<const FoldResult>);   // reduce-by-sort
```
- `parallel_cpcv` runs every CPCV fold of one alpha concurrently; `parallel_backtests` runs one full-sample backtest per alpha — each item a **pure map** over the **const** `AlphaStreams` (per-fold metric via `combine::compute_metrics` — the **same Sharpe convention** S1/P4 use), into a result table in fixed `(alpha_id, fold_id)` order. `cpcv_aggregate_mean_sharpe` is the only cross-item reduction: **reduce-by-sort (R3)** — copy → stable-sort by `(alpha_id, fold_id)` → **sequential Neumaier fold** → bit-identical regardless of input order, never an atomic running sum.

---

## Guarantees (all proven by non-vacuous tests — fail-on-bad AND pass-on-good)

- **Determinism survives parallelism:** the `{1,2,4,8}`-worker digest **==** the single-thread digest for **batch-eval AND CPCV**, on a 128-alpha / 28-fold worst-case (many-small-items) fixture; plus run-to-run stability. (`ParallelDeterminism`, the capstone.)
- **The pool does no result math:** each index processed exactly once, barrier, lowest-index deterministic exception rethrow; `for_each_worker` runs once per worker (a probe of the *old* dispenser routing was 2000/2000 bad — the fix is real).
- **Bit-exact oracle:** a 1-ULP value change, a swapped/renamed root, or a reshape flips the digest.
- **No cross-worker FP accumulation:** map pattern + fixed-order assemble; the only reduction is reduce-by-sort (order-independent, bit-equal to a hand Neumaier fold).
- **No shared mutable state:** const Panel/AlphaStreams + exclusive pre-indexed output slots + one stateful `Engine` per worker. Race-freedom argued **by construction** (TSan unsupported on clang-cl) and validated by the matrix.
- Builds under clang-cl `/W4 /permissive- /WX` + strict-FP; **27 tests**; full engine suite **882/882 green**; **atx-core unmodified**.

## Backbone decision (web-researched)

**Hand-rolled fixed-worker deterministic pool, NOT oneTBB.** This workload (independent alphas/backtests/folds → pre-indexed slots → fixed-order assemble) is the *easy* case for determinism — byte-identical **by construction**, so TBB's `parallel_deterministic_reduce` is redundant here (and "may differ from sequential" anyway), and TBB is the only candidate adding a compiled redistributable DLL against the header-only philosophy. **Taskflow** is the header-only fallback if a real task-graph DAG ever appears; **oneTBB** only if nested/irregular numeric kernels emerge. (Implementation plan §0.2.)

## Known residuals (p1 backlog)

1. **atx-core `concurrent/task_pool.hpp` Pattern-B lift** — promote `parallel::DetPool` into atx-core L4 as `atx::core::concurrent::TaskPool`; the engine swaps via the one-line `using Pool = ...` switch point (the S1 `stats_ext` precedent).
2. **Strategy-B CSE-preserving batch-eval** — recover the cross-program CSE win; needs an `Engine::evaluate_root(program, r)` entry. Profile-gated.
3. **Linux-CI ThreadSanitizer job** — the upstream race-freedom cross-check (clang-cl has no TSan).
4. **Lowest-index-error tie-break test** — the behavior is implemented + reasoned; a directed two-distinguishable-errors test would assert *which* index wins. Minor.
5. **NUMA panel partitioning** — profile-gated; skipped in v1 (L3-resident panel; per-worker write locality already first-touched).

## Baton → next

S2 hands **S3** the throughput to evaluate a mined population per generation across cores (`parallel_evaluate` + the digest oracle), and **S1** the parallelism to run CPCV folds concurrently (`parallel_cpcv`) — both **without changing a single result digit**. The `DetPool` + digest oracles are reused verbatim by S3/S5; the `ParallelDeterminism` matrix is the template every later parallel feature re-proves against.

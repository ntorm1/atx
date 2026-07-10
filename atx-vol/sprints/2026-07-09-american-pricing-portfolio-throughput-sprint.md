# atx-vol American Pricing, Portfolio, and P&L Throughput Sprint

**Date:** 2026-07-09

**Status:** implementation-ready plan; baseline measured on the live `codex/atx-vol-fit-ms` worktree

**Scope:** American equity-option price, full Greeks, portfolio price, and portfolio P&L explain after the fitted surface is available

**Primary goal:** make `atx-vol` a state-of-the-art CPU pricer without weakening the reference pricing contract

---

## 1. Executive decision

`atx-vol` already has the right reference algorithm: the Andersen-Lake-Offengenden
(ALO) spectral-collocation pricer, a fast seven-boundary put finite-difference
Greeks path, a five-boundary put analytic/PDE Greeks path, a Chebyshev American
correction surrogate, unique-contract deduplication, and deterministic parallel
fan-out. The remaining throughput gap is not one isolated instruction. It is the
composition of six issues:

1. the Release build is still compiled for the x64 default SSE2 ISA, not AVX2;
2. every public batch API is currently a scalar loop;
3. dividend-paying call Greeks still use seventeen cold boundary solves;
4. `PricedSurface` queries repeat carry lookup, log-moneyness, curve lookup, and IV
   evaluation, while `PortfolioPricer` allocates frames and creates threads on
   every call;
5. the fast correction cache is dropped by `PricedSurface`, is baked at one
   representative carry, and computes higher derivatives expensively at query
   time;
6. the backtest rebuilds and reprices substantially the same book at adjacent
   steps instead of carrying the previous result forward.

The sprint therefore builds two explicit pricing lanes behind one API:

| Lane | Contract | Intended use |
|---|---|---|
| **Reference** | Current scalar arithmetic/order and cold ALO result; bit-stable where the existing API promises it | tests, replay, archive parity, model validation |
| **Production** | AVX2/AoSoA batch kernels and an optional carry-aware correction surrogate; bounded error against Reference | quote refresh, live risk, large portfolio and backtest throughput |

The production lane must never silently replace the reference lane. Every result
records its route, math mode, ISA, and whether a surrogate was used.

### Headline answer: how fast should this be?

The targets below are for **sustained batch throughput**, not an unsupported claim
that one scalar call has four-way SIMD latency. AVX2 processes four doubles per
vector, but transcendental cost, lane divergence, and memory traffic prevent a
perfect 4x gain.

| Operation | Measured now on i7-1260P | Sprint ship target | Stretch target |
|---|---:|---:|---:|
| Accurate cold ALO price, direct scalar | 1.77k prices/s/core; 564 us | **8k/s/core; <=125 us effective** | 20k/s/core |
| Fast cold ALO price, direct scalar | 17.1k prices/s/core; 58.5 us | **50k/s/core; <=20 us effective** | 100k/s/core; 10 us |
| `PricedSurface` cold price, mixed calls/puts | 10.4k contracts/s, 1 thread; about 96 us | **>=40k/s/core** | >=75k/s/core |
| Full cold Greeks, mixed calls/puts | 0.94k contracts/s, 1 thread; about 1.07 ms | **>=8k/s/core; <=125 us effective** | >=25k/s/core after implicit differentiation |
| Cold `pnl_explain`, mixed calls/puts | 0.92k contracts/s, 1 thread | **>=7k unique contracts/s/core** | >=20k/s/core |
| Carry-aware cached price | not available on `PricedSurface` | **>=500k/s/core; <=2 us** | >=1M/s/core |
| Carry-aware cached full Greeks | not available on `PricedSurface` | **>=125k/s/core; <=8 us** | >=250k/s/core |
| Eight-worker cold quote refresh | 56-58k unique contracts/s | **>=250k/s on this host** | >=400k/s |
| Eight-worker cold full Greeks | 4.6-5.6k unique contracts/s | **>=50k/s on this host** | >=100k/s |
| Eight-worker cached full Greeks | not available | **>=750k/s on this host** | >=1.5M/s |
| Position scatter/aggregation, prepriced uniques | 0.2-1.6M positions/s in the current mixed benchmark | **>=5M positions/s full frame; >=15M/s totals-only** | memory-bandwidth ceiling |

The ALO paper reports throughput close to 100,000 American prices/s/CPU at a
finite-difference-like precision level, so the fast-price stretch target has an
external reference point. There is no equally comparable published full-American-
Greeks throughput number; the Greeks targets above are derived from the measured
boundary-solve rate and the planned reduction from seventeen/seven/five solves to
five or one.

---

## 2. Baseline provenance and limitations

### 2.1 Machine and build

| Item | Baseline |
|---|---|
| CPU | Intel Core i7-1260P, 12 physical cores / 16 logical processors, 18 MiB L3 |
| Topology | hybrid P/E cores; `hardware_concurrency()==16` does not describe equal workers |
| Compiler | clang-cl 18.1.8 |
| Build | `cmake --preset rel`, `/O2 /Ob2 /DNDEBUG`, no `/arch:AVX2`, no LTO |
| Worktree | `C:\atx-wt\atx-vol-fit-ms`, HEAD `c7721aa`, plus the live uncommitted fit/performance changes |
| Correctness run | 28 targeted American/portfolio/P&L tests passed after rebuilding |

The compile database proves that `american.cpp` and `batch.cpp` receive no
`/arch:AVX2`. On x64, the compiler default is SSE2. An xsimd expression in this
build is therefore not evidence of an AVX2 kernel.

### 2.2 Measurements taken for this review

`american_iv_bench` supplied direct cold-pricer rates:

| Route | Rate | Accuracy reported by the existing harness |
|---|---:|---:|
| Accurate ALO cold | 1,772 prices/s | reference cold path |
| Accurate `AloPricer` warm | 2,471 prices/s | max warm/cold price difference 3.6e-5 |
| Fast ALO cold | 17,083 prices/s | fast preset |
| Fast `AloPricer` warm | 17,747 prices/s | only 1.04x in this sweep |
| Accurate American IV | 689 inversions/s | max vol error 8.0e-10 |
| Fast American IV | 1,882 inversions/s | max vol error 4.69e-6 |
| Cached American IV | 12,762 inversions/s | wing-degraded surrogate inversion |

After rebuilding the live portfolio changes, the 2,688-unique-contract mixed book
reported:

| Operation | 1 thread | 4 threads | 8/hardware threads |
|---|---:|---:|---:|
| Full `price` | 937 contracts/s | 2,292/s | 3,723/s at 8; 4,597/s at hw |
| Price-only quote refresh | 10,420/s | 36,028/s | 57,856/s at 8; 56,312/s at hw |
| `pnl_explain` | 915/s | 2,017/s | 5,374/s at hw |

With 100,000 positions deduped to the same 2,688 contracts:

| Operation | 1 thread | hardware threads |
|---|---:|---:|
| Full `price` | 5.25 s; 19k positions/s | 0.48 s; 209k positions/s |
| Price-only quote refresh | 0.421 s; 238k positions/s | 0.063 s; 1.59M positions/s |
| `pnl_explain` | 5.29 s | 1.13 s |
| Portfolio construction | 6.48 ms | n/a |

These are diagnostic numbers, not a stable CI baseline. Consecutive runs varied
materially because the existing executable performs one untuned sample per mode,
has no warm-up, runs on a hybrid CPU without worker affinity, and measures modes
sequentially while frequency and thermal state change. P0 repairs the harness
before any performance claim becomes a gate.

### 2.3 Current solve counts

For a genuine-early-exercise fast-preset option, one boundary solve uses seven
collocation points, sixteen fixed-point quadrature nodes, two Jacobi-Newton plus
up to two fixed-point sweeps, and a sixteen-node premium quadrature. The dominant
boundary work is roughly 384 inner integral nodes plus the final premium nodes.

| Route | Put | Dividend-paying call |
|---|---:|---:|
| Price | 1 boundary | 1 boundary through McDonald-Schroder |
| Full cold FD Greeks | 7 boundaries + 17 premium stencils | **17 boundaries** |
| `american_greeks_al` | 5 boundaries; theta/charm from PDE | **falls back to 17-boundary FD** |
| `pnl_explain`, analytic option | 5-boundary base Greeks + 1 target price | 17 + 1 boundaries |
| Correction-cache first-order/full bundle | no boundary solve, but many 3-D Clenshaw/derivative passes | same |

Call Greeks are therefore the largest exact algorithmic gap in a balanced book.

---

## 3. Current implementation map

### 3.1 American scalar kernel

`src/american.cpp` is a good small-state implementation, but it is scalar and
transcendental-bound:

- `eqn_b_ND` performs the collocation integral and calls `sqrt`, `log`, two
  exponentials, and two normal CDFs per quadrature node;
- `norm_cdf` is `0.5 * std::erfc(-x/sqrt(2))` from `atx-core/math.hpp`;
- `AlBoundary` and `AlWorkspace` use fixed arrays, so an AoSoA conversion does not
  require heap allocation;
- put spot stencils already reuse their spot-independent boundary;
- the five-solve put path correctly rejected a one-solve frozen-boundary shortcut:
  sigma and rate sensitivities genuinely need boundary sensitivity;
- `andersen_lake_call_slice` already proves that one call boundary can serve many
  internal-put spots when `(S,T,sigma,r,q)` is shared.

The previous attempt vectorized quadrature **within one option** and found xsimd
about 6.6x slower. That negative result does not close the SIMD question. The
profitable dimension is **four independent contracts across lanes**, with a
specialized vector math implementation and homogeneous route groups.

### 3.2 Batch layer

`include/atx/vol/batch.hpp` exposes good SoA signatures, but `src/batch.cpp` calls
the scalar function lane by lane. It has no American price/Greeks batch API and
no ISA dispatch. The existing output `std::span<Greeks>` is AoS, which is awkward
for vector stores and portfolio columnar output.

### 3.3 Surface query layer

`PricedSurface::{iv,fair_value,greeks,greeks_analytic}` each performs its own:

1. input validation;
2. linear term-context scan;
3. forward/carry interpolation;
4. `log(K/F)`;
5. surface IV lookup.

`PortfolioPricer::price` first calls `surf->iv`, then calls `fair_value` in
price-only mode or `greeks` in risk mode; those calls repeat the surface work.
`pnl_explain` calls base Greeks, shifted fair value, base IV, and shifted IV as
separate queries. A fused resolved-query primitive is an exact win.

### 3.4 Correction cache

`CorrectionCache` stores a 3-D Chebyshev tensor over `(k,T,sigma)` at one baked
`(r,q,side)`. `eval_grad` differentiates coefficient rows during each query. Full
cached Greeks call `eval_grad` repeatedly at `F+/-`, `sigma+/-`, and `T+/-` points,
performing about nine global tensor evaluations/partial transforms.

`VolaSession` uses this cache only for its default eSSVI path. A high-accuracy
polymorphic override deliberately falls back to cold ALO because one
representative `q` is inaccurate on carry-distant expiries. `PricedSurface`
drops both caches entirely for serialization. Consequently the canonical
portfolio/backtest artifact cannot use the production cached path.

### 3.5 Portfolio and P&L layer

Positive current properties:

- exact-bit contract deduplication;
- SoA public output frames;
- disjoint per-contract writes and fixed-order reductions;
- newly added bounded dedup reservation, `prices_only`, and parallel price scatter;
- deterministic totals across worker counts.

Remaining costs:

- `OptionContract` and unique intermediate results are AoS;
- `SurfaceSet::find` performs a binary search for every unique contract;
- contracts are not grouped by surface, side, expiry bracket, scheme, or route;
- every call allocates all intermediate and output vectors;
- price-only still allocates and writes every risk column as NaN;
- `portfolio_pricer.cpp` owns a second `parallel_blocks` implementation and creates
  fresh `jthread`s for each solve and scatter pass;
- P&L scatter remains serial in the current worktree;
- no totals-only/in-place API exists for risk loops that discard rows.

### 3.6 Backtest layer

`src/backtest.cpp` rebuilds a `std::vector<Position>`, `Portfolio`, and
`PortfolioPricer` for the surviving book each step. It also recomputes book
Greeks for recorded rows even when the same base Greeks were just computed by
the preceding P&L explain. The shifted mark at step `i` is the next base mark at
step `i+1`, but it is not retained. These are exact, higher-level wins and should
be taken before relying on approximate temporal warm starts.

---

## 4. Research findings and implications

Only primary papers and official toolchain documentation are used for the design
claims below.

1. [Andersen, Lake, and Offengenden, *High Performance American Option Pricing*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027)
   is the correct reference algorithm. It combines Jacobi-Newton boundary
   iteration, Gauss-Legendre quadrature, and Chebyshev interpolation, and reports
   throughput near 100k prices/s/CPU at a practical precision level. The sprint
   keeps ALO and improves its implementation/batching instead of replacing it
   with a low-accuracy tree.
2. [LLVM's vectorizer documentation](https://llvm.org/docs/Vectorizers.html)
   confirms that loop/SLP vectorizers are enabled but vector math calls generally
   need a vector library selected with `-fveclib` and `-fno-math-errno`. Current
   `/O2` alone cannot vectorize the `erfc/log/exp` hot loop into a profitable AVX2
   kernel.
3. [SLEEF](https://arxiv.org/abs/2001.09258) is a credible vector-libm candidate
   with explicit ULP variants and broad SIMD support. It must win a local bakeoff;
   it is not adopted on reputation alone.
4. [Microsoft's `/arch` documentation](https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64)
   states that x64 defaults to SSE2 and `/arch:AVX2` enables AVX2/FMA. The library
   needs separately compiled ISA objects plus runtime dispatch, not a global flag
   that would make the binary fail on older CPUs.
5. [Google Benchmark's user guide](https://google.github.io/benchmark/user_guide.html)
   supports warm-up time, repeated/interleaved runs, median/CV reporting, JSON,
   counters, and allocation measurement. These facilities directly repair the
   current single-shot benchmark.
6. [Windows CPU Set APIs](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemcpusetinformation)
   expose topology for a hybrid processor. Worker experiments must distinguish
   P-core-only, E-core-only, and all-core schedules instead of treating sixteen
   logical processors as identical.
7. [Efficient Automatic Differentiation of Implicit Functions](https://arxiv.org/abs/2112.14217)
   provides the general basis for differentiating the converged boundary system
   with the implicit function theorem. This is the technically sound one-solve
   Greeks experiment; freezing the boundary is not.
8. [LLVM ThinLTO documentation](https://clang.llvm.org/docs/ThinLTO.html) supports
   clang-cl plus `lld-link`. ThinLTO/PGO are final multipliers after structural
   work, not substitutes for batching.
9. [Le Floc'h and Healy, *Implying Volatility: How Fast Can We Go?*](https://arxiv.org/abs/2605.29102)
   is a Black-Scholes implied-volatility paper, not an American pricer benchmark.
   Its normalized, fixed-iteration, branch-light design can improve the European
   seed/core, but its numbers must not be labeled American-IV throughput.

---

## 5. Target architecture

```text
positions / lots
      |
      v
PreparedPortfolio (stable keys, aligned SoA, groups, scatter permutation)
      |
      +--> resolve SurfaceSet once per uid group
      |
      v
ResolvedQueryBatch (F, q, k, sigma computed once)
      |
      +---------------- Reference lane ----------------+
      |                                                |
      | scalar ALO or AVX2 AoSoA<4> ALO                |
      | exact route + scalar tail/fallback             |
      |                                                |
      +-------------- Production lane ----------------+
                                                       |
                      carry-aware CorrectionCacheV2    |
                      fused value/derivative Clenshaw  |
                                                       v
UniqueResultSoA (price + selected Greeks + status + route)
      |
      +--> deterministic fixed-order totals
      +--> parallel selected-column scatter
      +--> retained StepState for next P&L date
```

The execution plan is built once for a stable book. Snapshot-specific values are
resolved into reusable workspace. Public convenience APIs remain allocating
wrappers around in-place APIs.

---

## 6. Work packages

## P0 - Make performance measurable and enforceable

**Estimate:** 2 engineer-days

**Risk:** low

**Ship condition:** required before accepting later speedups

### P0.1 Replace the single-shot example with a benchmark matrix

Add `bench/american_pricing_bench.cpp` and
`bench/portfolio_throughput_bench.cpp` using Google Benchmark. Keep the existing
examples for human demonstrations.

Benchmark axes:

- route: accurate cold, fast cold, cached, European short-circuit;
- side: put, dividend call, no-dividend call;
- moneyness: 0.60, 0.80, 0.95, 1.00, 1.05, 1.20, 1.50;
- maturity: one day, one week, one month, six months, two years;
- volatility: 5%, 15%, 30%, 75%, 150%;
- API: raw price, delta only, FD Greeks, analytic Greeks, surface query, portfolio
  price-only, full price, P&L explain;
- batch size: 1, 4, 16, 256, 4,096, and the real OPRA board;
- position/unique ratios: 1:1, 10:1, 100:1, 1,000:1;
- worker schedule: 1, 2, 4, 8, P-core-only, E-core-only, all cores.

Each case must use at least 0.5 s warm-up, at least five randomly interleaved
repetitions, and JSON output. Report median, p95/p99 where applicable, coefficient
of variation, contracts/s, positions/s, and bytes/s.

### P0.2 Add algorithm counters

In a benchmark-only instrumentation build, count:

- boundary solves;
- Jacobi-Newton/fixed-point sweeps;
- early residual exits;
- premium evaluations;
- normal CDF/log/exp calls;
- scalar fallback lanes;
- cache hits, out-of-box clamps, and cold fallbacks;
- frame allocations/bytes and worker launches.

Add Windows ETW/VS Profiler or VTune runbook commands for cycles, instructions,
branch misses, and L1/L2/LLC misses. Save the compiler optimization record with
`/clang:-Rpass=loop-vectorize`, `-Rpass-missed`, and `-fsave-optimization-record`.

### P0.3 Establish checked-in baselines

Check in:

- `bench/baselines/i7-1260p-clang18.json`;
- a comparison script that fails only on a statistically significant >10%
  regression with CV <=5%;
- machine/compiler/commit/ISA/math-mode metadata;
- separate latency and throughput dashboards.

Do not gate on a noisy laptop absolute number in general CI. Gate ratios on every
machine and run the canonical absolute gate on one pinned performance host.

### P0 acceptance

- Existing output values unchanged.
- Benchmark CV <=5% for single-core kernels and <=10% for all-core cases.
- The harness visibly distinguishes scalar latency from batch effective ns/option.
- The current `kernel floor` mismatch is removed: it must benchmark exactly the
  same fused operation as the API under test.

---

## P1 - Exact structural wins in surface, portfolio, and P&L plumbing

**Estimate:** 4 engineer-days

**Risk:** low-to-medium

**Expected gain:** 1.2-2x small books; 3-10x high position/unique ratios; lower tail latency

### P1.1 Add one fused surface evaluation

Introduce an internal/public-low-level request:

```cpp
enum class EvalField : uint32_t {
  Iv = 1u << 0, Price = 1u << 1, FirstOrder = 1u << 2,
  SecondOrder = 1u << 3
};

struct ResolvedSurfacePoint {
  double K, T, F, q, k, sigma, df;
  const PricedSurface* surface;
};

Status PricedSurface::evaluate_batch(
    span<const double> K, span<const double> T, span<const Side> side,
    EvalField fields, EvaluationSoA out, EvaluationWorkspace& ws) const;
```

Resolve the T bracket, `F/q`, `log(K/F)`, curve slice, and `sigma` once. Price and
Greeks consume that resolved point. `PortfolioPricer::prices_only` must stop
calling `iv` and `fair_value` separately.

For a fixed expiry group, resolve carry and the two curve slices once for the
whole strike ladder.

### P1.2 Build `PreparedPortfolio`

At portfolio construction:

- retain public input order and exact-bit dedup semantics;
- build 32/64-byte-aligned SoA columns for `K`, `T`, `side`, `uid`, weights, and
  original unique index;
- stable-sort an execution permutation by
  `(uid, T-bracket, side, method, AlOpts, route)`;
- emit `ContractGroup {begin,end,uid,side,scheme}` records;
- retain reverse permutation for deterministic output;
- resolve one surface pointer per uid group per snapshot, not per contract.

Do not quantize `K`, `T`, or sigma to create groups. Group only equal execution
metadata; option values remain exact.

### P1.3 Add reusable workspace and selected output columns

Add:

- `PortfolioWorkspace::reserve(n_unique,n_positions)`;
- `price_into(..., PriceFrameView, workspace)`;
- `pnl_explain_into(..., PnlFrameView, workspace)`;
- `price_totals(...)` and `pnl_totals(...)` with no row-frame allocation;
- a field mask so price-only writes only `id/uid/price/pv/iv/status`, not eight
  NaN risk arrays;
- reusable aligned `UniqueResultSoA` scratch.

The existing returning APIs remain source-compatible wrappers.

### P1.4 Use one persistent pricing executor

Replace TU-local `parallel_blocks` and per-call `jthread` construction with a
long-lived `PricingExecutor` shared by the portfolio/backtest or a proven
repository-wide pool. Requirements:

- deterministic disjoint chunk ownership;
- fixed-order reduction after the barrier;
- no allocations or worker creation in steady state;
- topology policy: `Auto`, `PerformanceCores`, `AllCores`, explicit CPU set;
- nested-parallelism guard so fit and price pools do not oversubscribe each other.

For small `n_unique`, execute inline. Determine the parallel threshold from P0,
not a guessed constant.

### P1.5 Parallelize P&L scatter and separate reduction

Mirror the newly implemented price scatter: write disjoint P&L columns in
parallel, then reduce totals in input order. A totals-only request skips scatter
entirely.

### P1 acceptance

- Reference output is bit-for-bit equal for all existing price and P&L columns.
- `prices_only` returns the same IV/price/PV and does not allocate risk columns in
  the in-place/selected-frame API.
- No heap allocations and no thread creation after workspace/executor warm-up.
- 100k positions / 2,688 uniques: >=5M positions/s when uniques are prepriced;
  totals-only >=15M positions/s.
- Full targeted suite plus ASan/UBSan equivalent configuration remains green.

**Likely files:** `priced_surface.hpp/.cpp`, `portfolio_pricer.hpp/.cpp`,
`parallel_for.hpp`, new `pricing_executor.hpp/.cpp`, portfolio/P&L tests and benches.

---

## P2 - Remove unnecessary cold boundary solves

**Estimate:** 5 engineer-days plus a two-day time-boxed research spike

**Risk:** medium for call symmetry; high for implicit differentiation

**Expected gain:** 2-3x on balanced full-Greeks books before SIMD

### P2.1 Give calls the same boundary-reuse treatment as puts

For a call, McDonald-Schroder maps
`C(S,K,r,q) = P(K,S,q,r)`. The internal put boundary is homogeneous in its strike.
Represent a solved boundary in dimensionless form (`b/K`, or the existing `y`
coordinate plus normalized metadata), then rescale it for original-spot stencils.

Implement:

- `SolvedPutBoundary` as a reusable internal value;
- solve/evaluate APIs for dimensionless boundaries;
- call FD Greeks using seven unique `(sigma,r,T)` boundary states instead of
  seventeen scalar calls;
- call analytic/PDE Greeks using base, sigma+/-, and rate+/- boundary states,
  matching the five-solve put route;
- direct call-chain rule for delta/gamma/theta/charm under the symmetry map;
- scalar fallback for collapse, non-smooth exercise-boundary proximity, and
  unsupported rate/yield corners.

This is higher priority than micro-optimizing a put that is already on five solves.

### P2.2 Hoist invariant scalar work

Within each boundary:

- bind Gauss-Legendre tables once per scheme;
- precompute node transforms, half-tau factors, and values independent of the
  current boundary iterate;
- specialize the fixed fast scheme (`7x16x4`) and accurate scheme (`12x24/48x6`)
  with bounded loops and compiler-visible trip counts;
- flatten small helpers after inspecting inlining remarks;
- keep summation order unchanged in Reference mode.

### P2.3 Warm start across time, not just within a Greek stencil

Store the last converged **dimensionless** boundary by stable contract identity
`(uid,K,expiry,side,scheme)` in `PortfolioPricingState`. On the next snapshot:

- remap the prior boundary to the new residual T;
- use it as the seed for base and bump solves;
- exit as soon as the actual residual meets tolerance;
- cold-reseed after a configurable move guard or failed residual trend;
- record warm hit, sweep count, and fallback.

The current warm benchmark shows that skipping BAW seeding alone is small. This
task ships only if temporal coherence also reduces sweep count.

### P2.4 Time-box: implicit boundary differentiation

The converged collocation state satisfies `R(y; theta)=0`, with
`theta=(sigma,r,T)`. Freezing `y` is wrong. Instead:

1. expose a pure residual evaluator for the full collocation vector;
2. compute `J = dR/dy` and `R_theta` with forward AD or stable central checks;
3. solve `J * y_theta = -R_theta` using a dense pivoted LU (`n<=12`);
4. propagate `y_theta` through premium quadrature for vega/rho/theta;
5. derive vanna/charm and, if stable, second boundary derivatives for volga;
6. validate near the exercise boundary where derivatives may be non-smooth.

**Ship rule:** one-solve Greeks become production only if the full OPRA/corner
grid meets the Greek and P&L gates below and costs <=1.8 boundary-equivalents.
Otherwise keep the code as an experiment and ship the five-solve route. No frozen-
boundary approximation is accepted.

### P2 acceptance

- Dividend-call price and all Greeks pass a put-call-symmetry/reference grid over
  rates/yields, wings, near expiry, and exercise-region points.
- Five-solve call path is <=0.1 tick from cold FD and meets Greek gates.
- Mixed-book full Greeks >=3x P0 single-thread before SIMD.
- Warm state reduces median sweeps by >=40% on a multi-date real corpus or is left
  disabled.
- Implicit differentiation meets its ship rule or is explicitly killed with
  benchmark/error evidence.

**Likely files:** `american.cpp/.hpp`, new `american_boundary.hpp` internal header,
`american_test.cpp`, PDE/Richardson oracle support, portfolio state plumbing.

---

## P3 - AVX2 across independent contracts

**Estimate:** 6 engineer-days

**Risk:** medium-high

**Expected gain:** 1.8-3x per core over P2 for homogeneous batches

### P3.1 Add safe ISA multiversioning

Build separate objects:

- `american_kernel_scalar.cpp` with the current baseline ISA;
- `american_kernel_avx2.cpp` with `/arch:AVX2` and FMA;
- optional future AVX-512 object, not required for this sprint.

Dispatch once per process using CPUID plus OS AVX state checks. Expose the selected
ISA in benchmark metadata and result diagnostics. Tests must be able to force
Scalar or AVX2.

Do not compile the whole library with `/arch:AVX2`; archive readers and fallback
paths must continue to run on SSE2-era x64 hosts.

### P3.2 Use AoSoA<4> boundary state

Vectorize **four options**, not four quadrature nodes of one option:

```cpp
template<class V> struct AlBoundaryPack {
  array<V, kAlMaxNodes> y, tau, z, wbary;
  V T, K, sigma, r, q, xmax;
  mask_type active;
};
```

Each AVX2 lane preserves its own quadrature reduction order, which makes a
bit-stable arithmetic mode possible except for vector transcendental functions.
Group packs by side, scheme, and route to limit divergence. Maintain an active
mask across convergence sweeps; compact only between packs, never reorder public
outputs.

Degenerate, no-early-exercise, outlier, and failed lanes are patched through the
scalar reference path.

### P3.3 Vector-math bakeoff

Benchmark four candidates in the actual boundary kernel:

1. scalar `std::erfc/log/exp` baseline;
2. SLEEF AVX2 ULP-bounded functions;
3. clang `-fveclib=SLEEF` auto-vectorized calls;
4. a specialized normal-CDF approximation with scalar tail patching.

The winner must be selected on end-to-end price/Greeks throughput and accuracy,
not isolated CDF ns/call. xsimd's previous 6.6x regression remains a documented
negative baseline.

Use two math modes:

- `Reference`: scalar libm and current ordering;
- `FastDeterministic`: one fixed approximation/ISA implementation with results
  deterministic for that ISA, bounded against Reference.

Never enable global `/fp:fast`. Apply the minimum function/TU flags proven safe;
retain NaN, signed-zero, and error handling at API boundaries.

### P3.4 Add real American batch APIs

Add SoA APIs with caller workspace and per-lane status/route:

```cpp
Status american_price_batch(const AmericanBatchInput&, PriceBatchOutput&,
                            PricingKernel&, PricingWorkspace&);

Status american_greeks_batch(const AmericanBatchInput&, GreekFieldMask,
                             GreeksBatchSoA&, PricingKernel&,
                             PricingWorkspace&);
```

Also vectorize the existing Black-76 price/value-vega/Greeks batches. Change the
preferred Greeks output to SoA; retain the AoS wrapper for compatibility.

Integrate batches through `PreparedPortfolio` groups. A scalar loop around the
new API is not an accepted implementation.

### P3.5 Inspect code generation

For every AVX2 hot function, retain:

- an optimization-record check showing vectorization;
- a disassembly smoke test or documented inspection command;
- no unexpected scalar `erfc` inside the packed loop;
- cycles/option, instructions/option, and scalar fallback rate.

### P3 acceptance

- Scalar Reference remains bit-for-bit unchanged.
- AVX2 price max error <=1e-8 dollars against Reference for normal-domain points
  and <=0.001 dollars on the full stress grid; all larger-error lanes fall back.
- Full production price/Greek gates in section 9 pass.
- Homogeneous batch speedup >=1.8x vs P2 scalar; target >=2.5x.
- AVX2 never regresses batches smaller than four by more than 5%; dispatcher uses
  scalar below the measured crossover.
- No illegal instruction on forced-scalar CI.

**Likely files:** new kernel/dispatch files, `batch.hpp/.cpp`, CMake ISA object
libraries, `batch_test.cpp`, new benchmark targets.

---

## P4 - Carry-aware cached price and analytic cached Greeks

**Estimate:** 6 engineer-days

**Risk:** high but largest production payoff

**Expected gain:** 10-100x over cold full Greeks after amortization

### P4.1 Build `CorrectionCacheV2` for the actual term carry

The existing fixed-`q` cache is not sufficient for dense/high-accuracy surfaces.
Build the 3-D correction samples with the surface's actual `q_eff(T_node)` rather
than one representative q. This creates the composed function
`C(k,T,sigma; r, q_eff(T))` used for price queries.

For risk semantics:

- do not use the composed table's total T derivative as model theta;
- derive theta from the continuation PDE, matching the cold analytic route;
- derive charm from delta/gamma/speed and the PDE;
- build small `r+/-` correction tensors or an explicit r-derivative tensor so rho
  includes early-exercise-rate sensitivity;
- keep q as the locally resolved carry held fixed by the current Greek contract.

Compare this design against a small explicit q-axis cache. Select the smallest
representation that meets all accuracy gates; record memory and build time.

### P4.2 Precompute derivative coefficients

At build/load time, generate coefficient tensors for the derivatives actually
needed:

- `C`, `C_k`, `C_kk`;
- `C_sigma`, `C_k_sigma`, `C_sigma_sigma`;
- `C_r` (from explicit r sampling or an equivalent validated derivative).

This replaces repeated query-time coefficient differentiation and finite
differences of `eval_grad`. Clamp semantics must also return coherent zero
partials at out-of-box axes.

### P4.3 Fused multi-output Clenshaw

Lay out derivative channels as an AoSoA and evaluate four channels at a time with
AVX2 while sharing coordinates and loop control. This SIMD dimension uses only
FMA/adds and is independent of vector libm.

Provide:

- value-only fast evaluation;
- first-order evaluation;
- full second-order evaluation;
- four-options-at-once evaluation for large groups.

Use live-sized reusable scratch, not a 4,096-double stack array per query. Benchmark
global Clenshaw against a local tensor spline/low-rank representation only if the
global pass cannot meet 2 us value / 8 us full-Greeks targets.

### P4.4 Attach caches to `PricedSurface` and archives

Add an optional derived-cache payload with:

- version, dimensions, bounds, side, r/carry fingerprint, scheme/math mode;
- coefficient CRC and source-surface fingerprint;
- lazy load or bounded LRU materialization;
- cold fallback on missing/mismatched/failed cache;
- archive size and load-time metrics.

The cache may be produced asynchronously after the high-performance fit. It is
amortized work and must not silently inflate the latency-sensitive fit SLA. For a
new surface, allow `ColdUntilReady` and atomic publication of the immutable cache.

Set a build/amortization policy from measured numbers:

```text
use cache iff expected remaining queries * (cold_cost - cached_cost)
              > cache_build_cost + load/memory budget
```

### P4.5 Self-consistency is not enough

A cache can invert and reprice its own approximation perfectly while being wrong
versus cold ALO. Acceptance must compare independently against cold Reference and
the PDE/Richardson oracles. Report errors by price, vega, moneyness, maturity,
carry distance, and exercise proximity. Do not use only percent-within-bid/ask.

### P4 acceptance

- Price/Greeks accuracy gates in section 9 pass for all enabled cache cells.
- Out-of-gate cells use cold ALO and expose `route=ColdFallback`.
- Cached value >=500k contracts/s/core and cached full Greeks >=125k/s/core.
- Cache build time, bytes/surface, load time, and break-even query count are
  printed and stored in benchmark JSON.
- Archive round trip preserves the cache fingerprint and Reference surface result.
- Active-cache memory has a configurable hard cap and deterministic eviction.

**Likely files:** `correction.hpp/.cpp`, `priced_surface.hpp/.cpp`,
`surface_archive.hpp/.cpp`, session cache builder, new cache parity/throughput tests.

---

## P5 - Stateful portfolio and P&L explain

**Estimate:** 4 engineer-days

**Risk:** medium

**Expected gain:** about 2x backtest-step compute before kernel gains; much larger allocation reduction

### P5.1 Stable contract identity

Backtest state must key a listed option by `(uid,K,expiry_ts,side)`, not residual
floating-point T. Update T in an aligned column per date. Maintain incremental
position weights and contract reference counts so opening/closing lots does not
rebuild the whole dedup hash table.

### P5.2 Carry forward base state

Add `PortfolioStepState` containing, per unique contract:

- snapshot/surface fingerprint;
- residual T;
- base IV, mark, selected Greeks, status/route;
- optional dimensionless ALO boundary state;
- generation number for entry/exit invalidation.

Implement a step API that computes P&L and next risk together:

```cpp
Result<PnlTotals> advance(
    const SurfaceSet& next, Timestamp next_ts,
    PortfolioStepState& state, const PriceOptions&);
```

The prior target mark becomes the next base mark. If next-date risk is requested,
compute it once and retain it; `book_greeks` reads the retained totals rather than
repricing. Handle expiring lots before state promotion and new lots after the P&L
cut according to the current accounting semantics.

### P5.3 Axis-state fusion

For each contract, resolve:

- base local state once;
- shifted price at rolled T;
- shifted sigma at common base T for `dvol`;
- shared `dS`, `dt`, and `dr` once per uid group.

Write Taylor components directly from unique results. If the caller requests only
totals, aggregate unique contract weights and avoid position scatter entirely.

### P5.4 Deterministic invalidation

Invalidate cached marks/Greeks/boundaries on:

- surface or pricing-context fingerprint change not represented by the next step;
- option method/scheme/math-mode change;
- corporate-action contract adjustment;
- cache route/fingerprint change when strict replay is requested;
- entry/exit/expiry generation mismatch.

### P5 acceptance

- Existing NAV, settlement, financing, cost, and every P&L axis are bit-for-bit
  unchanged in Reference mode.
- No duplicate `book_greeks(base)` after a successful preceding step.
- Target-to-next-base mark reuse has a direct equality test.
- Steady-state backtest performs no portfolio rebuild, worker creation, or frame
  allocation.
- End-to-end steps/s improve >=2x over P0 at the same kernel mode; kernel P2-P4
  gains stack on top.

**Likely files:** `backtest.cpp/.hpp`, `portfolio_pricer.hpp/.cpp`, new state and
incremental-book tests, backtest benchmark.

---

## P6 - Final compiler and deployment pass

**Estimate:** 2 engineer-days

**Risk:** low if applied last

1. Test ThinLTO on the Release performance preset and retain it only for a >=3%
   geomean win with no build/deployment regression.
2. Collect a representative pricing/backtest PGO profile and test PGO separately.
3. Confirm hot call sites inline across TU boundaries and cold error paths remain
   outlined.
4. Verify `/O2`, `/arch:AVX2` only on ISA objects, floating-point flags, and vector
   library linkage in `compile_commands.json` and the final binary.
5. Add startup diagnostics: selected ISA, math mode, worker topology, cache policy.
6. Run sustained thermal tests; AVX2 can reduce clock frequency, so compare
   contracts/joule and all-core sustained rate, not just the first second.

**Acceptance:** retain each compiler feature only if the canonical benchmark
improves with identical correctness. No global `native` target and no undeclared
runtime DLL dependency.

---

## 7. Delivery sequence and dependencies

| Sequence | Increment | Depends on | Can ship alone? |
|---:|---|---|---|
| 1 | P0 benchmark/oracles | none | yes |
| 2 | P1 fused queries/prepared portfolio/workspace | P0 | yes |
| 3 | P2 five-solve calls + warm state | P0; P1 for state | yes |
| 4 | P3 AVX2 packed cold kernels | P0; benefits from P1 groups and P2 calls | yes, opt-in production mode |
| 5 | P4 cache V2 and fused cached Greeks | P0; P1 batch API; P3 Clenshaw SIMD optional | yes, opt-in with fallback |
| 6 | P5 stateful P&L/backtest | P1; benefits from P2/P4 state | yes |
| 7 | P6 LTO/PGO/deployment | all stable increments | yes |

Recommended staffing is one kernel owner and one portfolio/cache owner for four to
six weeks, with correctness review at every gate. If only one two-week sprint is
available, commit to **P0 + P1 + P2.1**. That produces reliable numbers, removes
repeated plumbing, and fixes the 17-solve call asymmetry without depending on an
approximation.

---

## 8. Implementation task ledger

| ID | Deliverable | Estimate | Proof |
|---|---|---:|---|
| P0-1 | Google Benchmark price/Greek matrix | 0.75 d | JSON median/CV |
| P0-2 | Portfolio/P&L ratios and topology matrix | 0.75 d | stable repeated rates |
| P0-3 | counters, optimization remarks, baseline compare | 0.5 d | checked-in baseline |
| P1-1 | fused resolved surface point/batch | 1.0 d | bit-equality tests |
| P1-2 | aligned `PreparedPortfolio` groups | 1.0 d | permutation/dedup tests |
| P1-3 | workspace, in-place, field mask, totals-only | 1.0 d | zero-allocation bench |
| P1-4 | persistent topology-aware executor + P&L scatter | 1.0 d | deterministic thread tests |
| P2-1 | normalized boundary solve/eval API | 1.0 d | put unchanged |
| P2-2 | seven/five-solve call Greeks | 2.0 d | call reference/PDE grid |
| P2-3 | cross-step warm state | 2.0 d | sweep reduction corpus |
| P2-X | implicit differentiation spike | max 2.0 d | ship/kill report |
| P3-1 | ISA dispatcher and AVX2 object | 0.75 d | forced ISA tests |
| P3-2 | AoSoA<4> boundary kernel | 2.0 d | scalar parity grid |
| P3-3 | vector-math bakeoff and tail patch | 1.5 d | error/speed frontier |
| P3-4 | public American/B76 SoA batch APIs | 1.0 d | lane/status tests |
| P3-5 | portfolio integration/disassembly gates | 0.75 d | end-to-end target |
| P4-1 | carry-aware/rho-correct cache design bakeoff | 1.5 d | error/memory report |
| P4-2 | derivative tensors and fused multi-output Clenshaw | 1.5 d | derivative oracle |
| P4-3 | `PricedSurface` cache route and fallback | 1.0 d | route/cold parity |
| P4-4 | archive/lazy/LRU integration | 1.0 d | round-trip/cap tests |
| P4-5 | amortization and production gates | 1.0 d | real-corpus bench |
| P5-1 | stable incremental book + step state | 1.5 d | lifecycle tests |
| P5-2 | fused advance/P&L/next-risk API | 1.5 d | bit-identical backtest |
| P5-3 | invalidation and totals-only integration | 1.0 d | mutation battery |
| P6 | ThinLTO/PGO/sustained deployment validation | 2.0 d | retained-feature report |

---

## 9. Correctness and numerical acceptance gates

Performance work does not ship on throughput alone.

### 9.1 Price gates

Use three independent references:

1. current scalar accurate ALO;
2. Richardson-refined Crank-Nicolson/PDE oracle on a larger corner grid;
3. fixed published/reference values where available.

For every side, exercise/no-exercise regime, rate/yield sign corner, moneyness,
maturity, and volatility bucket:

- Reference mode: preserve existing bit contracts and improve the current coarse
  PDE test from only `<0.5% relative` to a price-scaled oracle tolerance;
- Fast cold mode: max `|price_fast-price_reference| <= $0.001` and always
  `< $0.005` half-tick;
- Cached mode: p99 <=$0.001 and max <=$0.005 inside its declared box; otherwise
  route to cold fallback;
- preserve intrinsic lower bound, European lower bound where applicable,
  monotonicity, convexity, and put-call symmetry.

Do not use relative error alone for near-zero option values.

### 9.2 Greek gates

Build a high-accuracy Richardson/complex-step-compatible reference around the
accurate pricer, with bump studies per axis. Check absolute and scaled errors:

- delta: abs <=2e-5;
- gamma: abs <=2e-5 or relative <=2e-3 where `|gamma|>1e-3`;
- vega/rho/theta: contribution error under the canonical one-day/1-vol-point/1-bp
  shocks <=$0.001 per share;
- vanna/volga/charm: contribution error under the combined canonical shocks
  <=$0.001 per share;
- no NaN/Inf except documented invalid lanes;
- explicit exercise-boundary band reported separately, with scalar fallback if a
  derivative is non-smooth.

This contribution-based gate is more meaningful than a universal `1e-6 relative`
threshold for Greeks that cross zero.

### 9.3 P&L explain gates

The identity
`unexplained = total - sum(explained axes)` is tautological and is not an accuracy
test. Instead require:

- pure-axis bump tests with non-target axes exactly zero;
- normalized unexplained distribution vs move size, with p95/p99 and maximum;
- second-order convergence: halving a pure spot/vol move reduces residual at the
  expected order away from the exercise boundary;
- whole-book Reference and Production comparisons by axis;
- NAV/accounting equality including settlement, shares, financing, and costs;
- deterministic totals across worker count and execution grouping.

### 9.4 Determinism contract

- Reference scalar: cross-thread and archive bit identity where currently promised.
- Fast AVX2: bit identity for the same ISA/math-mode/build, all worker counts.
- Cross-ISA: numerical tolerance, not false bit-identity.
- Every persisted result records ISA, math mode, pricing route, cache fingerprint,
  compiler version, and scheme.

---

## 10. Performance acceptance gates

A work package ships only if all apply:

1. median speedup meets its package target on the synthetic matrix and a real OPRA
   corpus;
2. benchmark CV is within the P0 limit;
3. p99 latency does not regress by >10% unless the API is explicitly throughput-
   only;
4. scalar fallback rate is reported and <=5% on the normal production corpus;
5. no new steady-state allocation in raw kernel, prepared portfolio, or stateful
   P&L paths;
6. performance is measured cold-cache and warm-cache where both matter;
7. all-core measurement runs long enough to expose AVX frequency/thermal effects;
8. output bandwidth is reported separately from unique-contract compute;
9. full tests, warnings-as-errors, sanitizers, and archive compatibility pass;
10. a before/after JSON and a short decision note are committed.

---

## 11. Benchmark scenarios that must exist

| Scenario | Why |
|---|---|
| One fast ATM put | scalar latency floor |
| One dividend ATM call | current 17-solve Greek worst path |
| No-dividend call | European short-circuit ceiling |
| Four homogeneous options | AVX2 pack efficiency |
| Mixed side/scheme/corner pack | mask/fallback behavior |
| One full SPY chain by expiry | realistic grouping and smile divergence |
| 64-underlying, 2,688-unique book | current portfolio comparison |
| 100k/1M positions over 2,688 uniques | scatter/bandwidth/dedup |
| Two adjacent real dates | P&L state and warm boundary reuse |
| 250-date rolling book | sustained backtest, allocation, cache amortization |
| Deep wing/near expiry/high vol grid | error and divergence stress |
| Negative/near-zero rate-yield corners | short-circuit/fallback correctness |

Each portfolio case reports both **unique contracts/s** and **positions/s**. A
headline positions/s number without its dedup ratio is rejected.

---

## 12. Risks and mitigations

| Risk | Mitigation |
|---|---|
| AVX2 vector libm changes prices | explicit math modes, scalar oracle, tail/error fallback |
| Hybrid P/E cores create noisy or slower `hw` runs | topology discovery, pinned benchmark modes, persistent executor |
| AVX2 lowers sustained clock | long thermal run; retain scalar crossover policy |
| Call homogeneity loses bit identity | Reference remains scalar; validate production to tick/Greek gates |
| Boundary derivatives are non-smooth near exercise | detect proximity and fall back to five/seven-solve FD |
| Implicit Jacobian is ill-conditioned | pivoted LU, condition/residual gate, strict two-day kill rule |
| Carry-aware cache gives wrong theta/rho semantics | PDE theta/charm, explicit r derivative, cold comparison |
| Cache is self-consistent but cold-inaccurate | independent cold/PDE gates; never score only round-trip/in-band |
| Cache build ruins fit latency | async publication, archive persistence, amortization threshold |
| Derivative tensors consume too much memory | bounded LRU, lazy materialization, measured per-surface cap |
| Prepared sorting changes public order/reduction | reverse permutation and fixed input-order reduction |
| Stateful P&L reuses stale results | fingerprints, generations, explicit invalidation battery |
| LTO/PGO complicates builds | final optional preset; retain only measured features |
| Dirty worktree evolves during implementation | isolate commits by work package; rebase/check overlap before edits |

---

## 13. Explicit non-goals

- Replacing high-performance fitting or changing fitted curve parameters.
- GPU/CUDA in this sprint; CPU batching must be saturated first.
- Changing listed-option contract/accounting semantics.
- Global `-ffast-math` or `/fp:fast`.
- Quantizing strikes, maturities, or vols to manufacture shared boundaries.
- Claiming FlashIV's Black-Scholes numbers as American-IV numbers.
- Removing the scalar cold oracle.
- Serializing mutable warm-start state as part of the immutable surface.
- Treating more threads as a substitute for a faster per-core kernel.

---

## 14. Definition of done

The sprint is complete when:

- [ ] the benchmark/oracle suite is stable and checked in;
- [ ] Reference mode is unchanged and all numerical gates pass;
- [ ] dividend calls no longer use the 17-boundary full-Greeks route in Production;
- [ ] American price and full-Greeks SoA batch APIs execute real AVX2 kernels;
- [ ] the binary safely dispatches scalar/AVX2 at runtime;
- [ ] prepared portfolio pricing is allocation-free/thread-spawn-free in steady state;
- [ ] price-only and totals-only avoid unused columns;
- [ ] `PricedSurface` can serve a validated carry-aware cache with cold fallback;
- [ ] cached derivative evaluation is fused and query-time coefficient
  differentiation is gone;
- [ ] P&L explain carries target marks/next risk forward without duplicate repricing;
- [ ] all ship throughput targets in section 1 are met on the pinned host or the
  relevant increment is documented as killed with evidence;
- [ ] benchmark JSON, accuracy report, cache amortization report, and deployment
  metadata are committed;
- [ ] README performance claims are updated to distinguish scalar latency, batch
  throughput, cold/reference, cached/production, and unique vs position rates.

At that point `atx-vol` has a defensible state-of-the-art architecture: ALO remains
the reference, the cold path uses symmetry, differentiation, batching, AVX2, and
topology-aware parallelism, while the production path serves a carry-correct,
derivative-aware surrogate at portfolio scale with a transparent fallback to the
reference model.

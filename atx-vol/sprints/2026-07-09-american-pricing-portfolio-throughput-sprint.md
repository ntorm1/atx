# atx-vol American Pricing, Portfolio, and P&L Throughput Sprint

**Date:** 2026-07-09 (rev. 2, evidence-verified)

**Status:** implementation-ready plan. Every claim about the current code carries a
`file:line`; every external claim carries a primary URL. Claims that could not be
sourced are listed in §16 rather than asserted.

**Scope:** American equity-option price, full Greeks, portfolio price, and portfolio
P&L explain — everything downstream of the fitted surface.

**Primary goal:** make `atx-vol` a state-of-the-art CPU pricer without weakening the
reference pricing contract.

---

## 0. What changed in this revision

The first draft was written against a reading of the code, not a line-by-line audit.
Four independent audits (kernel, portfolio/P&L, cache/batch/build, literature) found
the strategy sound and eight specific claims wrong. Corrections that change what we
build, in priority order:

1. **The no-dividend call short-circuit already exists — and is subtly WRONG.**
   `american.cpp:1165-1168` short-circuits a `q<=0` call to Black-76. The exact
   never-exercise condition is `q<=0 AND q<=r` (calls) / `r<=0 AND r<=q` (puts).
   Under `r<0` the code returns a European price for an option that can be optimally
   exercised early. **This is a correctness bug, not a performance gap.** New §P0.5.

2. **The put strike ladder does N boundary solves; the call ladder does 1.**
   `andersen_lake_call_slice` (`american.cpp:1182-1288`) exploits the fact that the
   internal-put strike is `Kp=S`, constant across the slice. Puts have no analogue —
   `correction.cpp:339-343` literally falls back to a per-node scalar solve. The
   exercise boundary is homogeneous of degree one in strike, so this is recoverable.
   New §P2.0.

3. **A σ-axis Chebyshev interpolant of the *dimensionless boundary* is the single
   largest structural win, and it is not in the old plan.** For one expiry the only
   axis that varies across a smile ladder is σ. ~8 boundary solves + interpolation
   replaces ~40. New §P2.5. This is distinct from `CorrectionCache`, which
   interpolates the *price correction*, not the boundary.

4. **The "memory-bandwidth ceiling" on the position scatter does not exist.**
   `PriceFrame` is 101 bytes/position across 14 columns. The old 5M positions/s
   target is **2.5% of a 20 GB/s store budget** — ~40× headroom. The unique-result
   table is 96 B × 2,688 = 252 KiB, which is **L2-resident**, so the gather is not
   the limiter either. The scatter is bound by per-lane compute and by
   `2·(n_threads−1)` `jthread` spawns *per `price()` call*. §4.5, §P1.

5. **The AVX2 downclocking risk is wrong for our host.** Sustained license-based
   downclocking is an AVX-512 / server-Xeon phenomenon. On modern Intel *client*
   parts 256-bit AVX2 incurs **zero** license-based downclock (measured, Ice Lake
   client). Our host is an i7-1260P — a client part. §5.6.

6. **The prior "xsimd is 6.6× slower" negative result was never an AVX2
   experiment.** No translation unit in this repo receives `/arch:AVX2`; x64
   clang-cl defaults to SSE2. That experiment compared 2-lane SSE2 polynomial
   transcendentals against scalar SVML-backed libm. It does not bound what a
   4-lane AVX2 kernel can do. §5.4.

7. **`prices_only` currently saves zero bytes.** All 14 columns are `resize(n)`'d and
   the 8 Greek columns are written (as NaN). The field mask is unimplemented; a real
   one saves 64 B/position = 63.4%. §4.5.

8. **There is no published American-Greeks throughput number anywhere.** The old
   plan's ">=8k full-Greeks/s/core" had no external anchor. Targets are now derived
   from a stated cost model against two real price-throughput anchors. §1, §16.

Also corrected: the per-quadrature-node transcendental count was undercounted by
~50% (§4.1); `CorrectionCache` is trivariate, not bivariate; `pnl_explain`'s
duplicate `book_greeks` is the *following* step's, not the preceding one's.

---

## 1. Executive decision

`atx-vol` already has the right reference algorithm: Andersen–Lake–Offengenden (ALO)
spectral collocation, a memoized seven-boundary FD Greeks path, a five-boundary
analytic/PDE Greeks path, a Chebyshev American-correction surrogate, exact-bit
contract dedup, and deterministic parallel fan-out.

The throughput gap is not one instruction. It is the composition of seven issues,
each now verified:

1. the Release build compiles every TU for the x64 default **SSE2** ISA, not AVX2
   (`build-rel/compile_commands.json`: `/O2 /Ob2 /DNDEBUG -std:c++20 -MD -Z7 /W4
   /permissive- /WX` — no `/arch`, no `/fp:`, no `-mavx*`, no `-flto`, no
   `-fveclib`);
2. every public batch API is a scalar loop, and none of them price an American
   (`batch.cpp`, all six functions);
3. dividend-paying **call** Greeks cost **17 cold boundary solves**
   (`american.cpp:1524-1525` + `:1610`);
4. a **put strike ladder costs one boundary solve per strike** although the boundary
   is homogeneous in `K` (`correction.cpp:339-343`);
5. `PricedSurface` repeats a **linear** term scan, carry interpolation, `log(K/F)`
   and an IV lookup on *every* one of its five query entry points
   (`priced_surface.cpp:66-69`, `:93-177`), while `PortfolioPricer` allocates 15–20
   vectors and spawns `2·(nt−1)` threads per call
   (`portfolio_pricer.cpp:198-256`, `:35-70`);
6. the correction cache is **dropped by `PricedSurface`** (`priced_surface.hpp:5-8`,
   `session.cpp:437-467`), is baked at **one representative carry**
   (`session.cpp:130-134`), and **differentiates coefficient rows at query time**
   (`correction.cpp:185-188`), costing **20 Clenshaw sweeps per full Greeks bundle**;
7. the backtest rebuilds the position vector, the `Portfolio`, and the
   `PortfolioPricer` **every step** (`backtest.cpp:40-51,73,77,118,150,154`) and
   discards a shifted mark that is bit-identical to the next step's base mark.

The sprint builds two explicit lanes behind one API:

| Lane | Contract | Intended use |
|---|---|---|
| **Reference** | Current scalar arithmetic/order and cold ALO result; bit-stable where the API promises it | tests, replay, archive parity, model validation |
| **Production** | AVX2/AoSoA batch kernels, boundary interpolation, and an optional carry-aware surrogate; bounded error against Reference | quote refresh, live risk, portfolio and backtest throughput |

The production lane must never silently replace the reference lane. Every result
records route, math mode, ISA, and whether a surrogate was used.

### Headline answer: how fast should this be?

**There is no published American-option *Greeks* throughput number** — not from the
ALO authors, not from any vendor, not from any paper (§5.8). We therefore anchor on
price throughput and derive Greeks from an explicit cost model.

**External price anchors (verified):**

- ALO, *Journal of Computational Finance* 20(1): *"The computational throughput of
  the algorithm is close to **100,000 option prices per second per CPU**."*
  (risk.net editor's letter; no CPU model given.)
- An independent QuantLib-ALO benchmark measures **45,000 American prices/s** and
  **16,500 calibrations/s** on a single **AMD Ryzen 9** core.

**Our measured floor** (single-shot, SSE2, no warm-up — see §3): 17.1k fast-cold
prices/s/core, 1.77k accurate-cold prices/s/core on an i7-1260P.

That 17.1k vs 45k gap is a **2.6× implementation-quality gap on a comparable
algorithm, before any SIMD**, which is consistent with the audited op counts (§4.1).

**Cost model.** A boundary solve dominates; a premium quadrature is ~4% of one
(16 nodes vs 384). Write `B` for one boundary-solve-equivalent.

| Route | Today | After P2 | After P2.5 (σ-Chebyshev, per 40-strike slice) |
|---|---:|---:|---:|
| Put price | 1 B | 1 B | ~0.24 B amortized |
| Dividend-call price | 1 B | 1 B | ~0.24 B amortized |
| Put full FD Greeks | 7 B + 17 prem | 7 B | ~2 B + interp |
| Put analytic Greeks | 5 B | 5 B | ~3 B (σ± become free) |
| **Dividend-call full Greeks** | **17 B** | **7 B (FD) / 5 B (analytic)** | ~3 B |

**Targets.** Stated per core on the pinned i7-1260P, as *sustained batch* rates.
AVX2 gives 4 doubles/vector; transcendental cost, lane divergence and masking make a
realistic packed speedup 2.5–3×, not 4×.

| Operation | Measured now (SSE2, unwarmed) | Ship target | Stretch |
|---|---:|---:|---:|
| Fast cold ALO price, scalar | 17.1k/s | **35k/s** (op-count work alone) | 45k/s (match QuantLib-ALO on a faster core) |
| Fast cold ALO price, AVX2 packed | n/a | **80k/s** | 100k/s (ALO paper's stated CPU figure) |
| Accurate cold ALO price, scalar | 1.77k/s | 4k/s | 6k/s |
| `PricedSurface` cold price, mixed | 10.4k/s | **40k/s** | 75k/s |
| Full cold Greeks, mixed calls/puts | 0.94k/s | **8k/s** | 25k/s |
| Cold `pnl_explain`, mixed | 0.92k/s | **7k/s** | 20k/s |
| Cached price (carry-aware, on `PricedSurface`) | **unavailable** | 500k/s | 1M/s |
| Cached full Greeks | **unavailable** | 125k/s | 250k/s |
| 8-worker cold quote refresh | 56–58k/s | 250k/s | 400k/s |
| 8-worker cold full Greeks | 4.6–5.6k/s | 50k/s | 100k/s |
| Position scatter, prepriced uniques | 0.2–1.6M pos/s | **20M pos/s full frame; 60M/s totals-only** | store-bandwidth bound at ~100–200M/s |

The scatter targets are raised 4× from the previous draft because the old numbers
were justified by a bandwidth ceiling that does not bind (§4.5).

**Every number in the "measured now" column is provisional** and must be re-measured
under §P0 before it gates anything (§3).

---

## 2. Non-negotiable invariants

1. Reference scalar output stays bit-for-bit identical where the API promises it.
2. No global `/fp:fast`, no `-ffast-math`, no `/arch:AVX2` applied library-wide.
3. No quantization of `K`, `T`, or σ to manufacture shared boundaries.
4. Deterministic totals across worker count, via fixed input-order reduction.
5. The scalar cold oracle is never removed.
6. A profile/route is a latency prior, never permission to drop an underlier.

---

## 3. Baseline provenance — and why every number must be re-measured

### 3.1 Machine and build

| Item | Baseline |
|---|---|
| CPU | Intel Core i7-1260P, 12 physical / 16 logical, hybrid P+E, 18 MiB L3 |
| Base clock | 2.10 GHz (`Win32_Processor.MaxClockSpeed`) |
| Compiler | clang-cl 18.1.8 |
| Build | `cmake --preset rel` → `/O2 /Ob2 /DNDEBUG`, **no `/arch:AVX2`, no LTO** |
| ISA | **SSE2** (x64 default) — confirmed against `compile_commands.json` |

`hardware_concurrency()==16` does not describe 16 equal workers on a hybrid part.

### 3.2 Why the existing numbers are not gate-grade

The audit of the benchmark harness (§4.6) found:

- **Google Benchmark is not a dependency of atx-vol.** It is absent from
  `vcpkg.json`; it is an opt-in `FetchContent` behind `ATX_BUILD_BENCH` (default
  **OFF**, `CMakeLists.txt:205-218`) used only by `atx-core/bench` and
  `atx-engine/bench`. There is **no `atx-vol/bench` directory**.
- Every atx-vol "benchmark" is a hand-timed `examples/*.cpp` under
  `ATX_BUILD_EXAMPLES` (default OFF): `steady_clock`, **no warm-up, one timed pass,
  a single mean rate, no variance**. DCE is guarded by a `sink`.
- Nothing forces Release. A number is only `/O2` if someone configured the `rel`
  preset.
- **`portfolio_pricer_bench`'s "kernel floor" measures the wrong thing.** It times
  `PricedSurface::fair_value + greeks`, which is **cold ALO with a null correction
  cache** (`priced_surface.hpp:112-114`). It is not the production floor.
- **No benchmark anywhere measures the cached-correction Greeks bundle** — the
  actual `served_cache` production hot path. The only Greeks timer in the tree is
  `american_test.cpp:693-757`, which is `DISABLED_` and cold-only.

Consequently §1's "measured now" column is diagnostic, not a baseline. §P0 repairs
the harness before any performance claim becomes a gate.

### 3.3 Current solve counts (verified)

| Route | Put | Dividend-paying call |
|---|---:|---:|
| Price | 1 boundary | 1 boundary (McDonald–Schroder) |
| Full cold FD Greeks | 7 boundaries + 17 premium stencils | **17 boundaries** |
| `american_greeks_al` | 5 boundaries; θ/charm from PDE | **falls back to 17-boundary FD** |
| `pnl_explain`, analytic | 5-boundary base + 1 target price | 17 + 1 |
| Cached first-order/full bundle | 0 boundaries, but **20 Clenshaw sweeps** | same |
| **Non-dividend call (`q<=0`)** | — | **0 boundaries** (Black-76 short-circuit) |

Evidence: the put FD path memoizes on `(dσ, dr, dT)` with **spot excluded from the
key** (`american.cpp:1496-1497`), so 17 stencils collapse to a
`std::array<BndCache,7>` (`:1476`). A call has no such memo: each spot bump changes
the internal-put *strike* `Kp=S`, so every stencil is a fresh solve
(`:1524-1525`). `american_greeks_al` bails to the 17-solve FD path for any non-put
(`:1610`) and also when `r-hr <= 0` (`:1628-1631`).

**Dividend-call Greeks are the largest exact algorithmic gap in a balanced book.**

---

## 4. Verified implementation map

### 4.1 American scalar kernel — the real op count

`src/american.cpp` is a clean, small-state implementation. It is scalar and
**transcendental-bound**.

The previous draft counted the *visible body* of `eqn_b_ND` (`:594-612`): 1 `sqrt`,
1 `log`, 2 `exp`, 2 `norm_cdf` per quadrature node. It missed
`al_boundary_at(bnd, u)` (`:600`), called **once per quadrature node**, which adds
1 `sqrt` (`:526`) and — via `b_from_y` (`:482-485`) — another `sqrt` + `exp`.

**True cost per boundary-quadrature node: 3 `sqrt`, 1 `log`, 3 `exp`, 2 `norm_cdf`,
~(l+2) divisions.** The premium-quadrature node (`:722-740` + its own
`al_boundary_at` at `:730`) costs 3 `sqrt`, 3 `exp`, 1 `log`, 2 `norm_cdf`.

`norm_cdf(x) = 0.5·erfc(−x/√2)` (`atx-core/include/atx/core/math.hpp:203-205`),
scalar `std::erfc`. **No vectorizable Φ exists anywhere in the tree**; the C
library's Chebyshev-Φ path was never ported (`batch.cpp:1-14`, `correction.hpp:25-26`
both mark it "deferred").

**Scheme parameters (verified against `scheme_from_opts`, `:413-451`):**

| preset | boundary nodes *l* | FP quad *m* | premium quad *p* | JN | FP | tol |
|---|---:|---:|---:|---:|---:|---:|
| `al_fast_opts()` = `{7,16,4,1e-8}` (`:1143`) | 7 | 16 | 16 | 2 | 2 | 1e-8 |
| accurate (`nullopt`) | 12 | 24 | **48** | 2 | 4 | 1e-10 |
| `al_default_opts()` **passed explicitly** | 12 | 24 | **24** | 2 | **6** | 1e-10 |

> **Doc bug.** `american.hpp:56-58` still documents `al_fast_opts` as `{7,16,6}` /
> "2 Jacobi-Newton + 4 fixed-point sweeps". The code returns `max_newton_iter=4` →
> 2 JN + 2 FP. Fix the header. Note also that passing `al_default_opts()`
> *explicitly* silently collapses the premium quadrature from 48 to 24 nodes and
> raises FP sweeps to 6 — a different cost/accuracy point under the same name
> (`:436-440`).

**Transcendentals per boundary solve** (interior nodes = `l−1`, sweeps = JN+FP):

| | inner evals | erfc | exp | log | sqrt | div |
|---|---:|---:|---:|---:|---:|---:|
| fast (6×16×4) | 384 | 768 | 1,152 | 384 | 1,152 | ~3.5k |
| accurate (11×24×6) | 1,584 | 3,168 | 4,752 | 1,584 | 4,752 | ~22k |

Plus a BAW seed of ~380 (fast) / ~700 (accurate) heavy calls
(`al_seed_boundary`, `:99-121`, `:55-71`).

**Sanity check.** 17.1k fast prices/s ⇒ 58.5 µs ⇒ ~123k cycles at 2.1 GHz, over
~3,456 transcendental calls ⇒ **~35 cycles per transcendental**. That is exactly
where a scalar `erfc`/`exp` lands. The kernel is *at* its scalar transcendental
floor. There is no mystery overhead to find — the only ways forward are (a) fewer
transcendentals, (b) cheaper ones, (c) more per instruction.

**Existing structure worth keeping:** `AlBoundary` (~1,312 B) and `AlWorkspace`
(~304 B) are fixed `std::array` aggregates on the stack (`:390-410`, `kAlMaxNodes=32`
at `:28`). An **AoSoA<4> transform needs no heap allocation** — the arrays simply
widen to `array<double×4, 32>`.

### 4.2 What the SIMD comment actually proves

`american.cpp:585-593` records two reverted attempts: xsimd measured **~6.6× slower**,
and Intel SVML intrinsics "compile only under MSVC cl.exe, not the project's
clang-cl toolchain."

Both facts are true and both are **weaker than they look**:

- **No TU in this repo gets `/arch:AVX2`.** With clang-cl on x64 the default is
  SSE2, so `xsimd::batch<double>` is **2 lanes, not 4**. The experiment pitted
  2-lane polynomial `exp/log/erfc` against scalar libm that is itself
  SVML/CRT-backed. A 6.6× regression is unsurprising and says nothing about a
  4-lane AVX2 kernel with a real vector libm.
- xsimd is **dead in atx-vol**: it is `FetchContent`'d globally
  (`CMakeLists.txt:146-153`) and its include dir reaches atx-vol's command line, but
  the only `#include <xsimd/xsimd.hpp>` is `atx-core/include/atx/core/simd.hpp:45`.
  atx-vol's target links `atx::core` + `atx_warnings` only
  (`atx-vol/CMakeLists.txt:85-90`).
- Corollary: **`atx-core`'s own SIMD reductions are also running 2-wide.**

The profitable dimension was also wrong: the reverted attempt vectorized **quadrature
nodes within one option**. The correct axis is **four independent contracts across
lanes** (§P3.2).

### 4.3 Batch layer

`batch.hpp` exposes SoA signatures; `batch.cpp` calls the scalar kernel lane by lane.
All six functions are Black-76 / European / eSSVI — **there is no American batch API
at all**, and no ISA dispatch. Greeks output is AoS `std::span<Greeks>`
(`batch.hpp:131`).

### 4.4 Surface query layer

`PricedSurface` has **five** query entry points — `iv`, `fair_value`, `greeks`,
`greeks_analytic`, and `delta` (`priced_surface.cpp:93,111,127,145,164`) — and each
independently performs: validation → **linear** term scan → forward/carry
interpolation → `log(K/F)` → surface IV lookup.

The term scan is a linear forward walk:
`while (hi < ctx_.size() && ctx_[hi].T <= T) ++hi;` (`:66-69`).

- `PortfolioPricer::price` resolves the surface **twice** per unique contract: once
  for the standalone `iv()` (`portfolio_pricer.cpp:211`) and once inside
  `greeks*/fair_value`. Exactly one of the two is redundant. (Risk mode does *not*
  additionally call `fair_value`; the mark comes off `g->price`, `:228-233`.)
- `pnl_explain` resolves the surface **four times** per unique contract
  (`:362-370`), of which one is strictly redundant (base resolved twice at the same
  `T_b`). American solves per unique contract = base-Greeks solves **+ 1**.
- `SurfaceSet::find` is a `lower_bound` per unique contract (`:163-170`) — once in
  `price`, **twice** in `pnl_explain` (`:348-349`).
- **No expiry grouping and no bracket reuse anywhere.** Unique contracts are iterated
  in first-appearance insertion order (`:199`, `:341`).

### 4.5 Portfolio and P&L layer — roofline, corrected

**Good properties (keep):** exact-bit dedup; SoA output frames; disjoint per-contract
writes; fixed-input-order reduction; deterministic totals across worker counts;
parallel price scatter.

**The scatter is not memory-bound.** The arithmetic:

- `ContractPx` = `double fair_value` + `AmericanGreeks` (9 doubles) + `double iv` +
  `PriceStatus` → **96 B**. Unique table = 96 × 2,688 = **252 KiB** → exceeds L1
  (48 KB), **fits L2** (1.25 MB) 5× over. The "random" gather `px[contract_ix(i)]`
  (`:260`) hits **L2**, not DRAM.
- `PriceFrame` = 14 columns = `u64 id` (8) + `u32 uid` (4) + 11 × `f64` (88) +
  `u8 status` (1) = **101 B/position**.
- Store ceiling at 20 GB/s: `20e9 / 101` ≈ **198M positions/s** (≈99M charging RFO).
- The old **5M positions/s** target = 505 MB/s = **2.5% of budget** → ~40× headroom.
- L2 read ceiling for the gather ≈ 2.1×10⁹ reads/s → **~400×** over 5M/s.

**Conclusion: neither bandwidth- nor gather-bound.** The limiters are (a) a status
branch and 8 scalar multiplies fanned across 14 store streams (`:266-287`), and (b)
**`2·(n_threads−1)` `std::jthread` constructions per `price()` call** (`:199`,
`:258`) that never amortize because the pool is rebuilt every call.

Two direct consequences for the plan:

- **Raise the scatter targets** (done, §1).
- **Do not justify `PreparedPortfolio`'s stable sort by scatter locality.** Sorting
  positions by `contract_ix` buys nothing here and would cost an O(n) sort plus a
  reverse permutation while endangering the input-order reduction. Its real value is
  **expiry-bracket reuse and homogeneous SIMD packs** (§P3.2). Justify it there.

**Remaining costs (verified):**

- `OptionContract` and unique intermediates are AoS.
- Every call allocates: `price()` = **15** heap allocations single-threaded (1 `px`
  vector + 14 `resize`), rising to `17 + 2·(nt−1)`; `pnl_explain()` = **20**, rising
  to `21 + (nt−1)`.
- **`prices_only` saves nothing today.** All 14 columns are `resize(n)`'d
  (`:243-256`) and the 8 Greek columns are *written* as NaN (`:277-278`). A real
  field mask drops 8 × 8 = **64 B/position (63.4%)**, leaving 37 B.
  *(The totals are now NaN too, `:294-297` — correct, but it is a value change, not a
  bytes-touched change.)*
- `portfolio_pricer.cpp` owns a **second, private `parallel_blocks`** (`:35-70`); it
  does not include `parallel_for.hpp` even though that header exists precisely to
  stop TUs hand-copying the primitive (`parallel_for.hpp:8-9`).
- **P&L scatter is serial** (`:408-472`), unlike the price scatter.
- No totals-only or in-place API exists (`portfolio_pricer.hpp:298-305`).

### 4.6 Correction cache — quantified

- **Trivariate** Chebyshev tensor over `(k_log, T, σ)` of `(P_amer − P_euro)/F`,
  first-kind roots grid, DCT-II coefficients, three nested Clenshaw recursions
  (`correction.cpp:54-76`, `:112-162`).
  *(Doc bug: `correction.hpp:11` calls it "bivariate".)*
- **`(r, q, side)` are baked in** (`correction.cpp:281-283`), and the session bakes
  at **one representative carry** `q_rep` from the mid expiry
  (`session.cpp:130-134,147-155`).
- Production dims are `16 × 8 × 12` (`session.cpp:144-146`) = 1,536 doubles =
  **12 KB per side**, ~24 KB per session. The 64³ maximum would be 2 MB.
- **`eval_grad` differentiates coefficient rows at *query* time**
  (`correction.cpp:185-188, 202-207, 232-234`) — for the `k_log` partial that is
  `n_T·n_s = 96` `cheb_diff_coefs` calls **per query**.
- A **full cached Greeks bundle costs 20 Clenshaw sweeps** (9 `eval_grad` calls,
  `american.cpp:947-996`) plus 3 `black76_greeks`. **Four of the nine calls
  (`:981,982,988,989`) need only `ds` but always run a discarded value sweep**
  (`correction.cpp:456-457`) — 4 free sweeps to reclaim immediately.
- Each `eval`/`eval_grad` stack-allocates `std::array<double,4096>` = **32 KB** of
  which only 96 doubles are live (`correction.cpp:410,453`), plus two zero-init
  64-double scratch arrays per call.
- `served_cache()` returns `nullptr` whenever a curve override is set
  (`session.hpp:392-398`), and **`PricedSurface` carries no cache at all**
  (`priced_surface.hpp:5-8,33-40,157-159`; `session.cpp:437-467` drops it).

**Consequence:** every `PortfolioPricer` and backtest query runs **cold ALO**. The
cheap cached route the live eSSVI session uses is unreachable from the serialized
surface — the canonical portfolio/backtest artifact.

### 4.7 Backtest layer

`backtest.cpp` rebuilds `std::vector<Position>` (`:40-51`, `:118`), `Portfolio`
(`:73`, `:150`) and `PortfolioPricer` (`:77`, `:154`) **every step**; the hedge
overlay rebuilds a third time per uid (`:643-657`).

The shifted mark at step *i* — `st->fair_value(K, T_t, side)` (`:364`) — is
**bit-identical** to step *i+1*'s base mark (`:376`, same surface/K/side/T after
`base = std::move(shifted)` at `:393`) and is discarded; `compute_step` returns only
`StepPnl` (`:105-114,175`).

> **Correction to the previous draft.** The duplicated `book_greeks` is not "the base
> Greeks the preceding P&L explain just computed". `book_greeks` runs *after* the
> move-swap, on the new base (`:400`, `:808`), so date-*i* base Greeks are computed
> twice: once as row *i*'s `book_greeks`, once as step *(i+1)*'s `pnl_explain` base.
> Two caveats: `book_greeks` reprices the **full surviving book** while `pnl_explain`
> reprices only the **alive** subset (`:121-144`), so the overlap is their
> intersection; and inception row 0 (`:356`) is duplicated by step 1.

---

## 5. Research findings and implications

Only primary papers and official toolchain documentation are used. Items I could not
read directly are listed in §16.

**5.1 ALO is the right reference algorithm — keep it.**
Andersen, Lake & Offengenden, *High Performance American Option Pricing*, SSRN
2547027; *J. Computational Finance* 20(1), Sep 2016, 39–87.
<https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027>
Jacobi–Newton boundary iteration + Gauss–Legendre quadrature + Chebyshev
interpolation of the transformed boundary. Throughput *"close to 100,000 option
prices per second per CPU"* (risk.net JCF editor's letter); *"10 or 11 significant
digits in less than one-tenth of a second."* **The paper reports no Greeks
throughput.** The scheme tuples exposed by QuantLib's `QdFpAmericanEngine`
(fast `(l,n,m,p)=(7,2,7,27)`; accurate `(25,5,13,1e-8)`; high-precision
`(1e-10,10,30,1e-10)`) are a useful cross-check on our own `(l,m,p)` choices.

**5.2 The exercise boundary is homogeneous of degree one in strike.**
Merton (1973) establishes degree-one homogeneity of the option price in `(S,K)`;
Healy (2021, <https://arxiv.org/abs/2109.15157>, Eq. 22) writes the boundary fixed
point explicitly as `S⋆ᵢ = K · N(tᵢ,·)/D(tᵢ,·)` — proportional to `K`, with a
dimensionless ratio. Our code already *is* homogeneous (`al_xmax_put` ∝ K,
`american.cpp:453-473`; `eqn_b_ND` uses only ratios `b/K`, `b/bu`, `:575-576,604`) —
it simply never exploits it for puts. **This licenses §P2.0 and §P2.5.**

**5.3 Put–call symmetry (McDonald–Schroder).**
McDonald & Schroder (1998), *A Parity Result for American Options*, JCF 1(1) 5–13:
*"the price of a call option with underlying asset price S, strike price K, interest
rate r, and dividend yield d is equal to the price of an otherwise identical put
option with asset price K, strike price S, interest rate d, and dividend yield r.
The result is true for both European and American options."* Schroder (1999), RFS
12(5) 1143–1163, derives the same via change of numeraire.
`andersen_lake_call_slice` already relies on this. **This licenses §P2.1.**

**5.4 The build has no AVX2, and that invalidates the prior SIMD verdict.**
Microsoft `/arch (x64)`: *"The default instruction set is SSE2 if no /arch option is
specified"*; `/arch:AVX2` enables AVX2 **and FMA**.
<https://learn.microsoft.com/en-us/cpp/build/reference/arch-x64>
clang-cl accepts `/arch:AVX2` and maps it to `-march=haswell`. Microsoft's own doc
recommends `__cpuid` dispatch rather than a global flag
(<https://learn.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex>). Our
`compile_commands.json` contains zero `/arch`, `/fp:`, `-mavx*`, `-flto`,
`-fveclib`, `-fno-math-errno`. **Therefore the "xsimd 6.6× slower" result was
measured at SSE2 (2-lane) and does not bound an AVX2 kernel.** Re-run the bakeoff
under `/arch:AVX2` on an isolated TU before concluding anything.

**5.5 Vector math is a bakeoff, not a foregone conclusion.**
SLEEF (Shibata & Petrogalli, *IEEE TPDS* 31(6), 2020;
<https://arxiv.org/abs/2001.09258>) provides 1-ULP and 3.5-ULP variants and — crucially
— **AVX2 double-precision `erf`/`erfc`**: `Sleef_erfd4_u10avx2` (1.0 ULP) and
`Sleef_erfcd4_u15avx2` (1.5 ULP below |x|=26.2). But its performance claim is
*parity*, not a multiple: *"the performance of our library is comparable to that of
Intel SVML"*; §7.2 reports comparable reciprocal throughput. **Do not write "SLEEF
is N× faster than libm."**
clang's `-fveclib` accepts `{Accelerate, libmvec, MASSV, SVML, SLEEF,
Darwin_libsystem_m, ArmPL, AMDLIBM}` and vectorizing math calls **requires
`-fno-math-errno`** (<https://llvm.org/docs/Vectorizers.html>).
Agner Fog's VCL inlines `exp/log/sqrt/pow` on `Vec4d` at typically <2 ULP but
**does not inline `erf`/`erfc`/`cdfnorm`** — those route to SVML. VCL alone cannot
supply our Φ.
Hand-rolled rational Φ remains competitive: Cody (1969), *Math. Comp.* 23:631–637,
reports maximal relative errors down to 6×10⁻¹⁹–3×10⁻²⁰.
*(Correction: West (2005), Wilmott 70–76, uses **Hart's Algorithm 5666**, not Cody's,
and claims only "accurate to double precision throughout the real line" — no ULP
figure.)*

**5.6 AVX2 does NOT downclock modern Intel client parts — the old risk was wrong.**
The Cloudflare result (<https://blog.cloudflare.com/on-the-dangers-of-intels-frequency-scaling/>)
is an **AVX-512** effect on a **Xeon Silver 4116** server part. Travis Downs measured
an **Ice Lake client** i5-1035G4: *"for heavy 256-bit instructions, there is **zero**
license-based downclocking"*
(<https://travisdowns.github.io/blog/2020/08/19/icl-avx512-freq.html>). Our i7-1260P is
an Alder Lake **client** part. **Demote this from a design risk to a routine
sustained-thermal check.** (Retain the check: heavy 256-bit work still raises power.)

**5.7 One-solve Greeks via implicit differentiation is sound — and unpublished.**
For a converged `R(y; θ) = 0`, the implicit function theorem gives
`∂y/∂θ = −(∂R/∂y)⁻¹ (∂R/∂θ)`; you differentiate the *converged point*, never the
solver iterations. General basis: Margossian & Betancourt, *Efficient Automatic
Differentiation of Implicit Functions*, <https://arxiv.org/abs/2112.14217>
(companion: Blondel et al., <https://arxiv.org/abs/2105.15183>). Adjoint cost bound:
Giles & Glasserman, *Smoking adjoints*, Risk, Jan 2006 — reverse mode costs at worst
~4× the primal **regardless of the number of sensitivities**.
**No published work differentiates the ALO collocation residual for American
Greeks.** The nearest is in 't Hout (2024), <https://arxiv.org/abs/2401.13361>, which
takes Greeks by finite differences on the PDE complementarity problem. Treat §P2.4 as
genuine research with a hard kill rule, not as reimplementation.

**5.8 No published American-Greeks throughput exists.**
Not in ALO, not from Numerix/FINCAD/MatLogica, not in any paper found. The only
credible comparators are price throughput: ALO's ~100k/s/CPU and an independent
QuantLib-ALO measurement of **45,000 American prices/s + 16,500 calibrations/s on a
single AMD Ryzen 9 core** (<https://tastyhedge.com/blog/how-to-calibrate-american-options-really-fast/>).
**Anchor Greeks targets to a cost model against these, and say so** (§1).

**5.9 Chebyshev in the parametric direction — the boundary is the unexploited axis.**
Gass, Glau, Mahlstedt & Mair, *Chebyshev interpolation for parametric option
pricing*, Finance & Stochastics 22(3); <https://arxiv.org/abs/1505.04648> — proves
*"(sub)exponential convergence and explicit error bounds"* for interpolating the
**price** across parameters. Glau, Mahlstedt & Pötz, *Dynamic Chebyshev*, SIAM J. Sci.
Comput. 41(1); <https://arxiv.org/abs/1806.05579> — American via Chebyshev on the
**value function**, "exponential [convergence] if there is a single varying
parameter", and confirms fast convergence of *"prices and sensitivities"*.
Chebyshev on the **boundary** is what ALO already does *in τ*. **Chebyshev on the
boundary in σ (offline, per (τ, r, q)) appears to be unpublished** — and it is
exactly what a smile ladder needs (§P2.5).

**5.10 QD+ is a better seed than BAW.**
ALO seeds its boundary with **QD+** solved by Halley's method — Li (2010),
*Analytical approximations for the critical stock prices of American options*,
Rev. Derivatives Research 13(1):75–99 (SSRN 1482409). We seed with BAW
(`al_seed_boundary`, `american.cpp:99-121`), costing ~380 (fast) / ~700 (accurate)
heavy calls. A better seed reduces sweep count directly. New §P2.2b.

**5.11 Negative rates break the short-circuit *and* can create two boundaries.**
Exact never-exercise regions (Healy 2021, §2.2): **call ⇔ `q ≤ 0 ∧ q ≤ r`**;
**put ⇔ `r ≤ 0 ∧ r ≤ q`**. When `r<0`, a *double continuation region* can appear —
Battauz, De Donno & Sbuelz (2015), *Real Options and American Derivatives: The Double
Continuation Region*, Management Science 61(5):1094–1107; numerics in Andersen &
Lake (2021), *Fast American Option Pricing: The Double-Boundary Case*, Wilmott,
DOI 10.1002/wilm.10969 (two boundaries when `r<q<0` or `q<r<0`; *"the two boundaries
may be solved independently"*). **See §P0.5 — our code's guard is `q<=0` alone.**

**5.12 Ruled out, with reasons.**
- **COS method** (Fang & Oosterlee, *Numer. Math.* 114, DOI 10.1007/s00211-009-0252-4)
  requires a characteristic function; it shines for Lévy/Heston and is **not** the
  natural tool for vanilla Black–Scholes American, where ALO dominates.
- **Leisen–Reimer** trees (Applied Math. Finance 3(4):319–346) achieve order-2
  convergence — excellent as an independent **oracle**, not as a hot path.
- **GPU.** Published ~150× American speedups are for **Monte-Carlo / binomial
  path-based** methods. **No published GPU-vs-CPU crossover exists for semi-analytic
  ALO.** The natural parallelism here is batched fixed-point iteration across
  contracts — i.e. AVX2. GPU stays a non-goal.

**5.13 Benchmarking and codegen tooling.**
Google Benchmark supports warm-up, repetitions, median/CV, JSON, counters
(<https://google.github.io/benchmark/user_guide.html>). clang-cl accepts
`/clang:-Rpass=loop-vectorize`, `-Rpass-missed`, `-Rpass-analysis`, and
`-fsave-optimization-record` (YAML). Windows CPU Set APIs expose hybrid topology
(<https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemcpusetinformation>).

---

## 6. Target architecture

```text
positions / lots
      |
      v
PreparedPortfolio (stable keys, aligned SoA, groups, scatter permutation)
      |                       [grouping justified by bracket reuse + SIMD packs,
      |                        NOT by scatter locality]
      +--> resolve SurfaceSet once per uid group
      |
      v
ResolvedQueryBatch (F, q, k, sigma resolved ONCE per (uid, expiry) group)
      |
      +--------------------- Reference lane ---------------------+
      |   scalar ALO, exact route, scalar tail/fallback           |
      |                                                           |
      +-------------------- Production lane ----------------------+
      |   AVX2 AoSoA<4> ALO across independent contracts          |
      |   + per-(tau,r,q) sigma-Chebyshev boundary interpolant    |
      |   + carry-aware CorrectionCacheV2 (fused Clenshaw)        |
      v
UniqueResultSoA (price + selected Greeks + status + route)
      |
      +--> deterministic fixed-order totals
      +--> parallel selected-column scatter (field-masked)
      +--> retained StepState for the next P&L date
```

The execution plan is built once per stable book. Snapshot values resolve into
reusable workspace. Public convenience APIs remain allocating wrappers over in-place
APIs.

---

## 7. Work packages

### P0 — Make performance measurable, and fix the correctness bug we found

**Estimate:** 2.5 engineer-days · **Risk:** low · **Ship condition:** required before
accepting any later speedup

**P0.1 — A real benchmark harness.** Google Benchmark is not an atx-vol dependency.
Add it (`vcpkg.json` or the existing `ATX_BUILD_BENCH` FetchContent), create
`atx-vol/bench/`, and add `american_pricing_bench.cpp` +
`portfolio_throughput_bench.cpp`. Keep the existing examples for human demos.

Axes: route (accurate cold / fast cold / cached / European short-circuit); side
(put / dividend call / no-dividend call); moneyness {0.60, 0.80, 0.95, 1.00, 1.05,
1.20, 1.50}; maturity {1d, 1w, 1m, 6m, 2y}; vol {5%, 15%, 30%, 75%, 150%}; API (raw
price, delta-only, FD Greeks, analytic Greeks, surface query, portfolio price-only,
full price, P&L explain); batch size {1, 4, 16, 256, 4096, real OPRA board};
position:unique ratio {1:1, 10:1, 100:1, 1000:1}; worker schedule {1, 2, 4, 8,
P-only, E-only, all}.

≥0.5 s warm-up, ≥5 randomly interleaved repetitions, JSON out. Report median, p95/p99,
CV, contracts/s, positions/s, bytes/s.

**Two harness bugs to fix while here:** the "kernel floor" must benchmark the same
fused operation as the API under test (today it times a cold null-cache
`PricedSurface`), and **a cached-Greeks benchmark must exist** — nothing currently
measures the production `served_cache` path.

**P0.2 — Algorithm counters** (instrumented build): boundary solves; JN/FP sweeps;
early residual exits; premium evaluations; `norm_cdf`/`log`/`exp` calls; scalar
fallback lanes; cache hits / out-of-box clamps / cold fallbacks; frame allocations
and bytes; worker launches. Capture `/clang:-Rpass=loop-vectorize`,
`-Rpass-missed`, `-fsave-optimization-record`; add a VTune/ETW runbook for cycles,
instructions, branch misses, L1/L2/LLC misses.

**P0.3 — Checked-in baselines.** `bench/baselines/i7-1260p-clang18-sse2.json`, plus
an AVX2 baseline once P3.1 lands. A comparison script that fails only on a
statistically significant >10% regression with CV ≤5%. Gate *ratios* everywhere;
gate absolutes only on one pinned host.

**P0.4 — Fix two stale doc bugs.** `american.hpp:56-58` (`al_fast_opts` is
`{7,16,4}` → 2 JN + 2 FP, not `{7,16,6}` → 4 FP) and `correction.hpp:11`
("bivariate" → trivariate). Also document that passing `al_default_opts()`
explicitly changes the premium quadrature from 48 → 24 nodes and FP sweeps 4 → 6.

**P0.5 — CORRECTNESS: repair the early-exercise short-circuit.**
`american.cpp:1165-1168` returns a European price for any call with `q<=0`. The exact
condition is `q<=0 ∧ q<=r`; the symmetric put guard at `:1169` uses `r<=0` where the
exact condition is `r<=0 ∧ r<=q`. Under `r<0` this **under-prices American calls**.
The whole ALO path assumes an internal-put rate `> 0` (`al_solve_put` itself returns
European at `r<=0`, `:906-911`), so the fix has two parts:

1. tighten both guards to the exact regions (§5.11) — cheap, and it *removes* a
   silent mispricing;
2. for the `r<0` region now exposed, either return `Unsupported` explicitly, or
   implement the Andersen–Lake (2021) double-boundary scheme. **Decide by policy:
   does the book see negative rates?** Returning a wrong number silently is not an
   option.

Add a rate/yield corner grid to the test suite: `{r,q} ∈ {−2%, −0.5%, 0, +0.5%, +5%}²`,
compared against a PDE/Leisen–Reimer oracle.

**P0 acceptance:** existing output values unchanged (except the `r<0` corner, which
becomes correct or explicitly unsupported); CV ≤5% single-core, ≤10% all-core; the
harness distinguishes scalar latency from batch effective ns/option; the cached path
is measurable.

---

### P1 — Exact structural wins in surface, portfolio, and P&L plumbing

**Estimate:** 4 engineer-days · **Risk:** low–medium
**Expected gain:** 1.2–2× small books; **5–20×** at high position:unique ratios
(revised up: the ceiling is ~40× away, not 2×)

**P1.1 — One fused surface evaluation.** Resolve the T-bracket, `F/q`, `log(K/F)`,
curve slice and σ **once**, then let price and Greeks consume the resolved point.

```cpp
enum class EvalField : uint32_t {
  Iv = 1u<<0, Price = 1u<<1, FirstOrder = 1u<<2, SecondOrder = 1u<<3
};
struct ResolvedSurfacePoint { double K, T, F, q, k, sigma, df; const PricedSurface* surface; };

Status PricedSurface::evaluate_batch(std::span<const double> K, std::span<const double> T,
                                     std::span<const Side> side, EvalField fields,
                                     EvaluationSoA out, EvaluationWorkspace& ws) const;
```

Also: replace the **linear** term scan (`priced_surface.cpp:66-69`) with a binary
search, and resolve carry + both curve slices **once per expiry group** for a whole
strike ladder. `PortfolioPricer` must stop calling `iv()` separately from
`greeks*/fair_value` (kills 1 of 2 resolutions in `price`, 1 of 4 in `pnl_explain`).

**P1.2 — `PreparedPortfolio`.** Retain public input order and exact-bit dedup. Build
32/64-B-aligned SoA columns for `K, T, side, uid, weight, original_unique_index`.
Stable-sort an execution permutation by `(uid, T-bracket, side, method, AlOpts,
route)`; emit `ContractGroup{begin,end,uid,side,scheme}`; retain the reverse
permutation for deterministic output; resolve one surface pointer per uid group per
snapshot.

Group only on **equal execution metadata** — never quantize `K`, `T`, or σ.

> **Justify this by bracket reuse and SIMD pack homogeneity, not by scatter
> locality.** §4.5 shows the scatter has ~40× store headroom and an L2-resident
> gather; sorting for locality alone would be cost without benefit.

**P1.3 — Workspace, in-place APIs, and a *real* field mask.**
Add `PortfolioWorkspace::reserve(n_unique, n_positions)`;
`price_into(..., PriceFrameView, ws)`; `pnl_explain_into(..., PnlFrameView, ws)`;
`price_totals(...)` / `pnl_totals(...)` with no row-frame allocation; a field mask so
`prices_only` **allocates and writes only** `id/uid/price/pv/iv/status` (37 B) rather
than all 14 columns (101 B) — a 63.4% cut in touched bytes; reusable aligned
`UniqueResultSoA` scratch. Existing returning APIs become wrappers.

**P1.4 — One persistent pricing executor.** Delete the TU-local `parallel_blocks`
(`portfolio_pricer.cpp:35-70`) and the per-call `jthread` construction. Requirements:
deterministic disjoint chunk ownership; fixed-order reduction after the barrier; **no
allocation and no thread creation in steady state**; topology policy
`{Auto, PerformanceCores, AllCores, ExplicitCpuSet}`; a nested-parallelism guard so
the fit pool and price pool cannot oversubscribe each other. Execute inline below a
measured `n_unique` threshold — determine it from P0, not by guess.

**P1.5 — Parallelize the P&L scatter** (mirror the price scatter) and separate the
reduction. Totals-only requests skip scatter entirely.

**P1 acceptance:** Reference output bit-for-bit equal on all existing price and P&L
columns; `prices_only` returns identical IV/price/PV and touches 37 B/position; zero
heap allocations and zero thread creation after warm-up; 100k positions / 2,688
uniques ≥ **20M positions/s** full-frame and ≥ **60M/s** totals-only with uniques
prepriced; sanitizer configs green.

**Files:** `priced_surface.*`, `portfolio_pricer.*`, `parallel_for.hpp`, new
`pricing_executor.*`, portfolio/P&L tests and benches.

---

### P2 — Remove unnecessary cold boundary solves

**Estimate:** 6 engineer-days + a 2-day time-boxed spike
**Risk:** medium (symmetry, boundary interpolation); high (implicit differentiation)
**Expected gain:** **3–5×** on balanced full-Greeks books *before* SIMD

**P2.0 — Give puts a strike-slice API (NEW).**
The boundary is homogeneous of degree one in `K` (§5.2), yet a put ladder does one
solve per strike while `andersen_lake_call_slice` does one per slice. Add
`andersen_lake_put_slice(S, strikes, T, sigma, r, q, out)`: one dimensionless
boundary `b(τ)`, rescaled per strike, then a per-strike premium quadrature.

Immediate beneficiary: `correction.cpp:339-343`, where the cache builder's **put rows
currently fall back to a scalar per-node solve** while call rows already use the
slice. Constant-σ ladders drop from *N* solves to 1.

**P2.1 — Give calls the same boundary reuse as puts.**
Under McDonald–Schroder (§5.3) `C(S,K,r,q) = P(K,S,q,r)`. Represent a solved boundary
in dimensionless form (`b/K`, or the existing `y` coordinate plus normalized
metadata) and rescale for original-spot stencils. Implement:

- `SolvedPutBoundary` as a reusable internal value;
- solve/evaluate APIs over dimensionless boundaries;
- call FD Greeks over **7 unique `(σ, r, T)` boundary states instead of 17 scalar
  solves**;
- call analytic/PDE Greeks over base, σ±, r± — matching the five-solve put route,
  removing the `american.cpp:1610` fallback;
- direct call chain-rule for δ/γ/θ/charm under the symmetry map;
- scalar fallback for collapse, exercise-boundary proximity, and unsupported
  rate/yield corners.

This is higher priority than micro-optimizing a put that already runs on five solves.

**P2.2 — Hoist invariant scalar work.** Per §4.1, `al_boundary_at` is re-evaluated at
**every quadrature node**, costing 2 `sqrt` + 1 `exp` per node (a third of the
per-node transcendental budget). The quadrature abscissae are fixed per scheme, so
the boundary can be evaluated at all `m` nodes **once per sweep** instead of once per
node. Also: bind Gauss–Legendre tables once per scheme; precompute node transforms
and half-τ factors; specialize the fixed `7×16×4` and `12×24/48×6` schemes with
compiler-visible trip counts; keep summation order unchanged in Reference mode.

Expected: ~1.3–1.4× scalar, from op count alone.

**P2.2b — Replace the BAW seed with QD+ (NEW).** ALO seeds with QD+ / Halley
(§5.10); we use BAW, costing ~380 (fast) / ~700 (accurate) heavy calls before the
first sweep. Ship only if it reduces median sweep count; measure sweeps, not wall
time, first.

**P2.3 — Warm-start across time.** Store the last converged **dimensionless** boundary
keyed by `(uid, K, expiry, side, scheme)` in `PortfolioPricingState`. Next snapshot:
remap to the new residual T, seed base and bump solves, exit as soon as the true
residual meets tolerance, cold-reseed after a move guard or a failing residual trend.
Record warm hit, sweep count, fallback. The measured warm/cold `AloPricer` gap is
only ~1.04× today, so **this ships only if temporal coherence actually cuts sweeps.**

**P2.4 — Time-boxed spike: implicit boundary differentiation.**
Converged collocation satisfies `R(y; θ)=0`, `θ=(σ,r,T)`. Freezing `y` is wrong.
Instead: expose a pure residual evaluator; form `J=∂R/∂y` and `R_θ` (forward AD or
stable central checks); solve `J·y_θ = −R_θ` by dense pivoted LU (`n ≤ 12`);
propagate `y_θ` through the premium quadrature for vega/rho/theta; derive
vanna/charm and, if stable, second derivatives for volga; validate near the boundary
where derivatives may be non-smooth.

**No published work does this for ALO (§5.7).** Treat as research.
**Ship rule:** production only if the full OPRA/corner grid meets the §9 gates *and*
costs ≤ 1.8 boundary-equivalents. Otherwise keep as an experiment and ship the
five-solve route. **A frozen-boundary approximation is never accepted.**

**P2.5 — σ-axis Chebyshev interpolation of the dimensionless boundary (NEW — the
largest structural win).**

For one expiry and one `(r, q)`, `b(τ; σ)` is smooth in σ. Across a smile ladder σ is
the *only* varying axis (§5.2). So:

1. Choose a Chebyshev grid of `n_σ ≈ 6–10` nodes spanning the slice's σ range.
2. Solve the dimensionless boundary at each node (`n_σ` solves, reusable across all
   strikes and both sides via symmetry).
3. Interpolate `b(τ; σ_k)` for each strike's σ; run only the cheap premium quadrature
   per strike.

**Cost.** A 40-strike slice today: 40 boundary solves. After: `n_σ = 8` solves +
40 premium quadratures ≈ `8 + 40×0.04` ≈ **9.6 solve-equivalents → ~4.2×** on a full
board price. Greeks compound: σ± bumps become *interpolant evaluations*, so the
analytic put route's 5 boundary states collapse toward 3 (base + r±), and an
optional r-axis (2 more nodes) amortizes those away too.

**Why this is defensible.** Chebyshev interpolation in a smooth parameter converges
(sub)exponentially with explicit error bounds (Gass et al., §5.9), and Dynamic
Chebyshev demonstrates the value function *and its sensitivities* converge fast.
ALO already Chebyshev-interpolates the boundary **in τ**; this adds the σ direction.
No published work does exactly this — so it must be **gated by measurement, not
assumed**.

**Risks and gates.**
- `b(τ; σ)` loses smoothness as `σ → 0` and near expiry. Clamp the interpolation box;
  fall back to a direct solve outside it, and report `route=ColdFallback`.
- The interpolant is per `(τ, r, q)`; a board with per-expiry `q_eff` needs one per
  expiry — cheap, since `n_σ` solves are shared by the whole ladder.
- Error gate: per-strike boundary error must translate to ≤ $0.001 price error and
  meet the §9.2 Greek gates, verified against cold ALO on the real OPRA corpus.
- Ship rule: ≥2.5× on a real SPY board with all §9 gates green, else keep behind a
  flag.

**P2 acceptance:** dividend-call price and all Greeks pass a
symmetry/PDE reference grid over rates, yields, wings, near-expiry and exercise-region
points; the five-solve call path is ≤0.1 tick from cold FD; mixed-book full Greeks
≥3× P0 single-thread before SIMD; `andersen_lake_put_slice` is bit-identical to
per-strike `andersen_lake` at constant σ; P2.5 meets its ship rule or is disabled;
warm state cuts median sweeps ≥40% or is left off; implicit differentiation ships or
is killed **with evidence**.

**Files:** `american.cpp/.hpp`, new internal `american_boundary.hpp`, new
`boundary_interp.hpp/.cpp`, `american_test.cpp`, PDE/Leisen–Reimer oracle support,
portfolio state plumbing.

---

### P3 — AVX2 across independent contracts

**Estimate:** 6 engineer-days · **Risk:** medium
**Expected gain:** 2.5–3× per core over P2 on homogeneous batches

(Risk downgraded from medium-high: the frequency objection is void on client parts,
§5.6, and the prior negative result was never an AVX2 measurement, §4.2/§5.4.)

**P3.1 — Safe ISA multiversioning.** Separate objects: `american_kernel_scalar.cpp`
(baseline ISA) and `american_kernel_avx2.cpp` (`/arch:AVX2`, FMA). Optional future
AVX-512 object, not required. Dispatch once per process via `__cpuid` + OS AVX state
check. Expose the selected ISA in benchmark metadata and result diagnostics; tests
must be able to force Scalar or AVX2. **Never compile the whole library
`/arch:AVX2`** — archive readers and fallback paths must still run on SSE2-era x64.

*(Also worth a look: `atx-core`'s existing xsimd reductions are compiling 2-wide for
the same reason. Out of scope here, but note it.)*

**P3.2 — AoSoA<4> boundary state.** Vectorize **four options**, not four quadrature
nodes of one option:

```cpp
template<class V> struct AlBoundaryPack {
  std::array<V, kAlMaxNodes> y, tau, z, wbary;
  V T, K, sigma, r, q, xmax;
  mask_type active;
};
```

`AlBoundary`/`AlWorkspace` are already fixed `std::array` stack aggregates
(§4.1/F), so the pack needs **no heap allocation** (~5.2 KB / ~1.2 KB per 4-lane
block). Each lane preserves its own quadrature reduction order, which makes a
bit-stable arithmetic mode possible except for vector transcendentals. Group packs by
side/scheme/route (this is what `PreparedPortfolio`'s grouping is *for*). Maintain an
active mask across sweeps; compact only between packs, never reorder public outputs.
Degenerate, no-early-exercise, outlier and failed lanes patch through the scalar
reference path.

**P3.3 — Vector-math bakeoff.** Benchmark four candidates **inside the real boundary
kernel**, under `/arch:AVX2`:

1. scalar `std::erfc/log/exp` baseline;
2. SLEEF AVX2 (`Sleef_erfcd4_u15avx2`, `Sleef_expd4_u10avx2`, `Sleef_logd4_u10avx2`);
3. clang `-fveclib=SLEEF` auto-vectorized calls (**requires `-fno-math-errno`**);
4. a specialized rational Φ (Cody 1969 / Hart 5666) computed **directly**, not via
   `erfc`, with scalar tail patching.

Note the kernel needs `Φ(d±)`, so routing through `erfc(−x/√2)` pays an extra scale;
a direct Φ is a real candidate. Agner Fog's VCL is **not** sufficient alone (no
inline `erfc`). SLEEF's claim is *parity with SVML*, not a multiple over libm — the
win must come from **4 lanes**, not from the scalar function being faster.

Selection on end-to-end price/Greeks throughput **and** accuracy, never isolated
ns/call. The xsimd 6.6× regression stays a documented negative baseline — with the
caveat that it was measured at SSE2.

Two math modes: `Reference` (scalar libm, current ordering) and `FastDeterministic`
(one fixed approximation/ISA, deterministic for that ISA, bounded against Reference).
Never enable global `/fp:fast`. Apply minimum function/TU flags proven safe; retain
NaN, signed-zero and error handling at API boundaries.

**P3.4 — Real American batch APIs.**

```cpp
Status american_price_batch(const AmericanBatchInput&, PriceBatchOutput&,
                            PricingKernel&, PricingWorkspace&);
Status american_greeks_batch(const AmericanBatchInput&, GreekFieldMask,
                             GreeksBatchSoA&, PricingKernel&, PricingWorkspace&);
```

Per-lane status and route. Also vectorize the existing Black-76 price / value+vega /
Greeks batches. Make **SoA the preferred Greeks output**; keep the AoS
`std::span<Greeks>` wrapper for compatibility. Integrate through `PreparedPortfolio`
groups. **A scalar loop around the new API is not an accepted implementation.**

**P3.5 — Inspect codegen.** For every AVX2 hot function retain: an optimization-record
check showing vectorization; a disassembly smoke test or documented inspection
command; proof of **no scalar `erfc` inside the packed loop**; cycles/option,
instructions/option, and scalar-fallback rate.

**P3 acceptance:** scalar Reference bit-for-bit unchanged; AVX2 price max error
≤1e-8 USD on normal-domain points and ≤0.001 USD on the full stress grid, with larger
lanes falling back; §9 gates pass; homogeneous batch speedup ≥2.0× vs P2 scalar
(target ≥2.5×); AVX2 never regresses batches < 4 by more than 5% (dispatcher uses
scalar below the measured crossover); no illegal instruction on forced-scalar CI.

---

### P4 — Carry-aware cached price and analytic cached Greeks

**Estimate:** 6 engineer-days · **Risk:** high, largest production payoff
**Expected gain:** 10–100× over cold full Greeks after amortization

**P4.1 — `CorrectionCacheV2` for the actual term carry.** The fixed-`q` cache is not
adequate for dense/high-accuracy surfaces (which is *why* `served_cache()` returns
`nullptr` under a curve override, `session.hpp:383-398`). Build the 3-D samples with
the surface's actual `q_eff(T_node)` rather than one representative `q`, yielding
`C(k, T, σ; r, q_eff(T))`.

Risk semantics — **do not take shortcuts here**:

- do **not** use the composed table's total `T`-derivative as model theta; derive
  theta from the continuation PDE, matching the cold analytic route;
- derive charm from delta/gamma/speed and the PDE;
- build small `r±` correction tensors (or an explicit `r`-derivative tensor) so rho
  includes early-exercise-rate sensitivity;
- hold `q` fixed at the locally resolved carry, per the current Greek contract.

Compare against a small explicit `q`-axis cache; pick the smallest representation that
meets every accuracy gate; record memory and build time.

**P4.2 — Precompute derivative coefficients.** `eval_grad` differentiates coefficient
rows **per query** (`correction.cpp:185-188,202-207,232-234`) — 96 `cheb_diff_coefs`
calls for the `k` partial alone. Generate, at build/load time, tensors for
`C`, `C_k`, `C_kk`, `C_σ`, `C_kσ`, `C_σσ`, `C_r`. Clamp semantics must return coherent
zero partials at out-of-box axes.

**Free win to take first:** four of the nine `eval_grad` calls in
`american_greeks_first_order` (`american.cpp:981,982,988,989`) need only `ds` but
always run a discarded value sweep (`correction.cpp:456-457`). **20 sweeps → 16**
with a `value_needed` flag, before any restructuring.

**P4.3 — Fused multi-output Clenshaw.** Lay derivative channels out as AoSoA and
evaluate four channels at once with AVX2, sharing coordinates and loop control — this
SIMD dimension uses only FMA/adds and is independent of the vector libm. Provide
value-only, first-order, full-second-order, and four-options-at-once entry points.

**Right-size the scratch:** today every `eval`/`eval_grad` stack-allocates
`std::array<double,4096>` = 32 KB with 96 doubles live (`correction.cpp:410,453`).
Use live-sized reusable scratch. Benchmark global Clenshaw against a local tensor
spline / low-rank representation only if the global pass misses the 2 µs value /
8 µs full-Greeks targets.

**P4.4 — Attach caches to `PricedSurface` and archives.** Optional derived-cache
payload carrying version, dims, bounds, side, `r`/carry fingerprint, scheme, math
mode, coefficient CRC, source-surface fingerprint. Lazy load or bounded LRU;
cold fallback on missing/mismatched/failed cache; archive size and load-time metrics.
Produce asynchronously **after** the fit — it is amortized work and must not inflate
the latency-sensitive fit SLA. Allow `ColdUntilReady` with atomic publication of the
immutable cache.

Policy: `use cache iff expected_remaining_queries × (cold_cost − cached_cost) >
cache_build_cost + load/memory budget`.

**P4.5 — Self-consistency is not accuracy.** A cache can invert and reprice its own
approximation perfectly while being wrong versus cold ALO. Acceptance compares
independently against cold Reference **and** the PDE/Leisen–Reimer oracles, reporting
error by price, vega, moneyness, maturity, carry distance and exercise proximity.
**Never score only percent-within-bid/ask.**

**P4 acceptance:** §9 gates pass for every enabled cache cell; out-of-gate cells use
cold ALO and expose `route=ColdFallback`; cached value ≥500k contracts/s/core and
cached full Greeks ≥125k/s/core; build time, bytes/surface, load time and break-even
query count printed into benchmark JSON; archive round-trip preserves the cache
fingerprint and the Reference surface result; active-cache memory has a hard cap and
deterministic eviction.

---

### P5 — Stateful portfolio and P&L explain

**Estimate:** 4 engineer-days · **Risk:** medium
**Expected gain:** ~2× backtest-step compute before kernel gains; a much larger
allocation reduction

**P5.1 — Stable contract identity.** Key a listed option by
`(uid, K, expiry_ts, side)`, never by residual floating-point `T`. Update `T` in an
aligned column per date. Maintain incremental position weights and contract refcounts
so opening/closing a lot does not rebuild the dedup table.

**P5.2 — Carry forward base state.** `PortfolioStepState` per unique contract:
snapshot/surface fingerprint; residual `T`; base IV, mark, selected Greeks,
status/route; optional dimensionless ALO boundary; generation number.

```cpp
Result<PnlTotals> advance(const SurfaceSet& next, Timestamp next_ts,
                          PortfolioStepState& state, const PriceOptions&);
```

The prior target mark **is** the next base mark, bit-identically (§4.7) — reuse it,
and assert the equality directly. If next-date risk is requested, compute it once and
retain it so `book_greeks` reads the retained totals instead of repricing. Handle
expiring lots before state promotion and new lots after the P&L cut, per current
accounting semantics.

> Note the off-by-one (§4.7): the duplicate is row *i*'s `book_greeks` versus step
> *(i+1)*'s `pnl_explain` base, and `book_greeks` covers the full surviving book while
> `pnl_explain` covers only the alive subset. Reuse only their intersection.

**P5.3 — Axis-state fusion.** Per contract resolve base local state once; shifted
price at rolled `T`; shifted σ at common base `T` for `dvol`; shared `dS`, `dt`, `dr`
once per uid group. Write Taylor components directly from unique results. Totals-only
callers aggregate unique-contract weights and skip position scatter entirely.

**P5.4 — Deterministic invalidation** on: surface/pricing-context fingerprint change
not represented by the next step; option method/scheme/math-mode change; corporate
action; cache route/fingerprint change under strict replay; entry/exit/expiry
generation mismatch.

**P5 acceptance:** NAV, settlement, financing, cost and every P&L axis bit-for-bit
unchanged in Reference mode; no duplicate `book_greeks(base)` after a successful
preceding step; a direct equality test for target→next-base mark reuse; steady-state
backtest performs no portfolio rebuild, no worker creation, no frame allocation;
end-to-end steps/s ≥2× P0 at the same kernel mode.

---

### P6 — Final compiler and deployment pass

**Estimate:** 2 engineer-days · **Risk:** low if applied last

1. ThinLTO on the Release perf preset; retain only for ≥3% geomean with no
   build/deploy regression.
2. Collect a representative pricing/backtest PGO profile; test PGO separately.
3. Confirm hot call sites inline across TU boundaries and cold error paths stay
   outlined.
4. Verify in `compile_commands.json` **and the final binary**: `/O2`; `/arch:AVX2`
   **only** on ISA objects; float flags; vector-library linkage.
5. Startup diagnostics: selected ISA, math mode, worker topology, cache policy.
6. Sustained thermal run. *(Per §5.6 expect no license-based downclock from 256-bit
   on this client part — but still compare contracts/joule and all-core sustained
   rate, not the first second.)*

**Acceptance:** retain each compiler feature only if the canonical benchmark improves
with identical correctness. No global `native` target, no undeclared runtime DLL
dependency (this is why SVML-by-DLL lost to SLEEF/static as the default).

---

## 8. Delivery sequence

| # | Increment | Depends on | Ships alone? |
|---:|---|---|---|
| 1 | P0 harness + oracles + **P0.5 correctness fix** | none | yes |
| 2 | P1 fused queries / prepared portfolio / workspace / executor | P0 | yes |
| 3 | P2.0 put slice + P2.1 five-solve calls + P2.2 hoist | P0 | yes |
| 4 | **P2.5 σ-Chebyshev boundary** | P0, P2.0 | yes, opt-in |
| 5 | P3 AVX2 packed kernels | P0; benefits from P1 groups, P2 | yes, opt-in |
| 6 | P4 cache V2 + fused cached Greeks | P0, P1 batch API; P3 Clenshaw optional | yes, opt-in with fallback |
| 7 | P5 stateful P&L / backtest | P1; benefits from P2/P4 state | yes |
| 8 | P6 LTO/PGO/deployment | all stable increments | yes |

Staffing: one kernel owner + one portfolio/cache owner, four to six weeks, with
correctness review at every gate.

**If only one two-week sprint is available: commit to P0 + P1 + P2.0 + P2.1.**
That produces trustworthy numbers, removes the repeated plumbing, fixes a real
mispricing (`r<0`), and closes the 17-solve call asymmetry — none of it depending on
an approximation.

---

## 9. Correctness and numerical acceptance gates

Performance work does not ship on throughput alone.

### 9.1 Price gates

Three independent references: (1) current scalar accurate ALO; (2) a
Richardson-refined Crank–Nicolson PDE oracle on a larger corner grid; (3)
Leisen–Reimer (order-2 convergent) and fixed published values where available.

For every side, exercise/no-exercise regime, rate/yield sign corner, moneyness,
maturity and volatility bucket:

- **Reference mode:** preserve existing bit contracts; upgrade the current coarse PDE
  test from "<0.5% relative" to a price-scaled oracle tolerance;
- **Fast cold:** max `|price_fast − price_ref| ≤ $0.001`, always `< $0.005`;
- **Cached / interpolated-boundary:** p99 ≤ $0.001 and max ≤ $0.005 inside the
  declared box; otherwise route to cold fallback;
- preserve intrinsic and European lower bounds, monotonicity, convexity, and
  put–call symmetry;
- **`r<0` corner:** correct against the double-boundary oracle, or explicitly
  `Unsupported`. Never silently European.

Do not use relative error alone for near-zero option values.

### 9.2 Greek gates

Build a Richardson/complex-step-compatible reference around the accurate pricer, with
per-axis bump studies. Absolute and scaled:

- delta: abs ≤ 2e-5;
- gamma: abs ≤ 2e-5, or relative ≤ 2e-3 where `|gamma| > 1e-3`;
- vega/rho/theta: contribution error under the canonical 1-day / 1-vol-pt / 1-bp
  shocks ≤ $0.001 per share;
- vanna/volga/charm: contribution error under the combined canonical shocks
  ≤ $0.001 per share;
- no NaN/Inf except documented invalid lanes;
- the exercise-boundary band is reported **separately**, with scalar fallback where a
  derivative is non-smooth.

Contribution-based gating beats a universal `1e-6 relative` threshold for Greeks that
cross zero.

### 9.3 P&L explain gates

`unexplained = total − Σ(explained axes)` is tautological and is **not** an accuracy
test. Require instead: pure-axis bump tests with non-target axes exactly zero;
normalized unexplained distribution vs move size with p95/p99 and max; second-order
convergence (halving a pure spot/vol move reduces the residual at the expected order
away from the boundary); whole-book Reference-vs-Production comparison by axis;
NAV/accounting equality including settlement, shares, financing and costs;
deterministic totals across worker count and execution grouping.

### 9.4 Determinism contract

- Reference scalar: cross-thread and archive bit identity where currently promised.
- Fast AVX2: bit identity for the same ISA / math mode / build, at all worker counts.
- Cross-ISA: numerical tolerance, **not** false bit-identity.
- Every persisted result records ISA, math mode, pricing route, cache fingerprint,
  compiler version, and scheme.

---

## 10. Performance acceptance gates

A package ships only if **all** apply:

1. median speedup meets its target on the synthetic matrix **and** a real OPRA corpus;
2. benchmark CV within the P0 limit;
3. p99 latency does not regress >10% unless the API is explicitly throughput-only;
4. scalar fallback rate is reported and ≤5% on the normal production corpus;
5. no new steady-state allocation in the kernel, prepared portfolio, or stateful P&L
   paths;
6. measured cold-cache and warm-cache where both matter;
7. all-core runs long enough to expose frequency/thermal effects;
8. output bandwidth reported separately from unique-contract compute;
9. full tests, warnings-as-errors, sanitizers, and archive compatibility pass;
10. a before/after JSON and a short decision note are committed.

---

## 11. Benchmark scenarios that must exist

| Scenario | Why |
|---|---|
| One fast ATM put | scalar latency floor |
| One dividend ATM call | today's 17-solve Greeks worst path |
| One **no-dividend** call | European short-circuit ceiling **and** the `r<0` guard |
| **Negative / near-zero r,q corners** | P0.5 correctness; double-boundary regime |
| Four homogeneous options | AVX2 pack efficiency |
| Mixed side/scheme/corner pack | mask + fallback behavior |
| **One full SPY expiry ladder, 40 strikes, real smile** | **P2.5 σ-Chebyshev payoff** |
| One full SPY chain by expiry | realistic grouping and smile divergence |
| 64-underlying, 2,688-unique book | portfolio comparison |
| 100k / 1M positions over 2,688 uniques | scatter, dedup, field mask |
| **Cached-Greeks bundle on `served_cache`** | the production path nothing measures today |
| Two adjacent real dates | P&L state and warm boundary reuse |
| 250-date rolling book | sustained backtest, allocation, cache amortization |
| Deep wing / near expiry / high vol grid | error and divergence stress |

Each portfolio case reports **unique contracts/s and positions/s**. A headline
positions/s without its dedup ratio is rejected.

---

## 12. Risks and mitigations

| Risk | Mitigation |
|---|---|
| AVX2 vector libm changes prices | explicit math modes, scalar oracle, tail/error fallback |
| ~~AVX2 lowers sustained clock~~ | **Void on client parts (§5.6).** Keep a sustained thermal check; do not design around it |
| SLEEF is only at SVML parity, not faster than libm | the win must come from 4 lanes; bakeoff decides, and scalar stays the crossover default |
| SVML needs a runtime DLL | prefer SLEEF/static or a hand-rolled Φ; never an undeclared DLL dependency |
| Hybrid P/E cores make `hw` runs noisy | topology discovery, pinned bench modes, persistent executor |
| Call homogeneity loses bit identity | Reference stays scalar; validate Production to tick/Greek gates |
| **σ-boundary interpolant non-smooth as σ→0 / near expiry** | clamp the box, per-strike error gate, cold fallback with `route` recorded |
| **σ-interpolant is unpublished for the boundary** | gate on measurement against cold ALO; ship behind a flag |
| Boundary derivatives non-smooth near exercise | detect proximity, fall back to five/seven-solve FD |
| Implicit Jacobian ill-conditioned | pivoted LU, condition/residual gate, strict two-day kill rule |
| Carry-aware cache gives wrong θ/ρ semantics | PDE theta/charm, explicit r-derivative, cold comparison |
| Cache self-consistent but cold-inaccurate | independent cold/PDE gates; never score only round-trip/in-band |
| Cache build ruins fit latency | async publication, archive persistence, amortization threshold |
| Derivative tensors consume too much memory | bounded LRU, lazy materialization, measured per-surface cap |
| Prepared sorting changes public order/reduction | reverse permutation, fixed input-order reduction |
| Stateful P&L reuses stale results | fingerprints, generations, explicit invalidation battery |
| **`r<0` silently returns European today** | **P0.5 — fix before anything else** |

---

## 13. Explicit non-goals

- Replacing high-performance fitting or changing fitted curve parameters.
- **GPU/CUDA.** Published ~150× American speedups are Monte-Carlo/binomial; **no
  published GPU crossover exists for semi-analytic ALO** (§5.12). Saturate CPU
  batching first.
- **COS method.** Requires a characteristic function; not the natural tool for
  Black–Scholes American (§5.12).
- Changing listed-option contract or accounting semantics.
- Global `-ffast-math` / `/fp:fast`.
- Quantizing strikes, maturities, or vols to manufacture shared boundaries.
- Claiming Black–Scholes IV throughput (e.g. "Let's Be Rational" ~2.8M/s) as
  American-IV throughput.
- Removing the scalar cold oracle.
- Serializing mutable warm-start state as part of the immutable surface.
- Treating more threads as a substitute for a faster per-core kernel.

---

## 14. Implementation task ledger

| ID | Deliverable | Est. | Proof |
|---|---|---:|---|
| P0-1 | Google Benchmark price/Greek matrix (incl. cached path) | 1.0 d | JSON median/CV |
| P0-2 | Portfolio/P&L ratio + topology matrix | 0.75 d | stable repeated rates |
| P0-3 | Counters, opt-remarks, baseline compare | 0.5 d | checked-in baseline |
| P0-4 | Fix `al_fast_opts` + `bivariate` doc bugs | 0.1 d | doc diff |
| **P0-5** | **Exact early-exercise regions + `r<0` policy** | **0.75 d** | **rate/yield corner grid vs PDE oracle** |
| P1-1 | Fused resolved surface point/batch; binary term scan | 1.0 d | bit-equality tests |
| P1-2 | Aligned `PreparedPortfolio` groups | 1.0 d | permutation/dedup tests |
| P1-3 | Workspace, in-place, **real** field mask, totals-only | 1.0 d | zero-allocation bench; 37 B/pos |
| P1-4 | Persistent topology-aware executor + parallel P&L scatter | 1.0 d | deterministic thread tests |
| **P2-0** | **`andersen_lake_put_slice` (strike homogeneity)** | **1.0 d** | **bit-identical to per-strike at const σ** |
| P2-1 | Normalized boundary solve/eval API | 1.0 d | put unchanged |
| P2-2 | Seven/five-solve call Greeks | 2.0 d | call reference/PDE grid |
| P2-2b | Hoist per-node `al_boundary_at`; QD+ seed spike | 1.0 d | op-count + sweep-count drop |
| P2-3 | Cross-step warm state | 2.0 d | sweep reduction on real corpus |
| **P2-5** | **σ-axis Chebyshev boundary interpolant** | **2.5 d** | **≥2.5× on a real SPY board; §9 gates** |
| P2-X | Implicit differentiation spike | max 2.0 d | ship/kill report |
| P3-1 | ISA dispatcher + AVX2 object | 0.75 d | forced-ISA tests |
| P3-2 | AoSoA<4> boundary kernel | 2.0 d | scalar parity grid |
| P3-3 | Vector-math bakeoff (incl. direct rational Φ) + tail patch | 1.5 d | error/speed frontier |
| P3-4 | Public American/B76 SoA batch APIs | 1.0 d | lane/status tests |
| P3-5 | Portfolio integration + disassembly gates | 0.75 d | end-to-end target |
| P4-1 | Carry-aware/ρ-correct cache design bakeoff | 1.5 d | error/memory report |
| P4-2 | Kill 4 wasted Clenshaw sweeps; derivative tensors; fused Clenshaw | 1.5 d | 20→16→fused; derivative oracle |
| P4-3 | `PricedSurface` cache route + fallback | 1.0 d | route/cold parity |
| P4-4 | Archive / lazy / LRU integration | 1.0 d | round-trip/cap tests |
| P4-5 | Amortization + production gates | 1.0 d | real-corpus bench |
| P5-1 | Stable incremental book + step state | 1.5 d | lifecycle tests |
| P5-2 | Fused advance / P&L / next-risk API | 1.5 d | bit-identical backtest |
| P5-3 | Invalidation + totals-only integration | 1.0 d | mutation battery |
| P6 | ThinLTO / PGO / sustained deployment | 2.0 d | retained-feature report |

---

## 15. Definition of done

- [ ] the benchmark/oracle suite is stable, checked in, and measures the **cached**
      path;
- [ ] Reference mode is unchanged and all numerical gates pass;
- [ ] **the `r<0` early-exercise region is correct or explicitly unsupported**;
- [ ] a put strike ladder costs **one** boundary solve at constant σ;
- [ ] dividend calls no longer use the 17-boundary full-Greeks route in Production;
- [ ] the σ-Chebyshev boundary interpolant ships behind a gate, or is killed with
      evidence;
- [ ] American price and full-Greeks SoA batch APIs execute **real AVX2 kernels**,
      compiled with `/arch:AVX2` on ISA objects only;
- [ ] the binary safely dispatches scalar/AVX2 at runtime via `__cpuid`;
- [ ] prepared portfolio pricing is allocation-free and thread-spawn-free in steady
      state;
- [ ] price-only touches 37 B/position, not 101 B;
- [ ] `PricedSurface` can serve a validated carry-aware cache with cold fallback;
- [ ] cached derivative evaluation is fused and query-time coefficient
      differentiation is gone;
- [ ] P&L explain carries target marks and next risk forward without duplicate
      repricing;
- [ ] every ship target in §1 is met on the pinned host, or the increment is
      documented as killed with evidence;
- [ ] benchmark JSON, accuracy report, cache amortization report and deployment
      metadata are committed;
- [ ] README performance claims distinguish scalar latency, batch throughput,
      cold/reference, cached/production, and unique-vs-position rates.

At that point ALO remains the reference; the cold path uses strike homogeneity,
put–call symmetry, σ-direction boundary interpolation, batching, AVX2 and
topology-aware parallelism; and the production path serves a carry-correct,
derivative-aware surrogate at portfolio scale with a transparent fallback to the
reference model.

---

## 16. Unverified claims and open questions

Recorded so nobody treats them as settled.

- **The ALO paper body was not readable** (SSRN returns 403). The `(l,m,n,p)` tuples
  in §5.1 come from QuantLib's `QdFpAmericanEngine`, not from the paper. The
  ~100k prices/s figure is verbatim from the risk.net JCF editor's letter; the
  "10–11 significant digits" line is from the indexed abstract. **No CPU model or
  year is attached to the 100k figure**, so it is an order-of-magnitude anchor, not a
  target to match on a 2-GHz laptop core.
- **Whether ALO's paper computes Greeks at all** is unknown (body inaccessible). What
  is certain is that **no Greeks throughput number is published anywhere**.
- `B(τ;K) = K·b(τ)` is not printed verbatim in any primary I could read. It follows
  from Merton's degree-one homogeneity plus Healy (2021) Eq. 22's `S⋆ = K·N/D` form.
  Before building P2.5 on it, **verify numerically**: solve the boundary at
  `(K, σ, r, q)` and `(λK, σ, r, q)` and assert `b` matches to solver tolerance. This
  is a 20-minute test and it de-risks the largest work package.
- Cody (1969)'s 6e-19–3e-20 relative-error figure is corroborated by two secondary
  indexes; the AMS PDF is gated.
- Merton (1973) Theorem text was not fetched (both open PDFs failed); the result is
  standard.
- SLEEF's `erf`/`erfc` are **not benchmarked in the SLEEF paper** — the parity claim
  covers the functions they did benchmark. Our Φ bakeoff must measure them directly.
- Intel's first-party AVX turbo-license tables were not fetched; §5.6 rests on
  third-party *measurements* (Cloudflare, Travis Downs), which is arguably stronger.
- Giles & Glasserman's exact *Risk* volume/pages (19(1):88–92) come from a secondary
  summary; venue and date are confirmed.

**Open engineering questions to settle in P0:**

1. Does the book ever see `r < 0`? The answer decides whether P0.5 is a two-line
   guard or a double-boundary implementation.
2. What is the real σ range per expiry on the OPRA corpus? It sets `n_σ` for P2.5.
3. What is the measured `n_unique` crossover below which inline execution beats the
   executor?
4. Re-measure everything in §1 under `/arch:AVX2` **and** SSE2, so the AVX2 speedup
   is attributed to the kernel and not to the flag.

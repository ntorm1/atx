# Pricing and Greeks Pipeline Code Review

**Scope:** `atx-vol` pricing, implied-volatility, Greeks, American exercise, portfolio pricing, and P&L-explain pipelines  
**Reviewed revision:** `01d88b8` (`Merge worktree-atx-vol-c0710: competitiveness sprint C0+C1+C2 (rev 2, parallel-safe)`) plus the current working tree as observed on 2026-07-11  
**Review method:** direct symbol/call-site inspection with `rg`, focused source reads, test/benchmark/build inspection, and a targeted test run. The repository-requested codebase-memory MCP graph tools were not available to this agent, so symbol-aware `rg` was used as the fallback. No production code was changed.

## Executive summary

The numerical core is unusually well tested and explicit about conventions. Black-76 formulas, American exercise-regime routing, cold-vs-analytic Greek parity, deterministic portfolio reductions, prepared-book grouping, persistent worker execution, and caller-owned output views are all substantive strengths.

The principal issue is that the end-to-end portfolio hot path stops short of the new batch kernels. `PreparedPortfolio` and `PricedSurface::evaluate_batch` are live, but the latter still loops over contracts and calls scalar `american_price` or scalar `american_greeks_fd`/`american_greeks_al`. The standalone `american_price_batch`, `american_greeks_batch`, Black-76 AVX2 SoA kernels, sigma-slice boundary interpolation, and implicit-differentiation Greeks are not used by `PortfolioPricer`. Moreover, the American boundary AVX2 route deliberately defaults to scalar. This makes several SoA/SIMD structures staging infrastructure rather than a vectorized production hot path.

Two correctness defects should be fixed before further throughput work:

1. `Portfolio::retime` is not transactional, and a warmed `PortfolioWorkspace` can silently keep stale prepared K/T/grouping when non-first contracts change.
2. `american_price_batch` changes a process-global ISA override for each call. Concurrent calls can observe or restore each other's override, violating per-call route selection and potentially dispatching AVX2 where a caller did not request it.

No P0 issue was found. The highest-confidence throughput opportunity is not another local kernel micro-optimization: it is connecting a model/preset-correct batch API to `PricedSurface::evaluate_batch`, then measuring the whole portfolio path.

## End-to-end pipeline map

```text
Position[]
  -> Portfolio::create
       exact-bit contract dedup: (uid, K, T, side)
  -> PortfolioPricer + PortfolioWorkspace
  -> ensure_prepared
       PreparedPortfolio: sort unique contracts by (uid, side, T bits)
       aligned K/T/uid columns + side + reverse permutation + groups
  -> solve_uniques (parallel ranges on persistent PricingExecutor)
       SurfaceSet::find once per touched group/range
       PricedSurface::evaluate_batch
         equal-T run detection
         interp_forward once per run
         per lane:
           resolve_with_carry
             k = log(K/F), sigma = CurveSurface::iv(k,T)
           evaluate_resolved
             price-only -> american_price -> Andersen-Lake or BAW
             Greeks     -> american_greeks_fd (default) or american_greeks_al
  -> reverse-permutation write into unique ContractPx[]
  -> scatter_rows (parallel) and/or fixed-input-order reduce_price_totals
  -> PriceFrame / PriceTotals

PnL explain:
  same prepared groups
  -> base evaluate_batch(IV + American price + full Greeks)
  -> shifted evaluate_batch(IV at base T)
  -> shifted evaluate_batch(American price at rolled T)
  -> Taylor terms + unexplained residual
  -> deterministic row scatter/reduction
```

The public scalar/standalone routes are separate:

```text
black76_price / black76_greeks / implied_vol
  -> legacy span batch wrappers (scalar loops)
  -> simd::* raw-pointer batch APIs (AVX2 where available)

american_price_batch
  -> compact genuine-American lanes
  -> american_put_boundary_batch
       Auto -> scalar today; ForceAvx2 -> AVX2 boundary kernel

american_greeks_batch
  -> scalar american_greeks_fd/al per lane, optionally parallel
```

Evidence: [`src/portfolio_pricer.cpp:211`](../../src/portfolio_pricer.cpp#L211), [`src/portfolio_pricer.cpp:284`](../../src/portfolio_pricer.cpp#L284), [`src/priced_surface.cpp:306`](../../src/priced_surface.cpp#L306), [`src/priced_surface.cpp:346`](../../src/priced_surface.cpp#L346), [`src/prepared_portfolio.cpp:49`](../../src/prepared_portfolio.cpp#L49), [`src/american_batch.cpp:70`](../../src/american_batch.cpp#L70), [`src/american_batch.cpp:139`](../../src/american_batch.cpp#L139).

## Findings

### P1  Confirmed defect  warmed portfolio workspaces can silently price stale maturities after `retime`

**Confidence:** High  
**Area:** Correctness, lifecycle, data structures

`PortfolioPricer::retime` publicly mutates the owned portfolio. `PortfolioWorkspace` retains a `PreparedPortfolio` containing copied K/T/uid columns and a T-dependent permutation. Cache reuse is guarded only by portfolio address, unique count, and a fingerprint of the *first* unique contract. If `retime` leaves the first contract unchanged but changes another contract, `ensure_prepared` reuses stale T values and stale grouping. The next `price_into`, `price_totals`, or `pnl_explain_into` can therefore price the wrong maturity while reporting `Ok`.

The source documents the residual ABA hazard, but it is not merely a destruction/reconstruction edge: the class itself exposes in-place `retime`, making same-object content mutation an ordinary supported operation. Evidence: [`include/atx/vol/portfolio_pricer.hpp:449`](../../include/atx/vol/portfolio_pricer.hpp#L449), [`include/atx/vol/portfolio_pricer.hpp:303`](../../include/atx/vol/portfolio_pricer.hpp#L303), [`src/portfolio_pricer.cpp:376`](../../src/portfolio_pricer.cpp#L376), [`src/portfolio_pricer.cpp:411`](../../src/portfolio_pricer.cpp#L411), [`src/prepared_portfolio.cpp:86`](../../src/prepared_portfolio.cpp#L86).

**Impact:** Silent wrong PV and Greeks after a partial tenor roll; group order and equal-T carry reuse may also be wrong. This affects all reusable-workspace APIs.

**Remediation:** Give `Portfolio` a monotonically increasing structural/version counter, increment it only after a successful mutation, store the version in `PortfolioWorkspace`, and rebuild on mismatch. A full exact content hash is also acceptable but costs O(U) each call; an explicit version is O(1) and exact. If `Portfolio` is intended immutable while a workspace exists, remove public mutation or make `PortfolioPricer::retime` explicitly invalidate all registered workspaceswhich is harder with caller-owned workspaces.

**Tests:** Warm a workspace on a three-contract book; retime only contracts 2/3 while contract 1 is bit-identical; compare every price/Greek/status and totals field against a fresh workspace. Repeat for `price_into`, `price_totals`, `pnl_explain_into`, and changed T ordering that moves a contract between equal-T runs.

### P1  Confirmed defect  `Portfolio::retime` can partially mutate before returning an error

**Confidence:** High  
**Area:** Correctness, error handling

`Portfolio::retime` validates and writes each deduplicated contract in one pass. If an inconsistent tenor is detected for a later deduplicated contract, earlier `contracts_[i].T` values have already changed, while the per-position copies are updated only after the entire validation loop. The error return can therefore leave `contracts_` and `positions_` mutually inconsistent. Evidence: [`src/portfolio_pricer.cpp:103`](../../src/portfolio_pricer.cpp#L103), especially writes at [`src/portfolio_pricer.cpp:123`](../../src/portfolio_pricer.cpp#L123) before the later position update at [`src/portfolio_pricer.cpp:127`](../../src/portfolio_pricer.cpp#L127).

**Impact:** A rejected roll can corrupt subsequent dedup/pricing assumptions and combine with the stale-workspace issue above.

**Remediation:** Use two phases: validate and stage one tenor per unique contract without mutation; only after all validation succeeds commit both contract and position copies, then increment the portfolio version.

**Tests:** Use at least two deduplicated contracts. Supply consistent tenors for the first and inconsistent tenors for duplicate positions of the second. Assert the error and byte-identical portfolio state before/after.

### P1  Confirmed concurrency defect  American batch per-call ISA selection races through process-global state

**Confidence:** High  
**Area:** Correctness, concurrency

`american_price_batch` constructs `IsaScope`, which saves a process-global atomic ISA value, writes the requested per-call value, and restores the saved value on scope exit. The CPU API explicitly says this override is intended for coarse set-once startup/test control, not per-call toggling. Two concurrent batches can interleave save/set/dispatch/restore and use the wrong route; a `ForceAvx2` call can influence a nominally scalar call. Atomic access removes a C++ data race but does not make the compound scope operation semantically isolated. Evidence: [`src/american_batch.cpp:20`](../../src/american_batch.cpp#L20), [`src/american_batch.cpp:82`](../../src/american_batch.cpp#L82), [`include/atx/vol/simd/cpu.hpp:37`](../../include/atx/vol/simd/cpu.hpp#L37), [`src/simd/cpu.cpp:55`](../../src/simd/cpu.cpp#L55), [`src/simd/american_boundary_batch.cpp:62`](../../src/simd/american_boundary_batch.cpp#L62).

**Impact:** Nondeterministic route diagnostics and output differences within the documented AVX2 tolerance; on a non-AVX2 host, an overlapping forced-AVX2 scope can be unsafe because `ForceAvx2` deliberately bypasses CPUID.

**Remediation:** Pass `SimdIsa` as an argument to `american_put_boundary_batch` (or resolve it to a local function pointer) and remove `IsaScope`. Keep the global override only as an optional default used when the call requests `Auto`.

**Tests:** A barrier-controlled two-thread test repeatedly overlaps ForceScalar and ForceAvx2 calls and asserts each returned route. Run the scalar side under a non-AVX2 CI configuration. A TSAN test alone will not catch this logical race.

### P1  Confirmed integration/performance gap  prepared SoA does not reach a batch American solver

**Confidence:** High  
**Area:** Performance, hot-path wiring

`solve_uniques` calls `PricedSurface::evaluate_batch`, but that function only hoists carry resolution for equal-T runs. Its inner loop invokes `evaluate_resolved` once per lane, and that invokes scalar American price/Greek functions. No production call site connects `PortfolioPricer` to `american_price_batch`, `american_greeks_batch`, or `simd::black76_greeks_batch_soa`. Searches found those APIs used by their tests (and Python uses the older scalar span wrapper), not by the portfolio engine. Evidence: [`src/portfolio_pricer.cpp:246`](../../src/portfolio_pricer.cpp#L246), [`src/priced_surface.cpp:325`](../../src/priced_surface.cpp#L325), [`src/priced_surface.cpp:346`](../../src/priced_surface.cpp#L346), [`src/priced_surface.cpp:269`](../../src/priced_surface.cpp#L269), [`src/simd/greeks_batch.cpp:48`](../../src/simd/greeks_batch.cpp#L48).

Even the standalone American price batch defaults to scalar because `kShipAvx2Boundary` is false after measuring only ~1.61.7x for the isolated boundary kernel. Evidence: [`src/simd/american_boundary_batch.cpp:30`](../../src/simd/american_boundary_batch.cpp#L30), [`src/simd/american_boundary_batch.cpp:53`](../../src/simd/american_boundary_batch.cpp#L53), [`include/atx/vol/american_batch.hpp:113`](../../include/atx/vol/american_batch.hpp#L113).

**Impact:** The live portfolio price-only path still performs one cold scalar American solve per unique contract. Full Greeks default to seven boundary states per unique contract through finite differences. Aligned columns and groups currently yield bracket/carry reuse and parallel scheduling, but not lane-vectorized price or Greek computation.

**Remediation:** Do not directly splice the existing batch API into the hot path yet: it lacks per-surface `AmericanMethod` and resolved `AlOpts`. First define a non-owning resolved batch request carrying exact `S,K,T,sigma,r,q,side,method,opts`, plus per-lane `Status`. Integrate it at the equal-T-run level of `evaluate_batch`, preserve reverse-permutation writes, and gate on full-frame parity and end-to-end throughputnot isolated kernel speed.

**Tests/benchmarks:** Compare batch vs current scalar `evaluate_resolved` across methods, AL presets, exercise regimes, surface kinds, calls/puts, invalid lanes, and thread counts. Add portfolio benchmark rows that report route counts and measure the same warmed `price_into`/`price_totals` workloads before/after.

### P2  Confirmed API mismatch  standalone American batches cannot reproduce `PricedSurface`

**Confidence:** High  
**Area:** Correctness risk, feature wiring

`PricedSurface` forwards its resolved `AmericanMethod` and `AlOpts` to scalar pricing and Greeks. `PricingKernel` contains only ISA, analytic-Greek selection, and an optional executor. `american_price_batch` calls `andersen_lake` with default options and has no BAW route; `american_greeks_batch` likewise calls default-option FD/analytic functions. Thus wiring it navely would change prices for archived/live surfaces configured with a non-default AL preset or BAW. Evidence: [`src/priced_surface.cpp:174`](../../src/priced_surface.cpp#L174), [`src/priced_surface.cpp:270`](../../src/priced_surface.cpp#L270), [`include/atx/vol/american_batch.hpp:109`](../../include/atx/vol/american_batch.hpp#L109), [`src/american_batch.cpp:113`](../../src/american_batch.cpp#L113), [`src/american_batch.cpp:160`](../../src/american_batch.cpp#L160).

**Impact:** This is likely why the attractive-looking batch API remains dormant. Treat it as a standalone experimental interface, not a drop-in production replacement.

**Remediation/tests:** Fold this into the resolved-batch request above. Add explicit tests with fast/default/custom `AlOpts` and BAW showing scalar/batch equality.

### P2  Confirmed performance issue  full-Greek portfolio output is still AoS internally and computes all fields

**Confidence:** High  
**Area:** Data structures, performance

`PriceFrame` is SoA, and SIMD Black-76 has a nullable-column SoA sink, but the American portfolio solve materializes `std::vector<AmericanGreeks>` (AoS) for uniques, then copies eight fields into per-position SoA columns. `PricedSurface::EvalField` only distinguishes first-/second-order groups; once any Greek is requested the full American bundle is evaluated. `PriceFieldMask` similarly offers only Marks vs all Greeks. Evidence: [`src/portfolio_pricer.cpp:438`](../../src/portfolio_pricer.cpp#L438), [`src/portfolio_pricer.cpp:246`](../../src/portfolio_pricer.cpp#L246), [`src/portfolio_pricer.cpp:323`](../../src/portfolio_pricer.cpp#L323), [`include/atx/vol/simd/greeks_batch.hpp:29`](../../include/atx/vol/simd/greeks_batch.hpp#L29).

**Impact:** Extra unique-result traffic, AoS-to-SoA scatter, and unnecessary boundary solves for common delta/vega-only consumers. The existing `PricedSurface::delta` and `vega` fast paths prove that partial sensitivities can be substantially cheaper.

**Remediation:** Introduce per-Greek field selection through `PortfolioPricer` -> `PricedSurface` -> American batch, with native SoA unique-result columns. Special-case price+delta, price+vega, and first-order-only requests before general full-bundle work. Retain the existing full-frame wrapper for compatibility.

**Tests:** Poison unrequested output spans and assert they are untouched; verify selected fields bit-match the corresponding full bundle; benchmark delta-only, vega-only, first-order, and full second-order books.

### P2  Confirmed algorithmic issue  legacy `aggregate_greeks` bucket construction is O(number of legs  number of buckets)

**Confidence:** High  
**Area:** Data structures and algorithms

The legacy portfolio Greeks API linearly scans the growing output vector for every option leg. With many UIDs/expiries/groups, this is quadratic. It also runs scalar European Black-76 Greeks per leg rather than deduplicating identical contracts or using the available SIMD batch. Evidence: [`src/portfolio_greeks.cpp:29`](../../src/portfolio_greeks.cpp#L29), [`src/portfolio_greeks.cpp:32`](../../src/portfolio_greeks.cpp#L32), [`src/portfolio_greeks.cpp:49`](../../src/portfolio_greeks.cpp#L49), [`src/portfolio_greeks.cpp:82`](../../src/portfolio_greeks.cpp#L82).

**Impact:** Poor scaling for broad books; duplicate pricing work. It also creates two public risk semantics: this path is European and raw-quantity weighted, while `PortfolioPricer` is American and multiplier-weighted. That distinction is documented but easy for downstream users to misuse.

**Remediation:** Build `unordered_map<group_key,index>` (reserved to the expected bucket count) or sort/reduce by key; deduplicate contract evaluations; batch Black-76 Greeks by homogeneous expiry/surface context. Consider renaming or strongly typing the legacy aggregate to make its European/raw-qty convention explicit.

**Tests:** Adversarial many-bucket scaling benchmark; parity for every `AggMode`; explicit convention tests contrasting raw qty vs dollar multiplier and European vs American results.

### P2  Confirmed observability gap  per-lane model failures are collapsed into coarse status values

**Confidence:** High  
**Area:** Error handling, features

American batch has only `Ok` and `Unsupported`, intentionally combining invalid arguments, numerical failure, and the unsupported double-continuation regime. `PortfolioPricer` maps any evaluation error or non-finite price to `NumericError`, losing the underlying error code. Evidence: [`include/atx/vol/american_batch.hpp:47`](../../include/atx/vol/american_batch.hpp#L47), [`src/american_batch.cpp:113`](../../src/american_batch.cpp#L113), [`src/portfolio_pricer.cpp:260`](../../src/portfolio_pricer.cpp#L260), [`src/portfolio_pricer.cpp:270`](../../src/portfolio_pricer.cpp#L270).

**Impact:** Production monitoring cannot distinguish bad data, unsupported negative-carry economics, boundary collapse, or internal numerical failures. This complicates fallback policy and makes route-quality regressions hard to detect.

**Remediation:** Carry a compact per-lane error enum/code through batch and portfolio results, with `PriceStatus` as the coarse compatibility projection. Add counters by route and error code.

### P2  Feature gap  double-continuation American regimes are explicitly unimplemented

**Confidence:** High  
**Area:** Correctness coverage / missing feature

The single-boundary Andersen-Lake and BAW implementations return `NotImplemented` for the negative-carry double-continuation corner; the behavior is consistently propagated rather than silently returning a European price, which is the correct current failure mode. Evidence: [`include/atx/vol/american.hpp:426`](../../include/atx/vol/american.hpp#L426), [`src/american.cpp:1278`](../../src/american.cpp#L1278), [`src/american.cpp:1794`](../../src/american.cpp#L1794), [`src/american.cpp:1905`](../../src/american.cpp#L1905).

**Impact:** Some negative-rate/rich-yield markets cannot be priced or risked. Portfolio lanes become `NumericError` without preserving the reason.

**Next step:** Implement a two-boundary solver or an approved PDE/tree fallback, then add price/Greek oracle grids around regime transitions. Until then, expose the unsupported reason distinctly.

### P3  Improvement  fixed finite-difference steps need conditioning policy and error estimates

**Confidence:** Medium  
**Area:** Numerical accuracy, algorithms

Default American Greeks use `hS=1e-3*S`, `hv=1e-3` absolute volatility, `hr=1e-4`, and `hT=1e-3` years, with a one-sided time stencil near expiry. These choices are tested and internally consistent, but fixed absolute vol/rate/time steps can be truncation- or cancellation-dominated across very low vol, high vol, near expiry, and low-price wings. Evidence: [`src/american.cpp:1929`](../../src/american.cpp#L1929), [`src/american.cpp:2168`](../../src/american.cpp#L2168).

**Impact:** Greek noise is likely to dominate before price error in extreme regimes; theta/charm semantics also differ between FD and analytic/PDE routes by design.

**Remediation:** Add scale-aware steps, Richardson checks on selected stress lanes, and a per-Greek quality/route diagnostic. Do not silently change pinned production numbers; introduce and gate a versioned scheme.

### P3  Improvement  `noexcept` SIMD IV fallback can terminate on error-message allocation failure

**Confidence:** Medium  
**Area:** Error/lifetime robustness

`simd::implied_vol_batch` is `noexcept`, but its scalar fallback calls `implied_vol`, whose error paths construct message-bearing `Error` objects. The header acknowledges that a failing scalar patch may allocate. If that allocation throws, `noexcept` terminates the process. Evidence: [`include/atx/vol/simd/iv_batch.hpp:23`](../../include/atx/vol/simd/iv_batch.hpp#L23), [`src/simd/iv_batch.cpp:18`](../../src/simd/iv_batch.cpp#L18), [`src/implied_vol.cpp:143`](../../src/implied_vol.cpp#L143).

**Impact:** Only an allocation-failure/error-lane corner, but contrary to robust batch semantics.

**Remediation:** Use allocation-free error codes inside numeric kernels and format messages only at API boundaries, or remove `noexcept` if exceptions are part of the supported runtime.

## Dormant and partially wired inventory

| Capability | Current HEAD status | Evidence / implication |
|---|---|---|
| Exact contract dedup | **Wired** | `Portfolio::create` hash-dedups `(uid,K,T,side)`; [`src/portfolio_pricer.cpp:65`](../../src/portfolio_pricer.cpp#L65). |
| Prepared aligned K/T/uid SoA, reverse permutation, `(uid,side,T)` grouping | **Wired** | Built lazily and retained by workspace; [`src/prepared_portfolio.cpp:33`](../../src/prepared_portfolio.cpp#L33), [`src/portfolio_pricer.cpp:411`](../../src/portfolio_pricer.cpp#L411). |
| Equal-T carry/bracket reuse | **Wired** | `evaluate_batch` detects raw-bit-equal T runs; [`src/priced_surface.cpp:325`](../../src/priced_surface.cpp#L325). |
| Persistent pricing executor / parallel unique solve and scatter | **Wired** | `run_ranges` / `run_blocks`; [`src/portfolio_pricer.cpp:284`](../../src/portfolio_pricer.cpp#L284), [`src/portfolio_pricer.cpp:306`](../../src/portfolio_pricer.cpp#L306). |
| Prices-only and totals-only APIs | **Wired** | Skip Greek columns or all row scatter; [`src/portfolio_pricer.cpp:486`](../../src/portfolio_pricer.cpp#L486), [`src/portfolio_pricer.cpp:534`](../../src/portfolio_pricer.cpp#L534). |
| Analytic Andersen-Lake Greeks option | **Wired but opt-in** | Five boundary states vs seven FD states; [`src/priced_surface.cpp:198`](../../src/priced_surface.cpp#L198), [`src/american.cpp:2231`](../../src/american.cpp#L2231). |
| Per-group `GroupRoute` metadata | **Stored, not consumed** | Source explicitly says nothing reads it; [`src/portfolio_pricer.cpp:404`](../../src/portfolio_pricer.cpp#L404). |
| `american_price_batch` | **Standalone/test-only; not portfolio-wired** | Compaction + boundary batch exists; [`src/american_batch.cpp:70`](../../src/american_batch.cpp#L70). Auto is scalar. |
| `american_greeks_batch` and nullable Greek SoA | **Standalone/test-only; scalar lanes** | No vector Greek stencil; [`src/american_batch.cpp:139`](../../src/american_batch.cpp#L139). |
| Black-76 AVX2 price/vega/Greek/IV kernels | **Implemented, not in American portfolio hot path** | Runtime-dispatched standalone APIs; [`src/simd/black76_batch.cpp:34`](../../src/simd/black76_batch.cpp#L34), [`src/simd/greeks_batch.cpp:48`](../../src/simd/greeks_batch.cpp#L48). |
| Legacy span batch API in `batch.cpp` | **Public but scalar** | Explicit bounded scalar loops; [`src/batch.cpp:1`](../../src/batch.cpp#L1). This coexists confusingly with `simd::*` APIs of similar names. |
| Fixed-sigma cross-strike AL slices | **Wired into correction-cache construction and Python, not portfolio query pricing** | Calls in correction construction; [`src/correction.cpp:341`](../../src/correction.cpp#L341). |
| Sigma-interpolated cross-strike slices | **Tests/research only** | Call sites are tests/corpus probes; implementation at [`src/boundary_interp.cpp:266`](../../src/boundary_interp.cpp#L266). |
| Implicit boundary-differentiation Greeks | **Research/test-only** | Public only in `detail`; [`include/atx/vol/american.hpp:517`](../../include/atx/vol/american.hpp#L517), implementation [`src/american.cpp:2786`](../../src/american.cpp#L2786). |
| QD+ critical-price seed | **Measurement spike, not production-wired** | Explicit source note; [`src/american.cpp:213`](../../src/american.cpp#L213). |
| Double-continuation American solver | **Missing** | Explicit `NotImplemented`; see P2 finding. |
| Surface-twist risk shock | **Missing/reserved** | Returns `NotImplemented`; [`src/portfolio_risk.cpp:192`](../../src/portfolio_risk.cpp#L192). |

## Correctness and numerical strengths

- The scalar Black-76 implementation shares core definitions and handles zero-time/zero-vol collapse consistently; Greeks document forward-delta, calendar-theta, and fixed-forward rho conventions. Evidence: [`src/black76.cpp:22`](../../src/black76.cpp#L22), [`src/greeks.cpp:12`](../../src/greeks.cpp#L12).
- Implied vol validates finiteness and no-arbitrage bounds, uses a guarded SR-2017 seed, Halley iteration, step bounding, and explicit vega-collapse/non-convergence errors. Evidence: [`src/implied_vol.cpp:141`](../../src/implied_vol.cpp#L141), [`src/implied_vol.cpp:165`](../../src/implied_vol.cpp#L165).
- American pricing explicitly classifies European, American, and unsupported regimes and propagates unsupported cases rather than silently substituting a wrong European result.
- American FD Greeks price the same cold graph as fair value, share spot-independent boundaries, preserve exact base-price identity, and propagate the first pricer error. Evidence: [`src/american.cpp:1919`](../../src/american.cpp#L1919), [`src/american.cpp:2138`](../../src/american.cpp#L2138).
- Portfolio totals are reduced in fixed input order after parallel disjoint solves, preserving deterministic bits across thread counts. Evidence: [`src/portfolio_pricer.cpp:339`](../../src/portfolio_pricer.cpp#L339).
- P&L explain reprices target American marks and keeps an explicit unexplained residual rather than pretending the truncated Taylor expansion is exact. Evidence: [`src/portfolio_pricer.cpp:778`](../../src/portfolio_pricer.cpp#L778).
- SIMD kernels patch degenerate/deep-wing/non-converged lanes through scalar references, and tests cover forced scalar/AVX2 routes.

## Test, build, and benchmark assessment

### Commands run

```powershell
git rev-parse --short HEAD
git status --short -- atx-vol
rg ... src include tests bench
cmake --build C:\atx\build-rel --target atx-vol-tests -j 4
C:\atx\build-rel\bin\atx-vol-tests.exe \
  --gtest_filter=Black76*:Greeks*:ImpliedVol*:American*:AmericanBatch*:PricedSurface*:PortfolioPricer*:PreparedPortfolio*:PnlGreeksConsistency*:Simd* \
  --gtest_brief=1
```

The focused existing binary ran 140 tests from 20 suites: **139 passed and 1 failed** in 10.7 s. The failure was `PreparedPortfolio.GroupedPriceEqualsIndependentOracleAndPinnedFingerprint`: computed `7007578824662444381`, pinned `18234180065510186026`. The test's independent-oracle comparison passed before the stale golden assertion, so this is evidence of an out-of-date/review-needed golden rather than proof of a pricing defect, but the regression gate is currently red and should not be ignored.

The attempted current rebuild did not complete within the 60 s command window because multiple concurrent repository builds were active; therefore the run above used `C:\atx\build-rel\bin\atx-vol-tests.exe` timestamped 2026-07-11 16:34. Claims in this report rely on current source inspection; the test result is explicitly not represented as a clean current-HEAD build.

The benchmark suite is broad and includes query, ladder, warmed `price_into`, `price_totals`, price-only, analytic-Greek, thread-count, scatter-only, American slice, and SIMD microbenchmarks. Evidence: [`bench/portfolio_throughput_bench.cpp:277`](../../bench/portfolio_throughput_bench.cpp#L277), [`bench/portfolio_throughput_bench.cpp:303`](../../bench/portfolio_throughput_bench.cpp#L303), [`bench/american_pricing_bench.cpp:244`](../../bench/american_pricing_bench.cpp#L244). Missing gates are end-to-end route attribution, retime correctness with warmed workspaces, concurrent per-call ISA semantics, partial-Greek portfolio workloads, and a model/preset-correct American batch integrated into `PricedSurface`.

### Working tree note

At review time the relevant dirty files were `atx-vol/CMakeLists.txt`, `include/atx/vol/correction.hpp`, `include/atx/vol/counters.hpp`, `src/correction.cpp`, and `tests/correction_test.cpp`, plus untracked Python/reference/docs/sprint/example artifacts. They were preserved. The two confirmed lifecycle/concurrency defects and the portfolio-to-batch integration findings are in files not shown as modified by `git status`. Correction-cache observations should be interpreted against the current working tree, not only commit `01d88b8`.

## Prioritized recommendations

1. **Correctness gate:** make `retime` transactional and add an exact portfolio version to workspace invalidation.
2. **Concurrency gate:** eliminate per-call mutation of the global SIMD override.
3. **Clean the regression signal:** rebuild current HEAD, rerun the focused/full suite, investigate and deliberately repin or fix the prepared-portfolio golden.
4. **Define a production-resolved batch contract:** include method, exact AL preset, status/error code, fields, and local ISA selection.
5. **Integrate price-only batches first:** equal-T runs in `PricedSurface::evaluate_batch` are the natural seam. Measure full `price_into`/`price_totals`, not only boundary-kernel throughput.
6. **Add selective Greek routes and native SoA:** price+delta, price+vega, first-order, then full second-order. Use existing delta/vega fast paths as correctness references.
7. **Only then vectorize Greek stencils / revisit the AVX2 ship gate:** optimize the BAW seed and transcendentals based on end-to-end profiles.
8. **Consolidate public portfolio risk semantics:** clearly separate or unify American dollar Greeks and legacy European raw-quantity aggregation; replace the quadratic bucket scan.
9. **Close model coverage gaps:** two-boundary/PDE fallback for unsupported negative-carry regimes and surface-twist scenario support.
10. **Promote research kernels only through gates:** implicit Greeks, sigma-slice interpolation, and QD+ seed need corpus accuracy, stability, and portfolio-throughput acceptance criteria before hot-path wiring.

## Proposed acceptance gates for the next sprint

- Retime success and failure are transactional; a warmed workspace always matches a fresh workspace bit-for-bit after any supported book mutation.
- Concurrent American batch calls honor their own requested ISA/route under a deterministic overlap test.
- Current build and full test suite are green; golden changes include a written numerical reason.
- Integrated batch price matches scalar `PricedSurface::evaluate` for every method/preset/regime lane within the existing route's declared contract; error codes match exactly.
- No regression in deterministic totals across 1/2/4/8 threads.
- Route counters prove the production portfolio workload actually reaches the intended batch kernel.
- End-to-end warmed `price_into(Marks)` and `price_totals(Marks)` improve materially on representative single-name and multi-name books; isolated microkernel gains alone do not satisfy the gate.
- Partial Greek requests avoid unrequested solves/stores and bit-match the full bundle for requested fields.


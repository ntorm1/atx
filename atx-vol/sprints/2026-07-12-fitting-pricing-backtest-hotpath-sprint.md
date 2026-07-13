# Fitting -> Pricing -> Backtest Hot-Path Sprint

**Date:** 2026-07-12  
**Reviewed revision:** `3e9bb48` (`main`)  
**Data contract:** cached Databento OPRA `cbbo-1m` Parquet and fitted ATXVSA/SurfaceDb archives under `C:/atx/data`  
**Implementation profile:** `C:/atx/.agents/cpp/agent.md` (C++20, TDD, no UB, warnings-as-errors)

## Outcome

The curve optimizer is not the order-of-magnitude bottleneck. The largest losses are at
pipeline boundaries:

1. corpus/populate parallelism can nest board fan-out and expiry fan-out, creating
   approximately `H * H` runnable tasks;
2. canonical risk fitting repeatedly performs expensive American price-to-IV certification,
   while incremental refits can spend hundreds of milliseconds certifying inputs for a
   sub-millisecond optimizer;
3. auto-selection evaluates five curve families across up to eight expiries on ambiguous
   boards, even when the direct profile route is adequate;
4. the archive/backtest path discards live-session query acceleration, then recomputes cold
   American Greeks at adjacent steps;
5. several data handoffs copy or reconstruct whole boards/surfaces, and long backtests retain
   every snapshot.

This sprint keeps mathematical correctness and admission contracts fixed; exact floating-point
bits are not a product contract. Performance wins may use bounded scheduling, certified-state
reuse, fewer duplicate calculations, better data movement, and economically immaterial
algorithmic approximations under the tolerance policy below. Any route, curve-family, or
admission change is part of the output and must be explicitly tested and reported.

## Measured baseline

All numbers below were measured from Release executables on the local 16-logical-CPU host.
Timing variance is material, so acceptance comparisons use same-process medians and record CV.

| Layer | Real/synthetic fixture | Baseline |
|---|---|---:|
| Low-level fit kernel | real SPY OPRA, `fit/surface_cold/spy_real` | 50.99 ms median; 25.76 fits/s |
| Canonical HFT fit | ten real SPY regimes | 233.39 ms p50; 645.74 ms p95; worst clean in-band 98.69% |
| Auto fit | same ten real SPY regimes | 3.46-5.38 s best on first regimes; one risk admission failure |
| 20-name Robust auto-fit | real 2026-07-01 OPRA | 22.8 s wall, 109.9 CPU-s, 3.56 s fit p50 |
| 20-name Fast auto-fit | same boards | 9.8 s wall, 60.4 CPU-s, 2.71 s fit p50 |
| Dual surface v2 cold | generic liquid board | 1.21-1.55 s p50 by quality mode |
| Dual surface one-expiry update | spread-only update | 0.61-0.68 s p50; optimizer 0.34-0.41 ms |
| Portfolio full Greeks | 2,688 uniques, 8 threads | 286 ms median; 9.39k unique contracts/s |
| Portfolio marks | same book | 72.3 ms median; 37.2k unique contracts/s |
| Real SPY backtest | 123 dates, fitted OPRA archives | 1.535 s; 79.5 steps/s |
| Synthetic multi-name backtest | 19 priced steps, 160 final lots | 0.45 s median; 42.2 steps/s |

The checked-in real fitting benchmark calls the low-level eSSVI driver, not
`OptionChain -> PricerFitter -> admission -> archive -> price -> backtest`. It is a kernel
floor and cannot be used as the production throughput number.

## Implemented result (local `main`)

The first implementation pass completed the highest-leverage boundary fixes and made the
production gap measurable. Same-host Release reruns are intentionally reported with their
observed variance rather than treating one sample as a universal constant.

| Work | Result |
|---|---|
| Canonical real-OPRA facade benchmark | Added HFT mark and dual-risk cases with route, family, health, admission, quote, and slice counters. |
| Real SPY low-level kernel | 28.37 ms median, 13.1% CV, 46.7 fits/s in the closing run (baseline 50.99 ms; host load was variable, so this is a floor comparison, not an isolated causal speedup). |
| Real SPY HFT mark facade | 113 ms median, 9.4% CV, 106k quotes/s; admitted Healthy, 35 slices. |
| Real SPY latency dual-risk facade | 2.021 s median, 9.4% CV, 5.96k quotes/s; admitted Degraded, 29 served slices. |
| Surface-v2 cold | Closing Latency/Balanced/Accuracy p50 167/237/276 ms; every sample admitted. |
| Surface-v2 one-expiry update | Closing Latency/Balanced/Accuracy p50 35.7/55.2/89.7 ms versus 0.61-0.68 s baseline; input certification still consumes 31.0/48.3/75.7 ms. |
| Hierarchical fitting | Added `fit_workers` independently of valuation threads; outer board fan-out forces one inner worker, removing the `H^2` task explosion. |
| Certified reuse | Spread-only updates reuse audited IV/vega only when row eligibility and log-forward coordinate are unchanged within `1e-5`; mid/flag/hostile carry changes rebuild. |
| SurfaceDb | Stored product policy now drives the request and served health/provenance is persisted. Risk policy is re-applied after numerical overlays. |
| Selector | Added observable expiry/time budgets and harness controls. A 20-name 2 s budget reduced wall 22.8 -> 15.3 s and CPU 109.9 -> 76.7 s, but breached the 0.01 in-band gate, so exhaustive selection remains the default. |
| Chain valuation | Unset bid/ask sides no longer invoke IV inversion and have separate unset/failure counters. Closing real SPY All-fields t8 improved 1.967 -> 1.267 s (1.55x), with 4.57x t1-to-t8 scaling; valid outputs remained equivalent. |
| Fit proposal cache | Fast/Robust scalar-rate fits use certified de-Am proposals by default; Accurate/HFT, term-rate, ConvexDense, and empty-cache paths stay cold. Cold comparisons use IV and quoted-spread price gates. |
| Carry correctness | Live, priced, and archive paths geometrically interpolate forward and log-discount state, derive coherent `q_eff`, preserve exact pillars, and extrapolate tails at flat endpoint yield/rate. |
| Backtest correctness | Opening costs enter row-0 NAV/P&L; cash funding and share carry no longer double-charge `r`; expiry crossing without an exact settlement observation fails closed. `snap_to_listed=true` is rejected by the continuous model strategy and points to the listed OPRA workflow. |
| Snapshot lifetime | Private backtests use a deterministic capacity-3 coalescing LRU; caller-supplied caches remain reusable/unbounded. A 12-date test proves retained entries stay at three. |
| Selective listed pricing | Listed-dispersion roll construction requests fused Price/Delta/Vega instead of full Greeks: six legs use 27 rather than 42 American boundary solves (35.7% fewer) and match an analytic oracle. |
| Real SPY backtest | Closing internal timing 1.305 s for 122 priced steps, 93.5 steps/s versus 1.535 s / 79.5 steps/s baseline; bounded lifetime is the larger release benefit. |

The closing measurements confirm the central diagnosis: the 28 ms fit kernel is not the
2.0 s admitted dual-risk pipeline. Certification, risk construction/validation, selector
breadth, and data handoffs are the next-order costs.

## 2026-07-13 query-kernel and OPRA second pass

The follow-up review separated three workloads that had previously been compared as if they
were the same operation:

1. OPRA packet/message decode and latest-quote mutation;
2. dirty-contract analytics after quote coalescing; and
3. full-chain price/Greek refresh after a surface, spot, or carry generation changes.

An OPRA quote message names one option series; it does not imply that a venue or market maker
reruns a cold American solver for every option on every message. The official February 2025
[OPRA SIP metrics](https://cdn.opraplan.com/documents/OPRA_SIP_Metrics_February_2025.pdf)
report a 47.6 million-message/s actual peak. The September 2025
[capacity projection](https://cdn.opraplan.com/documents/notices/OPRA_Capacity_Projections_Update_0925.pdf)
projects July 2026 capacity of 13.575 million messages per 100 ms and 1.562 million per 10 ms,
with median SIP latency under 18 microseconds. The
[Pillar output specification](https://cdn.opraplan.com/documents/OPRA_Pillar_Output_Specification.pdf)
defines compact per-series messages and dynamic root sharding, while the
[multicast network specification](https://cdn.opraplan.com/documents/OPRA_Common_IP_Multicast_Distribution_Network.pdf)
describes 102 regular-hours lines plus redundant streams and gap recovery. Cboe likewise
describes consuming roughly 90 million feed messages/s while performing *millions*, not tens
of millions, of analytics calculations/s in its
[Options Analytics architecture](https://www.cboe.com/solutions/options-analytics).

The repository's current real-data acceptance fixture is Databento `cbbo-1m`: a historical
one-minute consolidated snapshot. It validates real symbols, strikes, expiries, quotes, carry,
and surface behavior, but it is not a raw OPRA packet-replay or sequence/gap-recovery test.
Feed throughput and analytics throughput therefore get separate SLAs below.

### Implemented query-kernel result

The cached-American Greek bundle previously differentiated a correction tensor with sixteen
off-point sweeps. A differentiated nested-Clenshaw jet now returns correction value, gradient,
and the required Hessian entries in one coefficient traversal. Cached price also shares one
log-moneyness calculation between Black-76 and the correction tensor.

| Release kernel, ATM put | Before | After | Change |
|---|---:|---:|---:|
| Cached American price | 3.488 us | 1.032 us | 3.38x faster |
| Cached full Greeks | 48.828 us | 3.348 us | 14.58x faster |

The whole-chain evaluator now computes model IV/price/Greeks once per consecutive expiry run,
and the selected-ID overload snapshots and values only dirty rows. The first Release real-SPY
run (14,014 contracts, 35 expiries) produced:

| RepresentativeFast field set | 1 thread | 8 threads |
|---|---:|---:|
| Model price | 19.5 ms | 4.5 ms |
| Full Greeks | 52.0 ms | 11.6 ms |
| Model IV + price + Greeks | 51.3 ms | 11.9 ms |
| Bid/ask/mid IV bands | 410.8 ms | 141.9 ms |
| All fields | 500.7 ms | 131.4 ms |

The closing warm+5 medians use deterministic 128-row same-expiry chunks and a mid-IV-first warm
start. Relative to the historical mixed-path 1.267 s number, 131.4 ms is **9.64x**, close to but
not honestly a repeated 10x result. The apples-to-apples same-process comparison is stronger:
cold/representative medians were 150.1/7.1 ms for model price (21.1x), 1023.7/16.2 ms for full
Greeks (63.1x), and 10526/304.2 ms for fully cold versus fast all-fields (34.6x). The latter is
not comparable to the old mixed-path 1.267 s run, whose bands already used cached inversion.
No one-shot or mismatched-denominator 10x ratio is treated as the SLA.

### Explicit price/Greek accuracy tiers

`QueryPricingTier` makes approximation a named configuration contract:

| Tier | Intended use | Serving behavior |
|---|---|---|
| `LegacyCompatible` | existing callers | historical cached-eSSVI/cold-override behavior |
| `ColdReference` | admission, risk, reconciliation | Andersen-Lake price and finite-difference Greeks |
| `RepresentativeFast` | backtest screening and broad refresh | one fixed representative-carry correction pair |
| `CarryBank` | tighter fast marks | bounded fixed-carry cache bank with query-time interpolation |

The representative tier is deliberately retained. On the first same-fitted-surface comparison
its median absolute price difference from cold Andersen-Lake was **$0.007666**. That is a
plausible screening error for many backtests and accompanies the order-of-magnitude whole-chain
gain. It is not sufficient by itself for unconditional execution or risk because the full-chain
p99/max were $7.97/$10.59, delta p99 was 0.0536, and rho/second-order Greek tails were much
larger. The error is concentrated in carry-distant/deep-wing contracts and is obscured by a
single median.

The final constant-weight carry bank reduced full-chain price p99/max to $2.43/$5.91 and OTM
p99/max to $0.878/$1.19. Its median price error was slightly worse at $0.009423, confirming that
carry mismatch is only one error source; broad-box Chebyshev interpolation also matters. The
35-expiry 25-delta OTM strangle error was $0.468 median/$0.902 max for representative and $0.266
median/$0.567 max for carry. Constant-weight two-cache interpolation is landed and preserves
fixed-carry theta/charm semantics. Parallel bank construction cut the live fit to 131 ms versus
106 ms representative, but archived one-use banks still have a poor construction break-even.

### Archived backtest connection and gate result

The archive bytes remain unchanged. `MarketSnapshot` now attaches transient query acceleration
before publishing its immutable `SurfaceSet`; `RunConfig` propagates the tier through both
backtest engines, base/shifted loads, prefetch, and reconciliation. `SnapshotCache` keys include
normalized path plus tier, so cold and fast snapshots cannot alias. Legacy/Cold remain the
default, and every out-of-box fast query falls back to the requested cold route.

Real 123-date SPY OPRA-fitted archives expose the amortization boundary clearly:

| Mode (fresh process, warm OS cache) | Cold | Representative | Carry |
|---|---:|---:|---:|
| End-to-end run median, 3 samples | 1547 ms | 2396 ms | 7836 ms |
| Speedup versus cold | 1.00x | 0.65x | 0.20x |
| Preload/build median | 1043 ms | 2157 ms | 6365 ms |
| Reused/preloaded engine median | 436.6 ms | 64.3 ms | 96.9 ms |
| Reused-engine speedup | 1.00x | 6.79x | 4.51x |

The representative cache therefore does **not** produce a real 10x end-to-end win for a sparse
two-leg daily strategy. It is faster after reuse, but cache construction dominates a one-pass
corpus. Parallel side/center construction improved one-shot representative 3230 -> 2031 ms and
carry 43035 -> 5350 ms before the balanced repeated run; it does not change the break-even
conclusion. Repeated parameter sweeps can share the caller-owned unbounded cache, while a one-pass
backtest should stay cold until caches are persisted, shared, or demand-built.

Economic drift versus cold, aligned over all 123 dates:

| Metric | Representative | Carry |
|---|---:|---:|
| Final NAV difference | -$14,587 | -$19,326 |
| Maximum absolute NAV-path difference | $44,689 | $47,899 |
| Daily P&L abs error median / p95 / max | $3,371 / $13,715 / $21,478 | $3,117 / $12,571 / $19,110 |
| Daily P&L sign flips | 5 | 5 |
| Sign flips with absolute cold P&L > $10k | 0 | 0 |
| Unpriced rows / wrong lot-count rows | 0 / 0 | 0 / 0 |

This is useful as a candidate screen, not as a NAV/risk publication path. The production policy
is fast-screen then cold-confirm every accepted/rejected boundary candidate and all reported
economics. A $0.0077 median option mark is retained as evidence, not treated as a strategy-level
safety certificate.

### OPRA-to-analytics target pipeline

```text
multicast line workers -> sequence/gap recovery -> symbol decode
                                              -> dense latest-quote state
                                              -> dirty bitmap/list + 10-100 us coalescer
                                              -> one-writer underlier/expiry analytics workers
                                              -> double-buffered marks/risk snapshots
```

- Line workers own packet ordering and recovery, not model state; a root can be distributed
  across multiple OPRA lines.
- A dense symbol table and structure-of-arrays quote state apply last-write-wins updates without
  constructing `OptionChain`/`ChainSnapshot` objects per message.
- Quote changes dirty only bid/ask/mid IV. Surface/spot/carry generations dirty model marks and
  Greeks. A spot burst publishes an immediate delta/gamma Taylor mark and schedules exact
  repricing instead of synchronously repricing the full board per tick.
- One writer owns each underlier's mutable analytics state. Fitted surfaces and published output
  buffers are immutable/copy-on-write, eliminating locks in readers.
- Feed decode, effective-NBBO mutations, dirty analytics, refits, and full refreshes each have a
  separate latency/throughput counter. Raw multicast replay is required before any OPRA
  messages/s claim; `cbbo-1m` remains the numerical acceptance corpus.

### Second-pass delivery plan

| Package | Concrete work | Acceptance gate |
|---|---|---|
| Q1: cached derivative kernel | Fused correction jet, cached price log reuse, correct spot-coordinate chain rules | `/W4 /WX`; price and every Greek agree with independent FD within stated epsilons; >=5x cached-Greek kernel speedup |
| Q2: explicit serving tiers | Cold, representative, and bounded carry-bank contracts; no implicit approximation from `use_correction_cache` | tier selection is observable; fitting/admission behavior is unchanged unless separately configured |
| Q3: carry interpolation | Fixed-carry cache pairs and constant query-time blending; do not differentiate blend weights into theta/charm | endpoint identity, blended price/vega/Greek FD tests; report build/query/error frontier |
| Q4: expiry-run fusion | Resolve curve/carry/cache once, fuse model and band work, use one parallel fan-out | all-fields repeated t8 median >=8x cold on real SPY; deterministic outputs/counters |
| Q5: dirty-row API | O(k) selected snapshot/value path preserving order/duplicates | work and allocations independent of full-chain size; selected rows equal canonical rows |
| Q6: caller-owned analytics state | `value_rows_into`/`value_expiry_into`, generation tags, reusable SoA buffers and dirty bitmap | zero steady-state allocation; one quote update touches O(1) state and O(k) requested analytics |
| Q7: persistent scheduling | one-writer underlier workers and a reusable bounded executor | no per-call `jthread` creation; p99 dirty-batch latency measured under concurrent underliers |
| Q8: adaptive backtest gate | calibrate by tenor/side/moneyness/liquidity; cold-confirm candidates inside the error margin | strategy decisions/NAV satisfy an explicit economic limit; no full-board median used as a safety certificate |
| Q9: raw OPRA replay | multicast decoder, sequence/gap/retransmission counters, LWW quote store, coalescer | line-rate packet replay with zero loss; separate effective-update and analytics rates |
| Q10: cache tiling/SIMD | local k/T tiles or higher-order central tiles, batch Black-76/jet evaluation | improve liquid OTM/25-delta p99 materially without losing the >=8x all-fields target |

Q1-Q5 are implemented and pass their focused gates. Q8 now has the tier plumbing, real-corpus
error evidence, and cold-fallback routing, but the adaptive calibration and automatic
cold-confirm stage are not implemented. Q6-Q7 and Q9-Q10 remain because the public API still
allocates snapshots/results, starts threads per fan-out, and has no raw multicast input contract.

#### Ordered next sprint (two-week implementation plan)

| Priority / estimate | Deliverable | Concrete tasks | Exit gate |
|---|---|---|---|
| P0, 2 days | Adaptive screen + cold confirmation | Record selected strike/quantity/achieved delta/query route; define price/delta margin bands; rerun cold only for candidates whose rank/threshold could change; publish cold economics only | zero decision mismatch versus all-cold on the 123-date corpus; >=3x total screening throughput on books above measured break-even |
| P0, 2 days | Amortized cache lifecycle | Add cache-build/query counters and a demand threshold; share immutable correction tables by full `(box,nodes,r,q,side,opts)` key; evaluate an archive sidecar for offline-built tables | sparse two-leg one-pass is never slower than cold by >5%; repeated sweeps reuse one build; no cross-tier alias |
| P1, 2 days | Caller-owned SoA evaluation | Add `value_rows_into`/`value_expiry_into`, generation tags, dirty bitmap/list, and reusable inversion scratch | zero steady-state allocations; selected six-ID path remains O(k); canonical row parity within epsilon |
| P1, 2 days | Persistent scheduling | Replace per-call `jthread` fan-out with bounded underlier workers and work-stealing expiry chunks; keep deterministic output slots/reductions | process threads <= `H+2`; p95 dirty-batch latency under concurrent underliers improves >=2x |
| P1, 1 day | Backtest state reuse | Carry target-date prepared Greeks into the next base step; index lots/UID aggregates without changing reduction order | >=40% fewer cold boundary solves; NAV/Greek differences within economic policy |
| P2, 3 days | Raw OPRA replay harness | Decode Pillar messages, sequence/gap/retransmission accounting, dense symbol-id LWW quote SoA, 10-100 us coalescer, independent feed/effective-update/analytics counters | zero loss on recorded multicast replay; publish packet, effective quote, dirty analytics, refit, and full-refresh rates separately |

The first checkpoint is the adaptive confirmation stage, not another global epsilon relaxation.
The second is cache amortization: without it, sparse archived backtests should select cold even
though the underlying cached price/Greek kernels are much faster.

#### 2026-07-13 closing verification

- Strict clang-cl Debug and Release builds passed for tests, the chain benchmark, American
  benchmark, and real SPY backtest example.
- The change-relevant Release filter passed 174 tests with three expected counters-off skips.
- The complete Release executable ran 1,409 tests: 1,393 passed, seven skipped, and the nine
  remaining failures are the already documented baseline set (four compiler-sensitive exact-bit
  American/correction pins plus five artifact-cache SPY/multiname baselines). No new suite failure
  remains.
- Selected-ID and thread-count results are deterministic; price/Greek/cache-blend comparisons use
  explicit numerical tolerances rather than an IEEE-754 identity product contract.
- The benchmark data source is identified as Databento `cbbo-1m`/OPRA-fitted archives. No raw
  OPRA message-rate claim is made.

## Correctness findings

### C0.1 Persisted SurfaceDb product policy is not the population request

`SymbolFitConfig::surface_policy` is stored, but population reconstructs only preset/curve and
selected numerical flags. A stored Risk product can therefore be fitted/persisted as the
legacy mark contract. Resolve one explicit request from stored policy and numerical overlay,
then assert that configured purpose, served purpose, archive provenance, and manifest
provenance agree.

### C0.2 SurfaceDb population drops fitter provenance

`fit_board` captures health/provenance, but `SurfaceArchiveItem` construction omits it. This
causes healthy/degraded/rejected and Mark/Risk state to round-trip as legacy/unknown.

### C0.3 Real-manifest SPY example cannot write on a fresh process

`spy_strangle_backtest` creates its output directory only on the synthetic branch. A fresh
real-manifest invocation completes the run and then fails opening its TSV. Directory creation
must be common to both branches and tested.

### C0.4 Production backtests default to exclude-and-report unpriced held lots

The library default is useful for research diagnostics but unsafe for production examples.
Production runners must explicitly choose `UnpricedLotPolicy::Error`; tests must retain the
diagnostic mode as an explicit opt-in.

### C0.5 Opening execution costs are omitted from inception NAV

Opening trades deduct costs from the cash ledger, but the inception row hard-codes NAV and P&L
to zero. The tearsheet then uses final NAV as total return, and existing attribution tests add
the first-row cost back as a special case. Initialize the post-trade row to `-opening_cost` (or
add an explicit pre-trade row) and require ordinary closure with no row-zero exception.

### C0.6 Cash financing plus share carry charges funding twice

When premium/cash financing is active, hedge-share purchases already create a cash balance that
accrues `-r`. The separate share-carry term also applies `(q-r)`, producing `q-2r`. With cash
financing enabled, share carry adds dividend yield only; without it, the combined `(q-r)`
shortcut remains valid.

### C0.7 Off-pillar forward and carry coordinates are inconsistent

Live and archived queries independently interpolate forward, `q_eff`, and rate. Even when each
pillar satisfies `F = S exp((r-q)T)`, independent interpolation does not preserve the identity.
Interpolate log-forward/discount state once and derive `q_eff` from that state at the query
maturity. This affects IV coordinates, American price/Greeks, delta-strike resolution, and
synthetic-tenor backtests.

### C0.8 Expiry crossing settles at the next snapshot spot

If a step crosses expiry, intrinsic is computed from the shifted snapshot even when that
snapshot is after the official settlement observation. Store/load settlement marks or fail
closed when an exact settlement observation is unavailable.

### C0.9 The generic OPRA-seeded strategy path is not a listed-contract backtest

The generic strategy resolver uses continuous target tenor/strike and fitted fair value;
`snap_to_listed` is documented as ignored. Label this path model-on-model and route tradeable
claims through the existing listed-OPRA identity/NBBO stack. A tradeable run must prove every
opened lot maps to an OPRA contract key and executable side.

### C1.1 Served eSSVI still uses a legacy observation population

Selector and non-eSSVI families use configured prepared observations; served eSSVI retains the
legacy compatibility population. Migrate only with a key-by-key parity scoreboard and explicit
model-change review.

### Already fixed on current main

Older reviews found non-transactional portfolio retiming, stale prepared-workspace reuse, a
process-global per-call ISA override, selector coverage bias, and uncaught fit-worker
exceptions. Current main has transactional retiming with logical revisioning, local ISA
routing, common-key selector coverage, and worker exception propagation. These are regression
contracts, not open sprint tasks.

## Performance and algorithm findings

### P0.1 Nested fitting creates an `H^2` scheduler

The outer corpus/populate scheduler runs multiple boards while each `PricerFitter` keeps the
default all-core expiry fan-out. On this host the theoretical peaks are 256 runnable fit tasks
for corpus and 128 for an eight-board populate batch. `PricerConfig::n_threads` controls chain
valuation, not fit workers.

### P0.2 Incremental refit repeats certification work

The certified cache rejects price/flag changes and falls back to rebuilding every European
observation. Spread-only updates can also touch robust-carry weights and unnecessarily fall
back even when the resolved forward is unchanged. The target fast path must:

- replay selected carry inputs;
- re-resolve carry when any selected input changed;
- reuse certified IVs only if the forward coordinate and eligibility are unchanged;
- refresh spread-derived weights without re-inversion;
- fall back to full certified reconstruction for any mid, flag, strike, or carry-coordinate
  change.

### P0.3 Selector work is unbounded relative to production latency

The default ambiguity route evaluates five families over up to eight expiries and American-
prices every held-out node. Real 100-board experiments already show that a four-expiry cap and
an explicit candidate time budget materially reduce fit CPU, but these knobs are not exposed
by the universe harness. Budget behavior must be observable and the zero-budget default must
preserve existing deterministic exhaustive selection.

### P1.1 Backtest repeats adjacent-date Greeks

P&L explain calculates base Greeks and target marks, book reporting calculates target Greeks,
and the next step recalculates those target Greeks as its base. Cache keys must include logical
book revision, snapshot timestamp, and every served surface generation.

### P1.2 Full Greek bundles are requested for partial consumers

Default finite-difference Greeks cost about 8.8 times a mark. Hedge and signal paths often need
only delta, vega, or IV. Add field-level requests and native SoA result columns before changing
any Greek method default.

### P1.3 Backtest bookkeeping is superlinear

Entry/exit membership, per-UID aggregation, and lot lookup repeatedly scan vectors. Build
stable per-step `lot_id -> index` and `uid -> aggregate` tables while preserving the existing
input-order floating-point reduction.

### P1.4 Fit-to-archive and OPRA ingestion copy too much

The batch loader materializes the whole range, examples copy frames into boards, session export
clones curve slices, and UID stamping clones again. The final design streams a bounded number of
dates and consumes fitted surfaces into archive items.

### P1.5 SnapshotCache is unbounded

The default one-pass backtest retains every reconstructed snapshot and creates an async load per
prefetch. Use a current/next bounded cache and one persistent prefetch worker.

## Configuration and curve-family contract

| Purpose/profile | Default family intent | Notes |
|---|---|---|
| Market mark, ultra-liquid ETF/index | LinearVariance / HFT | bounded retained nodes; no implicit Risk |
| Market mark, liquid single name | eSSVI / Fast | compact parametric backbone |
| Sparse/illiquid or vol product | SVI / Fast | lower capacity |
| Explicit event context | C8 when supported | fall back to eSSVI when thin/opening |
| Risk output | independently admitted shape-safe family | LinearVariance requests are promoted to ConvexDense |

The existing `spy_ytd_corpus` and `mag7_surfdb_populate` examples silently pin
`ConvexDense`, contradicting the policy table. Their default becomes `--curve auto`; an explicit
pin remains available and is printed/persisted. Curve changes are never labeled as pure
performance changes.

## Implementation work packages

### WP0 — Canonical benchmark and counters

**Files:** fitting/portfolio/backtest benchmark targets and a new real-OPRA pipeline harness.

- Time Parquet load, OptionChain build, profile/policy/selector, de-Am, slice fit,
  certification/admission, export/archive write, archive load, pricing, and backtest.
- Report route/profile/preset/family/purpose, quote counts, boundary solves, worker peak, and
  rejected samples.
- Use SPY plus at least one ordinary single name, one event-context name, and one sparse name.

**Gate:** benchmark fails if fixture fingerprint, resolved route, or admission changes.

### WP1 — Hierarchical fit budget

**Files:** `pricer_fitter.*`, `session.*`, `surface_parity.*`, corpus/populate schedulers, tests.

- Add an explicit fit-worker budget separate from valuation/pricing workers.
- Propagate it through every fit driver.
- Outer board fan-out assigns one expiry worker per board; single-board callers may use the
  full budget.
- Reuse a bounded executor/task arena in the follow-on patch; the first patch must at least
  eliminate nested fan-out.

**Gates:** peak process threads <= `H + 2`; archives/diagnostics agree within the stated
numerical and economic tolerances at budgets 1/H; MAG7+SPY one-date throughput >=6x the nested
baseline, or record the measured limit before moving to the persistent arena.

### WP2 — Surface policy and provenance correctness

**Files:** `surface_db_populate.cpp`, SurfaceDb/corpus tests, production examples.

- Resolve stored `SurfacePolicy` into `PricerConfig`.
- Preserve mandatory risk policy after numerical overlays.
- Write `FitSlot::provenance` into each archive item and manifest transaction.
- Default production examples to auto curve selection and fail on unpriced held lots.
- Fix real-manifest output directory creation.

**Gates:** Mark/Risk product intent round-trips; degraded carry-gap provenance round-trips;
fresh real-manifest example writes successfully; no silent held-lot exclusion.

### WP3 — Certified incremental observation reuse

**Files:** `session.*`, `pricer_fitter.*`, incremental tests and surface-v2 benchmark.

- Add row-keyed certified observation state.
- Re-invert only changed mids; reweight unchanged-IV rows for spread changes.
- Replay/re-resolve carry only when its selected inputs changed.
- Keep full-fit fallback for coordinate/eligibility changes.

**Gates:** non-carry one-row update input stage <=5 ms and >=10x faster; selected-carry
spread-only update <=10 ms when forward is unchanged; cold-rebuild surface and admission are
equivalent within the numerical/economic tolerance policy below; hostile carry change still
fails closed or requests a full fit.

### WP4 — Selector budget and value-chain duplicate-work removal

**Files:** `curve_selector.*`, `universe_autofit.cpp`, `pricer_fitter.cpp`, focused tests.

- Expose `oos_max_expiries`, candidate budget, sparse floor, and direct-confidence controls.
- Report candidates evaluated and budget exhaustion.
- Skip bid/ask IV inversion when the quote side is unset and count unset versus failed.
- Fuse model-IV/price/Greek resolution so the all-fields path does not price twice.

**Gates:** zero-budget default remains exhaustive; four-expiry/budgeted mode selects an admitted family;
100-board fit CPU <=60% of the unbudgeted baseline with median in-band within 0.01; all-fields
value-chain >=1.5x; valid two-sided outputs remain economically equivalent.

### WP5 — Backtest reuse and linear-time bookkeeping

**Files:** `backtest.cpp`, `portfolio_pricer.*`, backtest benchmarks/tests.

- Retain prepared portfolio/workspace by logical book revision.
- Carry target-date Greek state into the next step.
- Replace repeated vector membership scans with stable indexed/sorted structures.
- Reserve result columns and reuse survivor scratch.

**Gates:** >=40% fewer boundary solves; P&L/NAV/Greeks agree within the economic tolerance
policy; 1,000-lot x 252-date
stress run has O(N+U) bookkeeping and >=2x throughput.

### WP6 — Bounded streaming and snapshot lifetime

**Files:** OPRA batch/stream API, corpus/populate, `snapshot_cache.*`, archive loader.

- Stream at most 2-3 dates from load through fit/write.
- Move frames/boards; consume surface export once.
- Keep only current/next snapshots by default with one prefetch worker.

**Gates:** RSS effectively constant from 125 to 1,250 dates; zero QuoteFrame copies after load;
one archive open per date; archive semantics and backtest results remain economically equivalent.

### WP7 — Selective SoA pricing and curve lookup layout

**Files:** priced surface, American batch, portfolio pricer, curve surface, benchmarks.

- Add price+delta, price+vega, first-order, and full field masks.
- Write unique results directly to SoA columns.
- Store contiguous maturity metadata and reuse a single carry/curve bracket.

**Gates:** delta+vega <=2x mark cost; analytic full bundle <=65% of FD inside pinned
tolerances; real 35-pillar IV query <=2 us/query after reproducing the reported regression.

## Delivery order

1. WP0 baseline skeleton plus C0.5/C0.6 accounting fixes.
2. WP1 worker-budget fix and WP2 policy/provenance fixes.
3. C0.7 forward/carry unification and C0.8 settlement contract.
4. WP3 incremental certification reuse.
5. WP4 selector/value-chain improvements and real 100-board rerun.
6. WP5 backtest state reuse and data structures.
7. WP6 bounded pipeline lifetime.
8. WP7 selective kernels/layout.

Implemented on local `main` in this goal: WP0-WP3, the selector and unset-side portions of WP4,
bounded snapshot lifetime from WP6, and one production selective-pricing slice from WP7. WP5's
prepared-portfolio/adjacent-Greek reuse, streaming OPRA ingestion, persistent executors, and the
remaining SoA/layout work stay as measured follow-on packages; their gates were not claimed as
complete. C0.5-C0.9 are closed, including explicit model-on-model versus listed-contract
semantics.

## Release gates

- `/W4 /WX` Release build and focused Debug tests pass.
- No known-good risk surface is replaced by a rejected candidate.
- Fit and archive outputs are deterministic across worker budgets.
- Real OPRA route/family/purpose is printed and captured with results.
- Every reported speedup includes before/after medians, p95 when available, and CV.
- No benchmark claims use the low-level eSSVI case as the canonical pipeline number.
- Any intentionally changed curve selection is reviewed as a model/configuration change with
  accuracy evidence, not hidden inside a performance commit.

## First-pass closing verification (before the 2026-07-13 query-kernel pass)

- Release `/W4 /WX` builds passed for the test binary, fitting and surface-v2 benchmarks,
  chain pricer, real SPY backtest, and universe auto-fit harness.
- The final change-relevant Release run passed 1,360 tests with 7 expected skips. It excluded
  only 9 failures already documented on the reviewed `main`: 4 exact-bit American/correction
  pins, 2 stale SPY family-policy expectations, and 3 multiname legacy pins.
- The new combined hot-path regression set passed 20/20, including carry/archive identity,
  certified incremental reuse, SurfaceDb overlay/provenance, expiry fail-close, listed-strategy
  fail-close, and selective Price/Delta/Vega routing.
- `git diff --check` is clean. Unrelated pre-existing untracked workspace files were preserved.

## Numerical and economic tolerance policy

Exact IEEE-754 or archive-byte identity is not a release requirement. Optimizations may change
rounding, use a different convergent algorithm, or modestly relax an internal epsilon when all
of the following hold:

- the algorithm remains mathematically valid and terminates under its documented bounds;
- independent price-bound, butterfly, calendar, carry, inversion, and admission checks pass;
- results are deterministic within the same stated tolerance for a fixed configuration;
- route, purpose, curve family, and fallback changes are reported explicitly;
- the change is economically immaterial on the OPRA acceptance corpus.

Default materiality limits (a work package may tighten them or justify a different normalized
limit):

| Output | Acceptance limit |
|---|---:|
| Option price | <= 5% of quoted half-spread and <= $0.001/share where quotes are valid |
| Implied volatility | <= 1e-5 absolute vol (0.1 vol bp) on liquid nodes |
| Delta | <= 1e-5 absolute per share |
| Other Greeks | <= 0.1% relative or a documented small absolute floor near zero |
| Per-step/backtest NAV | <= max($0.01, 1 bp of gross step P&L) after intentional accounting fixes |

Correctness fixes such as charging previously omitted execution cost are exempt from the P&L
equivalence limit; they require an analytic identity and an explicit before/after economic
explanation instead.

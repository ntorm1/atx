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

## Closing verification

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

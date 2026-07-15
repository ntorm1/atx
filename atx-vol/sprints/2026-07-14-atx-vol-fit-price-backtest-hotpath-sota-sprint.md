# atx-vol Fit → Price → Backtest Hot-Path Sprint — "Beat SOTA on Speed & Accuracy"

**Date:** 2026-07-14
**Author:** deep-dive review (6 parallel explorer agents + SOTA web research + 3 spot-verifications against source)
**Status:** implementation in progress on local `main`. Every code claim carries a `file:line`; every optimization has an acceptance gate and a change-class note (pure-refactor vs. accuracy-trading vs. accuracy-improving). **Bit-identity is not required** — see §4's economic-correctness gate.
**North star:** make atx-vol the fastest *and* most accurate open options-pricing / vol-surface stack — beat Jäckel (IV inversion), Andersen–Lake (American), and the Vola Dynamics / SpiderRock envelope (whole-universe surface fit).

## Implementation ledger (live)

Commit SHAs are recorded after the corresponding code commit lands; ledger-only updates therefore use a following documentation commit.

| Item | Status | Commit | Verification / notes |
|---|---|---|---|
| Pre-W0 correctness and pipeline-connection prerequisite | complete | `d3aa2853ed6d3dc5234b5d95646b56bd319f3e86` | Strict Debug and Release builds; PCH-off hygiene build; integrated hot-path slice: 290 tests in each configuration, 289 passed and one expected counters-disabled skip. Adds exact risk-seed/target-mark reuse, strict provenance/admission, immutable indexed lot identity, fail-before-work validation, forced-cold economic routing, and retained pricing state. |
| W0.1 Canonical end-to-end bench | complete | `4b769ddc525804b9e90ca1a861e5bc7f21ab2284` | Real-OPRA `fit/e2e/{spy_real,100name}` plus four backtest routes, stage/corpus/policy metadata, checked-in Release JSON, and required-name CTest. Corpus rows execute one operation per process; backtest rows retain warmup/repetitions. |
| W0.2 Always-on lightweight counters | complete (host-noise caveat) | `4b769ddc525804b9e90ca1a861e5bc7f21ab2284` | One randomized TLS sample per 64 root operations; relaxed atomics occur only when publishing a sample. Interleaved 15-pair old/new process A/B found no regression (paired median new/old `0.9418`), but late host outliers drove ~85% CV, so a statistically tight `<1%` upper bound remains a pinned-host remeasurement item rather than a claimed result. |
| W0.3 Accuracy harness | complete | `4b769ddc525804b9e90ca1a861e5bc7f21ab2284` | Deterministic commit/exhaustive modes, route classification, bounded stratified fast-vs-forced-cold bundles, optional Prices-only full valuation, success/failure CTests, and checked-in 110-row CSV. Two full runs were byte-identical. |
| W1.1 Wire SIMD batch | pending | — | — |
| W1.2 Binary-search surface bracket | pending | — | — |
| W1.3 Fast query tier by default in backtest | pending | — | Representative route is a screen-only candidate until cold tail confirmation. |
| W1.4 Fix `term_rates_` detection | pending | — | — |
| W1.5 Drop diagnostic double-work on pinned surfaces | pending | — | — |
| W2.1 Bind σ-independent geometry once | pending | — | — |
| W2.2 Reusable `AloPricer` | pending | — | — |
| W2.3 Fewer + faster carry pairs | pending | — | — |
| W2.4 Closed-form PCP borrow | pending | — | — |
| W2.5 Kill redundant audit + double score | pending | — | — |
| W2.6 Warm-start adjacent strikes on cold path | pending | — | — |
| W3.1 Slice-σ shared exercise boundary | pending | — | — |
| W3.2 Fix the CrossValidation selector | pending | — | — |
| W3.3 Per-slice Legacy fallback | pending | — | — |
| W3.4 Admission correctness | pending | — | — |
| W4.1 Global dynamic board queue | pending | — | — |
| W4.2 Pool the fit-side fan-out | pending | — | — |
| W4.3 Parallel + projected OPRA ingest | pending | — | — |
| W4.4 Backtest MTM loop de-thrash | pending | — | Prerequisite retained-pricer work landed in `d3aa285`; acceptance benchmark remains. |
| W4.5 Guard H² generically + serial-below-threshold pricing | pending | — | — |
| W5.1 Table-drive CStar no-arb projection | pending | — | — |
| W5.2 Analytic CStar Jacobian + w'' | pending | — | — |
| W5.3 Vectorized Cody rational-erfc Φ | pending | — | — |
| W5.4 Trim IV iteration + fix tolerance | pending | — | — |
| W5.5 Unlock the AVX2 American boundary batch | pending | — | — |
| W5.6 Evaluate the ~60 ns IV formula | pending | — | Research stretch; may be shelved by its stated gate. |

**Representative-fast decision:** retain it as a candidate screening gate. The observed representative price error of about **$0.0077** may be economically acceptable, but speed and error claims are not accepted from a single observation. W0 measures the full distribution; any screen route must cold-confirm decision boundaries and tail cases, satisfy §4, and preserve economically meaningful backtest outcomes. Preliminary measurements did **not** establish a stable 10× end-to-end speedup, so no such claim is currently made.

**Build-throughput defect observed during verification:** Debug, Release, and hygiene trees currently share `C:\atx-cache\deps\spdlog-build`; concurrent builds can race and link mixed `_ITERATOR_DEBUG_LEVEL` objects. Gates are being run sequentially until configuration-isolated dependency output is implemented and measured.

**Wave-0 baseline and diagnosis (`4b769dd`):** the original apparent SPY pricing cost was a benchmark defect: the canonical fit row requested `Prices | Bands`, so every quote paid one model mark plus as many as three cold American-IV inversions, and Google Benchmark repeated the entire corpus. That path measured `30.315 s` total (`29.777 s` in value) for 12,040 quotes. The corrected canonical Prices-only, one-corpus-operation row measures `1.031 s` total: `997.814 ms` fit and `26.951 ms` value. The 100-name row measures `211.834 s` for 100 attempts / 94 fitted names / 257,692 valued quotes; external all-attempt timing attributes `211.028 s` to fit attempts, including `57.175 s` that successful-session-only timings previously hid in failed/unreported attempts. This confirms the remaining order-of-magnitude target is surface fitting/de-Americanization, not model-mark vectorization.

**Backtest route evidence:** cold median is `1,134.3 ms` (noisy, CV `22.64%`). Prebuilt RepresentativeFast is `41.37 ms` (27.4x faster, CV `0.97%`) but is economically invalid as an authoritative route on this fixture (`-$14,566.78` final NAV delta, `$44,669.88` maximum NAV delta, `1.2695M` maximum vega delta). Representative screen plus cold confirmation is `128.34 ms` (8.84x faster, CV `0.88%`) with `+$5.25` final NAV delta, `$12.74` maximum NAV delta, and `156.85` maximum vega delta. RepresentativeFast therefore remains screening-only; cold confirmation owns economically consequential decisions.

**Accuracy oracle evidence:** the checked-in CSV has SHA-256 `E796C2D83707D7F0017C378991C811FEA044ECDD92CBA657A18982C319A284EB`, 104 successful names and six explicit fit failures (ASTS, BRK.B, CBRS, COHR, CRDO, RKLB). Across 354,880 valid candidates, 296,028 (83.416%) classify RepresentativeFast; commit mode evaluates 6,656 deterministic stratified pairs with zero evaluation failures. Price passes the half-spread gate on 97.746% and the half-tick gate on 59.495%; only 38.552% pass the full conservative economic bundle. Median board-median price error is `$0.00046049`, board-median p95 is `$0.27172`, and the global sampled maximum is `$13.6103`. The observed `$0.0077` case is plausible and potentially useful, but is not representative of the tails.

**Additional price-only structural win:** uncached Andersen-Lake ladders now use the existing eight-node sigma-boundary interpolator in fixed 128-row, side-specific stack blocks. Boundary work becomes `8 * ceil(call_rows/128) + 8 * ceil(put_rows/128) + fallbacks` instead of one solve per row. Cached and Greek routes are unchanged; real-SPY corpus maximum price difference versus scalar cold is `$0.00003564`, with deterministic 1/8-worker output and counters-on boundary-collapse tests.

**Wave-0 verification:** strict Release build; 16/16 focused regression tests; full Release suite 1,550 passed / 7 expected skips / 0 failed across 1,557 enabled tests; four accuracy/name CTests passed; counters-on focused tests passed; PCH-disabled strict Debug build of tests, canonical benchmark, and accuracy panel passed. The audit also fixed purpose-specific mark/risk fitted-chain provenance and invalidated a semantically stale SPY fit cache; the rebuilt unchanged recipe scores 95.35% in-band versus 83.35% from the stale artifact. Historical bit pins are diagnostic only: rounding-scale movements use tight epsilon gates, while charm retains an independent cached-price cross-finite-difference oracle.

> **Scope note (read first — it reframes everything).** "Fitting" and "backtest" are two *different* CPU domains that the request conflated:
> - **Fit / ingest phase** (one-time corpus build): `load_opra_daterange` → `OptionChain::from_frame` → `PricerFitter::fit` → archive write. This is where the ~27.7 ms/name lives.
> - **Backtest MTM loop** (`run_backtest`): consumes **pre-fitted** `.atxvsa` archives; does **not** fit and does **not** read parquet. Its cost is portfolio re-pricing + book bookkeeping.
>
> The order-of-magnitude gap the user feels is dominated by the **fit phase**, and inside it by **de-Americanization** (American→European IV inversion), *not* curve solving (curve solve is 1–2% of fit CPU). The plan is sequenced around that fact.

---

## 1. Executive diagnosis — where the order of magnitude went

Three independent, previously-recorded measurements agree, and this review confirmed the mechanisms at `file:line`:

| Measurement | Value | Source |
|---|---|---|
| Mean serial single-name fit | **27.7 ms** (p50 4.5 s/board *robust auto* on heavy universe boards; direct-route single names ~0.9–2 s) | `docs/reviews/2026-07-12-universe-autofit-deep-dive.md` |
| De-Am observation build share of fit CPU | **60–77%** | universe deep-dive §CPU profile; confirmed `deamer.cpp`, `calib.cpp` |
| Curve *solving* share of fit CPU | **1–2%** | fit_timing_probe |
| CrossValidation selector share of fit CPU | **82%** (when it fires) | universe deep-dive §Routing |
| Per-option valuation | **~0.5 ms/option** (3× American IV inversions per option, no `(K,T)` dedup) | universe deep-dive §Valuation |

**Diagnosis in one line:** atx-vol spends its time *cold-inverting the same American option many times* — across borrow pairs, across strikes, across bid/ask/mid, across selector candidates, across diagnostic audit passes — while the excellent SIMD/vectorized kernels it already ships sit **unwired** on the hot path.

The single largest structural facts, each verified this session:

1. **Public `batch.cpp` API is a scalar for-loop** (`src/batch.cpp:59-62, 78-81, 99-104, 119-129, 151-158`). It never dispatches to the tested `atx::vol::simd::*` AVX2 kernels. Every caller of `black76_price_batch` / `implied_vol_batch` / `black76_greeks_batch` leaves the 4-lane SIMD layer on the floor. **✅ verified.**
2. **The de-Am carry solve inverts 12 co-terminal pairs at the *accurate* Andersen–Lake quadrature (12/24/48) for a 1e-4 target** (`include/atx/vol/deamer.hpp:239`, `src/deamer.cpp:409-431`, `al_default_opts()={12,24,8}` at `src/american.cpp:1479`).
3. **The exercise boundary is re-seeded cold and never shared across strikes** in a slice, though `andersen_lake_put_slice_sigma` (prices many strikes from ~8 boundary solves) already ships (`include/atx/vol/american.hpp:217`). **✅ mechanism verified.**
4. **`al_bind_geometry` re-pays σ-independent geometry (`geo_zc`, `wv·exp(r·u)`, `wv·exp(q·u)`) on every `price()` call** (`src/american.cpp:1438`) — i.e. on every Newton residual (~10/inversion) — when it depends only on fixed `(T,r,q)`. **✅ verified** (per-*sweep* recompute was already hoisted; per-*σ-trial* recompute remains).
5. **The CrossValidation selector runs up to 5 candidates × 8 expiries full cold fits with an unbounded `time_budget_ms=0`** (`src/curve_selector.cpp:449-537`, `include/atx/vol/curve_selector.hpp:111`), and on every measured population a hard `essvi` pin beats it on *both* speed and quality.
6. **The served/pricing path defaults to the cold query tier** (`query_accelerator_==nullptr`, `include/atx/vol/query_pricing.hpp` default `LegacyCompatible`), so a backtest reprices every option on the full cold Andersen–Lake path unless a fast tier is explicitly requested (`src/priced_surface.cpp:388`).
7. **`CurveSurface::locate` is a linear scan** over slice maturities on *every* price/greek query (`src/vol_curve.cpp:211-214`). **✅ verified.**

---

## 2. Targets to beat (SOTA, cited)

From the web-research pass. Numbers are single-core unless a full node is stated. `IV inversion` = recover Black vol from a price (~1000× costlier than a forward price).

| # | Metric | SOTA number | Method | Source |
|---|---|---|---|---|
| a | IV inversion, ns/op | **~180 ns** (machine precision, ≤2 iters); challenger **~60 ns** | Jäckel "Let's Be Rational": asymptotic seed + 2× Householder(4). Challenger: single Halley in variance space | jaeckel.org/LetsBeRational.pdf; arxiv 2604.24480, 2606.17065 |
| b | European BS price, ns/op | **~4.4 ns/op/core** (AVX-512 SP); **~0.09 ns/op** full 48-core node; GPU ~2 ns | SIMD closed-form, Xeon 8260L 10.9 B opt/s | arxiv 2204.13740v3 |
| c | American price, µs/op | **~10–22 µs/op**; ~60 µs/op for full IV inversion | Andersen–Lake–Offengenden spectral collocation (now QuantLib `QdFpAmericanEngine`) | SSRN 2547027; tastyhedge.com |
| d | Single-name surface fit | **sub-ms → sub-100 µs/name** (fast end) | Vola Dynamics SVI/SSVI/**C\***; SpiderRock cubic-spline SRCubic (full universe ~45 s cycle, ~90% within bid-ask) | voladynamics.com; docs.spiderrockconnect.com |
| e | SVI slice calibration | **sub-ms → few ms/slice** | Zeliade quasi-explicit: 5→2 param reduction, linear inner solve | zeliade zwp-0005 |

**Where atx-vol stands vs. targets:**

| Axis | atx-vol now | SOTA | Gap |
|---|---|---|---|
| Single-name surface fit | ~27.7 ms (heavy) / ~0.9 ms (light direct) | sub-100 µs → sub-ms | **~30–270×** |
| American price (served, cached tier) | Black-76 + Chebyshev (fast) | ~10–22 µs | at/near par when the cache is used |
| American price (cold, fit path) | ~170 µs/inversion (0.5 ms / 3 inversions) | ~60 µs calibration | **~3×** + 3× dedup waste |
| European batch price | scalar (~tens of ns) | ~4.4 ns/op/core | **~10–100×** (SIMD unwired) |
| IV inversion algorithm | SR-2017 rational seed + Halley | Jäckel LBR ~180 ns | **algorithmically competitive** (good seed already) |

**Strategic read:** the IV *algorithm* is already SOTA-adjacent — the loss is entirely in **how many times** it runs and **whether the vectorized kernels are wired**. Vola Dynamics ships the same "C\* nested curve" family atx already has in `cstar.cpp`; closing the arb-projection cost there (Wave 5) is a direct shot at their differentiator.

---

## 3. Findings by dimension (evidence table)

Severity: **P0** stop-ship · **P1** high-impact correctness/perf · **P2** material · **P3** localized. `dim`: C=correctness, P=perf, D=data-structures/algorithms, K=config/curve-type, T=throughput.

### 3.1 Correctness

| file:line | dim | sev | finding |
|---|---|---|---|
| `src/curve_fit.cpp:272,303,367`; `src/surface_parity.cpp:347,381,397` | C | P1 | Partial fits admitted as success: a board that fits one easy expiry and fails the rest returns Ok, `used_fallback=false`, and the robust fallback family never runs. (prior review F-02) |
| `src/dense_slice.cpp:42,130` | C | P1 | `qp_active_set` returns `Ok(x)` on iteration exhaustion though the contract promises `Internal`; `n_active` diagnostic never populated. A capped/ill-conditioned QP publishes a suboptimal surface as success. (prior review F-04) |
| `src/cstar.cpp:98-103` | C | P1 | Butterfly no-arb gate (`w''` via `(w₊−2w₀+w₋)/1e-8`) loses ~8 digits to cancellation → can **false-flag or false-clear** static arbitrage near the boundary. Analytic `w''` exists in closed form. |
| `src/snapshot_cache.cpp:24-43` | C | P2 | `SnapshotCacheKey=(path,tier)` with no mtime/content hash: re-fitting to the same path serves a **stale in-memory surface**. Mitigated only by per-date distinct paths. |
| `src/priced_surface.cpp:352-357` | C/P | P2 | `term_rates_` set by exact float `df() != exp(-r·T)` — almost never bit-equal → flag ~always true → 2 extra `std::log`/query even on genuinely flat-rate surfaces. |
| `src/implied_vol.cpp:179` + `include/atx/vol/types.hpp:71` | C | P2 | IV convergence test compares a **price** residual to `kIvTol=1e-12` (documented as vol-units). Below price noise, so it rarely fires — convergence leans on the step test and pays an extra iteration. |
| `src/american_iv.cpp:221-239` | C | P2 | Seed-down bracket capped at 16×0.93 (≈0.31× seed); if the true IV is below that and above the floor, silently clamps to `kIvMin` instead of widening the bracket. Reachable from a stale warm-start. |
| `docs/reviews/2026-07-12-…` §calendar | C | P2 | Calendar repair domain (`|k|≤0.7`) ≠ report domain (`|k|≤3`), so `calendar_arb_free` is ~always false for essvi/svi even when near-money is clean. |

### 3.2 Performance (fit hot path — the 60–77%)

| file:line | dim | sev | finding | est. |
|---|---|---|---|---|
| `src/american.cpp:1438` (`al_bind_geometry` per `price()`) | P | P1 | σ-independent geometry (`geo_zc`, 2×`exp`/node) re-paid on each of ~10 Newton residuals/inversion. Bind-static-once in the `AloPricer` lifetime. | ~1.2–1.5× per inversion |
| `include/atx/vol/deamer.hpp:239`; `src/deamer.cpp:409-431` | P/D | P1 | Carry solve de-Ams **12** pairs, each an independent fixed-point of 2 cold inversions, at the **accurate** 12/24/48 quad for a 1e-4 target. | ~1.8–2× |
| `src/deamer.cpp:109-152`; `src/calib.cpp:594` | D | P1 | Boundary re-seeded cold every FP step and per strike; never shared across strikes/legs though `andersen_lake_put_slice_sigma` (`american.hpp:217`) prices a whole slice from ~8 boundary solves. | ~2–4× (biggest structural) |
| `src/american.cpp:1331-1349` | P | P2 | `AloPricer::State` embeds ~16 KB `geo_*`; `make_unique<State>()` value-inits ⇒ ~17 KB heap + 16 KB memset **per inversion**, built per-strike-per-FP-iter. | alloc/memset elimination |
| `src/calib.cpp:608-650` | P | P1 | On the **Accurate** route, `audit_european_equiv_iv` runs a 3rd guaranteed-pass cold accurate solve/strike (the inversion already locked σ). ~1 of ~3 cold solves/strike is pure waste. | ~1.15–1.3× |
| `src/calib.cpp:566-576` | P | P2 | A 2nd "independent score" inversion per strike when scoring is on and anchor ≠ Mid. | up to 2× on scored strikes |
| `src/calib.cpp:541-542,594` | P | P2 | Cross-strike de-Am warm-start gated OFF unless `max_obs_per_slice>0` — the default cold ConvexDense fit warm-starts nothing, so every inversion starts from the European seed instead of the adjacent strike's converged σ. | ~1.1–1.3× |
| `src/dividend.cpp:95-110` | D | P2 | PCP borrow via 200-iter bisection @ 1e-10 where the map is smooth+monotone; closed form `b = -ln((lhs·e^{rT}+K)/G)/T` needs **zero** iterations. | removes ~33 evals/pair |
| `src/curve_fit.cpp:254-272` | P | P2 | When cert caches ≠ fit caches, certification does a **second full** `resolve_chain_forward` (another 300–400-solve carry) per chain. | up to 2× carry |

### 3.3 Performance (pricing / serving path)

| file:line | dim | sev | finding | est. |
|---|---|---|---|---|
| `src/batch.cpp:47-173` | P/D | P1 | Public `*_batch` API is scalar; never calls the tested AVX2 `simd::*` kernels. | **~3–4×** batch, free |
| `src/vol_curve.cpp:211-214` | D | P1 | `CurveSurface::locate` linear scan (+virtual `->T()`) per price/greek query; `PricedSurface::interp_forward` already binary-searches — the surface bracket didn't get it. | O(n_slices)→O(log) |
| `src/priced_surface.cpp:919,941-968` | P/D | P1 | Carry is hoisted per equal-T run but the **vol bracket is not**: an N-strike ladder runs N linear `locate` scans + N×2 virtual slice setups for one identical bracket. | N-1 scans removed |
| `include/atx/vol/query_pricing.hpp` default + `src/priced_surface.cpp:388` | P/K | P1 | Default tier leaves accelerator null → backtest reprices on full cold Andersen–Lake unless `RepresentativeFast`/`CarryBank` requested. Machinery exists; default just leaves it cold. | multiple× in-box |
| `src/priced_surface.cpp:529,546` | P | P2 | `resolve_with_carry` does **two** T-bracket searches over parallel arrays (one binary in `interp_forward`, one linear in `surface_.iv`). | one search removed |
| `src/portfolio_pricer.cpp:888-919` | P | P3 | `solve_pnl_uniques` issues 3 `evaluate_batch` passes/group; the shifted-iv and price passes could share one resolve when `dt==0`. | dt==0 fast path |
| `include/atx/vol/counters.hpp:42-45,184-199` | K | P2 | Cache hit/miss/fallback counters compiled out unless `ATX_VOL_COUNTERS` → **no production cache-hit visibility**. | observability |

### 3.4 Data-structures / algorithms & numerical kernels

| file:line | dim | sev | finding | est. |
|---|---|---|---|---|
| `src/cstar.cpp:542-559,97-113,561-613` | D | P1 | No-arb/calendar projection: 240-pt grid × 3× `cstar_slice_w` × 30-iter bisection × groups, recomputing a **constant** modal basis `B[i][j]` and `sqrt(theta)` each point. Table-drive → BLAS-1 over β. | **~10–50×** on arb path |
| `src/cstar.cpp:398-443`; `src/cstar_calib.cpp:90-101` | D/C | P1 | LM Jacobian via central finite-differences where analytic forms are trivial: slower **and** loses ~half the digits; recomputes `w` twice/obs/iter. | ~2–4× + accuracy |
| `include/atx/vol/detail/norm_cdf_cheb.hpp:25`; `src/math.hpp:203` | K | P2 | Φ is a degree-48 Chebyshev/Clenshaw (47-long FMA chain) + a scalar wing-patch for `|d|>6`. A ~8–14-term Cody rational erfc reaches full double precision — fewer FMAs **and** removes the wing patch (accuracy win). | ~2–3× on Φ |
| `src/iv_batch_avx2.cpp:294-297` | P | P2 | AVX2 IV does 2 unconditional Halley steps + a 3rd evaluate; most lanes converge after 1 with the SR-2017 seed. | ~1.3–1.5× IV batch |
| `src/american_boundary_batch.cpp:54` (`kShipAvx2Boundary=false`) | D | P2 | Even where wired, the AVX2 American-boundary batch is gated OFF (measured 1.6× < 2.0× gate) because the per-lane BAW seed stays scalar. Vectorize/cheapen the seed to clear the gate. | unlock 4-lane |
| `src/cstar_calib.cpp:85-86` | D | P3 | `MatX::Zero(dim,dim)` heap-allocates every `build_normal_eq_w` (per LM iter, `dim≤16`); use fixed-cap `Eigen::Matrix<double,16,16>`. | alloc removal |
| `src/curve.cpp:154-159` | D | P3 | `YieldCurve::zero(T)` does `exp` then `-log` that immediately undoes it; interpolate log-df directly. | minor |

### 3.5 Config, curve types, and pipeline throughput

| file:line | dim | sev | finding | est. |
|---|---|---|---|---|
| `src/curve_selector.cpp:449-537`; `include/atx/vol/curve_selector.hpp:111` | K/T | P1 | CrossValidation: 5 candidates × 8 expiries full cold fits, **unbounded** `time_budget_ms=0`, non-population-comparable scoring; a hard `essvi` pin beats it on speed AND quality on every measured population. | ~2–5× on CV boards |
| `src/curve_fit.cpp:233`; `src/calib.cpp:608-650` | K/C | P1 | "Strict" (Configured) prep audit drops rows below `kMinPreparedFitRows=5` → slice vanishes → whole-board admission fails ("80% failure" root cause). Per-slice Legacy fallback recovers them. | recover ~80% boards |
| `src/session.hpp:116` (`score_parity=true`) + dead correction cache | K/T | P1 | For a pinned/override served curve, `score_parity` runs a 2nd full de-Am pass and `use_correction_cache` builds ~1632 boundary solves the override never reads = 60–80% of pinned-fit CPU. | **2.8–5.0×** (measured) |
| `src/surface_db_populate.cpp:182-202` | T | P1 | Per-date static block partition + join barrier: SPY (~4× a single name) gates the whole date → **1.9 effective cores on a 12-core box**. | ~3×+ wall |
| `src/opra_batch.cpp:344-424` | T | P1 | `load_opra_daterange` is a strictly serial date×symbol loop; the slow-board tail (11.4% >50 ms) has nothing to hide behind. Prior P4a fan-out reverted/not on branch. | ~min(cores,N) |
| `src/opra_batch.cpp:409` | T | P2 | `read_parquet` with no projection — full table (all cols/rows) decoded per file per rebuild; only ~8 columns consumed. | IO reduction |
| `include/atx/vol/parallel_for.hpp:151,213`; `src/fit_scheduler.cpp:96` | T | P2 | Fit-side fan-out creates/joins `std::jthread` **per board** (two divergent schedulers); the pooled `pricing_executor` already solves this on the pricing side. | 10–30% sustained |
| `src/backtest.cpp:64-77,608,934,1003` | P/T | P1 | Shared `RetainedBookPricer` driven with alternating lot-keys → `Portfolio::create`+workspace re-reserve ~2×/step; `retime` fast-path rarely fires. `workspace_={}` discards capacity + deep-copies lots every book change. | large per-step |
| `src/pricer_fitter.hpp:184` (`fit_workers=0`) | K/T | P2 | Inner auto-fan-out unguarded generically → H² thread explosion for any outer-parallel caller that doesn't pin `fit_workers=1` (corpus/populate guard it; nothing else does). | stability |

**Curve-type inventory** (`include/atx/vol/vol_curve.hpp:67-83`): `ConvexDense(0)`, `Essvi(1)`, `Svi(2)`, `LinearVariance(3)`, `C8(4)`, `SplineVol(5)`. Default selector candidates = 5 families (Convex, Linear, eSSVI, SVI, C8); **`SplineVol` (SpiderRock SRCubic) is excluded from the v1 auto set** (`curve_selector.cpp:30-49`, appended only if `spline_candidate` set). Positive design confirmed: `SurfaceSet::find` is integer binary-search, `solve_uniques` dedups on `(uid,K,T,side)`, `instance_id_` closes the stale-surface ABA hazard, prefetch overlaps IO/compute, and the backtest has **no look-ahead bias** (`backtest.cpp:222-249,555`).

---

## 4. Sub-agent-driven development protocol

Every task below is written to be executed by an **independent implementation sub-agent** with no shared state beyond the repo. The dispatching thread (you) owns sequencing, review, and the benchmark gate.

> **Governing principle — economic correctness, not bit-identity.** Bit-identity is **not** a hard requirement anywhere in this sprint. A sub-agent is **encouraged** to keep and implement any change that (a) is algorithmically correct, (b) improves performance and/or accuracy, and (c) introduces **no economically-relevant error** (defined below) — even when it moves results beyond the last ULP. When a change deviates from the prior numeric output, the requirement is to **document the deviation in the code** (why it is correct, what it changes, the bound it holds) and to prove the economic bar on the panels. Bit-identity, where it still holds, is treated as a convenient *telltale* that a refactor was pure — not as the goal. The goal is a faster, more accurate surface.

**Economically-relevant error (the real gate).** A change is acceptable if, on the SPY + 100-name ADV panels, it holds **all** of:
- **Price:** per-option abs error ≤ `min(0.5 × tick, 0.1 × option vega × 1e-4)` and always **strictly inside** the quote half-spread it feeds (a de-Am/fit change must not move a price across its own bid/ask). No sign flips.
- **IV:** abs error ≤ **1e-4 vol points** (1 bp of vol) vs. the higher-accuracy reference, or comfortably inside the strike's bid/ask IV band — whichever is looser.
- **No new arbitrage:** butterfly (convexity), calendar (monotone total variance), and vertical (slope) constraints hold at least as well as before — an accuracy change may only *reduce* violations, never introduce them.
- **Aggregate quality does not regress:** in-band fraction ≥ prior, χ² ≤ prior, vol-RMSE ≤ prior on the 100-name panel (medians, best-of-3).

Anything inside those bounds is "no economically-relevant error" and is free to land. Anything that improves accuracy (tighter IV, fewer arb flags, better wings) is *preferred* over the status quo even if slower per-op, provided net throughput still meets the DoD.

**Per-task contract (the sub-agent MUST):**
1. **Read before write.** Open every `file:line` in the task's "Files" list and the surrounding function before editing. Confirm the finding still reproduces at HEAD (line numbers drift — grep the symbol, don't trust the number blindly).
2. **TDD.** Write/extend a test that fails for the current behavior *first*, then implement. The test asserts the **economic bound** the change claims (price/IV tolerance, arb-free, quality non-regression) against a captured higher-accuracy reference — not byte equality, unless the change is genuinely a pure hoist and byte equality is the cheapest possible check.
3. **Classify & document the change** as one of (label is descriptive telemetry for the reviewer, not a gate):
   - **`pure-refactor`** — intended to leave output unchanged (hoist/wiring/scheduling). If it happens to be byte-identical, say so; if it drifts a few ULPs, that is still fine — hold the economic bound and note it.
   - **`accuracy-improving`** — deliberately tightens results (analytic derivatives, better Φ, more arb-free). **Preferred.** Document the improvement and show the arb/quality panel moved the right way.
   - **`accuracy-trading`** — deliberately trades a *provably economically-negligible* amount of accuracy for speed (fewer borrow pairs, fast AL scheme). Document the traded bound and prove it clears the economic gate above.
   Every deviating change carries an in-code comment: *what changed numerically, why it's correct, and the bound it holds.*
4. **Benchmark best-of-3** (this laptop's turbo/thermal variance is large — the prior sprint saw 26 s↔64 s on the same config). Record wall, CPU, effective cores, p50/p95, and the accuracy triple. Compare against the Wave-0 baseline row for the same benchmark name.
5. **No new P0/P1 correctness debt.** If the task touches an admission/QP/no-arb path, add the missing status/rejection reason rather than widening a silent `continue`. Economic-correctness freedom applies to *numeric output*, never to admission/arb safety — those only get stricter.
6. **Determinism preserved.** Results must be reproducible across worker counts where the code promises it (existing `CurveFitParallel` tests); the promise is *deterministic*, not necessarily *bit-identical to the old serial value* — update the golden if a change legitimately improves it.

**Dispatch pattern:** waves are ordered by dependency. Within a wave, tasks are independent and can be dispatched in parallel (worktree isolation recommended for any wave that mutates the same TU). Land Wave 0 first — you cannot verify anything else without it.

---

## 5. The sprint — waves and tasks

Rough impact stacking (multiplicative, fit phase): W1.5 (2.8–5×) × W2 de-Am bundle (3–5×) × W4.1 scheduling (3×) targets the ~30× that turns 27.7 ms → sub-ms, with W5 pushing kernel-level accuracy past SOTA. Treat the individual estimates as directional; the Wave-0 harness produces the real numbers.

### Wave 0 — Instrumentation & baseline *(prerequisite; do first, serially)*

| Task | Goal | Files | Acceptance gate |
|---|---|---|---|
| **W0.1** Canonical end-to-end bench | Add `fit/e2e/{spy_real,100name}` and `price/backtest/*` benchmark cases that time the **blessed** `OptionChain→PricerFitter→VolaSession→to_priced_surface→run_backtest` path (not `essvi_calib_surface`). Emit per-stage timings: carry solve, obs de-Am, slice fit, audit, value. | `bench/fitting_throughput_bench.cpp`, new `bench/e2e_hotpath_bench.cpp`, `bench/baselines/*.json` | Baseline JSON regenerated on the pinned host at Release; benchmark-name coverage becomes a CI check (closes prior F-10). |
| **W0.2** Always-on lightweight counters | Promote cache hit/miss/fallback + per-inversion `exp`/solve counts to a sampling-safe always-on counter (behind a cheap atomic, not `ATX_VOL_COUNTERS` compile-out). | `include/atx/vol/counters.hpp:42-45,184-199`, `src/priced_surface.cpp:564` | Backtest prints effective cache-hit rate and cold-fallback rate; overhead < 1% measured. |
| **W0.3** Accuracy harness | One command that reports in-band fraction, χ², vol-RMSE, and bid/ask-miss on SPY + the 100-name ADV panel, from a fitted corpus. This is the regression oracle every result-moving task runs against the §4 economic bar. | new `tools/accuracy_panel.py` or `examples/accuracy_panel.cpp`, reuse `docs/reviews` fixtures | Produces a stable CSV; two runs agree to noise. |

### Wave 1 — Free wins *(pure-refactor or economically-safe; parallel-dispatchable)*

| Task | Class | Goal | Files | Gate | Est. |
|---|---|---|---|---|---|
| **W1.1** Wire SIMD batch | pure-refactor | Route `black76_price_batch`/`implied_vol_batch`/`black76_greeks_batch`/`value_and_vega_batch`/`essvi_w_batch` through `simd::have_avx2()` → `detail::*_avx2`, mirroring `simd/black76_batch.cpp:38`, `simd/iv_batch.cpp:41`. Keep scalar fallback. | `src/batch.cpp:47-173` | Existing batch tests bit-parity (kernels already tested); ~3–4× on batch bench. | **3–4×** |
| **W1.2** Binary-search surface bracket | pure-refactor | Replace `CurveSurface::locate` linear scan with `std::lower_bound` over a cached contiguous slice-T array; add a `Bracket` API and resolve it **once per equal-T run** in `evaluate_batch`, calling `slice->w(k)` directly. | `src/vol_curve.cpp:211-214`, `src/priced_surface.cpp:919` | Ladder pricing bench; ULP-equal on SPY. | O(log)+devirt |
| **W1.3** Fast query tier by default in backtest | pure-refactor | Plumb `RepresentativeFast`/`CarryBank` through `SnapshotCache::load` on the backtest serving path so `price_resolved` takes the Black-76+Clenshaw cached route, not cold Andersen–Lake. | `src/priced_surface.cpp:388`, `src/snapshot_cache.cpp:190`, `src/backtest.cpp:390` | In-box points within existing tier tolerance; measure MTM-loop speedup. | multiple× |
| **W1.4** Fix `term_rates_` detection | pure-refactor (fixes a spurious slow branch) | Compare with tolerance or a stored per-slice rate instead of exact `df() != exp(-r·T)`; skip the per-endpoint `-log(df)/T` on flat-rate surfaces. | `src/priced_surface.cpp:352-357` | Flat-rate surface takes constant-rate branch; term-rate surface unchanged. | 2 logs/query |
| **W1.5** Drop diagnostic double-work on pinned surfaces | pure-refactor (served bytes unchanged) | When the served curve is a non-eSSVI override or admission is mark-only, default `score_parity=false` and skip building the correction cache the override never reads. | `src/pricer_fitter.cpp:527`, `src/session.hpp:103,116`, `src/curve_fit.cpp:408-439` | **Byte-identical archive** for the pinned path (already proven size-identical); 2.8–5.0× on backfill. | **2.8–5.0×** |

### Wave 2 — De-Americanization inner-loop *(the 60–77%; W2.1–2.2 pure-refactor, W2.3–2.6 accuracy-trading within the economic bound)*

| Task | Class | Goal | Files | Gate | Est. |
|---|---|---|---|---|---|
| **W2.1** Bind σ-independent geometry once | pure-refactor | Split `al_bind_geometry` into `_bind_static` (zc, `wv·exp(r·u)`, `wv·exp(q·u)` — bound in the `AloPricer` ctor since `T,r,q` fixed) and `_bind_sigma` (`geo_v=σ·sqrt(t_u)` per `price()`). Guard with `geo_static_bound`. **Watch the known accurate-scheme regression** (obs 23864). | `src/american.cpp:692,1331-1349,1438` | ULP-equal price round-trip on both fast & accurate schemes; `ExpCalls` counter drops ~(residuals-1)×. | ~1.2–1.5×/inv |
| **W2.2** Reusable AloPricer (kill alloc/memset) | pure-refactor | Thread-local reusable `AloPricer`; reset the contract instead of `make_unique<State>()` per inversion; stop value-initializing the ~16 KB `geo_*` arrays. | `src/american.cpp:1331-1349`, `src/deamer.cpp:109-152` | No heap alloc in the inversion inner loop (verify via allocator counter); byte-identical expected (pure hoist — cheapest check). | alloc elim |
| **W2.3** Fewer + faster carry pairs | accuracy-trading | `max_borrow_pairs` 12→4–5; give the carry solve its own `al_opts=al_fast_opts()` and relax `kInnerIvTol`→~1e-4 (borrow target is 1e-4). Relax the `rmse_pcp<1e-6` reporting contract if it breaches. | `include/atx/vol/deamer.hpp:238-239`, `src/deamer.cpp:36,409-431` | Forward/borrow accuracy on SPY within existing PCP tolerance; no in-band regression on 100-name panel. | ~1.8–2× |
| **W2.4** Closed-form PCP borrow | pure-refactor | Replace the 200-iter bisection with `b = -ln((lhs·e^{rT}+K)/G)/T` (G already computed in `hybrid_forward`); expose G once per `(S,r,T,divs)`. | `src/dividend.cpp:38-40,95-110` | Borrow matches bisection to 1e-8 on a fixture sweep; removes ~33 evals/pair. | ~1.05–1.15× |
| **W2.5** Kill redundant audit + double score | pure-refactor | Skip `audit_european_equiv_iv` when `route==Accurate` (match `deamer.cpp:588`); reuse the fit inversion's σ as the score when anchor is Mid (skip the 2nd inversion). | `src/calib.cpp:566-576,608-650` | Same accepted `(K,side)` keys and σ; ~1 of 3 cold solves/strike removed. | ~1.15–1.3× |
| **W2.6** Warm-start adjacent strikes on cold path | pure-refactor | Thread `warm_call`/`warm_put` across k-sorted strikes even when `max_obs_per_slice==0`; each inversion seeds from the neighbour's converged σ. Gate behind a flag if bit-identity is contractually required. | `src/calib.cpp:541-542,594,659-665` | Newton residual count drops (counter); σ within audit tolerance. | ~1.1–1.3× |

### Wave 3 — Structural: shared boundary + selector + admission

| Task | Class | Goal | Files | Gate | Est. |
|---|---|---|---|---|---|
| **W3.1** Slice-σ shared exercise boundary | accuracy-trading (last-ULP) | Rework the de-Am inversion forward-map onto `andersen_lake_put_slice_sigma`/`_call_slice_sigma` so all strikes in a slice share ~8 boundary solves instead of one boundary per strike. Intermediate step: persist one `AloPricer` per carry leg across FP iterations. | `include/atx/vol/american.hpp:217,226`, `src/calib.cpp:594`, `src/deamer.cpp:131` | Price round-trip within tolerance; boundary-solve counter drops from O(strikes) to O(σ-nodes). | **~2–4×** |
| **W3.2** Fix the CrossValidation selector | accuracy-trading | Prepare the board's observations **once** (keyed, configured) and reuse across candidates; enforce a real `time_budget_ms`; score candidates on a common expiry/strike population with a coverage floor; select expiries by liquidity stratification, not prefix order. Consider defaulting to an `essvi` pin where CV historically loses. | `src/curve_selector.cpp:106,116,449-537`, `include/atx/vol/curve_selector.hpp:111` | On the 100-name panel: selector ok-set ⊇ pinned-essvi ok-set OR CV disabled; fit CPU on CV boards drops ≥2×; no quality regression. | ~2–5× on CV |
| **W3.3** Per-slice Legacy fallback (thin-slice rescue) | pure-refactor | On `fit_observations().size() < kMinPreparedFitRows`, retry the slice under `LegacyEssviCompatibility` prep (or the LinearVariance per-slice fallback) before dropping it and failing whole-board admission. | `src/curve_fit.cpp:226,233`, `src/calib.cpp:617-645`, `include/atx/vol/prepared_fitting.hpp:49-59` | Recovers ≥500 of the 874 "no usable slice" boards from the universe run; recovered surfaces meet the accuracy floor. | +~80% coverage |
| **W3.4** Admission correctness (fold prior F-02/F-04) | correctness | Structured `SurfaceBuildReport` with per-slice outcomes + reasons; treat coverage/QP-nonconvergence failure as a *build* failure so the fallback ladder runs; `qp_active_set` returns `Internal` on exhaustion and populates `n_active`. | `src/curve_fit.cpp:272,303,367`, `src/surface_parity.cpp:347`, `src/dense_slice.cpp:42,130,525` | One-slice board is rejected for a multi-expiry board; capped QP fails unless KKT-verified. | correctness |

### Wave 4 — Threading & pipeline throughput

| Task | Class | Goal | Files | Gate | Est. |
|---|---|---|---|---|---|
| **W4.1** Global dynamic board queue | pure-refactor (results) | Replace the per-date static partition + join barrier with one atomic-claim queue over **all** boards across all dates (reuse `run_bounded_fit_tasks`), so SPY no longer gates every date. | `src/surface_db_populate.cpp:182-202`, `src/fit_scheduler.cpp:59` | Effective cores 1.9→≥6 on the 12-core box; determinism preserved. | ~3×+ wall |
| **W4.2** Pool the fit-side fan-out | pure-refactor | Route the de-Am prepass through the persistent `pricing_executor` (or a sibling fit pool) with the existing re-entrancy guard, instead of `std::jthread`-per-board. Unify the two divergent outer schedulers. | `include/atx/vol/parallel_for.hpp:151,213`, `src/fit_scheduler.cpp:96`, `include/atx/vol/pricing_executor.hpp:3-19` | No per-board thread creation (counter); 10–30% on sustained backfill. | 10–30% |
| **W4.3** Parallel + projected OPRA ingest | pure-refactor | Fan `load_opra_daterange` out per-(symbol,date) over a pool with dynamic scheduling (disjoint slots, serial post-join); pass a column projection to `read_parquet` for the ~8 consumed columns; fingerprint-skip unchanged files. | `src/opra_batch.cpp:344-424,409`, `src/opra_panel.cpp:268` | Load wall ~min(cores,N)×; decoded bytes drop; identical frames. | ~min(cores,N) |
| **W4.4** Backtest MTM loop de-thrash | pure-refactor | Give `compute_step`/`execute`/`book_greeks` their own retained pricers (or a 2-slot cache keyed by book identity) so a stable book hits `Portfolio::retime`; stop `workspace_={}` (grow-only reserve); reuse `key_`/`alive` capacity; hoist the `alive` buffer. | `src/backtest.cpp:64-77,219-220,608,934,1003` | Per-step allocations drop to ~0 for a stable book; PnL byte-identical expected (pure scheduling/alloc change). | large per-step |
| **W4.5** Guard H² generically + serial-below-threshold pricing | pure-refactor | Pin inner `fit_workers=1` (or a shared budget) whenever an outer scheduler owns the machine, generically — not just corpus/populate. Clamp per-step MTM pricing to 1 thread below ~8 unique contracts. | `src/corpus_board_fit.cpp:231`, `src/backtest.cpp:271`, `include/atx/vol/parallel_for.hpp:41-45` | No oversubscription under nested parallelism; small-book steps stop paying pool dispatch. | stability |

### Wave 5 — Kernel & accuracy (push past SOTA on both axes)

| Task | Class | Goal | Files | Gate | Est. |
|---|---|---|---|---|---|
| **W5.1** Table-drive CStar no-arb projection | pure-refactor | Precompute the fixed-grid modal basis `B[i][j]` + base/base'/base'' once; hoist `sqrt(theta)`; make `w,w',w''` a BLAS-1 sweep over β. | `src/cstar.cpp:542-559,97-113,561-613` | Same projection result within tolerance; **10–50×** on the arb-projection bench. | **10–50×** |
| **W5.2** Analytic CStar Jacobian + w'' | accuracy-improving | Replace central finite-differences with closed-form `f'(z)` and modal derivatives; fuse `w` with the gradient; fixes the Roper butterfly-gate FD noise. Fixed-cap Eigen matrices. | `src/cstar.cpp:98-103,398-443`, `src/cstar_calib.cpp:85-101` | Gradient matches analytic reference; butterfly gate no longer false-flags on the fixture; ~2–4× on `build_normal_eq_w`. | ~2–4× + accuracy |
| **W5.3** Vectorized Cody rational-erfc Φ | accuracy-improving | Replace the degree-48 Clenshaw Φ with an ~8–14-term vectorized Cody rational erfc; **remove** the `|d|>6` scalar wing-patch (full-range vector accuracy). | `include/atx/vol/detail/norm_cdf_cheb.hpp:25`, `src/math.hpp:203`, `src/vector_math.hpp:136,165` | Φ error ≤ current across full range incl. wings; wing-patch deleted; ~2–3× fewer FMAs. | ~2–3× on Φ |
| **W5.4** Trim IV iteration + fix tolerance | pure-refactor | Gate the 2nd Halley step behind a per-lane residual mask (AVX2 IV); fix the mis-scaled `kIvTol` (relative price tol or rely on the vol-step test). | `src/iv_batch_avx2.cpp:294-297`, `src/implied_vol.cpp:179`, `include/atx/vol/types.hpp:71` | Machine-precision IV maintained; ~1.3–1.5× IV throughput. | ~1.3–1.5× |
| **W5.5** Unlock the AVX2 American boundary batch | pure-refactor | Vectorize/cheapen the per-lane BAW seed so the batch clears the 2.0× ship gate (currently 1.6×, `kShipAvx2Boundary=false`); wire the batch into the (post-W3.1) slice-σ de-Am path. | `src/american_boundary_batch.cpp:54`, `src/american_boundary_avx2.cpp:101-128` | Batch ≥2× scalar on the boundary bench; flag flipped on; bit-parity with scalar. | unlock 4-lane |
| **W5.6** (stretch) Evaluate the ~60 ns IV formula | research→accuracy-trading | Prototype the 2025 single-Halley-in-variance-space explicit IV formula against SR-2017+Halley; adopt if it holds machine precision at lower cost. | new `bench/iv_shootout_bench.cpp`, `src/implied_vol.cpp` | Median error ≤ 1.6e-16 at < current ns/op, else shelve. | up to ~3× IV |

---

## 6. Sequencing & dependencies

```
Wave 0  (baseline + counters + accuracy oracle)         [BLOCKS ALL — land first]
   │
   ├── Wave 1  free wins            (W1.1..W1.5 parallel; all measured against W0)
   │
   ├── Wave 2  de-Am inner loop     (W2.1,W2.2 first → they de-risk W2.3..W2.6)
   │        │
   │        └── Wave 3  structural  (W3.1 depends on W2.2 AloPricer reuse;
   │                                 W3.2/W3.3/W3.4 independent)
   │
   ├── Wave 4  threading/pipeline   (W4.1..W4.5 mostly independent of 1–3;
   │                                 W4.4 independent of fit entirely)
   │
   └── Wave 5  kernels/accuracy     (W5.1..W5.6 independent; W5.5 wants W3.1 landed)
```

- **Land order for fastest visible ROI:** W0 → W1.5 (measured 2.8–5×, byte-identical) → W1.1 (3–4× batch, free) → W4.1 (3× wall) → W2 bundle → W3.1. That path alone targets the 30× that closes the surface-fit gap.
- **Every result-moving task (accuracy-trading *and* accuracy-improving: W2.3, W2.6, W3.1, W3.2, W5.2, W5.3, W5.6) gates on the W0.3 accuracy panel against the §4 economic-error bar** — prove price/IV inside the documented bound, no new arb, aggregate quality non-regressing. Accuracy-*improving* tasks additionally show the arb/quality panel moved the right way. Bit-identity is never the gate; the economic bound is.
- **Worktree isolation:** W2.* and W3.1 all touch `american.cpp`/`deamer.cpp`/`calib.cpp` — serialize or worktree them to avoid conflicts; W1, W4.3, W4.4, W5 touch disjoint TUs and can run fully parallel.

---

## 7. Definition of done (sprint-level gates)

| Gate | Target |
|---|---|
| Single-name serial fit (SPY, Release, best-of-3) | **< 1 ms** direct route (from ~27.7 ms heavy / ~0.9 ms light); stretch sub-500 µs |
| Full 100-name panel fit CPU | ≥ **10×** reduction vs. the W0 baseline, no in-band/χ² regression |
| Backtest MTM loop | per-step allocations ~0 for a stable book; cached tier hit-rate reported and > 90% in-box |
| European batch price | within ~2× of the ~4.4 ns/op/core AVX-512 target on this ISA (SIMD wired) |
| Accuracy (100-name panel) | in-band fraction ≥ current, χ² ≤ current, vol-RMSE ≤ current, **zero** false butterfly-arb flags from FD noise; net accuracy **improved** where W5 lands |
| Economic bound | every accuracy-trading change proven inside the §4 bar (price ≤ min(½ tick, 0.1·vega·1e-4) & inside half-spread; IV ≤ 1e-4 vol; no new arb) and documented in-code |
| Correctness | no partial-fit-as-success; QP reports non-convergence; snapshot cache cannot serve stale |
| Observability | always-on cache-hit + per-stage timing; canonical e2e benchmark names gated in CI |

---

## 8. Appendix — provenance

- **Evidence base:** 6 parallel explorer agents (fit de-Am hot path, fit orchestration/config/curve-selection, pricing path, backtest/pipeline throughput, numerical kernels, SOTA web research), the two prior review docs (`docs/reviews/fitting_pipeline_code_review.md`, `docs/reviews/2026-07-12-universe-autofit-deep-dive.md`), and the `2026-07-11` backfill-throughput sprint (RC1–RC4).
- **Spot-verified this session:** `src/batch.cpp:47-173` (scalar batch — confirmed), `src/vol_curve.cpp:202-221` (`locate` linear scan — confirmed), `src/american.cpp:680-724,1428-1481` (`al_bind_geometry` per-`price()` σ-independent recompute — confirmed, with the nuance that per-*sweep* recompute was already hoisted).
- **Known landmine:** the geometry hoist has a prior accurate-scheme regression (memory obs 23864) — W2.1 must re-run the accurate-scheme round-trip, not just fast.
- **SOTA source URLs** are inline in §2; only Jäckel LBR (a) and the Intel AVX-512 BS throughput (b) are hard primary numbers — treat (d)/(e) vendor envelopes as directional.
```

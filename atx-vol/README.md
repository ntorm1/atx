# atx-vol

Equity-options pricing and volatility-fitting library — a C++20 port of the C17
`ats-vol` library, refactored to the atx house style (`.agents/cpp/agent.md`)
and layered above `atx-core`.

## Status: full port

The upstream `ats-vol` (~65k LOC of C across ~90 translation units) has been
ported in full to idiomatic, tested C++20, and then extended with a
Vola-Dynamics American-equity parity layer (see below), a composable
`VolaSession` handle, and a cached high-performance pricing hot path. It passes
**584 GoogleTest cases across 118 suites** under `/W4 /permissive- /WX` (clang-cl).
The C library's arena / SoA-slab / thread-local / hand-written-AVX2 machinery is
re-expressed with `std::vector` + Rule of Zero + `atx::core` (error vocabulary,
linear algebra, hashing) rather than transliterated; the numerics are ported
faithfully and mirror the upstream test tolerances.

## Ported modules

| Layer | Header(s) | What it does |
|---|---|---|
| Types / version | `types.hpp`, `version.hpp` | `Side`, `ExerciseStyle`, numeric constants, `atx::core` error re-exports |
| Black-76 | `black76.hpp` | European price, price+aux (d1/d2/Φ), fused price+vega, price-from-log-moneyness |
| Greeks | `greeks.hpp` | Analytic B76 delta/gamma/vega/theta/rho/vanna/volga/charm |
| Implied vol | `implied_vol.hpp` | Stefanica–Radoicic (2017) seed + Halley inversion → `Result<double>` |
| American pricers | `american.hpp`, `correction.hpp` | Andersen–Lake (Gauss-Legendre + Chebyshev boundary), BAW, American Greeks, Chebyshev American-minus-European correction cache, Crank–Nicolson PDE oracle |
| Surface eval | `surface.hpp` | Lightweight SVI / eSSVI per-slice closed-form evaluators |
| Surface (calibration-grade) | `vol_surface.hpp` | `VolSurface` (tagged eSSVI/SVI + full slice params), backbone/residual/total eval, grad3/grad4, Mingone reparam, φ_max |
| Rates curves | `rates_curve.hpp` | Yield (Fritsch–Carlson) / forward / dividend / borrow (HTB) curve set |
| Universe | `universe.hpp` | Ticker↔uid interning, SoA per-(uid,expiry) chain registry, quote ingest, LRU eviction |
| Arbitrage | `arb.hpp` | Calendar + butterfly (Roper density) checks, SVI-MM admissibility, calendar projection/repair, quote pre-fit filters |
| Robust math | `detail/robust.hpp` | Huber loss/weight, strided Huber weights, order-statistic quantile |
| Calibration infra | `calib.hpp` | `CalibOpts`, `FitObs`, observation builder (IV-inversion + vega-spread weighting + filter cascade), accept-predicate |
| eSSVI calibrator | `essvi_calib.hpp` | Cube-space Levenberg–Marquardt (analytic Mingone Jacobian) + IRLS-Huber + wing residual + sequential driver + calendar projection |
| SVI calibrators | `svi_calib.hpp` | Zeliade quasi-explicit (BLLS + Nelder–Mead), Martini–Mingone constrained LM, raw↔JW |
| C8 family | `c8.hpp`, `c8_calib.hpp` | SVI-JW backbone + ATM/wing curvature bumps (8p), basis, raw↔JW, Roper arb projection, IRLS-Newton calibrator |
| CStar family | `cstar.hpp`, `cstar_calib.hpp` | C16M modal parametrization (base + 11 modal coeffs, C5/C8/C12/C16 tiers), block LM, calendar/butterfly arb |
| Vol derivatives | `derivatives.hpp` | Variance-swap strip, Carr–Lee vol swap, aged/marquee PnL, realized-vol tracker |
| Profile registry | `profile.hpp` | Underlier classification, per-profile calib/filter knobs, optimization-level + refit cadence + tier priority |
| Unified fit policy | `fit_policy.hpp` | Board/profile/session/event routing to an effective preset + curve; direct high-confidence routes and held-out ambiguity fallback |
| Portfolio | `portfolio.hpp` | Leg/book/agg types, bulk pricer + select, portfolio pricing + 8-Greek aggregation by agg-mode |
| Portfolio risk | `portfolio_risk.hpp` | Scenario/shock revaluation, plan/resolve/explain (PnL attribution) |
| Projection | `projection.hpp` | Projection spine: forward-T interp, coord conversion, inserted-slice eval, delta-solve, project-compare |
| Contract projection | `contract_projection.hpp` | Relative maturity/strike definitions to absolute-expiry American contracts, marks, and Greeks; prepared deterministic batch API |
| Surface archive | `surface_archive.hpp` | ATSVSA binary format writer/reader, CRC-32C, schema-hash guard, symbol lookup, concurrent-read-safe |
| Data ingestion | `data.hpp` | SpiderRock quote-frame model, install-into-universe path, ISO/year-fraction kernels |
| Batch kernels | `batch.hpp` | Scalar-backed batch B76 price/vega/from-lnfk, IV, Greeks, eSSVI-w (batch == scalar, bit-exact) |
| American IV | `american_iv.hpp` | Invert an American premium → implied vol (safeguarded Newton on the American pricer); the de-Americanization primitive |
| Dividends / borrow | `dividend.hpp` | Hybrid dividend forward (escrowed cash → proportional blend, `S=S̃+D`), European put-call-parity borrow solver |
| De-Americanization | `deamer.hpp` | Chain → European-equivalent IVs + per-term implied borrow (OTM-leg selection, fixed-point American-PCP borrow, q_eff bridge) |
| Fit metrics | `fit_metrics.hpp` | Per-vol error bars from bid-ask, reduced-χ², "minimum edge" band, avE5 |
| Parity metrics | `parity.hpp` | Re-Americanized fair-value-within-bid-ask fraction, mid-RMSE (vol & price), reduced-χ², edge band |
| S3 / SSVI curve | `s3.hpp` | Exact Vola S3 shape curve in normalized-strike form, wings `C±`, analytic Roper `g`, ATF butterfly bound |
| Panel fixtures | `panel.hpp` | Known-truth synthetic American-equity chain generator + exception-free CSV chain loader |
| Parity harness | `surface_parity.hpp` | Multi-expiry de-Am → fit → re-Am → metrics; interpolation + calendar-arb parity |
| Composable session | `session.hpp` | `VolaSession` — build once from a snapshot, then query `iv`/`fair_value`/`greeks`/`diagnostics` at any `(K,T)`; owns a per-side `CorrectionCache` hot path |
| Unified library layer | `chain.hpp`, `pricer_fitter.hpp` | `OptionChain` (id-addressed board, tick-to-quote update) → `PricerFitter` (fit + owns `unique_ptr<FittedSurface>`) → deterministic multi-threaded `value_chain` with per-field output flags |
| Real-data loader | `opra_panel.hpp` | Databento OPRA cbbo-1m (NBBO) Parquet → `QuoteFrame`, OSI/OCC symbol parser, front-PCP spot implication |
| High-throughput portfolio | `portfolio_pricer.hpp` | Contract dedup, bounded build hints, parallel SoA scatter, price-only quote cadence or full American-Greeks risk cadence |

## Unified auto policy (2026-07)

`PricerConfig{}` now classifies the live board and directly routes dense ETFs and
dense event boards to the 48-node HFT linear-variance path; sparse, vol-product,
and event-specific parametric routes remain configurable through `FitContext` and
`FitPolicyConfig`. On the ten-slice real SPY matrix the default-auto path is
**88.49 ms p95** with a **98.61%** worst-slice clean price-in-NBBO score. The
fourteen-board breadth corpus and million-row quote results are documented in
[the unified fit-policy design](../docs/superpowers/specs/2026-07-09-atx-vol-unified-fit-policy.md).

## Vola Dynamics parity (American equity options)

A second layer brings the library to feature + fit parity with the Vola Dynamics
analytics library for **American equity options**, following Klassen's published
methodology (*Arbitrage-Free Parametric Volatility Surfaces and Real-Time
Fitting*, 2017). See `docs/superpowers/specs/2026-07-04-atx-vol-vola-parity-design.md`
for the full gap analysis.

**The pipeline (Vola's workflow).** American quotes are light exotics, so all
fitting happens on *European-equivalent* implied vols:

```
American chain → de-Americanize (imply borrow per term via American PCP,
                 invert each American premium → European-equivalent IV)
              → fit an arbitrage-free parametric curve (eSSVI / SVI / C8 / CStar)
              → re-Americanize the fitted vols → model American fair value
              → parity metrics (RMSE, fair-value-%-within-bid-ask, reduced-χ²)
```

- **De-Americanization** (`american_iv.hpp`, `deamer.hpp`): the European-equivalent
  IV is the lognormal σ that reprices the American quote through the American
  pricer; recovered by a safeguarded Newton that is self-consistent by
  construction. The OTM leg is inverted per strike (least early-exercise
  premium). σ is only identifiable where the *American* option carries time
  value — deep-ITM early-exercise quotes are correctly reported at the vol floor.
- **Dividends & borrow** (`dividend.hpp`): hybrid model `S = S̃ + D` (escrowed
  cash blending to proportional), with the per-term borrow implied from
  put-call parity — the discrete-cash-dividend handling that is Vola's
  differentiator. `blend=0, borrow=0` reproduces the existing Battig-Jarrow
  escrowed forward bit-for-bit.
- **Curve family** (`s3.hpp` + existing `eSSVI`/`SVI`/`C8`/`CStar`): Vola's
  nested curves S3 → C5..C12 map directly onto atx-vol's existing
  `eSSVI`/`SVI-JW`/`C8`/`CStar` tiers (the C `ats-vol` library was itself
  modeled on this family). `s3.hpp` adds the exact 3-parameter S3/SSVI shape
  curve `σ²(z)=σ₀²[½(1+s₂z)+√(¼(1+s₂z)²+½c₂z²)]` with its analytic Roper
  density and closed-form ATF butterfly bound `s₂²≤(4+2c₂)/(1+¼σ̂₀²)`; its
  density matches `arb.hpp`'s Roper `g` to machine precision.
- **Fit objective** (`fit_metrics.hpp`): reduced-χ² (Vola's primary metric) plus
  per-vol error bars from bid-ask spreads and the "minimum edge" band (a model
  vol inside the error band carries no statistical edge) — on top of the
  existing vega/spread IRLS-Huber weighting and interval/anchor loss.
- **Parity metrics** (`parity.hpp`): fraction of re-Americanized fair values
  inside the bid-ask, mid-RMSE, reduced-χ².

**Acceptance harness** (`surface_parity.hpp`; suite
`SurfaceParity`). On a known-truth synthetic American-equity panel
(discrete cash dividend + borrow + skewed smile), the full pipeline:
recovers the injected borrow and forward; fits within bid-ask
(**fair-value-within-bid-ask ≥ 0.95**, mid-vol-RMSE ≤ 5e-3, reduced-χ² ≤ 2);
interpolates in maturity to match the truth surface (linear-in-total-variance)
and stays calendar-arbitrage-free. The event-W-shape test documents that the
3-parameter SSVI-family curves (S3 = eSSVI backbone) cannot represent negative
ATM curvature — exactly why the nested `C8`/`CStar` curves exist (Vola's
S5→C8→C12 χ² improvement).

**Composable session & high-performance hot path** (`session.hpp`). `VolaSession`
ties the whole pipeline into one handle: `build`/`from_frame` de-Americanizes and
fits every expiry into an ascending-T eSSVI `VolSurface` once, after which
`iv(K,T)`, `fair_value(K,T,side)` (re-Americanized), `greeks(...)`, and aggregate
`diagnostics()` answer cheaply with no refit (forward/carry interpolated in T).
When `use_correction_cache` is set (default), it builds a per-side Chebyshev
`CorrectionCache` over the chain's `(k,T,σ)` box and routes every American
inversion and re-pricing through the cached pricer (Black-76 + correction). The
same cache prices both legs, so the invert/re-price round-trip stays
self-consistent; a null/failed cache degrades transparently to cold Andersen-Lake.

**One-include API + named presets.** `#include "atx/vol/vol.hpp"` pulls the whole
public surface (grouped, with a 10-line quickstart in the header). Fit policy is a
single choice via `FitPreset {Fast, Accurate, Robust, Hft}` +
`make_session_inputs(preset, S, r, now)` / `apply_fit_preset(in, preset)` — no
hand-assembling the de-Am / calib / cache / repair structs. `Robust` (the
market-maker default) turns on `MonotoneFit` calendar repair for a surface that is
calendar-arb-free near-money at held quality; `Fast` is the cold-≈0.36 s hot path;
`Accurate` pins the reference Andersen-Lake preset. For repricing a whole expiry,
`fair_value_ladder`/`greeks_ladder` take a SoA strike ladder and reprice it in one
call (per-expiry context resolved once; per-strike NaN isolation), bit-identical to
the scalar path — an ergonomics/robustness primitive, not a speedup (the per-strike
cached pricer dominates: measured ~1× vs the loop, ≈6.2 µs/option).

**Tick-to-quote incremental refit.** Update the addressable board first, then ask
the owning facade to prepare, fit, admit, and publish one expiry transactionally:

```cpp
ATX_TRY_VOID(chain.update_quotes(ids, bids, asks));
ATX_TRY(const auto refit, fitter.refit_expiry(chain, expiry_id));
ATX_TRY(const auto values, fitter.value_chain(chain, OutputField::Bands));
```

The first safe tranche supports eSSVI with `CalendarRepair::None`. It warm-starts
from the published slice, rebuilds canonical carry/de-Americanized observations,
refreshes parity and aggregate diagnostics, validates the entire candidate with
the normal admission oracle, and atomically retains last-known-good state on any
failure. `MonotoneFit`, `Project`, other curve families, a different chain
instance, or dirty non-target expiries return an explicit error rather than
silently changing semantics. `VolaSession::refit_slice` remains compatibility-only
and unsafe: it accepts caller-built rows and has no transaction, admission, or
snapshot-provenance contract. The safe fit uses `essvi_fit_slice`'s optional
`warm` seed — the whole Mingone cube seeds from the prior optimum — and
`CalibOpts::prior_strength` adds a Tikhonov pull toward the prior to stabilise thin
ticks. Safe-facade performance claims must include preparation, validation, and
publication; the older optimizer-only `refit_slice` measurements do not describe
this API.

**Unified library layer** (`chain.hpp`, `pricer_fitter.hpp`; `examples/chain_pricer_bench`).
The Vola-Dynamics-style lifecycle in one object graph: an `OptionChain` is a
single-underlier board where every option is a unique `OptionId` (the packed
universe `ContractId` — no side table), built with `from_frame` and mutated by
`update_quotes(ids, bids, asks)` (tick-to-quote, keyed by id). A `PricerFitter`
takes a chain + optional `PricerConfig`, fits, and **owns the fitted surface**
(`unique_ptr<FittedSurface>` wrapping the `VolaSession`). `value_chain(chain,
fields, n_threads)` prices the whole chain into SoA columns, requesting only the
fields wanted via `OutputField {ModelPrice, ModelIV, BidIV, AskIV, MidIV, Greeks}`;
the cold bid/ask/mid American-IV inversions are embarrassingly parallel, fanned
out across `std::jthread` workers with a static block partition so the result is
**bit-identical for any thread count** (disjoint output slots, pure const reads —
the `parallel_for` determinism pattern). Model price/IV/Greeks flow through the
fitted surface's cached hot path; every number is bit-consistent with the
`VolaSession` scalar queries (the facade adds ownership + parallelism, never a
different number).

The **SOTA American-IV method** is a cheap *surrogate* in the root-find, not a
pricer (Andersen-Lake-Offengenden 2015; Longo, *Chasing Speed*, SSRN 2025;
Le Floc'h & Healy 2026): `value_chain` inverts bid/ask/mid through the session's
per-side Chebyshev `CorrectionCache` (Black-76 + one interpolation per residual,
seeded by the surface IV) instead of a cold Andersen-Lake solve per residual
(12 BAW root-finds + sweeps + quadrature + polish). That plus a dedicated
`american_vega` (one cache eval vs the full-Greeks bundle's ~7) is a ~15-20×
per-inversion win. **Measured (real SPY OPRA, 14,556 legs, `build-rel`):**
`value_chain(All)` runs **21,395 inv/s/core → 93,612 inv/s on 4 cores** (a full
43.7k-inversion board reprice in **0.40 s**, vs ~137 s for the naive cold path),
bit-identical across thread counts; the surface build (with the call-side
`andersen_lake_call_slice` boundary reuse) owns 35 slices in **245 ms**. Single-core
20-21k inv/s sits in the ~20-33k/s/core SOTA American-IV frontier. See
`examples/chain_pricer_bench` (scaling + determinism + tick-update) and
`examples/american_iv_bench` config 4 (isolated cold-vs-surrogate on a known-truth
board: cold 1.2k inv/s at 1e-9 round-trip, surrogate 20.5k inv/s at ~1e-3 near-ATM,
wing-degraded where vega vanishes).

**Cross-strike call boundary reuse** (`andersen_lake_call_slice`, `american.hpp`).
A call `C(S,K,r,q)=P(K,S,q,r)` has an internal-put strike `Kp=S` (the fixed spot),
so the Andersen-Lake early-exercise boundary is identical across a slice's call
strikes at one σ — one cold boundary solve prices the whole strike row, each price
bit-identical to a per-strike `andersen_lake`. Wired into the call-side
`CorrectionCache::build` (its k-row sampler is exactly this shape) for an
`n_strikes`-fold cut in boundary solves on the surface-fit hot path, at zero change
to the produced surface.

**Real-data proof** (`opra_panel.hpp`; `examples/opra_parity_bench`). A real
Databento **OPRA cbbo-1m (NBBO)** XOM chain slice (2026-06-05 15:55 ET) is loaded
from Parquet (OSI symbols parsed, spot implied from the front-expiry PCP forward),
de-Americanized and fit end-to-end. The Parquet is produced offline from the
cached DBN by atx-core's `opra_dbn_to_parquet` — **no API spend**. Result over
1134 contracts / 19 expiries (spot $150.16, 438 OTM quotes, 18 slices):

| metric | value | Vola reference |
|---|---|---|
| fair value within bid-ask (mean / worst) | **98.5% / 91.3%** | its headline gate |
| mean reduced χ² | **0.207** | C8 = 0.599, C12 = 0.021 |
| mean vol-RMSE | 0.019 | — |
| cold vs cached agreement (in-bid-ask, χ²) | 98.5% vs 98.5%, 0.214 vs 0.207 | self-consistent |
| **cold whole-surface fit (single-thread)** | **≈0.36 s** (438 quotes, ≈0.8 ms/quote) | Vola cold-start class |
| `fair_value` query hot path (cached vs cold) | **6.6 µs vs 103 µs (15.5×)** | — |

**Real-data proof — SPY index at scale** (`examples/spy_diag`, `tests/spy_real_test.cpp`).
The same pipeline on a real **SPY OPRA cbbo-1m** slice (2026-06-05, cached DBN → Parquet,
`SPY.OPT`): **13,889 contracts across 35 expiries** (weeklies → LEAPs), implied spot
≈$739, a steep index crash-put skew, and **penny-tight NBBO half-spreads**.

| metric | value |
|---|---|
| median **vega-weighted vol-RMSE**, liquid slices (T ≥ 1wk) | **≈0.010 (1.0 vol pt)** |
| liquid slices within 2 vol pts (vega-weighted) | **26 / 30** |
| whole-surface build, cached hot path (13.9k contracts) | **≈8.6 s** (≈0.6 ms/contract) |
| `fair_value` query hot path (cached) | **≈6.5 µs (75×)** |

Two findings the SPY slice forced, both documented rather than papered over:
- **In-bid-ask is the wrong gate at penny spreads.** SPY NBBO half-spreads are ~1¢, so a
  ~0.4 vol-pt fit already lands *outside* bid-ask — the raw in-bid-ask reads ~12% while the
  vega-weighted vol-RMSE is ~1 vol pt. Vol-RMSE (vega-weighted, near-money) is the accuracy
  metric; fraction-in-bid-ask is a tick-size metric. The apparent "failure" was a metric
  trap, not a fit failure.
- **The wing-residual layer is not a net win on SPY.** The eSSVI backbone alone already fits
  the tradeable smile to ~1 vol pt vega-weighted; the additive HingeQuad residual only
  reshapes the low-vega deep wings (which vega weighting discounts) and *over-fits* sparse
  event wings, so it stays off (matching the pre-existing `profile.cpp` note). Ultra-short
  (< ~1wk) and the deepest tails (|k| > 1.5) are separate regimes, reported separately.

A deterministic **synthetic** SPY known-truth oracle (`tests/support/spy_fixture.hpp`,
`examples/spy_surface_bench`) complements the real slice: with no quote noise the fit
recovers the truth exactly (0.0 bp ATM, 100% in-bid-ask, calendar-arb-free) — a zero-bias
round-trip check, where the real slice supplies the noisy-data accuracy number above.

**Cold-start fit performance.** The single-threaded cold XOM surface fit went from
**9.0 s → ≈0.36 s (≈25×)** with fit quality held (in-bid-ask, χ², vol-RMSE
unchanged) and all acceptance tests green. Levers, in order of impact: precompute
the Chebyshev-Lobatto barycentric nodes/weights out of the Andersen–Lake boundary
hot loop; a surface-fit `al_fast_opts()` Andersen–Lake preset (7 collocation, 16-pt
Gauss-Legendre, 2 Jacobi-Newton + 2 fixed-point sweeps) threaded as the session
default, with the IV-inversion tolerance matched to the pricer's price-accuracy
floor (a tighter IV tol than the pricer resolves collapses safeguarded Newton into
bisection — each step a full American solve — and *slows* the fit); a single
redundant per-strike inversion removed from the borrow solve (`resolve_chain_forward`);
and per-leg Newton warm-starts across the borrow fixed point. Once the cold path is
this fast, the correction cache no longer speeds up a *one-shot* surface build (it
costs more to build than it saves there) — its payoff is the repeated-query hot
path (15.5× above). **In-solve SIMD/AVX2 vectorization of the cold AL inner loop
does not help here** (see the SIMD note under *Deferred*); the separate
batch-across-contracts *marks* path is AVX2-packed and ships Auto-ON.

Run: `opra_dbn_to_parquet` then `opra_parity_bench` (both opt-in via
`-DATX_BUILD_EXAMPLES=ON`; the bench skips cleanly if the Parquet is absent).

**Calendar arbitrage (near-money CLOSED).** The raw independent-per-slice fit
crosses in total variance (55 crossings on the 18-slice XOM surface over k∈[−3,3];
26 inside |k|≤0.6). `FitPreset::Robust` / `CalendarRepair::MonotoneFit` — a
θ-floor + active-set one-sided w-floor calendar-constrained fit — clears the
near-money window **|k|≤0.6: 26 → 0 at held quality** (fair-value-in-bid-ask
98.5%→98.5%, reduced χ² 0.207→0.209, vol-RMSE 0.0190→0.0191). Contrast the
post-hoc θ-bump projection (`CalendarRepair::Project`): strictly arb-free over the
full |k|≤3 grid but it lifts a crossing slice's ATM level off market and collapses
quality (98.5%→20.4%, χ² →749) — kept opt-in for callers that require the strict
wing guarantee. Deep-wing (|k|→3, ~20σ, no quotes) strict no-arb without a θ-bump
needs a φ-slope term-structure constraint and stays deferred; Vola's
calendar-coupled joint mode with per-term error bars remains the richer target.
See `docs/superpowers/specs/2026-07-04-atx-vol-sota-hft-roadmap.md`.

**Fixtures.** Beyond the real OPRA path above, the repo commits no vendor
option-chain data, so the acceptance suites use the known-truth synthetic
generator + a CSV loader (`panel.hpp`). Matching Vola's literal RMSE-in-bps needs
a Vola/market feed that is not in-repo — the known-truth self-consistency plus
the Crank-Nicolson PDE oracle is the rigorous in-repo substitute that makes the
synthetic numbers falsifiable.

**Deferred (documented, no impact on the shipped acceptance path).** A native
discrete-cash-dividend PDE American pricer (the escrowed-forward Andersen-Lake
path is self-consistent and used for both generation and re-Americanization);
the eSSVI asymmetric-ρ / Fengler / candidate-selection research knobs; a
multi-underlier de-Americanized surface driver over the calibration pool.

## Folded into atx-core

The standard-normal special functions the pricer hot path needs were folded into
the shared numeric layer (per the port brief): `atx::core::norm_cdf` /
`norm_pdf` (plus `inv_sqrt_2pi` / `inv_sqrt_2` constants) in
`atx-core/include/atx/core/math.hpp`, covered by
`atx-core/tests/normal_dist_test.cpp`. atx-core already supplied the arena,
linear-algebra (`solve_spd` backs every LM/ridge normal-equation solve),
hashing (`hash_bytes` for the archive symbol index), and `Result`/`Status`
vocabulary the C library got from `ats-base`, so nothing else needed folding.

## Deferred (documented per-file)

Faithful numeric behavior is preserved everywhere; the following are throughput
or research-mode refinements deferred as clearly-marked `// PORT NOTE:`s, none of
which change the numerical results of the shipped paths:

- **SIMD vectorization — batch-across-contracts AVX2 is SHIPPED (Auto-ON marks);
  in-solve vectorization is not.** The American-boundary **marks** batch and the
  laned analytic-greeks bundle ship **AVX2-ON under the default `ATX_SIMD_ISA=Auto`**
  (`kShipAvx2Boundary` / `kShipAvx2Greeks`, `simd/american_boundary_batch.cpp`): on
  an AVX2 host the default marks are the 4-lane boundary pack (≈2.5–3.1× on the dev
  box, quiet-window A/B ratified), **~1e-13 USD** from the scalar oracle
  (economically nil — 10+ orders below a tick) and thread-count-invariant via the
  same-predicate tile schedule. So `batch == scalar` is **no longer bit-for-bit by
  default** on AVX2 hosts: the contract relaxes from bit-reproducible-by-default to
  reproducible-per-host. `ATX_SIMD_ISA=ForceScalar` restores the exact scalar
  boundary solve (run as a dedicated non-AVX2 test leg — `atx-vol-pricing-forcescalar`
  in `tests/CMakeLists.txt`); `ForceAvx2` forces the pack on any host; the B76 span
  batches (`batch.hpp`) stay scalar-backed.
  What is **not** vectorized is the Andersen–Lake Gauss-Legendre quad loop *within a
  single boundary solve*: **in-solve vectorization was measured two ways and both
  reverted.** That inner kernel is transcendental-bound (per point: 2×√, log, 2×exp,
  2×Φ) and already SoA + L1-resident, so there is no cache/AoS→SoA win — the only
  lever is faster vector transcendentals. (1) A portable **xsimd** AVX2 rewrite ran
  **≈6.6× slower** — xsimd's polynomial `exp`/`log`/`erfc` are far heavier than the
  SVML-backed scalar libm the loop already calls, and 4-wide throughput could not
  overcome the per-call cost. (2) **Intel SVML** vector intrinsics
  (`_mm256_exp_pd`/`_mm256_cdfnorm_pd`) do beat scalar but compile only under MSVC
  `cl.exe`; the project's **clang-cl** toolchain exposes no SVML, and the one clang
  route (`-fveclib=SVML`) would add a fragile Intel SVML runtime-DLL dependency to
  the whole library. The profitable route is exactly the **batch-across-contracts**
  SoA American solver (many independent boundaries advanced 4-wide) — which now
  **exists and is the shipped Auto-ON marks path**; the scalar kernel remains the
  in-toolchain optimum only for the single-boundary *inner* loop.
- **SpiderRock parquet decoder.** The in-memory quote-frame / install path is
  ported and tested; the bespoke ~1.4k-line Thrift/Parquet byte-decoder (built on
  `malloc`/`goto`, and with no committed `.parquet` fixture — the upstream tests
  skip when it is absent) is stubbed to `NotImplemented`, with the full column→
  field schema mapping documented for a re-port onto `atx::core::io::LazyParquet`.
- **eSSVI LM research knobs.** The load-bearing w-domain cube LM (analytic
  Jacobian) + wing residual + calendar projection + sequential driver are ported.
  The Fengler nonparametric overlay, three-way candidate selection, the
  Andersen–Lake-correction-inside-the-LM objective, and the 4D asymmetric-ρ LM are
  documented as deferred.
- **Archive side-blobs.** `VolSurface` slices round-trip bit-identically; the C
  archive's optional curve-set / profile / AL-correction-cache blob sections are
  not emitted (those types are not part of atx-vol's archived surface).

## Build & test

Wired into the top-level atx CMake as `atx-vol` (alias `atx::vol`), linking
`atx::core`. From a VS Developer shell (or via `scripts/atx-build.ps1`):

```powershell
cmake --preset dev
cmake --build build --target atx-vol-tests
ctest --test-dir build -R "Black76|Greeks|ImpliedVol|Surface|Curve|Universe|Arb|Essvi|Svi|C8|CStar|American|Correction|Portfolio|Bulk|Batch|Data|Profile|Cadence|Deriv|Realized|VolProjection|CurveProjection|SurfaceArchive"
```

Or run the full suite directly: `build/bin/atx-vol-tests.exe` (556 tests). The
Vola-parity harness alone: `atx-vol-tests.exe --gtest_filter='SurfaceParity.*:VolaSession.*:OpraPanel.*:DeAmer.*:Parity.*:AmericanIv.*:HybridDiv.*:S3.*:FitMetrics.*:Panel.*'`.

Real-data OPRA parity + throughput benchmark (opt-in examples):

```powershell
cmake --preset dev -DATX_BUILD_EXAMPLES=ON
cmake --build build --target opra_dbn_to_parquet opra_parity_bench
build/bin/opra_dbn_to_parquet.exe   # cached DBN -> Parquet (no API spend)
build/bin/opra_parity_bench.exe     # de-Am + fit + parity + cold-vs-cached timing
```

## Conventions

Follows `.agents/cpp/agent.md`: C++20, no UB, `enum class`, `const`/`noexcept`/
`[[nodiscard]]` by default, expected failures via `atx::core::Result<T>` (not
exceptions), Rule of Zero, `/W4 /permissive- /WX` clean. Public headers live
under `include/atx/vol/`; the namespace is `atx::vol`.

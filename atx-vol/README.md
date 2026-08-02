# atx-vol

Equity-options pricing and volatility-fitting library — a C++20 port of the C17
`ats-vol` library, refactored to the atx house style (`.agents/cpp/agent.md`)
and layered above `atx-core`.

## Status: full port

The upstream `ats-vol` (~65k LOC of C across ~90 translation units) has been
ported in full to idiomatic, tested C++20, and then extended with a
Vola-Dynamics American-equity parity layer (see below), a composable
`VolaSession` handle, and a cached high-performance pricing hot path. It passes
**2,618 GoogleTest cases across 377 suites** under `/W4 /permissive- /WX`
(clang-cl). That number is *measured*, not maintained — re-derive it with
`build/bin/atx-vol-tests.exe --gtest_list_tests` rather than trusting the prose,
which is how a count in a README stops rotting silently (see *Build & test* for
the ctest-side number and what the difference is).
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
| Calibration pool | `calib_pool.hpp` | Cadence priority queue + deterministic multi-underlier fan-out (`std::jthread`) |
| Vol derivatives | `derivatives.hpp` | Variance/vol-swap strip + Carr–Lee vol swap, aged-trade dispatch, capped variance/vol swaps + mid-life vol-swap dispatch (lognormal RV distribution engine, auto Carr–Lee-consistent vol-of-vol calibration), finite-difference greeks for every kind, dated idempotent fixings on the realized-vol tracker, Richardson strip quadrature-error estimate, wing trust band on strip reads (flat-vol tails beyond the certified ±0.5 fit band, `DerivConfig::wing_clamp_k`) |
| Vol-derivative book | `deriv_book.hpp` | Portfolio-layer pricing of variance/vol-swap position books against a `SurfaceSet` — the additive companion to `portfolio_pricer.hpp` for swap legs; SurfaceRef carry bridge (`detail/deriv_ref_bridge.hpp`), row-level failure + NaN-if-not-computed `PriceTotals` shared with the option pricer, `combine_totals` for one desk-level number; tenor hygiene is the caller's |
| RV distribution kernel | `detail/rv_lognormal.hpp` | Gauss-Hermite (order 21) + split-domain Gauss-Legendre (order 64) quadrature, lognormal expectation / truncated-expectation / call / sqrt-moment identities backing the capped and mid-life vol-derivative pricers |
| Swap-leg toolkit | `swap_leg.hpp` | The reusable pieces every swap-carrying strategy shares: `swap_contract_for_lot` (the engine's `SwapLot`→`DerivContract` transcription), `solve_cycle_swap` (fixing-schedule count + bridge-priced fair strike + vega-targeted qty, fail-soft), and `SwapSignalProbe` (the engine-accrual mirror behind the per-row `swap_*` greek signal columns, NaN discipline included). With `LifecycleSpec::Holding::FixedExpiryRestrike` + `StrategySpec::swap_legs` (strategy.hpp) it expresses the whole strangle-vs-varswap comparison declaratively — `examples/varswap_compare_example.cpp` → `tools/render_strangle_vs_varswap.py` |
| Profile registry | `profile.hpp` | Underlier classification, per-profile calib/filter knobs, optimization-level + refit cadence + tier priority |
| Unified fit policy | `fit_policy.hpp` | Board/profile/session/event routing to an effective preset + curve; direct high-confidence routes and held-out ambiguity fallback |
| Scenario risk | `scenario_grid.hpp` | 2-D (spot% × vol) second-order Taylor P&L matrix over a `PortfolioPricer` book — one deduped full-Greeks solve, every cell reconstructed analytically |
| PnL attribution | `pnl_attribution.hpp` | Additive base→shifted decomposition into the Vola vocabulary (spot / ATF / skew / curvature / 2nd-order vol / rates / time / unexplained) |
| Projection | `projection.hpp` | Projection spine: forward-T interp, coord conversion, inserted-slice eval, delta-solve, project-compare |
| Contract projection | `contract_projection.hpp` | Relative maturity/strike definitions to absolute-expiry American contracts, marks, and Greeks; prepared deterministic batch API |
| Surface archive | `surface_archive.hpp` | ATXVSA2 (magic `ATXVSA20`) binary format writer/reader, CRC-32C, schema-hash guard, symbol lookup, concurrent-read-safe |
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
Fitting*, 2017). See [the Vola-parity design](../docs/superpowers/specs/2026-07-04-atx-vol-vola-parity-design.md)
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

A deterministic **synthetic** SPY known-truth oracle (`atx/vol/spy_fixture.hpp` —
Tier-B and installed, so a consumer can build the same board without vendoring a
copy of the test tree — plus `examples/spy_surface_bench`) complements the real
slice: with no quote noise the fit
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
98.5%→98.5%, reduced χ² 0.207→0.209, vol-RMSE 0.0190→0.0191). The post-hoc
θ-bump projection (`CalendarRepair::Project`) used to be strictly arb-free over
the full |k|≤3 grid at the cost of lifting a crossing slice's ATM level off
market (98.5%→20.4%, χ² →749 measured on this very surface) — and the risk
pipeline pinned it for every fit, which is how the sp100-2026 database served
XOM/CVX mid-tenor ATMs up to +25 vol pts over their own quotes (see
CHANGELOG "calendar level repairs no longer fabricate slice levels"). Project
now repairs over the certified ±0.5 band only, every calendar level repair
(both surface projectors and the eSSVI/SVI/C8 pair projections at the
`fit_slice_curve` seam, which also restrict to the two slices' tradeable
overlap) is bounded by a fidelity budget (10% of the slice's pre-repair ATM
total variance) and refuses beyond it, and a refused parametric candidate
walks the family ladder to the dense model instead of serving fabricated
levels. Deep-wing (|k|→3, ~20σ, no quotes) strict no-arb without a θ-bump
needs a φ-slope term-structure constraint and stays deferred; Vola's
calendar-coupled joint mode with per-term error bars remains the richer target.
See [the SOTA/HFT roadmap](../docs/superpowers/specs/2026-07-04-atx-vol-sota-hft-roadmap.md).

**Fixtures.** Beyond the real OPRA path above, the repo commits no vendor
option-chain data, so the acceptance suites use the known-truth synthetic
generator + a CSV loader (`panel.hpp`). Matching Vola's literal RMSE-in-bps needs
a Vola/market feed that is not in-repo — the known-truth self-consistency plus
the Crank-Nicolson PDE oracle is the rigorous in-repo substitute that makes the
synthetic numbers falsifiable.

**Deferred (documented, no impact on the shipped acceptance path).** A native
discrete-cash-dividend PDE American pricer (the escrowed-forward Andersen-Lake
path is self-consistent and used for both generation and re-Americanization);
the eSSVI asymmetric-ρ / Fengler / candidate-selection research knobs. The
third item used to read "a multi-underlier de-Americanized surface driver over
the calibration pool"; the pool it named (`calib_pool.hpp`, `calibrate_pool` /
`CadenceQueue`) was deleted unbuilt at v1 (CHANGELOG, *REMOVED*). Whole-panel
multi-underlier work now lands on `corpus.hpp`, which fans fits out across
(date, symbol) boards — so the deferred item is the *de-Americanized* driver, not
a missing pool.

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
  (`kShipAvx2Boundary` / `kShipAvx2Greeks`, `src/simd/american_boundary_batch.cpp`): on
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

## Historical portfolio VaR

`var.hpp` replays a delta-defined option and stock portfolio through a
`SurfaceDb`. An option is anchored on the reference date, converted to forward
log-moneyness and signed dollar delta, then independently restruck at that same
relative expiry and moneyness on each historical base date. Its resolved units
are held into the next stored date. Stock shares are likewise resized on the
base date and held across the transition, so stock hedges retain nonzero P&L.

```cpp
#include "atx/vol/surface_db.hpp"
#include "atx/vol/var.hpp"

using namespace atx::vol;

auto db = SurfaceDb::open("surface-db/sp100-2026").value();
std::vector<VarPosition> book{
    VarOptionPosition{"SPY", ProjectedMaturitySpec::months(3),
                      0.40, Side::Call, 25.0, 100.0},
    VarStockPosition{"SPY", -750.0},
};

VarRunConfig config;
config.reference_date = "2026-06-26";
config.date_begin = "2026-01-02";
config.date_end = "2026-06-26";
config.confidence = 0.99;
config.evaluation.n_threads = 8;

auto result = run_historical_var(db, book, config).value();
// result.risk.value_at_risk, result.risk.expected_shortfall, result.frames
```

Dates are sorted ascending and adjacent transitions are split into balanced,
contiguous ranges on the persistent pricing executor. Each transition is
independent, output order is deterministic, and exact duplicate contract
definitions share pricing work while retaining separate position rows and
dollar-delta targets. The default run fails closed on a missing or non-admitted
risk surface; `VarScenarioFailurePolicy::ExcludeFromDistribution` records those
scenarios and excludes them from the nearest-rank VaR/expected-shortfall sample.

## Backtest database

`BacktestDb` persists projection-backed cookie-cutter strategy histories in
checksummed binary partitions and resumes append-only daily updates from exact
engine checkpoints. The standard builder includes a theoretical 40-delta,
three-calendar-month strangle held to expiry and delta-hedged at every stored
close. See [docs/backtest-db.md](docs/backtest-db.md) for the storage contract,
library API, and `atx-vol-backtest-db-build` workflow.

**Variance/vol-swap lane (`backtest.hpp`).** The strategy-aware engine
(`run_backtest(Clock, IStrategy, ...)`) additionally carries an OTC
variance/vol-swap book (`PortfolioState::swap_lots`), additive to the option
lane: a book with no swap lots prices, accrues and settles exactly as before
the lane existed. Every step a lot is alive takes a dated fixing
(`RealizedTracker::observe_dated`) against that step's spot; a lot settles at
its exact-match expiry off the accrued realized rate, no re-pricing needed.
No early close in v1 — a strategy that erases a live swap lot gets
`InvalidArgument`, not a close. `swap_pv` / `swap_pnl` report per-row state and
flow columns. **`BacktestDb` does not yet persist the swap lane** — its
checkpoint and series schema predate it, so `atx-vol-backtest-db-build` and
resumed/appended runs refuse (rather than silently drop) a result or
checkpoint that actually carries swap data; a zero-swap run is unaffected.
Schema support is deferred to a follow-on task.

### Strangle vs. variance swap (declarative, `strategy.hpp` + `swap_leg.hpp`)

The acceptance analysis for that lane — one fixed-expiry, daily-restriked 40Δ
strangle against one uncapped variance swap struck fair and sized to the
strangle's *entry* vega, on a single `SurfaceDb` clock, delta-hedged daily —
is a **~20-line `StrategySpec`**: a `Strangle` leg under
`LifecycleSpec::Holding::FixedExpiryRestrike` plus one
`SwapLegSpec{VarSwap, MatchGroupVega}`. The bespoke `StrangleVsVarswapStrategy`
and its 600-line driver were retired behind a track-parity gate (XOM 2026,
per-row NAV within 7e-9 dollars, swap columns bit-identical — see the
changelog). `examples/varswap_compare_example.cpp` is the whole thing; it is an
**examples-gate** target, so configure with `-DATX_BUILD_EXAMPLES=ON`:

```powershell
cmake --preset dev -DATX_BUILD_EXAMPLES=ON
cmake --build build --target atx-vol-varswap-compare-example

build/bin/atx-vol-varswap-compare-example.exe `
    C:/atx-data/surface-db/sp100-2026 XOM <outdir>/track.tsv
python atx-vol/tools/render_strangle_vs_varswap.py <outdir>/track.tsv <outdir>/report.html
```

The example **probes every partition** in the window before stepping and builds
its clock from the sessions where the symbol's surface actually exists: a live
variance swap makes the engine fail the whole run closed on a missing board, so
a dark session cannot be stepped over. `reconcile_nav` is on, so every row's
NAV is audited against an independently recomputed liquidation value.
`swap_pv`/`swap_pnl` ride out as signal columns, so `track.tsv` keeps the one
frozen `write_backtest_pnl_tsv` schema.

**Reading the report.** `pnl_total` is the *whole* step total, so the strangle
leg is `pnl_total − swap_pnl` and the two legs sum back to `nav` by
construction. Swap liveness is read off `swap_vega`, never `swap_theta` —
`swap_theta` is legitimately NaN within one bump width of expiry while the swap
is still live. `DerivGreeks::theta` holds the realized accrual fixed
(`derivatives.hpp`), so a variance swap's reported theta carries only the
term-structure/discount effect and **not** its carry; it need not share the
option book's theta sign, and on a backwardated variance term structure it is
routinely positive while `gross_theta` is negative. The option book's entry
dollar vega rides in the `options_vega` column (the retired driver called it
`strangle_vega`; the renderer reads either).

## Build & test

Wired into the top-level atx CMake as `atx-vol` (alias `atx::vol`), linking
`atx::core`. From a VS Developer shell (or via `scripts/atx-build.ps1`):

```powershell
cmake --preset dev
cmake --build build --target atx-vol-tests
ctest --test-dir build -R "Black76|Greeks|ImpliedVol|Surface|Curve|Universe|Arb|Essvi|Svi|C8|CStar|American|Correction|Portfolio|Bulk|Batch|Data|Profile|Cadence|Deriv|Realized|VolProjection|CurveProjection|SurfaceArchive"
```

Or run the full suite directly: `build/bin/atx-vol-tests.exe`. Two counts are
published here, each with the command that produces it, because they are not the
same population and a single hand-written number would hide that:

| Count | Command | What it counts |
|---|---|---|
| **2,618** cases / **377** suites | `build/bin/atx-vol-tests.exe --gtest_list_tests` | GoogleTest cases in the atx-vol test binary |
| **2,625** registered tests | `ctest --test-dir build -N -L atx_vol` | the 2,618 above, discovered by `gtest_discover_tests`, **plus 7** lanes that are not GoogleTest cases: six Python driver/report tests and the `atx-vol-pricing-forcescalar` scalar-ISA leg (`tests/CMakeLists.txt`) |

Both were measured at the commit that wrote this section, against `cmake --preset
dev`. Re-run the commands rather than trusting the digits: nothing regenerates
them, so a stale number here is a documentation defect, not a test failure.

The Vola-parity harness alone: `atx-vol-tests.exe --gtest_filter='SurfaceParity.*:VolaSession.*:OpraPanel.*:DeAmer.*:Parity.*:AmericanIv.*:HybridDiv.*:S3.*:FitMetrics.*:Panel.*'`.

Real-data OPRA parity + throughput benchmark (opt-in examples):

```powershell
cmake --preset dev -DATX_BUILD_EXAMPLES=ON
cmake --build build --target opra_dbn_to_parquet opra_parity_bench
build/bin/opra_dbn_to_parquet.exe   # cached DBN -> Parquet (no API spend)
build/bin/opra_parity_bench.exe     # de-Am + fit + parity + cold-vs-cached timing
```

### Operator CLIs (`ATX_BUILD_TOOLS`, ON by default)

Three binaries ship with the package and install into `<prefix>/bin`:

| Binary | What it is for |
|---|---|
| `atx-vol-surface-db-build` | Build a production SurfaceDb from an OPRA hive window: load, auto-generate per-symbol fit configs, cell-aware streaming populate. Resumable and idempotent. See `docs/surface-db-build.md` |
| `atx-vol-surface-db` | Inspect and manage a built database: `info` / `partitions` / `symbols` / `config` / `query` / `verify` — no Python binding required |
| `atxvol_spy_dispersion_backtest` | The listed SPY-dispersion workflow: `build-corpus`, `build-schedule`, `run-backtest`, `project-schedule`, `run-projected-backtest`, `run-surface-backtest`, `run-projected-var`, `verify`, `runarchive dump` |

**`atxvol_spy_dispersion_backtest` is spelled deliberately.** It does not match
its `atx-vol-*` siblings, and that is a decision, not an oversight: external
operator scripts invoke it by that name, so renaming it at v1 would break callers
outside this repository to buy consistency inside it. Expect the name to stay.

They are ON by default because they are part of what atx-vol *is* at v1 — a
library plus the binaries that build, inspect and replay its artifacts.
`-DATX_BUILD_TOOLS=OFF` drops exactly these three executables and their install
rules; no library, test or example target changes. The default follows
`PROJECT_IS_TOP_LEVEL`, so a project that `add_subdirectory()`s this repo (the
Python wheel build does) does not pay to link operator binaries it cannot ship.

Their sources live in `atx-vol/tools/`. That directory is the tools tier, not a
synonym for "scripts": the acceptance drivers, benchmarks and worked
demonstrations stay behind `ATX_BUILD_EXAMPLES` (OFF by default).

## Configuration registry: the `ATX_*` environment variables

atx-vol reads **ten** `ATX_*` environment variables in shipped code — nine from
the library, one from a tool. None is required, and **none of them changes a
fitted, priced or archived value.** The ones that touch parallelism select how
much of the machine is used, and every atx-vol fan-out is documented
bit-identical for any worker count; the rest decide only whether a diagnostic is
printed. So an unset environment is a complete, correct configuration, which is
the property that makes this table short.

| Knob | Effect | Default | Scope | v1 status |
|---|---|---|---|---|
| `ATX_VOL_FIT_WORKERS` | Resolves the auto (`0`) worker count for `parallel_for` fan-outs. An explicitly requested non-zero count is honoured as-is and is *not* capped by it | `hardware_concurrency()` (min 1) | library — `detail/parallel_for.hpp` | **keep** |
| `ATX_SIMD_ISA` | Seeds the process-global SIMD override at load: `Auto`, `ForceScalar` or `ForceAvx2`. An in-process `set_simd_isa_override()` still wins | `Auto` | library — `src/simd/cpu.cpp` | **keep** |
| `ATX_VOL_FIT_ECORE_TIER` | Arms the E-core second scheduling tier. `2` arms it *without* the below-normal priority drop; any other non-zero value arms it *with* the drop | unset = off | library — `src/fit_scheduler.cpp` | **keep**, deprecation candidate |
| `ATX_VOL_CORPUS_DATE_BATCH` | Dates per corpus fan-out call. Scheduling only — output bytes do not depend on it | `8` | library — `src/dispersion_run.cpp` | **keep**, belongs in a config struct |
| `ATX_VOL_CORPUS_PHASE_TIMING` | Prints the corpus build's phase split. Collection is unconditional and cheap; only the report is gated | unset = off | library — `src/dispersion_run.cpp` | **keep** |
| `ATX_VOL_PROFILE` | Prints per-phase fit timings from `curve_fit` and `surface_parity`. **Not** the CMake option of the same name — see the collision note below | unset = off | library — `src/curve_fit.cpp`, `src/surface_parity.cpp` | **keep**, rename candidate |
| `ATX_SLICE_DEBUG` | Prints `curve_fit`'s per-slice fit-preparation outcome (why a chain did or did not become a fittable slice) | unset = off | library — `src/curve_fit.cpp` | **keep** |
| `ATX_VOL_ZC_BORROW` | `0` forces the owned-reconstruct archive path instead of the zero-copy borrow. Read once per process, so it cannot make a run non-deterministic | unset = borrow allowed | library — `src/backtest.cpp` | **keep**, deprecation candidate |
| `ATX_VOL_ZC_BACKING` | `map` or `copy` overrides the caller-declared `ArchiveBacking` on the borrow path. Read once per process | unset = the caller's choice stands | library — `src/backtest.cpp` | **keep**, deprecation candidate |
| `ATX_VOL_CACHE` | Default for the dispersion CLI's `--cache DIR`; an explicit `--cache` overrides it. Empty means disabled, which is the default behaviour | unset = disabled | tool — `tools/spy_dispersion_backtest.cpp` | **keep** |

**Nothing is deleted at v1, because nothing here is dead.** Every row has a live
read site and a live effect; the two knobs with real *consumers* outside their
own source are load-bearing:

- `ATX_SIMD_ISA` is set by a **registered test**: `tests/CMakeLists.txt` runs a
  scalar leg with `ENVIRONMENT "ATX_SIMD_ISA=ForceScalar"`, which is how the
  scalar patch-paths stay honest against the AVX2-ON marks. Deleting the env seam
  would delete that leg.
- `ATX_VOL_FIT_WORKERS` is the documented answer to nested parallelism —
  `scripts/atx-build.ps1` and `bench/README.md` both prescribe it, and a bench
  baseline records `ATX_VOL_FIT_WORKERS=1` as part of its method. A config struct
  cannot reach a test process that `ctest -jN` spawns, which is the exact problem
  it exists to solve. This is the case where an environment variable is the right
  mechanism rather than a shortcut.

The remaining seven library knobs are diagnostics and measurement levers whose
only documented users are sprint/bench recipes. They are proposed **keep** for
v1 on the narrow grounds that each is off by default, each is read once, and none
can change a result — but two follow-ups are worth naming rather than leaving
implicit:

- `ATX_VOL_CORPUS_DATE_BATCH` is the one knob that is genuinely *library
  behaviour configured from the environment* rather than a diagnostic. It should
  become a field on the corpus build config, with the environment read (if kept
  at all) as a fallback at the CLI seam.
- `ATX_VOL_ZC_BORROW` / `ATX_VOL_ZC_BACKING` / `ATX_VOL_FIT_ECORE_TIER` exist to
  A/B a decision inside one binary without a rebuild. That is a legitimate use,
  but each keeps an alternative code path alive for it; the E-core tier's own
  measurements are what set its default to off. Whether v1 wants to carry three
  such paths is a judgement call, not a defect.

**`ATX_VOL_PROFILE` means two unrelated things, and the collision is real.** As a
**CMake option** (`-DATX_VOL_PROFILE=ON`) it compiles the library with the
`ATX_VOL_PROFILE` macro defined, arming the compile-time phase-timer plane
(`detail/phase_profile.hpp`). As an **environment variable** it is read at
runtime by `curve_fit.cpp` and `surface_parity.cpp` to turn on two unrelated
timing printouts, and it works whether or not the macro was ever defined. Neither
mechanism observes the other. Renaming the environment side (say to
`ATX_VOL_FIT_TIMING`) is the fix; it is a behaviour change for anyone setting the
current name, so it is recorded here rather than done silently.

Out of scope for this table, deliberately: `DATABENTO_API_KEY`, read by the
Databento definition-export example and the OPRA puller scripts (not an `ATX_*`
name, and gated behind a network/cost opt-in), and the 17 further `ATX_*` names
read only by tests and benchmarks, neither of which ships.

## API stability policy (1.x)

The version is single-sourced from `project(atx VERSION ...)` and reaches C++ as
`atx::vol::version()`, `atx::vol::kVersion{Major,Minor,Patch,String}` and the
`ATX_VOL_VERSION*` macros (`atx/vol/version.hpp`). Feature-gate with the numeric
form, which is preprocessor-usable:

```cpp
#if ATX_VOL_VERSION >= ATX_VOL_VERSION_NUM(1, 1, 0)
  // ... 1.1 API ...
#endif
```

**What 1.0.0 freezes is a tier, not the whole tree.** The tiers are physical —
each is a directory and, where it matters, its own CMake target — so "is this
frozen?" is answered by where the header lives, not by judgement:

| Tier | Where | Count | Promise |
|---|---|---|---|
| **Tier-A** | exactly the headers `atx/vol/vol.hpp` includes | 56 | **Frozen for 1.x.** Closed under inclusion |
| **Tier-B** | other headers directly under `include/atx/vol/`, plus `simd/` | 23 + 9 | Public and supported to include; **not** frozen |
| `detail/` | `include/atx/vol/detail/` | 25 (+1 generated) | **No stability promise.** Installed because Tier-A reaches it |
| `tools/` | `tools/include/atx/vol/tools/` — target `atx::vol::tools` | 6 | CLI support. Not part of the shipped library surface |
| `research/` | `research/include/atx/vol/research/` — target `atx::vol::research` | 9 | Run orchestration. Not part of the shipped library surface |

Only the Tier-A count is machine-checked (below). The other four are prose and
therefore rot: Tier-B and `detail/` were each one short by the time this table was
next read, because `log.hpp` and `detail/log_emit.hpp` landed after it was
written. Re-derive them from the tree rather than editing a digit —
`grep -c '^#include "atx/vol/' include/atx/vol/vol.hpp` is Tier-A; each remaining
row is `ls` over the directory the row names, minus (for Tier-B) Tier-A and
`vol.hpp` itself. The `+1 generated` on `detail/` is
`detail/version_generated.hpp`, configure_file'd from `project(atx VERSION ...)`,
so an install prefix carries 26 there and the source tree 25.

*Closed under inclusion* is the load-bearing rule: a header named in a frozen
signature is frozen whether or not callers reach for it directly, so if a Tier-A
header includes another `atx/vol/` header, that header is Tier-A too. The only
permitted escape is downward into `detail/` or `simd/`, which promise nothing.

Tier-B is where the advanced per-family calibrators, the SoA/SIMD batch kernels,
the listed-dispersion vocabulary and the harness panels/fixtures live. They are
public and you may include them; they are simply outside what 1.x will not break.

**The manifest is `kTierA` in `atx-vol/tests/vol_umbrella_test.cpp`**, and it is
machine-checked, not documentation. Four contract tests fail the build on drift:
`UmbrellaIsExactlyTierA` (the umbrella's include list *is* the manifest, reported
as two directed differences so a shrunk API and a widened promise are told
apart), `UmbrellaAdmitsNoNonShippedTier`, `TierAIsClosedUnderInclusion`, and
`DemotedSurfaceContainersAreNotNamedInPublicHeaders`. Promoting or demoting a
header means editing that array in the same commit as the header move.

The package advertises `COMPATIBILITY SameMajorVersion`, which is the same
statement in CMake: a `find_package(atx-vol 1.0)` consumer accepts any 1.z,
because Tier-A is what it compiled against and Tier-A does not move in 1.x.

**Known limit, stated rather than implied.** `tools/` and `research/` are
path- and umbrella-separated but not *link*-separated: `atx-vol` links both
include roots PUBLIC, so a plain `atx::vol` consumer can still include them by
name. Plan item 5.6 (S5-T27) **evaluated that isolation and declined it** — do
not read this paragraph as an outstanding task. `atx-vol-research` links
`atx-vol-tools` INTERFACE, so containing the tools include root means demoting
the *research* edge as well, and the research headers are shipped: that is a
distribution-surface decision, not a build tidy, and it is deferred past v1. The
authority is the note in `atx-vol/CMakeLists.txt` beside the two targets, which
records both measured reasons.

## Linkage and distribution policy (v1)

**atx-vol 1.x ships static-only, and there is no `ATX_VOL_API` export macro.**
That is a decision made on evidence, not an unfinished item.

A DLL build of the atx libraries (`-DATX_SHARED_LIBS=ON`, the `dev-shared`
preset) links and runs, but it is not *correct* for this library. Every
instrumentation plane atx-vol carries is a header-inline global — the always-on
solve ledger and the lightweight sampler in `atx/vol/detail/counters.hpp`, the
phase timers in `atx/vol/detail/phase_profile.hpp` — and on Windows a C++17
inline variable gets **one instance per image**. The consumer scrapes its own
copy while the DLL increments the DLL's. This was measured on 2026-07-22 and is
why `dev-shared`'s own preset description says it is never the test gate.

`CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` does not fix it: a **data** symbol still has
to be declared `__declspec(dllimport)` in the consumer to bind to the exporting
image rather than to a local copy. Declaring it is precisely the `ATX_VOL_API`
plumbing, on every such global, forever. v1 does not take that on.

What follows from the policy:

| Configuration | Status |
|---|---|
| `ATX_SHARED_LIBS=OFF` (default) | Supported, tested, shipped — static archives |
| `ATX_SHARED_LIBS=ON` | Developer link-speed convenience only; **not** a gate, **not** distributable |
| `BUILD_SHARED_LIBS` | Not this project's switch. Configure **fails** with the reason rather than silently handing back static archives |
| `cmake --install` of a shared build | **Refused** at install time, rather than producing a prefix whose counters read zero |

The consumer-facing consequence is small: `find_package(atx-vol)` gives static
archives, and the exported targets carry the link interface they need. Nothing
in the public API depends on the library being shared.

The same reasoning is why the opt-in `ATX_VOL_COUNTERS` / `ATX_VOL_PROFILE`
instrumentation names its build configuration in an inline namespace: a
consumer compiled with a different view of those options than the library now
fails to **link**, naming the mismatch, instead of silently reading a plane
nobody writes. See the header comments in `atx/vol/detail/counters.hpp`.

## Conventions

Follows `.agents/cpp/agent.md`: C++20, no UB, `enum class`, `const`/`noexcept`/
`[[nodiscard]]` by default, expected failures via `atx::core::Result<T>` (not
exceptions), Rule of Zero, `/W4 /permissive- /WX` clean. Public headers live
under `include/atx/vol/`; the namespace is `atx::vol`.

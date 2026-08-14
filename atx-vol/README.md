# atx-vol

Equity-options pricing and volatility-fitting library — a C++20 port of the C17
`ats-vol` library, refactored to the atx house style (`.agents/cpp/agent.md`)
and layered above `atx-core`.

## Status: full port

The upstream `ats-vol` (~65k LOC of C across ~90 translation units) has been
ported in full to idiomatic, tested C++20, and then extended with a
Vola-Dynamics American-equity parity layer (see below), a composable
`VolaSession` handle, and a cached high-performance pricing hot path. It registers
**several thousand GoogleTest cases** under `/W4 /permissive- /WX` (clang-cl).
Deliberately not a digit: a precise count here is measured once and then rots
silently, so get the current one from
`build/bin/atx-vol-tests.exe --gtest_list_tests` (see *Build & test* for the
ctest-side number and the fixed +13 that separates them).
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
| American pricers | `american.hpp`, `correction.hpp` | Andersen–Lake (Gauss-Legendre + Chebyshev boundary), BAW, American Greeks, Chebyshev American-minus-European correction cache, Crank–Nicolson PDE oracle. The strictly-negative-rate double-continuation corner is **refused**, not approximated — see *Stated limitations* below |
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
| Scenario risk | `scenario_grid.hpp` | 2-D (spot% × vol) second-order Taylor P&L matrix over a `PortfolioPricer` book — one deduped full-Greeks solve, every cell reconstructed analytically, with cells outside the measured Taylor radius repriced exactly. **Sticky-strike: the smile is never rolled** — see *Stated limitations* below |
| PnL attribution | `pnl_attribution.hpp` | Additive base→shifted decomposition into the Vola vocabulary (spot / ATF / skew / curvature / 2nd-order vol / rates / time / unexplained) |
| Surface analytics | `analytics.hpp` | ATMF term structure, delta wings / RR / BF, skew & curvature, forward vol; Breeden–Litzenberger density, implied CDF/quantiles, BKM moments, and the OTM log-strip model-free implied vol (`var_swap_vol`) — **diagnostics read off a served surface, not the traded variance-swap product** (that is `derivatives.hpp`) |
| Projection | `projection.hpp` | Projection spine: forward-T interp, coord conversion, inserted-slice eval, delta-solve, project-compare |
| Contract projection | `contract_projection.hpp` | Relative maturity/strike definitions to absolute-expiry American contracts, marks, and Greeks; prepared deterministic batch API |
| Surface archive | `surface_archive.hpp` | ATXVSA2 (magic `ATXVSA20`) binary format writer/reader, CRC-32C, schema-hash guard, symbol lookup, concurrent-read-safe |
| Data ingestion | `data.hpp` | SpiderRock quote-frame model, install-into-universe path, ISO/year-fraction kernels |
| Batch kernels | `batch.hpp` | SoA batch B76 price/value+vega/from-lnfk, IV, Greeks, eSSVI-w. Price, value+vega, Greeks and eSSVI-w **runtime-dispatch to 4-lane AVX2** on an AVX2 host (`n ≥ 4`; eSSVI-w `n ≥ 16`), so they agree with the scalar kernel to the SIMD gate (~1e-6 abs + 1e-7 rel; ~1e-5 abs on the fused `vega` column; eSSVI ~1e-12), **not bit-for-bit**; from-lnFK and IV have no vector route and stay scalar-backed and bit-exact |
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

## Stated limitations (v1)

Four places where the library's behaviour is narrower than a module name
suggests. Each is a deliberate design choice with a single source of truth in a
header; they are collected here so a caller meets them before a production
surprise does, not after.

**1. American pricing REFUSES the strictly-negative-rate double-continuation
corner.** Under the McDonald–Schroder map both sides reduce to an internal put
characterized by its `(rate, yield)`, and `detail::classify_regime`
(`american.hpp`) is the one place that classifies it. When `yield < rate < 0` —
a *strictly* negative rate above the yield — a second exercise boundary appears
(Battauz–De Donno–Sbuelz 2015; Andersen–Lake 2021) that the single-boundary
Andersen–Lake scheme cannot represent, so **every American entry point returns
`ErrorCode::NotImplemented` (or NaN in the batch/lane channels) rather than a
silently-wrong single-boundary price**: `american_price`, `american_greeks_*`,
`exercise_boundary`, `assignment_risk`, `american_implied_vol`, the SoA batches,
and the correction-cache populator. `rate < 0 && rate <= yield` is a different
cell — early exercise is never optimal there, so American *equals* European
exactly and is served.

*The served output is discontinuous in `rate` at 0 for `yield < 0`, and that is
a property of what this library can compute, not of the option's value.* Hold
`yield < 0` and raise `rate` through 0: the cell is Unsupported just below 0 and
American (served) at exactly 0, because at `rate == 0` the exercise region stays
downward-connected. So a **carry solve or root-find that walks `rate` across 0
with a negative yield sees its objective undefined on one side and defined at
the point**, and a sensitivity taken by bumping `rate` around 0 in that corner
bumps into the refusal rather than a number. Do not assume a continuous
objective there. Widening the served set needs a two-boundary scheme, not a
predicate change. The full regime table and this note live at
`american.hpp` (`detail::classify_regime`); the gate is
`NegRateDomainMap.ZeroRateNegativeYield_IsSingleBoundaryAmerican`.

**2. `scenario_grid` is STICKY-STRIKE: it does not roll the smile.** Both cell
routes hold the base-resolved `sigma` and bump only the pricer inputs — the
surface is **not** re-fit and the smile is **not** re-anchored to the shocked
spot. An Exact cell answers "reprice *this* contract at base sigma + bump under
the shocked market inputs"; a Taylor cell reconstructs the same question from
one second-order Greek bundle. Carry (`q_eff`) is held for the same reason. A
desk that marks sticky-delta, or that needs the surface re-fit at each shocked
spot, will not get that number from this grid — the limitation is stated in full
under *"Exact cell semantics — sticky-strike, NO smile roll"* in
`scenario_grid.hpp`. The default Taylor radii (3% spot / 3 vol pts) are measured
per-axis; a cell at the **double corner** combines both residuals plus the vanna
cross-term and is gated at $0.0125 per share, not at the per-axis $0.005.

**3. No error bars on FITTED PARAMETERS.** `fit_metrics.hpp` publishes
**per-observation** error bars — a quote's price half-spread mapped through vega
to a vol uncertainty, `σ_err ≈ ½·(ask − bid)/vega` — plus the reduced-χ² and
"minimum edge" band built on them. That is a statement about the *data*, not
about the fit: atx-vol publishes **no** covariance, standard error or confidence
interval for a calibrated curve's parameters, and nothing here should be read as
one. Vola's calendar-coupled joint mode with per-term parameter error bars is a
post-v1 target, named in the calendar-arbitrage section below as a target and
nowhere as a feature.

**4. The VIX-style log-strip is an ANALYTIC, not the traded-product module.**
`analytics.hpp`'s `var_swap_vol` computes model-free implied vol
`sqrt(K_var(T))` by integrating the OTM log-strip over the **served** surface —
a diagnostic read of a surface the library already fitted, in the same family as
the Breeden–Litzenberger density, BKM moments and implied CDF beside it. It is
**not** the variance-swap product: that is `derivatives.hpp` (`DerivKind`,
`var_swap_fair_strike` / `vol_swap_fair_strike` / `deriv_price` / `deriv_greeks`,
Carr–Lee, capped and mid-life kinds, realized-vol accrual and dated fixings) with
`deriv_book.hpp` for a position book. A quoted log-strip number carries no
contract, no accrual state, no cap and no fixing schedule; do not mark a swap
position with it.

## How to read the performance figures in this file

Every timing, throughput and fit-quality number below is carried with its
producing target, and none of them was re-measured for the 1.0.0 release. That
is stated rather than implied, because most of them **cannot** be re-measured
from a clean checkout of this repository:

- **The real-data figures rest on licensed vendor market data that is not, and
  will not be, in-repo.** The XOM and SPY tables, the `value_chain` inversion
  rates, the cold-fit and cached-query timings and the calendar-repair quality
  deltas all come from Databento OPRA `cbbo-1m` boards materialised offline
  (`opra_dbn_to_parquet` → `opra_parity_bench` / `examples/spy_diag` /
  `examples/chain_pricer_bench`). That fixture class is the same one the
  *"What 'the matrix is green' covers"* table below marks **permanently
  unprovisioned** — 30 registered tests skip on it. Provision the data as
  described there and the numbers become reproducible; without it they are a
  record of a past measurement, not a claim you can check.
- **The synthetic-fixture figures are reproducible** — the known-truth SPY
  oracle (`atx/vol/spy_fixture.hpp`), `examples/spy_surface_bench`,
  `examples/american_iv_bench` and the benchmark targets in `bench/` need no
  vendor data.
- **Absolute numbers are pinned to one host** (i7-1260P / clang-cl 18; see
  `bench/README.md` for the quiet-window protocol and the CV≤5% rule). Only
  *ratios* are gated by `bench/compare_baseline.py`. Treat an absolute
  microsecond or seconds figure here as an order of magnitude on unlike silicon.

One figure **was** measured paired during the 1.0.0 release sprint and carries
its method; it lives in the CHANGELOG rather than here, because it justifies a
changed default: `RunConfig::prefetch_depth` `1 → 2`, **+15.2 % over 11 of 12
interleaved rounds**, one binary alternating the depth inside a single session
on the 135-session SPY-dispersion replay, medians and win-counts only. That is
the shape a citable figure has — paired, interleaved, with its win-count — and
it is the standard the pre-1.0 figures above were not held to.

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
  in `tests/CMakeLists.txt`); `ForceAvx2` forces the pack on any host.
  **The same relaxation applies to the B76 span batches** (`batch.hpp`), and this
  sentence used to claim the opposite ("stay scalar-backed"). It was wrong:
  `black76_price_batch`, `black76_value_and_vega_batch`, `black76_greeks_batch`
  and `essvi_w_batch` all take the 4-lane AVX2 route under the default `Auto`
  (`src/batch.cpp`) — at `n ≥ 4`, or `n ≥ 16` for eSSVI-w, whose per-lane cost is
  small enough that the setup only pays on large grids — patching only
  degenerate / deep-wing / tail lanes through the scalar kernel. Their agreement
  with the scalar source of truth is the SIMD gate — which is what
  `batch_test.cpp` actually asserts, an `expect_close` and never an equality —
  and that gate is **per output column, not one blanket number**: ~1e-6 absolute
  + 1e-7 relative on prices and Greeks, **~1e-5 absolute** on the fused batch's
  `vega` column (`batch_test.cpp:291,311`; vega is the larger quantity, so the
  same relative error is a looser absolute one), and ~1e-12 for eSSVI.
  `black76_price_from_lnfk_batch` and `implied_vol_batch` are the two
  that really are scalar-backed and therefore bit-exact: neither has a vector
  kernel (IV's measured AVX2 route was slower on the reference host).
  [`docs/simd_fastpath.md`](docs/simd_fastpath.md) has the kernel-by-kernel
  detail; note that its parity column reports a *measured* max deviation on one
  workload (0.0 on several), which is an observation, not the guarantee — the
  guarantee is the tolerance above.
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
the lane existed. Every step a lot is alive takes a dated fixing against that
step's spot — through the lane's own `observe_swap_fixing` (`backtest.cpp`),
which transcribes `RealizedTracker`'s accrual arithmetic rather than calling it,
for the two deliberate deviations documented on `SwapAccrual` (`backtest.hpp`);
the tracker has no production caller. A lot settles at
its exact-match expiry off the accrued realized rate, no re-pricing needed.
Fixings are booked on the INDEX convention (raw close-to-close returns): the
driver passes no dividend, and `RealizedTracker`'s single-name adjustment
(Task F-6) is not reachable from this lane until a corporate-actions feed
exists to source ex-dividend cash from.
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
| cases / suites | `build/bin/atx-vol-tests.exe --gtest_list_tests` | GoogleTest cases in the atx-vol test binary, including the `DISABLED_` ones |
| registered tests | `ctest --test-dir build -N -L atx_vol` | the cases above, discovered by `gtest_discover_tests`, **plus exactly 13** lanes that are not GoogleTest cases: `SpxWilmottReproUnit`, `atx-vol-reference-spy-dispersion`, `atx-vol-download-occ-ess`, `atx-vol-build-spy-top50-universe`, `Mag7DispersionReport`, `SpyDispersionPnlReport`, `atx-vol-python`, `atx-vol-pricing-forcescalar`, `atx-vol-e2e-benchmark-name-coverage`, `atx-vol-e2e-benchmark-name-checker-unit`, `atx-vol-american-shootout-name-coverage`, `atx-vol-compare-baseline-unit`, `atx-vol-pg-observability-name-coverage`. Registered by `add_test()` across three files, not one: `atx-vol/CMakeLists.txt` (`SpxWilmottReproUnit`), `tests/CMakeLists.txt` (the next seven) and `bench/CMakeLists.txt` (the five `*-name-coverage` / `*-unit` guards) |

**The stable thing is the gap, not the totals — and it is stable only per
configuration.** Absolute figures are deliberately not published: they grow with
every suite added, and a written one goes stale silently. The gap is
`registered − cases = the non-GoogleTest lanes`, which is **13 under
`cmake --preset dev`** and **15 under `rel` / `rel-avx2`**, because those two
configure `-DATX_BUILD_EXAMPLES=ON` and pick up the two `accuracy_panel` lanes.
A number that changes with the preset is not an invariant, so treat it as what it
is: the size of the enumerated list above, under a stated configuration.

**Nothing enforces it.** No test or CMake assertion checks the count; a
fourteenth `add_test()` would change the gap silently and this list would simply
be wrong. So verify it by set difference, not by subtraction — subtracting two
totals can return the expected number for compensating reasons and cannot tell
you which lanes are in the gap. Run both commands, diff the two name sets, and
confirm the difference is exactly the lanes named above.

Both commands assume `cmake --preset dev`. **The `rel` and `rel-avx2` presets
register exactly 2 more**, because they are configured with
`-DATX_BUILD_EXAMPLES=ON`, which adds the two `accuracy_panel` determinism lanes
(`AccuracyPanelDeterminism`, `AccuracyPanelFailureDeterminism`,
`atx-vol/CMakeLists.txt`). The GoogleTest population is identical on all three
presets — only the script lanes differ — so a gate log quoting a rel-side number
two higher than a dev-side one is not drift, it is that split. Like the 13, the
**+2 is the invariant**; the totals it applies to are whatever the commands
report today.

Every count in the section below comes from the same
measurement, quoted once; the *skip* inventory needs a full matrix run to
re-derive, so it carries the release-sprint measurement rather than a fresh one.
Re-run the commands rather than trusting the digits: nothing regenerates them,
so a stale number here is a documentation defect, not a test failure.

### What "the matrix is green" covers

Green means **every lane that can run, ran and passed**. It does not mean every
registered lane ran, and the difference is stable, deliberate and enumerated
below rather than being a backlog.

`DISABLED_` cases appear in **both** listings — `--gtest_list_tests` prints them
with their `DISABLED_` prefix, and `gtest_discover_tests` registers them with ctest
renamed `… (Disabled)` — so they are not what separates the two commands, and
neither command's total excludes them. ctest declines to *run* them; both still
*count* them. (An earlier version of this paragraph said the opposite, and quoted
a figure for them that was wrong in the same breath.) **63** reported `SKIPPED` at
the v1.0.0 release-sprint measurement on an AVX2 host with `cmake --preset dev`
and no market-data cache present — a dated figure from that one run, not a current
count. Every skip carries a reason string naming exactly
what would let it run; the six skip classes below account for all 63 as of that
measurement (the post-merge tree has not had a fresh full-matrix skip census).
The last row is **not** a skip class and contributes none of the 63 — it is the
other opt-in that changes what a green run covers, listed here because this is
where a reader looks for one.

| Gate class | What gates it | Skips | Provisioning status |
|---|---|---|---|
| Host CPU capability | `simd::has_avx2()` at test entry | **0** — this host has AVX2 | Nothing to provision. On a **non**-AVX2 host ~27 more sites skip (`american_batch_test.cpp` 16, `simd_isa_override_test.cpp` 10 + 1 env), so "green" there covers strictly less. The scalar path is separately gated by the `atx-vol-pricing-forcescalar` ctest lane, which runs the pricing suites with `ATX_SIMD_ISA=ForceScalar` and passes on every host. |
| Opt-in instrumentation build | `counters::counters_enabled()` — `-DATX_VOL_COUNTERS=ON`, i.e. the `dev-counters` preset | **21** | By design. These assert exact algorithm-counter values (solve counts, Clenshaw traversals, allocation-once, lazy-materialisation-once); the code under test runs in the default build, only the assertion channel is compiled out. Run `cmake --preset dev-counters` to include them. |
| Cached real-market fixtures | Presence of OPRA `cbbo-1m` parquet boards, the SPY fit corpus, and the shared board cache under `C:/atx-data/spy-dispersion/opra/` | **30** | Permanently unprovisioned in-repo: this is licensed vendor market data, too large to commit and not ours to redistribute. Materialise it with the Databento pull + `opra_dbn_to_parquet` (see the real-data section above) and the whole class runs. |
| Named external data (env) | `ATX_T7_DEFINITIONS_TSV` (5), `ATX_SP100_SURFACE_DB` (1) | **6** | Same reason as the row above, pointed at by an environment variable instead of a search path. All six are measurement harnesses / a real-database baseline, not correctness gates. |
| Opt-in long sweep | `ATX_VOL_LONG_CORPUS=1` | **1** | By design — a 250 + 10,000 synthetic-board property sweep, too slow for every run. Deterministic and runnable on any host. |
| Opt-in coverage widener (**not** a skip gate) | `ATX_VOL_SCOREBOARDS=1` | **0** | Skips nothing — it widens an assertion. Its one consumer is `SigmaInterpCorpus.RealBoard_WithinGates` (`spy_fit_corpus_test.cpp`), which runs in every default matrix; the flag takes that test's cold-Andersen-Lake reference comparison from **every 2nd strike to every strike** — a strict superset at the same tolerances. So an unset run is not a skipped test, it is half the reference cases. To widen it: set the variable, then `ctest -R SigmaInterpCorpus.RealBoard_WithinGates`. That test is in `ATX_VOL_SLOW_FILTER`, so it carries the `atx_vol_slow` label and an `-L atx_vol_fast` run never reaches it with or without the flag. There is no `nightly` preset — one was proposed in a 2026-07-11 plan and never added, so nothing sets this for you. |
| Structurally not a standalone lane | `ProcessScratchChild.*` (3) run only when their driver test re-invokes the binary as a child; `ScalarLegEnv.ForceScalarEnvSeedsScalarOverride` (1) runs inside the `atx-vol-pricing-forcescalar` lane; `atx-vol-python` (1) exits ctest's `SKIP_RETURN_CODE` unless the standalone `atx-vol/python` extension is built | **5** | Four of the five ARE covered — by the lane that drives them. Only `atx-vol-python` is genuinely uncovered by a default build; build the standalone Python project to run it. |

So a green matrix asserts: **every pure-computation, synthetic-fixture and
in-repo-fixture test passes, on the host's ISA, in the default build, plus the
forced-scalar pricing leg.** It does not assert the real-market accuracy gates,
the exact-counter gates, the long corpus sweep, or the Python binding suite —
each of which is a documented opt-in with a named way to turn it on, not an
unknown. Nor does it assert the *full-strike* cold-AL reference sweep: green
covers `SigmaInterpCorpus.RealBoard_WithinGates` at every 2nd strike unless
`ATX_VOL_SCOREBOARDS=1` is set. That one is a narrower result than a skip, and
easier to miss, because the test reports **passed** either way.

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

atx-vol reads the `ATX_*` environment variables below in shipped code. The table
is the list — count its rows and read its `Scope` column rather than trusting a
number in this sentence, because a hand-kept count is exactly the artifact that
goes stale when a row is added. None is required, so **an unset environment is a
complete, correct configuration**, which is the property that makes this table
short.

The **Result** column carries the safety claim: whether the knob can change a
fitted, priced or archived value. Read it per row. A prose taxonomy summarising
this table used to sit here and drifted from it three times — each new row landed
outside whatever buckets the paragraph had named — so the classification lives in
the table, where a new row cannot be added without filling it in.

One distinction the column cannot make on its own: `ATX_SIMD_ISA` is neutral
because the *runtime* dispatch is pinned bit-identical to scalar
(`AmericanPriceBatch.MatchesScalarBitIdentical` and the `*MatchesScalar*` family).
Last-place bits do differ between SSE2 and FMA builds, but that is
`kFmaContraction`, a **compile-time** `__FMA__` constant driven by `rel-avx2`'s
`/arch:AVX2`, not by this variable.

| Knob | Result | Effect | Default | Scope | v1 status |
|---|---|---|---|---|---|
| `ATX_VOL_FIT_WORKERS` | neutral | Resolves the auto (`0`) worker count for `parallel_for` fan-outs. An explicitly requested non-zero count is honoured as-is and is *not* capped by it | `hardware_concurrency()` (min 1) | library — `detail/parallel_for.hpp` | **keep** |
| `ATX_SIMD_ISA` | neutral | Seeds the process-global SIMD override at load: `Auto`, `ForceScalar` or `ForceAvx2`. An in-process `set_simd_isa_override()` still wins | `Auto` | library — `src/simd/cpu.cpp` | **keep** |
| `ATX_VOL_FIT_ECORE_TIER` | neutral | Arms the E-core second scheduling tier. `2` arms it *without* the below-normal priority drop; any other non-zero value arms it *with* the drop | unset = off | library — `src/fit_scheduler.cpp` | **keep**, deprecation candidate |
| `ATX_VOL_CORPUS_DATE_BATCH` | neutral | Dates per corpus fan-out call. Scheduling only — output bytes do not depend on it | `8` | library — `src/dispersion_run.cpp` | **keep**, belongs in a config struct |
| `ATX_VOL_CORPUS_PHASE_TIMING` | neutral | Prints the corpus build's phase split. Collection is unconditional and cheap; only the report is gated | unset = off | library — `src/dispersion_run.cpp` | **keep** |
| `ATX_VOL_PROFILE` | neutral | Prints per-phase fit timings from `curve_fit` and `surface_parity`. **Not** the CMake option of the same name — see the collision note below | unset = off | library — `src/curve_fit.cpp`, `src/surface_parity.cpp` | **keep**, rename candidate |
| `ATX_SLICE_DEBUG` | neutral | Prints `curve_fit`'s per-slice fit-preparation outcome (why a chain did or did not become a fittable slice) | unset = off | library — `src/curve_fit.cpp` | **keep** |
| `ATX_VOL_ZC_BORROW` | neutral | `0` forces the owned-reconstruct archive path instead of the zero-copy borrow. Read once per process, so it cannot make a run non-deterministic | unset = borrow allowed | library — `src/backtest.cpp` | **keep**, deprecation candidate |
| `ATX_VOL_ZC_BACKING` | neutral | `map` or `copy` overrides the caller-declared `ArchiveBacking` on the borrow path. Read once per process | unset = the caller's choice stands | library — `src/backtest.cpp` | **keep**, deprecation candidate |
| `ATX_VOL_AL_PROBE` | neutral | Arms the Andersen-Lake zone cycle-attribution probe, read once into `g_mode` at process load. Any non-empty value turns it on; a value containing `s`/`S` additionally records each cold boundary solve's normalized query state. Attribution only — the priced values it counts around are unaffected | unset = off | library — `src/al_probe.cpp` | **keep** |
| `ATX_VOL_AL_PROBE_OUT` | neutral | File path for the per-state binary trace `ATX_VOL_AL_PROBE`'s `s`/`S` flag records; the human-readable `alprobe.*` summary itself always goes to the stream `dump()` was called with, not this path. Unset just skips writing the trace file. Meaningless unless `ATX_VOL_AL_PROBE` is set | unset = trace not written | library — `src/al_probe.cpp` | **keep** |
| `ATX_VOL_CACHE` | neutral | Default for the dispersion CLI's `--cache DIR`; an explicit `--cache` overrides it. Empty means disabled, which is the default behaviour | unset = disabled | tool — `tools/spy_dispersion_backtest.cpp` | **keep** |
| `ATX_VOL_PREFETCH_DEPTH` | neutral | Overrides the projected replay's snapshot look-ahead depth (`projected_prefetch_depth()`). Malformed or out-of-range (cap `64`) falls back to the default rather than failing the run. Scheduling only — output is bit-identical at any depth | `2`, cap `64` | tool — `tools/spy_dispersion_backtest.cpp` | **keep** |
| `ATX_VOL_SOLVE_LEDGER` | neutral | Same env-gated shape as `ATX_VOL_PROFILE`: any non-empty value dumps the always-on solve ledger (AL boundary solves / premium evals / IV Newton iterations) as `ledger.<name> <count>` lines to stderr; the stdout build report shape is untouched | unset = off | tool — `tools/surface_db_build_main.cpp` | **keep** |
| `ATX_VOL_DISABLE_STRIP_BATCH` | neutral (pinned) | A/B seam: forces `var_swap_fair_strike` onto its per-node scalar surface-read loop even when `SurfaceT` exposes a batched `iv_batch`, so one binary can be measured both ways. `Strip.BatchedMatchesScalar*` pins the two bit-identical (`bits_equal_or_both_nan` across six fields plus `EXPECT_EQ` on flags and node count, over four quality tiers on flat and skewed surfaces), so this moves speed, not results — note those tests drive the seam through the setter below, not through this variable | `1` enables; unset or anything else = off. The env var seeds a static **once at process load**; an in-process `detail::set_strip_batch_disabled_for_test()` overrides it afterwards and wins, same as `ATX_SIMD_ISA` above | library — `src/derivatives.cpp` | **keep** |
| `ATX_VOL_DISABLE_BUMP_CACHE` | neutral (pinned) | Same shape, orthogonal knob: disables the greek bump table's read-vector cache (`BumpReadCache`/`CachedBumpView`). Speed only | as above, overridden in-process by `detail::set_bump_read_cache_disabled_for_test()` | library — `src/derivatives.cpp` | **keep** |
| `ATX_VOL_DISABLE_IV_EARLY_EXIT` | **MOVES BITS** | Forces `ConvexSliceFit::iv()`'s bisection to run its full fixed 64 iterations (pre-P-5 behaviour) rather than stopping at tolerance. **Unlike the two above, this one moves last-place bits** — which is its use: it is how the superseded Release/SSE2 golden fingerprint `17305682487856730537` is reproduced after the P-R re-pin (see `prepared_portfolio_test.cpp`). Reach for it to attribute a golden-hash delta to that seam instead of to a regression | `1` enables; unset or anything else = off. Read once at process load, and **unlike the two seams above there is no setter** — `dense_slice.cpp` exposes only the getter `iv_early_exit_disabled_for_test()`, so the environment is the only way in and this one genuinely cannot be toggled mid-process | library — `src/dense_slice.cpp` | **keep** |

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

The remaining library knobs are diagnostics, measurement levers and A/B seams
whose only documented users are sprint/bench recipes. They are proposed **keep**
for v1 on the narrow grounds that each is off by default and each is read once.
The third of those grounds — *none can change a result* — held while the table
listed only diagnostics and does **not** cover the A/B seams added above:
`ATX_VOL_DISABLE_IV_EARLY_EXIT` changes last-place bits by design, which is the
whole point of it. `ATX_VOL_DISABLE_STRIP_BATCH` and `ATX_VOL_DISABLE_BUMP_CACHE`
do satisfy it, and are pinned bit-identical by their own tests rather than merely
asserted to be. Two follow-ups are worth naming rather than leaving implicit:

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
name, and gated behind a network/cost opt-in), and the further `ATX_*` names whose
read sites do not ship. **The criterion is target membership, not directory and
not the name.** Resolve it by asking which CMake target the read site compiles
into, then whether that target is installed: `atx-vol` and the CLIs in
`cmake/atx-vol-install.cmake`'s `install(TARGETS ...)` lists ship; `atx-vol-tests`,
every `atx-vol-*-bench` and the `examples/` executables do not. Applying the
criterion by directory is how three shipping library knobs
(`ATX_VOL_DISABLE_STRIP_BATCH`, `ATX_VOL_DISABLE_BUMP_CACHE`,
`ATX_VOL_DISABLE_IV_EARLY_EXIT`) sat outside this table until 2026-08-14 —
they read from `src/`, not from a test. That set is deliberately
not enumerated here — a hand-kept count drifts as tests are added and nothing
catches it. Enumerate it when you need it, from the read sites rather than from
a naming pattern: `grep -rn 'getenv\|_dupenv_s\|GetEnvironmentVariable'` over
`atx-vol/`, then resolve every call site that reads a **parameter** rather than a
string literal back to the concrete names its callers pass. Several do, and a
name-pattern grep silently misses all of them — which is the whole reason to
enumerate by read site. Those three tokens are the complete set of env-access
mechanisms in shipping code: `atx-core` reads the environment nowhere, and each
`env_flag_enabled` helper is file-local to its own TU rather than a shared
wrapper, so nothing reaches the environment by a route the grep cannot see. One
exception is documented above rather than here because it changes what a green
run asserts: `ATX_VOL_SCOREBOARDS`.

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
| **Tier-A** | exactly the headers `atx/vol/vol.hpp` includes | 58 | **Frozen for 1.x.** Closed under inclusion |
| **Tier-B** | other headers directly under `include/atx/vol/`, plus `simd/` | 34 + 9 | Public and supported to include; **not** frozen |
| `detail/` | `include/atx/vol/detail/` | 31 (+1 generated) | **No stability promise.** Installed because Tier-A reaches it |
| `tools/` | `tools/include/atx/vol/tools/` — target `atx::vol::tools` | 6 | CLI support. Not part of the shipped library surface |
| `research/` | `research/include/atx/vol/research/` — target `atx::vol::research` | 9 | Run orchestration. Not part of the shipped library surface |

**Three of the five digits above had rotted by v1, and the first three no longer
can.** All five rows were re-derived at the release commit: Tier-A 56 → **57**,
Tier-B 23 → **31**, `detail/` 25 → **28**, while `simd/` 9, `tools/` 6 and
`research/` 9 held. The earlier note here said the table was "one short" because
`log.hpp` and `detail/log_emit.hpp` had landed; that was true when written, and
the gap then widened to 8 on Tier-B and 3 on `detail/` as the surface kept
moving — which is the point it was making. The first post-1.0.0 growth is
already in the table: the strategy DSL made `strategy.hpp` include
`swap_leg.hpp`, so closure-under-inclusion promoted it (Tier-A 57 → **58**),
and `detail/convex_recovery.hpp` landed alongside it (`detail/` 28 → **29**).
The v1.0.0 tag itself ships 57 / 31 / 28. Task P-5 (vol-derivatives
production sprint, review fix round 1) added `detail/dense_slice_price.hpp`
(the `ConvexSliceFit::iv()` / calendar-scan shared price projection) —
`detail/` 29 → **30**, no Tier-A/Tier-B change. Task F-9 (same sprint) added
`cboe_strip.hpp` — the CBOE discrete-strike variance strip, additive and
outside the umbrella — so Tier-B 31 → **32**, no Tier-A/`detail/` change. Task
F-R (same sprint) added `detail/butterfly_density.hpp` — the one Lee/Roper
density stencil and its violation floor, previously hand-copied at four call
sites — so `detail/` 30 → **31**, no Tier-A/Tier-B change. Task F-8 (same
sprint) added `surface_overlay.hpp` — the smile-shift/sticky-mode algebra
hoisted out of `derivatives.cpp`'s private bump views — so Tier-B 32 → **33**,
then `deriv_pnl.hpp` — the two-date swap P&L attribution — so Tier-B 33 →
**34**, neither touching Tier-A or `detail/`.

That drift is now caught by a test rather than by a reader.
`VolUmbrella.TierCountsMatchTheReadmeTable` (`tests/vol_umbrella_test.cpp`)
**parses the first three Count cells out of the table above** and compares them
to the live header tree — Tier-A against the umbrella manifest, Tier-B and
`detail/` by counting `.hpp` files in the directories this table names. **This
table is the pin.** Editing a cell here changes what the test asserts, and a
header landing without a matching edit fails it; there is no literal in the test
to keep in step, and no update procedure to remember.

That is the second fix of this rot, and the first one did not hold. The test
originally held three integer literals and asked, in its failure text, for a
human to update this table — which is a request, not a mechanism. This very
sentence restated the triple as a fourth copy and had to be corrected at
`222b379`, `1e0b708` and `738c9b4` before going stale once more, every time
because a lane fixed the copy it was shown. Removing the restatement got the
count down to two copies; parsing the table got it to one.
Previously the Tier-A *set* was machine-checked but no **count** was, and
nothing compared any of them to this table at all, which is how three rows rotted
undetected. `simd/` 9, `tools/` 6 and `research/` 9 remain prose: they are
outside `include/atx/vol/` and are not covered by that test, so re-derive those
three by hand. The `+1 generated` on `detail/` is likewise uncovered — it does
not exist in the source tree the test walks.

**Re-derive rather than trust any digit here.** The commands are one line each:
`grep -c '^#include "atx/vol/' include/atx/vol/vol.hpp` is Tier-A; each remaining
row is `ls` over the directory the row names, minus (for Tier-B) Tier-A and
`vol.hpp` itself. The `+1 generated` on `detail/` is
`detail/version_generated.hpp`, configure_file'd from `project(atx VERSION ...)`,
so an install prefix carries exactly one more `detail/` header than the source
tree the test walks. That is stated as a relation rather than as a second pair of
digits: it read "29 there and the source tree 28" until this edit — the v1.0.0
numbers, three `detail/` additions out of date — because updating the table row
above never prompted anyone to update a restatement down here.

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

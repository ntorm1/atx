# rev-gaps report (feature gaps vs goal) — verbatim

## CAPABILITY INVENTORY

| Capability | Maturity | Evidence |
|---|---|---|
| American pricer (Andersen-Lake spectral collocation, ~10-11 sig figs; BAW fallback) | SOLID | `include/atx/vol/american.hpp` (andersen_lake, baw_american, presets al_fast/al_default) |
| Cross-strike boundary reuse (call slice, put homogeneity, sigma-Chebyshev interp; 8.1x board throughput) | SOLID | `american.hpp:114-245`, README |
| Warm-started AloPricer for IV sweeps | SOLID | `american.hpp:275-301` |
| Hot-path cached pricer (Black-76 + Chebyshev American correction, fixed-carry, 2-cache carry blend) | SOLID | `american.hpp:329-336`, `correction.hpp`, README (6.5 us query, 15.5x) |
| American IV inversion (safeguarded Newton/rtsafe, bracket-guaranteed, European seed, warm start, batch w/ per-lane Status) | SOLID | `american_iv.hpp` |
| European IV inversion (Stefanica-Radoicic seed + Halley) | SOLID | `implied_vol.hpp` |
| Greeks: delta/gamma/vega/theta/rho/vanna/volga/charm + price (cached analytic, FD 17-solve, AL 5-solve analytic, delta-only, vega-only) | SOLID | `american.hpp:346-487` (AmericanGreeks, american_greeks_fd/_al, american_delta, american_vega_al) |
| Adjoint (AAD) greeks — Christianson through-iteration, ~83% domain, FD fallback | SOLID (new) | `include/atx/vol/detail/adjoint_greeks.hpp`, `docs/adjoint_greeks_design.md`, `tests/adjoint_greeks_test.cpp` |
| Skew-adjusted delta (sticky-delta/strike blend, SpiderRock VegaSlope) | PARTIAL (delta only) | `adjusted_greeks.hpp` |
| SoA batch price/greeks (AVX2 boundary kernel gated OFF — scalar default; greeks batch = grouped scalar) | PARTIAL | `american_batch.hpp` ("Honest scope" note) |
| Discrete cash dividends → forward (escrowed Battig-Jarrow + Klassen hybrid blend + borrow) | SOLID (forward-level) | `curve.hpp:156`, `dividend.hpp` (hybrid_forward) |
| Discrete dividends IN the American pricer | MISSING | `american.hpp:92-93` ("cash divs folded into the forward beforehand"); README "Deferred: native discrete-cash-dividend PDE American pricer"; `PricingRoute` slot 3 `DISCRETE_DIV_FD_CACHE` reserved/removed (`types.hpp:47`) |
| Dividend/borrow sensitivities (dP/dDiv, dP/dq, dDelta/dDiv) | MISSING | No API anywhere; `AmericanGreeks` has 8 greeks only; adjoint computes internal dP/dq for European rung but does not expose it (`detail/adjoint_greeks.hpp:32`) |
| Rate term structure | PARTIAL | `curve.hpp` YieldCurve (Fritsch-Carlson); `market_env.hpp` (fit lowers to ONE representative rate); `priced_surface.hpp:339-341` rate_at(T) per-slice from stored df |
| Implied borrow from PCP (de-Am fixed point, robust multi-pair, warm start, leave-one-out confidence, term-structure interp/extrap fallback) | SOLID | `deamer.hpp` (imply_term_borrow, CarrySource, CarryDiagnostics), `dividend.hpp` (imply_borrow_european_pcp) |
| HTB detection (persistence-gated q_eff sweep) | SOLID | `curve.hpp:229-282` |
| Negative-rate regimes | PARTIAL | `american.hpp:489-517` classify_regime: double-continuation (yield<rate<=0) returns NotImplemented/NaN by design; `tests/american_negrate_domain_test.cpp` |
| Event/earnings vol — fit+serve layer (eMove censoring, FLEX recombination, censored term fit) | SOLID (fit layer) | `event_vol.hpp`, `earnings_term_fit.hpp`, `sr_tenor_grid.hpp` |
| Event awareness IN pricing serve layer (PricedSurface) | MISSING | PricedSurface carries no EventSchedule; event-aware w only in `projection.hpp` w_on_inserted_slice + session eMove solve |
| Vol-time clock (SpiderRock hybrid trading/non-trading clock, NYSE calendar 2024-28) | PARTIAL | `vol_time.hpp` — TimeSpec opt-in; calendar hardcoded 2024-2028, no half-days; greeks/theta not clock-aware |
| Settlement / expiry conventions (AM/PM settle, expiry instant) | STUB | `src/opra_panel.cpp:483-490` — expiry parsed to MIDNIGHT UTC of expiry date; 0DTE/same-day contracts DROPPED at ingest; no AM/PM settle flag anywhere |
| Adjusted/non-standard contracts | STUB | `occ_ess.hpp` — parses OCC ESS report but only used as an EXCLUSION list; no adjusted-deliverable pricing |
| Early-exercise boundary exposure / exercise probability / exercise flagging | MISSING | `al_boundary_at` internal-only (`src/american.cpp:661`); no public boundary, no P(exercise), no should-exercise API |
| IV inversion garbage-quote handling | SOLID-ish | `american_iv.hpp`: OutOfRange on sub-intrinsic/above-upper, intrinsic clamps to kIvMin=0.005, Unavailable on non-convergence; crossed/locked flagged at install (`data.hpp` kQFlagCrossed); NaN + counted provenance in `pricer_fitter.hpp` ChainValuation |
| Parity/acceptance metrics, arb checks, fallback carry, admission gates | SOLID | `parity.hpp`, `arb.hpp`, `deamer.hpp`, `pricer_fitter.hpp` |
| Real-data breadth | PARTIAL | Real OPRA: SPY (13.9k contracts/35 exp) + XOM; spy_top50 universe csv; accuracy panel success=MU, failure=BRK.B; no committed multi-name earnings/HTB/0DTE fixtures |

## GAPS

1. **No discrete-dividend American pricer (escrowed q_eff only).** AL pricer takes continuous carry q; discrete cash divs folded into F via q_eff bridge (`deamer.hpp:34-46`). Misprices early-exercise decision for deep-ITM calls around ex-dates + short-dated ITM puts spanning ex-date. README defers "native discrete-cash-dividend PDE American pricer"; `DISCRETE_DIV_FD_CACHE` route slot removed. Severity: **MAJOR** (BLOCKER for names with large divs vs spot). Scope: CN/FD or hybrid AL-with-jump pricer honoring `DividendEvent` schedule directly; wire as `AmericanMethod::DiscreteDivPde` with PDE golden oracle (`tests/support/oracle_pde_golden.hpp`) promoted to production; keep q_eff path as fast tier.

2. **No dividend or borrow sensitivities.** `AmericanGreeks` = {delta,gamma,vega,theta,rho,vanna,volga,charm}. No dP/dq, dP/dDiv_i, dDelta/dDiv, borrow sensitivity. Adjoint European rung computes ∂P/∂q but drops it at API (`detail/adjoint_greeks.hpp:32-38`). PnL attribution (`run_report.hpp`) has no div/borrow axis → borrow re-marks land in "unexplained". Severity: **MAJOR**. Scope: add q-sensitivity to american_greeks_al/adjoint (q± pair symmetric to r±), expose dP/dq, chain-rule to per-event dP/dDiv through hybrid_forward. ~1-2 wk.

3. **Settlement/expiry-instant conventions wrong-by-construction; 0DTE dropped.** Expiries parsed to midnight-UTC of OSI date (`opra_panel.cpp:483-490`): T mis-stated ~0.8 trading day for every PM-settled contract; same-day (0DTE) contracts hard-dropped at ingest. No AM/PM settlement flag anywhere. For weeklies/0DTE — highest-volume segment — library cannot fit front expiry at all on real data; short-dated T bias contaminates front of every term structure (event-vol `count_between` uses same instants). Severity: **BLOCKER**, cheap to fix. Scope: stamp true expiry instants (16:00 ET PM equity, 09:30 AM-settled index) in OSI parsing + `ExpiryInputs`, remove 0DTE drop, combine with VolTime intraday clock for T>0 through session; audit every `iso_to_ns(expiry)` consumer.

4. **Event/earnings vol never reaches pricing serve layer.** eMove/censoring complete in fit layer; `projection.hpp` queries event-aware; but `PricedSurface` carries no `EventSchedule`. Query T straddling earnings interpolates total variance linearly, smearing jump: fair values + theta around earnings wrong. Severity: **MAJOR**. Scope: embed optional EventSchedule + eMove in `PricingContext`/archive schema, route `resolve()` sigma through `event_aware_w`, define event-carved theta; touches surface_archive format (versioned).

5. **Early-exercise boundary / exercise probability not exposed.** Chebyshev boundary internal-only (`al_boundary_at`, `src/american.cpp:661`); no public critical price, P(early exercise), should-exercise/assignment-risk flag. Severity: MINOR-to-MAJOR. Scope: small — expose `exercise_boundary(K,T,r,q,sigma)` from retained AL state + assignment heuristic (div vs remaining time value); discrete-div part depends on gap 1.

6. **Rate term structure only partially plumbed.** Fit path lowers `MarketEnv` to ONE representative rate (front expiry), borrow solve absorbs residual term structure (`market_env.hpp:20-29`, `chain.hpp:97`). Conflates rate TS with borrow: implied borrow for 2y LEAP embeds SOFR curve slope, corrupting HTB signal + dP/dr. Severity: MINOR fit / **MAJOR** borrow analytics long-dated. Scope: thread `rate_at(T_i)` per expiry through de-Am/carry solve + correction-cache bake; serve side already term-rate capable.

7. **Double-continuation (negative-rate) regime NotImplemented.** By design fail-closed. Severity: MINOR. Defer.

8. **AVX2 batch pricing gated off; no vectorized Greek stencil.** AVX2 boundary kernel measured 1.7x < 2.0x ship gate → batch = scalar loop; greeks batch = grouping over scalar routes. Throughput rests on boundary-reuse + correction cache (93.6k inv/s/4-core). Severity: MINOR (documented, measured, mitigated).

9. **Fit-layer capabilities pricing layer can't honor.** (a) VolTime: theta is calendar ∂/∂T, no clock conversion — theta off VolTime-fitted surface in vol-time units un-flagged; calendar hardcoded 2024-2028, no half-days (`vol_time.hpp:44-52`). (b) Correction caches carry-baked — large intraday borrow move silently invalidates within stale-gates. (c) Sticky dynamics delta-only; no sticky gamma/theta. Severity: MINOR each, MAJOR aggregate for earnings-week 0DTE desk.

10. **Quote-quality salvage drop-only.** Sub-intrinsic → OutOfRange → NaN lane. No nearest-arb-consistent projection to salvage deep-ITM info. Acceptable given OTM-leg fitting design. Severity: MINOR.

**Bottom line.** Core pricing/IV/borrow/greeks SOTA-grade + heavily tested (SPY/XOM). Three things between library and "any US underlying": (1) expiry/settlement instants + 0DTE ingestion (BLOCKER, cheap), (2) discrete-div-aware American exercise + div/borrow risk (MAJOR), (3) earnings machinery into `PricedSurface` (MAJOR, schema-versioned).

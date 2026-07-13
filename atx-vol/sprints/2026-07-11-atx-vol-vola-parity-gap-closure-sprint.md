# atx-vol Vola-Parity Gap-Closure Sprint (V-sprint)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans. Each package below gets a detailed
> bite-sized TDD plan under `docs/superpowers/plans/` at execution time (house pattern,
> mirroring `2026-07-07-atx-vol-surface-noarb-integrity.md`).

**Date:** 2026-07-11

**Status:** implementation-ready charter. Every claim about VolaDynamics carries a primary
URL from an adversarially-verified deep-research pass (2026-07-11, 18 sources, 25 claims
verified 3-vote, 3 marketing claims explicitly refuted); every claim about our code carries
`file:line` from a fresh two-stream audit (feature inventory + perf/quality review).

**Goal:** close the *feature* gaps between atx-vol and VolaDynamics' verified product
surface for American equity options pricing and fitting — the features that make a surface
"market-maker quality" — without duplicating the performance work already owned by prior
sprints.

**Architecture:** eight additive packages (V0–V7) layered on the existing canonical stack
(ALO pricer → de-Am → curve families → `PricedSurface` → `PortfolioPricer`). New
capability lands as new headers + opt-in routes; every default path stays bit-identical.

**Tech stack:** C++20, `atx::vol`, GoogleTest, Google Benchmark, clang-cl 18, `/W4 /WX`.

## Global constraints

1. Reference scalar output bit-identical where the API promises it; all new routes opt-in.
2. No global `/fp:fast`, no `-ffast-math`; AVX2 per-object or preset-only (C-sprint C0.4).
3. No-arbitrage is a gate: every fitted slice passes butterfly + calendar validators,
   including the new event-aware paths.
4. `Result<T>` error handling; no exceptions on hot paths; NaN only behind documented
   invalid lanes.
5. Deterministic totals across worker count (fixed input-order reduction).
6. New perf surface carries its own budget (§10); no regression on P/C-sprint gates.
7. Full gate green + warnings-as-errors + baseline benches within tolerance before merge.

---

## 1. Relationship to prior sprints — what this sprint is NOT

Three plans already own most of the competitiveness map. This sprint deliberately does
**not** duplicate them:

| Plan | Owns | Status |
|---|---|---|
| P-sprint `2026-07-09-american-pricing-portfolio-throughput` | American kernel perf (P0–P6): boundary hoist, templated kernels, AoSoA AVX2, warm state, correction cache, LTO/PGO | executed through P2.1; P2.2 in flight in worktree |
| C-sprint `2026-07-10-atx-vol-full-stack-competitiveness` | productization + throughput (C0–C7): bench credibility, stranded-fast-path wiring, vectorized calibrator, scenario grid + attribution, AAD, surrogate cache, dispersion, legacy retirement | locked, worktree started |
| Workmodule ladder A–H `2026-07-07-atx-vol-sota-engine-workmodule` | no-arb integrity (A), C8/CStar unification (B), cached Greeks all kinds (C), unified risk engine (D), deep-wing no-arb (E), discrete-div PDE + rate bootstrap (F), quote book (G), oracles/fuzz (H) | A in flight, B–H chartered |

**This sprint (V) owns the remaining product-feature axis:** the VolaDynamics module and
convention surface that no prior plan touches — extended Greeks, SSR smart delta, event/
earnings vol, Bayesian graduated-defense fitting, dividend implication, VIX module, 0DTE
robustness, and the negative-rate double-boundary completeness hole.

**Performance stance.** The two verified performance gaps — ~2.6× scalar price-throughput
deficit vs the 45k/s tastyhedge anchor (kernel at its scalar transcendental floor,
`sprints/2026-07-09:291-294`) and the 0.36 s de-Am-dominated cold surface fit
(`src/calib.cpp:336`) — are owned by P2.2/C4.1 and C2 respectively. V-sprint's perf
contribution is (a) hard budgets so new features don't erode those wins (§10), and (b) the
V3 prior/warm layer, which directly reduces LM iterations on refits (the C1.6/C2 warm-start
machinery gets a statistically principled prior instead of a bare seed).

---

## 2. What VolaDynamics verifiably is (deep-research pass, 2026-07-11)

Confidence key: ✓✓ = 3-0 adversarial vote on primary sources; ✓ = 2-1 or secondary.

- **Company/product** ✓✓: Vola Dynamics LLC (Timothy Klassen, ex-Goldman, designer of the
  2003 CBOE VIX methodology). Native C++/Python/Java/C# library, Windows/Linux/macOS,
  embedded drop-in model (no SaaS). Modules: **Pricer, Fitter, Curves** + **PnL
  Explanation, Vol Derivatives, VIX Pricer, Discount Curve Fitter, Div Fitter, Event
  Modeling, Event Var Fitter, FX**. (https://voladynamics.com/faq/, /products)
- **Curves** ✓✓: nested parametric hierarchy — 3-param S3 core (≡ SSVI slice; verifier
  algebra confirmed s2=ρφ, c2=φ²(1−ρ²)/2) with published exact necessary-and-sufficient
  butterfly no-arb domain (SSRN 2725700), plus proprietary tiers C5/C6/C7/C8/C10/C12m for
  liquid names, **negative at-the-forward curvature**, and **W-shaped earnings/election
  smiles** (SPX/SPY/ES/NVDA/TSLA). Calendar arb removed algorithmically ("no-arb mode",
  error-bar-weighted closest fit). Far-wing arb-freedom is a *capability* claim, not a
  guarantee (guarantee framing refuted 0-3). (Klassen GD Chicago 2017 deck, slides 17–41)
- **Fitter** ✓✓: Bayesian filtering transferring information **across strikes, expiries,
  and time**; fits one term at a time, transfers info between terms; minimizes χ² + soft
  penalties (can fit terms with fewer effective quotes than parameters); per-vol **error
  bars in and out** (derived from bid-ask); **graduated defense** — error bars widen on
  degraded data instead of kill switches; temporal filtering fills missing data.
  (https://voladynamics.com/products/vola-fitter)
- **American pricing** ✓✓: de-Americanization workflow — pick rate → pick cash divs →
  **imply per-term borrow from American put-call parity** → imply vol-by-strike → fit.
  **Choice of several discrete cash-dividend models** (SSRN 2634051; advocated default:
  hybrid S = pure-GBM + cash buffer, blending cash→proportional at long terms). Internal
  numerical method unpublished ("without a table method"). Greeks computation method
  (analytic/AD/bumped) unpublished — "full analytic set" framing refuted 0-3.
- **Greeks** ✓✓: delta, gamma, vega, volga, vanna, rho, **rhoBorrow, rhoDiv**, **two
  thetas** (vol-time vs rate/calendar-time, reported separately, plus discrete one-day
  theta honoring weekends/holidays/events), **fugit**; **"smart" delta/gamma via a
  configurable Skew-Stickiness-Ratio (SSR), used consistently across greeks, scenario
  analysis, and PnL attribution**. (https://voladynamics.com/, /faq/)
- **0DTE** ✓✓: "handles 0DTE and daily expirations … maintaining calendar arbitrage
  constraints across dozens of daily [expiries]". (FAQ)
- **Performance** ✓✓ *as attribution only*: "whole US options universe (4000+ underliers)
  on one box", "surfaces in milliseconds", "fraction of a second, without a table method" —
  all vendor marketing, **no hardware, no counts, no independent benchmark anywhere**; the
  fact-asserted version was refuted in verification. Do not chase these as numbers; C-sprint
  anchor discipline (tastyhedge 45k/16.5k per core, CPU named) remains our quantitative bar.

**Refuted in verification (do not build against):** universe-in-<1s as fact (1-2);
guaranteed far-wing arb-free extrapolation (0-3); full-analytic-greeks-incl-cross (0-3).

---

## 3. Gap matrix (Vola feature → our status → owner)

| # | Vola feature | atx-vol status (evidence) | Owner |
|---|---|---|---|
| 1 | S3/SSVI exact butterfly domain | HAVE — s3.hpp; Mingone cube by construction (`vol_surface.cpp:163`); MM validator planned | Sprint A / C2.5 |
| 2 | Borrow implied from American PCP | HAVE — `deamer.hpp:34-46` q_eff bridge, American-PCP-band-aware | — |
| 3 | De-Americanization workflow | HAVE — `calib.hpp:315`, `american_iv.hpp`; fast path + bias fix planned | C2.3 |
| 4 | Quote filtering / drop cascade | HAVE — `arb.hpp` FilterOpts, 7-stage cascade (`calib.hpp:260-281`) | — (V3 softens it) |
| 5 | Per-vol error bars (out) | HAVE but **unconsumed** — `fit_metrics.hpp:11` standalone | **V3** |
| 6 | Bayesian cross-expiry/temporal priors, χ²+soft penalties, graduated defense | **MISSING** — IRLS-Huber per-slice only; drivers pass no warm (`essvi_calib.cpp:919`) | **V3** |
| 7 | W-shaped earnings smiles + event modeling + Event Var Fitter | **MISSING** — C8 admits negative ATM curvature (`c8.hpp`) but no event calendar, no event-var decomposition, no W fit | **V2** |
| 8 | Higher-tier curve families unified | PARTIAL — C8 unified; CStar standalone-only (`vol_surface.hpp:64-68`) | Sprint B |
| 9 | Discrete cash-div model **choice** | PARTIAL — hybrid Klassen blend only (`dividend.hpp:77-86`); native div PDE deferred | Sprint F + **V4** |
| 10 | Div Fitter (imply cash divs from prices) | **MISSING** — divs are inputs; only borrow is implied | **V4** |
| 11 | Discount Curve Fitter (bootstrap) | MISSING — pillar interpolation only (`curve.hpp:75-106`) | Sprint F4 |
| 12 | rhoBorrow, rhoDiv | **MISSING** — 8-Greek set has single rho (`american.hpp:238`) | **V0** |
| 13 | Two thetas + discrete 1-day theta (calendar-aware) | **MISSING** — single calendar theta (`greeks.hpp`) | **V0** |
| 14 | Fugit | **MISSING** | **V0** |
| 15 | SSR smart delta/gamma, consistent across greeks/scenario/PnL | **MISSING** — spot-based delta only | **V1** |
| 16 | Scenario grids + PnL attribution (ATF/skew/curvature) | planned | C3 / Sprint D |
| 17 | VIX Pricer module | PARTIAL — var-swap strip + Carr–Lee exist (`derivatives.hpp`); no VIX-methodology index | **V5** |
| 18 | Vol Derivatives module | LARGELY HAVE — `derivatives.hpp` | — |
| 19 | 0DTE/daily expiries at maintained calendar no-arb | UNAUDITED — short-T edge cases known fragile (σ→0/T→0, `sprints/2026-07-09:862`); linear-in-w interp untested on daily ladders | **V6** |
| 20 | Negative-rate double-continuation American | **MISSING** — `classify_regime` returns NotImplemented (17 sites, `src/american.cpp:1234,…`) | **V7** (stretch) |
| 21 | PnL Explanation module | planned | C3.3 |
| 22 | FX module | out of scope (equities library) | non-goal |

Rows 5–7, 9–10, 12–15, 17, 19–20 are this sprint. Everything else: verified owned elsewhere.

---

## 4. Work packages

Package IDs `V0…V7` (no collision with P0–P6, C0–C7, Sprints A–H).

### V0 — Extended Greek set: rhoBorrow, rhoDiv, dual thetas, fugit
**Est.** 4 d · **Risk** low–medium · **Value:** direct convention parity; market makers ask
for these by name.

**Files:** modify `include/atx/vol/american.hpp` (+`src/american.cpp`),
`include/atx/vol/greeks.hpp`; create `include/atx/vol/trading_calendar.hpp`
(+`src/trading_calendar.cpp`), tests `tests/greeks_ext_test.cpp`.

**Interfaces (produced):**
```cpp
struct AmericanGreeksExt {          // additive; AmericanGreeks unchanged
  double rho_borrow;                // dP/d(borrow rate), per 1bp reported *100
  double rho_div;                   // dP/d(parallel relative div bump), per 1%
  double theta_vol;                 // vol-time decay: dP/dt holding carry/df fixed
  double theta_rate;                // carry/df decay: total calendar theta - theta_vol
  double theta_1d;                  // discrete next-trading-day theta (calendar-aware)
  double fugit;                     // expected exercise time (years), = T if never
};
Result<AmericanGreeksExt> american_greeks_ext(const AmericanInputs&, const ExtOpts&);
class TradingCalendar { /* is_trading_day, next_trading_day, year_fraction */ };
```

**Tasks:**
- **V0.1 rhoBorrow.** Borrow enters as continuous carry through `q_eff` (`dividend.hpp`);
  analytic on the cached/B76 leg via the existing q-partial chain, central-FD re-solve on
  the cold ALO leg (borrow bump → new boundary; reuse the S-independence seam,
  `american.cpp:1059-1067`). Gate: FD parity ≤$0.001/share per 1bp on the corner grid.
- **V0.2 rhoDiv.** Parallel relative bump of the discrete cash schedule (all `DividendEvent`
  amounts ×(1+ε)) → escrowed forward shift → re-solve. Also per-event variant behind the
  same API (vector output, opt-in). Gate: FD parity; zero for div-free names, exactly.
- **V0.3 Dual thetas.** Decompose existing calendar theta: `theta_vol` = PDE time-decay at
  frozen carry (we already extract PDE theta on the analytic path, `american_greeks_al`,
  `american.hpp:300`); `theta_rate` = total − theta_vol, cross-checked against an explicit
  df/carry-roll bump. Report both; existing single `theta` unchanged. Gate: components sum
  to the existing theta to 1e-10; pure-vol name (r=q=0) has theta_rate ≈ 0.
- **V0.4 Discrete 1-day theta + `TradingCalendar`.** NYSE calendar (weekends + fixed
  holiday rules; data table checked in, no network). `theta_1d = P(t + next trading day,
  schedule-aware) − P(t)` — crosses ex-div dates and (post-V2) event dates correctly.
  Gate: Friday→Monday theta ≈ 3 calendar days of decay; ex-div crossing matches escrowed
  forward drop.
- **V0.5 Fugit.** Primary route: backward-induction expected-exercise-time on the hardened
  CN PDE (Sprint F1 promotes `oracle_pricer_pde` to production; if F1 has not landed,
  implement on the test oracle and mark the production hook). Cheap route for the cached
  path: fugit from boundary-hitting probability via the existing GL quadrature over the
  solved ALO boundary (first-passage integrand shares `al_bind_geometry` machinery).
  Gate: fugit(T→0) → T; deep-ITM put fugit ≈ 0; parity PDE-vs-quadrature ≤1e-2·T.
- **V0.6 Portfolio plumbing.** Optional `AmericanGreeksExt` block on `PortfolioPricer`
  behind `PriceFieldMask` (off by default; column-major per C4.4 conventions).

**Acceptance:** all new Greeks FD-validated on the OPRA corner grid (§9); default bundles
bit-identical; batch path allocates nothing in steady state; docs updated in `greeks.hpp`.

### V1 — SSR "smart" delta/gamma (spot-vol dynamics layer)
**Est.** 3 d · **Risk** medium · **Depends:** V0 (lands in same Greek plumbing); C3
interfaces (consume-only).

**Files:** create `include/atx/vol/smart_greeks.hpp` (+`src/smart_greeks.cpp`); modify
`include/atx/vol/session.hpp` (per-name dynamics config), tests `tests/smart_greeks_test.cpp`.

**Interfaces (produced):**
```cpp
struct SpotVolDynamics { double ssr = 1.0; };   // 0 = sticky-strike … ~1.5 typical index
Greeks smart_adjust(const Greeks& raw, const SpotVolDynamics&,
                    double skew_slope /* dsigma/dlnK at k */, double vega, double vanna);
// smart_delta = delta + vega * ssr * skew_slope / S ; smart_gamma chains one order higher
```

**Tasks:**
- **V1.1 Core adjustment.** Smart delta/gamma from the fitted curve's analytic skew slope
  (`IVolCurve::w′` exists for every unified family). Exact convention documented: under a
  spot move dS, ATM vol moves by `ssr · skew_slope · dlnS`. Gate: ssr=0 reproduces raw
  (sticky-strike) delta bit-identically; ssr=1 matches an FD sticky-moneyness re-mark.
- **V1.2 Config + estimator.** Per-name `SpotVolDynamics` in `session.hpp`/`profile.hpp`;
  optional historical SSR estimator (regression of dATM-vol on d-lnS · skew) as a
  diagnostic, never a silent default. Gate: config round-trips; estimator reproduces a
  planted SSR on synthetic dynamics to ±0.05.
- **V1.3 Consistency contract.** The same `SpotVolDynamics` consumed by C3's scenario
  re-marks and C3.3 attribution axes (interface handshake documented in both sprints; if
  C3 lands first, wire theirs; if not, define here and C3 consumes). Gate: scenario spot
  bump with ssr=s equals smart-delta prediction to 2nd order on small bumps.

**Acceptance:** smart greeks opt-in, raw defaults untouched; FD parity gates green; one
documented SSR convention shared by greeks/scenario/attribution.

### V2 — Event / earnings vol modeling *(flagship gap)*
**Est.** 9 d · **Risk** high · **Depends:** Sprint A (calendar floor machinery); benefits
from Sprint B (C8/CStar tiers) but does not require it.

**Files:** create `include/atx/vol/event.hpp` (+`src/event.cpp`),
`include/atx/vol/event_fitter.hpp` (+`src/event_fitter.cpp`); modify
`include/atx/vol/vol_curve.hpp` (event-aware `CurveSurface` interpolation),
`include/atx/vol/arb.hpp` (event-aware calendar validator),
`include/atx/vol/surface_archive.hpp` (event schedule + fitted vars in blob; schema-hash
bump), tests `tests/event_test.cpp`, `tests/event_fitter_test.cpp`.

**Model.** Total implied variance decomposes as
`w_total(k,T) = w_diff(k,T) + Σ_{events e: t_e ≤ T} w_e(k)` — a smooth diffusive backbone
plus per-event variance bumps. Near-term pre-earnings smiles get the W shape from a
two-point (or two-component-Gaussian) jump mixture for the event component (research
anchor: mixture models reproduce W-smiles, arXiv:2209.14726; Klassen's C-tier W capability
is proprietary — we build the published-math equivalent). ATM event var `v_e = w_e(0)` is
the headline "implied earnings move" number.

**Tasks:**
- **V2.1 `EventSchedule` type + plumbing.** `{date, kind, prior_ann_var}` per underlier;
  flows chain → session → fit → `PricedSurface` → archive. Gate: round-trips through
  ATXVSA (schema-guarded); empty schedule is bit-identical everywhere.
- **V2.2 Event-aware term interpolation.** `CurveSurface` time-interpolation on the
  *diffusive* variance with event vars added as step functions at `t_e` — stops earnings
  variance smearing across expiries (today linear-in-w smears it,
  `vol_curve.hpp:228-280`). Gate: two expiries bracketing an event interpolate with the
  full event var in every T ≥ t_e query and none before; no-event surface bit-identical.
- **V2.3 Event Var Fitter.** Imply `v_e` from co-terminal expiry pairs bracketing each
  event (ATM forward-variance difference net of interpolated diffusive var), then jointly
  refine {v_e} + smooth diffusive term structure by χ² over all expiries (reuses LM
  machinery). Handles multiple events per horizon and dailies around the event. Gate: on a
  synthetic board with planted v_e, recovery ≤2% rel; on a real pre-earnings board (NVDA or
  TSLA via OPRA loader) the implied move matches the bracketing-straddle heuristic within
  documented tolerance.
- **V2.4 W-shaped pre-event slice.** Event smile component `w_e(k)` from the two-point
  jump mixture (±J with prob p): closed-form European mixture price → additive w-space
  contribution; composed with the fitted diffusive slice. Fit {J, p} (or symmetric J) per
  event from the pre-event slice residual. Gate: reproduces a W (two local minima) on a
  synthetic mixture board; improves in-band fraction vs plain eSSVI on the real pre-event
  board; composite slice passes the butterfly validator.
- **V2.5 Event-aware no-arb.** Calendar validator (`arb.hpp`) understands event steps:
  monotonicity required of `w_diff` with event vars added at the step — a calendar
  violation is only flagged when the *decomposed* surface violates. Butterfly checked on
  the composite slice. Gate: SPY dailies around an event: 0 false calendar positives, 0
  missed true violations on planted cases.
- **V2.6 Integration.** V0.4 `theta_1d` consumes the schedule (event-day decay); V5
  event-adjusted variance consumes {v_e}; dispersion/backtest read implied-move as a
  signal input (wiring only; strategy work out of scope).

**Acceptance:** planted-event recovery gates green; real pre-earnings board demonstrably
better-fit (in-band + reduced χ²) with the event layer than without; no-event paths
bit-identical; archive round-trip; all composite slices arb-validated.

### V3 — Bayesian fitting layer: priors, error-bar weighting, graduated defense
**Est.** 6 d · **Risk** medium · **Depends:** none (C1.6 warm-start plumbing helps; this
supersedes its bare seed with a principled prior).

**Files:** create `include/atx/vol/fit_prior.hpp`; modify `include/atx/vol/calib.hpp`
(weights/penalties), `src/essvi_calib.cpp` + `src/svi_calib.cpp` (prior term in LM/IRLS),
`include/atx/vol/fit_metrics.hpp` (consume, not just report), `src/curve_fit.cpp`
(cross-expiry prior threading), `include/atx/vol/priced_surface.hpp` (serve error bars),
tests `tests/fit_prior_test.cpp`.

**Tasks:**
- **V3.1 Error bars in.** Per-quote σ-error bars (already computed from bid-ask via vega,
  `fit_metrics.hpp`) become the χ² weights `w_i = 1/σ_err,i²` behind
  `CalibOpts::weight_mode = ErrorBar` (default `Legacy` unchanged, bit-identical). Gate:
  reduced χ² on the corpus becomes ~O(1) for good boards; legacy mode untouched.
- **V3.2 Soft-penalty priors (χ² + penalties).** Add a Tikhonov/prior term
  `(θ−θ_prior)ᵀ Λ (θ−θ_prior)` to the LM normal equations for eSSVI/SVI (the `warm` +
  `prior_strength` seam exists, `essvi_calib.cpp:646,919` — make it a real MAP term, not
  just a seed). Enables fitting slices with fewer quotes than parameters (sparse weeklies).
  Gate: a 3-quote slice fits stably with a prior and refuses (clean `Err`) without one;
  well-determined slices move ≤0.1·error-bar vs no-prior fit.
- **V3.3 Cross-expiry prior transfer.** Ascending-T pass: slice t_{i+1} gets prior from
  slice t_i's posterior (params + covariance proxy from the LM JᵀWJ). This is the "fit one
  term at a time, transfer information between terms" workflow. Serial dependency respects
  the C2.1 parallel design: prior pass is the already-serial Phase-2 loop
  (`curve_fit.cpp:272,304`). Gate: term-structure smoothness (param jumps between adjacent
  expiries) improves ≥30% on the SPY board at equal in-band.
- **V3.4 Temporal prior (cross-snapshot).** Previous snapshot's posterior as the prior with
  data-quality-scaled strength — the principled version of C1.6 warm-start. Gate: warm
  refit LM iterations drop ≥40% on the real corpus (C-sprint C1.6's own gate, now met via
  the MAP route) with fit quality within error bars of cold.
- **V3.5 Graduated defense.** Stale/locked/crossed/wide quotes get *inflated error bars*
  (configurable multipliers per `QuoteFlag`) instead of hard drops for all but the fatal
  flags; the 7-stage drop cascade (`calib.hpp:260-281`) becomes stage-configurable. Gate:
  on a corrupted-board test (10% stale + 5% crossed planted), graduated mode beats
  drop-mode held-out in-band; a fully-degraded board degrades smoothly (error bars widen,
  fit survives) instead of emptying the slice.
- **V3.6 Error bars out.** `PricedSurface` serves per-vol error bars (from fit posterior +
  quote error bars) via a `vol_err(k,T)` query; archived (schema bump shared with V2.1).
  Gate: round-trip; error bars widen monotonically with quote spread on synthetic boards.

**Acceptance:** legacy paths bit-identical; MAP/error-bar modes opt-in per `FitPreset`;
all §9 fit gates; warm-refit iteration gate met; graduated defense measurably beats drops
on corrupted boards.

### V4 — Div Fitter + dividend-model menu
**Est.** 5 d · **Risk** medium–high · **Depends:** Sprint F2 (PDE div jumps) for the
piecewise-GBM oracle leg; V4.1 independent.

**Files:** create `include/atx/vol/div_fitter.hpp` (+`src/div_fitter.cpp`); modify
`include/atx/vol/dividend.hpp` (`DividendModel` enum), `include/atx/vol/deamer.hpp`
(joint borrow+div), tests `tests/div_fitter_test.cpp`.

**Tasks:**
- **V4.1 Joint borrow + dividend implication.** Today we imply borrow with divs as inputs
  (`imply_term_borrow`, `deamer.hpp`). Add joint estimation: across expiries, per-term PCP
  gives `(borrow_t, PV_divs_t)` pairs; disentangle with the structural constraint that divs
  are a discrete quarterly-ish schedule (amounts piecewise-constant/slow-growing) while
  borrow is a smooth term curve — regularized least squares over the expiry ladder, using
  deep-ITM/OTM pairs where early-exercise premium is smallest (reuse the American-PCP band
  from `deamer.hpp`). Gate: on synthetic chains with known (borrow, divs), both recovered
  ≤5% rel; on a real dividend payer (XOM board), implied schedule within tolerance of the
  known declared dividend.
- **V4.2 `DividendModel` menu.** Enum {`EscrowedForward` (have), `HybridBlend` (have,
  default), `SpotPiecewiseGBM` (via Sprint F2 PDE), plus blend-parameter presets matching
  SSRN 2634051's recommendation (cash short-term → proportional long-term)}. One switch on
  the pricing entry points; de-Am round-trips through the selected model. Gate: models
  agree in the no-div limit bit-identically; documented price differences on the
  dividend-heavy corner grid vs the F2 PDE oracle.
- **V4.3 Model-choice validation report.** A diagnostic (example + test) scoring each model
  vs the PDE oracle across the div-heavy grid (short/long T, small/large D, near/far
  ex-date) — the evidence page for our default. Gate: report generated in CI-lite; hybrid
  default is within its documented error band everywhere.

**Acceptance:** joint implication recovers planted truth; declared-div reconciliation on a
real name; model menu opt-in with bit-identical defaults; validation report committed.

### V5 — VIX / variance module
**Est.** 3 d · **Risk** low · **Depends:** V2 for event-adjusted variant (V5.3 only).

**Files:** create `include/atx/vol/vix.hpp` (+`src/vix.cpp`); modify
`include/atx/vol/derivatives.hpp` (cross-wiring), tests `tests/vix_test.cpp`.

**Tasks:**
- **V5.1 Cboe-methodology VIX calc.** 30-day (and arbitrary-horizon) variance index from
  OTM strips per the Cboe 2003 methodology (Klassen designed it — direct parity): strip
  sum with ΔK weighting, forward from PCP, two-expiry time interpolation. Two inputs:
  raw quote board (chain-direct) AND fitted surface (strip from `PricedSurface`). Gate:
  chain-direct on the SPY board reproduces a hand-computed reference to 1bp of vol;
  surface-vs-chain spread reported (fit-quality diagnostic).
- **V5.2 Term structure.** VIX9D/30D/3M/6M analogues for any underlier; wired next to the
  existing var-swap strip in `derivatives.hpp` (share the strip integrator; kill any
  duplicate quadrature). Gate: monotone-input round-trip; matches `derivatives.hpp`
  var-swap rate in the continuous-strike limit.
- **V5.3 Event-adjusted variance.** Subtract V2 event vars from the index for an
  "ex-earnings clean vol" measure. Gate: planted-event synthetic recovers clean vol ≤1%.

**Acceptance:** chain-direct + surface-based indices agree within documented band; parity
vs var-swap strip; docs describe conventions (settlement, interpolation) precisely.

### V6 — 0DTE / short-dated robustness
**Est.** 4 d · **Risk** medium · **Depends:** none.

**Files:** modify `src/american.cpp` (short-T guards), `include/atx/vol/data.hpp` (time
conventions), `src/curve_fit.cpp` + `include/atx/vol/arb.hpp` (daily-ladder calendar),
tests `tests/zero_dte_test.cpp`; bench scenario in `bench/`.

**Tasks:**
- **V6.1 Intraday time convention.** Year-fraction resolution to minutes with a documented
  convention (calendar-time ACT/365F carried consistently pricing↔fit↔greeks; trading-time
  variant explicitly out of scope this sprint). Audit every `T` producer (`data.hpp` ISO
  kernels, OPRA loader) for silent day-floor truncation. Gate: a T=2-hours option prices,
  fits, and inverts without hitting the T≤0 degenerate lane.
- **V6.2 Short-T numerics audit.** ALO boundary and IV inversion at T ∈ {1e-4 … 3/365}:
  boundary solve stability (σ√T scaling), vega-collapse handling in `american_iv` (the
  bracket+bisection fallback must engage, not NaN), correction-cache box clamps near the
  T-edge (cold fallback verified). Gate: property sweep (σ×m×T grid at tiny T) — no NaN,
  price within PDE-oracle tolerance, IV round-trip ≤1e-4 where vega > threshold, clean
  `Unavailable` elsewhere.
- **V6.3 Daily-ladder calendar integrity.** Calendar validator + Sprint-A floor across
  dozens of daily expiries (SPY corpus has them): adjacent-day slices with hours of
  T-difference must not false-positive, and event steps (V2) on dailies must not trip it.
  Gate: full SPY daily ladder fits with 0 calendar violations by construction and in-band
  quality within the standing SPY gate.
- **V6.4 0DTE bench + gate.** `zero_dte` scenario in the bench suite (fit + IV + greeks on
  the front daily slice) with a checked-in baseline, so short-T perf/quality regressions
  gate like everything else. Gate: baseline committed, CV ≤5%.

**Acceptance:** T-convention documented and uniformly applied; short-T property sweep
green; SPY daily ladder arb-free at held quality; 0DTE bench gated.

### V7 — Negative-rate double-boundary American *(stretch)*
**Est.** 4 d · **Risk** high · **Depends:** none. Ship-or-kill: implement only if V0–V6
land early; otherwise carries to the next sprint with this charter as spec.

**Files:** modify `src/american.cpp` (double-continuation solver), `american.hpp` (regime
table), tests `tests/american_negative_rate_test.cpp`.

**Tasks:**
- **V7.1** Implement the double-continuation-region case (`r<0` with `r<q<0` / `q<r<0`) per
  Andersen–Lake 2021 (Wilmott) / Healy (arXiv:2109.15157): two boundaries, same Chebyshev/
  GL machinery mirrored, `classify_regime` (`american.hpp:355`) routes instead of returning
  `NotImplemented` (17 sites, `src/american.cpp:1234` et al.). Gate: parity vs the CN PDE
  oracle (which handles negative rates natively) ≤1e-6 on the negative-rate grid; all
  positive-rate paths bit-identical; the 17 NotImplemented sites collapse to routed code.

**Acceptance:** negative-rate grid priced to oracle parity, or the package is explicitly
killed with the measurement attached.

---

## 5. Delivery sequence (6 weeks, two sub-agent lanes)

| Week | Greeks/pricing lane | Fitting/surface lane |
|---|---|---|
| 1 | **V0.1–V0.4** (rhoBorrow/rhoDiv/thetas/calendar) | **V3.1–V3.2** (error bars in, soft penalties) |
| 2 | **V0.5–V0.6** (fugit, portfolio plumbing) · **V1.1–V1.2** | **V3.3–V3.5** (cross-expiry, temporal, graduated defense) |
| 3 | **V1.3** (SSR consistency contract) · **V4.1** (joint borrow+div) | **V2.1–V2.2** (event schedule, event-aware interp) |
| 4 | **V4.2–V4.3** (div model menu + validation) | **V2.3–V2.4** (event var fitter, W slice) |
| 5 | **V5.1–V5.3** (VIX module) | **V2.5–V2.6** (event no-arb, integration) · **V3.6** |
| 6 | **V7** (stretch) or buffer | **V6** (0DTE robustness + bench) |

Cross-sprint handshakes: V1.3 ↔ C3 (SpotVolDynamics interface); V0.5 ↔ Sprint F1 (PDE
promotion); V4.2 ↔ Sprint F2 (div-jump PDE); V2 benefits from Sprint B tiers. None are
hard blockers — each package defines the fallback inline.

**If compressed to 4 weeks, ship V0 + V1 + V3 + V2.1–V2.3** — extended greeks, smart
delta, the Bayesian fitter, and implied earnings moves; W-smiles (V2.4), VIX (V5), 0DTE
(V6), and negative rates (V7) defer.

---

## 6. Correctness gates

**9.1-style price/fit gates** (carried from C-sprint §9 verbatim): fast/cached ≤$0.001 vs
cold ALO in-box; every fitted slice — including event-composite slices — passes butterfly +
calendar validators; de-Am SPY parity regression stays green.

**New-Greek gates:** every V0/V1 Greek validated against bump-and-revalue around the
accurate pricer — rhoBorrow/rhoDiv ≤$0.001/share per canonical shock (1bp borrow, 1% div);
theta components sum to legacy theta ≤1e-10; theta_1d vs explicit calendar-stepped reprice
≤$0.001; fugit vs PDE backward induction ≤1e-2·T; smart delta at ssr=0 bit-identical to raw.

**Event gates:** planted event-var recovery ≤2% rel; no-event bit-identity; W-slice
butterfly-clean; real pre-earnings board in-band improvement demonstrated.

**Fitting gates:** legacy weight mode bit-identical; MAP fits within error bars of unpenalized
fits on well-determined boards; graduated defense beats drop-cascade on planted-corruption
boards; reduced χ² ~O(1) in ErrorBar mode on the good corpus.

**Determinism:** all new paths deterministic across worker count; archive round-trips
(schema-hash bumped once, shared by V2.1/V3.6) preserve served values bit-for-bit.

## 7. Performance budgets (gate, not aspiration)

| Surface | Budget |
|---|---|
| `american_greeks_ext` bundle (V0) | ≤1.6× the 8-Greek analytic bundle (adds 2 re-solves: borrow, div) |
| Smart-greek adjustment (V1) | ≤5% over raw bundle (pure post-processing) |
| Event-aware surface query (V2) | ≤1.2× plain `CurveSurface` query; no-event path 1.0× (bit-identical) |
| Event var fit (V2.3) | ≤15% over the plain board fit it wraps |
| ErrorBar/MAP fit modes (V3) | ≤10% over legacy per slice; temporal-prior warm refit ≥40% fewer LM iters |
| Joint borrow+div implication (V4.1) | ≤2× the borrow-only pass |
| VIX chain-direct (V5) | ≤1 ms per underlier per horizon on the pinned host |
| 0DTE slice fit (V6) | within the standing per-slice fit budget; baseline committed |
| Steady-state allocation | zero new allocations in kernel/prepared-portfolio paths (C-sprint rule 5) |

No V-package may regress any checked-in P/C-sprint baseline beyond the 1.10 ratio gate
(`compare_baseline.py`).

## 8. Bench/test scenarios that must exist

| Scenario | Package |
|---|---|
| Extended-Greek bundle on the OPRA corner grid (div-heavy, high-borrow, short-T) | V0 |
| Smart-vs-raw delta on a skewed index board, ssr ∈ {0, 1, 1.5} | V1 |
| Synthetic planted-event board (single + multiple events, dailies) | V2 |
| Real pre-earnings single-name board (NVDA/TSLA via OPRA loader) | V2 |
| Sparse-weekly slice (3 quotes) with/without prior | V3 |
| Corrupted board (10% stale, 5% crossed planted) graduated-vs-drop | V3 |
| Synthetic known-(borrow,div) chain ladder; real XOM declared-div reconciliation | V4 |
| SPY VIX-methodology strip vs hand reference | V5 |
| SPY full daily ladder (0DTE…) fit + arb validation + bench baseline | V6 |
| Negative-rate grid vs CN PDE oracle | V7 |

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Event decomposition non-identifiable on sparse boards (v_e vs diffusive term confounded) | bracketing-pair initializer + prior on v_e (V3 machinery); refuse (clean `Err`) below a quote-count floor; planted-truth gates |
| W-mixture fit unstable / arb-violating | fit in constrained (J,p) domain; composite slice always re-validated; fall back to plain slice with event step only (no W) |
| Joint borrow+div implication ill-conditioned (two unknowns, one PCP equation per term) | cross-expiry structural regularization; wide-band names fall back to borrow-only with declared-div input; synthetic recovery gate before real use |
| Fugit via ALO quadrature subtle (first-passage on approximated boundary) | PDE backward-induction is the reference route; quadrature route ships only at ≤1e-2·T parity |
| SSR convention mismatch with C3 scenario/attribution | single `SpotVolDynamics` type + written convention doc; cross-sprint handshake test (V1.3) |
| Schema churn (two archive additions) | one coordinated schema-hash bump (V2.1 + V3.6 together); round-trip tests both |
| Graduated defense hides genuinely bad data | fatal flags (Halted, Crossed beyond threshold) still drop; error-bar inflation logged per quote; corrupted-board gate measures both directions |
| Negative-rate work starves core packages | V7 is stretch by charter; ship-or-kill with measurement |

## 10. Explicit non-goals

- Re-doing anything owned by P-sprint, C-sprint, or Sprints A–H (§1 table) — including
  AAD, scenario grids, SIMD calibrator, discrete-div PDE internals, rate bootstrap.
- Chasing Vola's refuted/unverifiable marketing numbers (universe-in-<1s) as gates.
- Replicating proprietary C-tier functional forms — we build published-math equivalents
  (S3/SSVI domain + mixture W-smiles), not reverse-engineered clones.
- FX module; SLVJ/exotics calibration surface; trading-time clock (V6 documents calendar-time only).
- NN/GP fitters; GPU.

## 11. Implementation task ledger

| ID | Deliverable | Est. | Proof |
|---|---|---:|---|
| V0.1 | rhoBorrow (analytic cached + FD cold) | 0.5 d | FD parity corner grid |
| V0.2 | rhoDiv (parallel + per-event) | 0.5 d | FD parity; zero on div-free |
| V0.3 | Dual thetas (vol/rate split) | 0.5 d | components sum ≤1e-10 |
| V0.4 | TradingCalendar + discrete 1-day theta | 1.0 d | Fri→Mon + ex-div tests |
| V0.5 | Fugit (PDE route + quadrature route) | 1.0 d | limits + cross-route parity |
| V0.6 | Portfolio plumbing behind field mask | 0.5 d | zero-alloc batch; mask honored |
| V1.1 | SSR smart delta/gamma core | 1.0 d | ssr=0 bit-identity; FD sticky-moneyness |
| V1.2 | Dynamics config + SSR estimator diagnostic | 1.0 d | planted-SSR recovery ±0.05 |
| V1.3 | C3 consistency contract + handshake test | 1.0 d | scenario-vs-smart-delta parity |
| V2.1 | EventSchedule type + archive | 1.0 d | round-trip; empty bit-identity |
| V2.2 | Event-aware term interpolation | 1.5 d | bracketing-expiry step test |
| V2.3 | Event Var Fitter | 2.5 d | planted ≤2%; real-board implied move |
| V2.4 | W-shaped pre-event slice (jump mixture) | 2.0 d | synthetic W + real in-band win |
| V2.5 | Event-aware calendar/butterfly validation | 1.0 d | 0 false pos/neg on planted |
| V2.6 | theta_1d/VIX/dispersion integration wiring | 1.0 d | consumers read schedule |
| V3.1 | Error-bar χ² weights (ErrorBar mode) | 1.0 d | reduced χ² ~O(1); legacy bit-identity |
| V3.2 | MAP soft-penalty prior in LM | 1.5 d | 3-quote slice fits; ≤0.1·bar drift |
| V3.3 | Cross-expiry posterior transfer | 1.0 d | ≥30% param-smoothness gain |
| V3.4 | Temporal prior (snapshot-to-snapshot) | 1.0 d | ≥40% LM-iter drop warm |
| V3.5 | Graduated defense (flag→bar inflation) | 1.0 d | corrupted-board win |
| V3.6 | Error bars served + archived | 0.5 d | round-trip; monotone-in-spread |
| V4.1 | Joint borrow+div implication | 2.0 d | synthetic recovery; XOM reconciliation |
| V4.2 | DividendModel menu | 1.5 d | no-div bit-identity; PDE deltas documented |
| V4.3 | Model validation report | 1.5 d | CI-lite report; default in-band |
| V5.1 | Cboe-methodology VIX calc (chain + surface) | 1.5 d | 1bp vs hand reference |
| V5.2 | Term structure + strip unification | 1.0 d | var-swap continuous limit |
| V5.3 | Event-adjusted clean vol | 0.5 d | planted recovery ≤1% |
| V6.1 | Intraday time convention audit | 1.0 d | 2-hour option end-to-end |
| V6.2 | Short-T numerics property sweep | 1.5 d | no-NaN grid; IV round-trip |
| V6.3 | Daily-ladder calendar integrity | 1.0 d | SPY dailies 0 violations |
| V6.4 | 0DTE bench + baseline | 0.5 d | committed baseline CV ≤5% |
| V7.1 | Double-boundary negative-rate solver | 4.0 d | PDE parity or documented kill |

Total: 33 d core + 4 d stretch across two lanes.

## 12. Definition of done

- [ ] `AmericanGreeksExt` (rhoBorrow, rhoDiv, theta_vol/theta_rate/theta_1d, fugit) ships
      FD-validated with a trading calendar, batch-clean, behind field masks;
- [ ] SSR smart delta/gamma ship with one documented convention consumed identically by
      greeks, scenario re-marks, and attribution;
- [ ] an event layer ships: schedules in the archive, event-step interpolation, implied
      per-event variance, W-capable pre-event slices, event-aware no-arb — planted-truth
      and real-board gates green;
- [ ] the fitter is Bayesian where it counts: error-bar weights, MAP priors (cross-expiry
      + temporal), graduated defense, error bars served — with the legacy path bit-identical;
- [ ] dividends can be implied jointly with borrow from listed prices and reconciled
      against declared dividends; a dividend-model menu with a validated default exists;
- [ ] a VIX-methodology variance module prices strips chain-direct and surface-based;
- [ ] 0DTE/daily ladders price, fit, and validate arb-free with a gated bench baseline;
- [ ] V7 ships to PDE parity or is killed with the measurement attached;
- [ ] every budget in §7 holds; no P/C-sprint baseline regresses; full gate green.

At that point the remaining deltas to VolaDynamics' *published* surface are exactly two:
their proprietary C-tier functional forms (we serve equivalent capability via C8/CStar +
the event mixture) and their unpublished internal throughput numbers (owned by P/C
sprints against named public anchors). Every marketing-page feature axis — extended
greeks, smart delta, earnings modeling, Bayesian fitting, div fitter, VIX module, 0DTE —
has a shipped, gated equivalent.

## 13. Open questions (settle during execution)

1. Real pre-earnings board sourcing: OPRA loader supports any symbol, but the checked-in
   corpus is SPY/XOM. Need one NVDA or TSLA pre-earnings day pulled and committed as a
   fixture (small parquet slice) for V2.3/V2.4 gates.
2. SSR default per profile: fixed 1.0, or profile-keyed (index vs single-name)? Ship
   fixed + estimator diagnostic first; revisit after C3 lands.
3. rhoDiv convention: per-1%-relative bump (chosen here, matches escrowed-forward algebra)
   vs per-cash-unit. Confirm against desk convention before freezing the API.
4. V0.5 fugit production route depends on Sprint F1 timing — if F1 slips past week 2, the
   quadrature route becomes primary and the PDE cross-check stays test-side.
5. Whether Vola's "two thetas" = (vol-time, rate-time) split exactly as modeled here —
   vendor docs say "with regard to rate or vol time" (homepage, verified); our decomposition
   is the standard desk interpretation and is documented as such.

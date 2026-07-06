# atx-vol — Universal American-Equity Breadth Expansion

**Date:** 2026-07-05
**Status:** Design of record (research + map) → implementation plan.
**Scope:** `atx-vol`. Research the volatility-curve / surface-parameterization
landscape, ground it against the current library, and map a phased expansion
that lets atx-vol **price and fit any American equity option** — across the whole
optionable universe (index/ETF, mega-cap event, liquid & ordinary single names,
illiquid small caps, hard-to-borrow / heavy-dividend names, vol products) and the
whole contract axis (0DTE/weeklies → LEAPs, deep wings).

This document does **not** invent a new pricing paradigm. atx-vol's frame —
*de-Americanize → fit an arbitrage-free parametric/dense curve → re-Americanize* —
is the correct one for American equity options (established: Klassen 2017;
de-Americanization literature). The expansion is (1) **activating** curve
machinery that is already built but not wired into the unified seam,
(2) **hardening** the fit path for thin / degenerate boards, and (3) **filling**
the two genuinely-new pieces of pricing math (discrete-dividend early exercise;
negative-carry) *only where a segment's acceptance gate proves the current
approximation fails*.

## Locked decisions (session `/goal`, 2026-07-05)

| Decision | Choice |
|---|---|
| Roadmap shape | **Hybrid: A-led, C-framed** — wire existing curves + selector + term structure first; use the 7 `ProfileKind`s as per-segment acceptance gates; add new pricing math only where a segment gate proves the approximation fails |
| Deliverable | **Design doc + implementation plan** — this spec, then `writing-plans` for a phased execution plan |
| Frame | Keep the de-Am → arb-free parametric fit → re-Am pipeline; extensions are additive through the `IVolCurve` / `VolCurveKind` seam, gated |

---

## Part I — Research: how vol curves parameterize surfaces

A volatility *surface* is a map `(K, T) → σ_impl`. Every production method is a
choice on two axes: **how it shapes one expiry's smile** and **how it couples
smiles across expiries** (the calendar dimension). Four family classes:

### A. Per-slice parametric (fit each expiry, interpolate the term)

The smile of one expiry is a low-dimensional closed form in log-moneyness
`k = ln(K/F)`; the surface is these slices interpolated **linear in total
variance** `w = σ²T` (the no-calendar-arb-friendly coordinate).

- **Polynomial / spline in strike or delta** ("vol-by-delta", bank standard):
  maximally flexible, *not* arbitrage-aware natively; needs a post-hoc no-arb
  projection.
- **SVI** (Gatheral 2004) — 5 params, `w(k) = a + b(ρ(k−m) + √((k−m)² + σ²))`.
  Raw / natural / **jump-wings (JW)** forms (JW exposes observable ATM
  vol/skew/wing slopes). Butterfly no-arb via the Roper density `g(k) ≥ 0`.
  **Limitation:** a *single* curvature parameter — cannot fit strongly
  asymmetric higher-order wings, and cannot produce **negative ATM curvature**.
- **gSVI / C8** — SVI-JW backbone + additive localized *bumps* (ATM curvature +
  asymmetric left/right deep-wing). Adds DoF exactly where SVI is rigid.
- **Wing model** (CBOE / Bali) — piecewise ATM + call/put wings with smoothing
  ranges; the exchange-standard "vol wing".
- **CStar / modal** (Vola Dynamics' nested C-family) — a base shape + fixed-center
  modal basis functions, tiered DoF (C5 → C8 → C12 → C16). The progressive-DoF
  ladder that closes χ² where fewer params misfit.

### B. Whole-surface parametric (couple all expiries)

One ATM total-variance term structure `θ_t` plus a shape function drives every
slice, so calendar arbitrage is controlled *by construction*.

- **SSVI** (Gatheral–Jacquier 2014) — `w(k,θ) = θ/2·(1 + ρφ(θ)k + √((φ(θ)k + ρ)² +
  (1−ρ²)))`. Calendar + butterfly arb-free under simple bounds on `φ`.
- **eSSVI** (Hendriks–Martini 2019; **Mingone 2022** global no-arb reparam) —
  SSVI with `ρ` and `φ` term structures. The current production standard for
  liquid single-name surfaces; a global no-arb domain reparametrization.
  **Same negative-ATM-curvature limitation as SVI** (the 3-param backbone).
- **SABR** (Hagan 2002) — `α, β, ρ, ν`; a stochastic-vol-implied smile, per-expiry;
  ubiquitous in rates, used for equity single-names. No-arb / arb-free-SABR
  variants exist for the low-strike breakdown.

### C. Nonparametric / dense (high DoF, arb-free by construction)

- **Fengler smoothing splines** (2009) — arb-free cubic spline on the call-price
  curve via QP.
- **Andreasen–Huge** (2011) — a single-step Dupire PDE that fits a discrete
  strike grid exactly and re-prices arbitrage-free.
- **Convex-QP dense call-price fit** — a decreasing-convex call curve is
  butterfly-arb-free by construction; high DoF; index-grade. atx-vol's
  ConvexDense is this.
- Tension splines / RBF — smoothness-vs-fit tradeoff knobs.

### D. Model-implied dynamics (out of scope for the fitter, noted for completeness)

Local vol (Dupire), Heston / Bates (+ jumps), local-stochastic vol (LSV), and
**rough vol** (rBergomi, rough Heston; Gatheral–Jaisson–Rosenbaum 2018, power-law
short-end skew) *derive* a surface from an SDE. These are the right tool for
**exotic pricing and smile dynamics**, not for fitting a listed American-equity
board to marks. They are explicitly **not** on the breadth path — the de-Am →
arb-free static fit → re-Am pipeline is.

### The calendar dimension (why it matters for breadth)

| Coupling strategy | Calendar arb handling | atx-vol status |
|---|---|---|
| Independent per-slice + linear-in-w interp | can cross in total variance; needs projection/repair | default; `MonotoneFit` clears near-money |
| Coupled term structure (SSVI/eSSVI) | arb-free by construction under `φ` bounds | eSSVI backbone live |
| Global dense + θ-floor | per-slice convex + calendar floor | ConvexDense live |

**The one breadth-critical fact:** SVI/SSVI/eSSVI's single curvature parameter
**cannot represent the negative-ATM-curvature W-shape** of a binary-event /
earnings smile, nor strongly asymmetric wings. This is *precisely* why the nested
C8 / CStar bump-families exist (Vola's documented S5 → C8 → C12 χ² ladder), and
why "any equity" needs more than the three currently-wired kinds.

---

## Part II — Current atx-vol coverage (ground truth)

### Curve families — three states

**LIVE & wired** into the unified seam (`VolCurveKind` `vol_curve.hpp:64`,
`curve_selector`, archive `kind_bits`, `VolaSession`):

| Kind | DoF | Role |
|---|---|---|
| `ConvexDense` (`vol_curve.hpp:118`) | node count, ~20–40 | index / dense boards (SPY workhorse, 99.5% in-band) |
| `Essvi` (`vol_curve.hpp:137`) | 3 (or 4 asym-ρ) | single-name backbone |
| `Svi` (`vol_curve.hpp:155`) | 5 | raw-SVI slice |

`IVolCurve` (`vol_curve.hpp:77`) exposes 5 virtuals: `w(k)`, `iv(k)`, `kind()`,
`dof()`, `clone()`. `CurveConfig` (`vol_curve.hpp:228`) + `fit_slice_curve()`
(`vol_curve.hpp:245`) are the per-slice dispatch. The default selector
(`curve_selector.cpp:21`) carries **only two candidates** — ConvexDense(40) +
eSSVI — scored by vega-weighted OOS in-band on an even/odd holdout with a
parsimony tie-break.

**BUILT but NOT wired** — full evaluator + calibrator + their own passing test
suites, but *unreachable* through the unified path (the `VolCurveKind` enum has
only three values, and `VolSurface`'s `Parametrization` routes these to `NaN`,
`vol_surface.hpp:74-75`):

| Family | DoF | Notes |
|---|---|---|
| **C8** (`c8.hpp`) | 8 (SVI-JW 5 + 3 bumps) | ATM curvature + asymmetric wings; Roper arb projection; eSSVI warm-seed |
| **CStar** (`cstar.hpp`) | 5 base + up to 11 modal (C5/C8/C12/C16 tiers) | fixed-center modal basis; block-coordinate LM; butterfly + calendar projection |
| **S3** (`s3.hpp`) | 3 (σ₀, s₂, c₂) | exact SSVI shape curve; closed-form Roper density + ATF butterfly bound; the eSSVI backbone shape |
| **Wing / SVI-MM** (`vol_surface.hpp:69`) | — | `Parametrization` tags present, evaluator not routed |

**Residual overlays** (`ResidualBasisKind`, `vol_surface.hpp:84`): HingeQuad
(live), Bspline / Cheby / WingBspline / **Fengler** (tags; Fengler nonparametric
overlay deferred per README).

### Pricing engine (`american.hpp`)

Andersen–Lake–Offengenden spectral collocation (accurate, ~10–11 sig figs) +
Barone–Adesi–Whaley (fast / boundary seed) + cached B76 + Chebyshev correction
(hot path). **Continuous-`q` only**: `andersen_lake(S,K,T,σ,r,q,side)` takes a
single continuous yield; **discrete cash dividends are folded into an escrowed
forward** (`forward_div_corrected`, Battig–Jarrow). Two documented limits:

1. **Native discrete-dividend early-exercise is deferred** (README "Deferred").
   The escrowed-forward AL is self-consistent (used for both generation and
   re-pricing) but is an *approximation* of the true discrete-dividend early
   exercise — it misses the exercise-optimal-just-before-ex-div structure that
   drives American *calls* on heavy-dividend names.
2. **Negative-carry corner returns `NotImplemented`** (`american.hpp:80-81`,
   `xmax ≤ 0`) — the asymptotic boundary collapses. Hits deep hard-to-borrow /
   negative-rate names.

Dividend/borrow (`dividend.hpp`): hybrid escrowed-cash → proportional **blend**
(single param) + per-term borrow implied via **European PCP** (`imply_borrow_
european_pcp`) *after* de-Americanization. `YieldCurve` (`curve.hpp:75`) is a
multi-pillar Fritsch–Carlson cubic-Hermite curve — but the ingest path feeds a
flat `{T=1, r}` pillar (sprint P2-3), so the *type* supports term structure the
*data path* doesn't yet supply.

### Segmentation already exists — the spec for "any equity"

`profile.hpp` ships a **7-kind `ProfileKind` taxonomy** with per-kind calib /
filter / pricing-route policy and a heuristic classifier
(`classify_profile` / `classify_underlier_with_ticker`):

`IndexEtfUltraLiquid`, `MegaCapEvent`, `LiquidSingleName`, `OrdinarySingleName`,
`IlliquidSmallCap`, `HtbDividendName`, `VolProduct`.

This taxonomy **is** the acceptance framework for breadth: "any American equity
option" = every `ProfileKind` has a green fit + price acceptance gate.

---

## Part III — The breadth gap map

### Segment axis (underlier)

| `ProfileKind` | Hard part | Covered? | What closes it |
|---|---|---|---|
| IndexEtf ultra-liquid | steep crash skew, penny spreads | ✅ ConvexDense | — |
| LiquidSingleName | moderate skew, dense | ✅ eSSVI/SVI (98.5% XOM) | — |
| OrdinarySingleName | default | ~✅ eSSVI | selector + robustness polish |
| **MegaCapEvent** | earnings **W-shape / negative ATM curvature**, term kink | ⚠️ eSSVI cannot shape it | **wire C8/CStar**; event-aware term node |
| **IlliquidSmallCap** | thin / one-sided boards, coarse strikes | ⚠️ calib robustness holes | thin-board regularization + anchoring; parsimony-aware selection |
| **HtbDividendName** | **discrete-div early exercise**, negative carry, borrow term structure | ⚠️ escrowed-fwd approx + `NotImplemented` corner | **discrete-div tree/PDE**; neg-carry AL fix; borrow curve |
| **VolProduct** (VXX/UVXY) | mean-reversion, contango/backwardation | ⚠️ routed to spy-default | dedicated dynamics — see Non-goals |

### Contract axis (any expiry, orthogonal to underlier)

- **Weeklies / 0DTE** (< 1 wk): near-expiry FD stencils probe outside `[0,T]`
  (`hT=1e-5`); strike-pinning; huge gamma. ⚠️
- **LEAPs** (long-dated): proportional-dividend regime, rate + borrow term
  structure; the flat `{T=1,r}` pillar corrupts long forwards/discounts. ⚠️
- **Deep wings / fat tails** (|k| > 1.5, ~20σ): strict wing no-arb without a
  θ-bump needs a **φ-slope term-structure constraint** (deferred); wing-residual
  over-fits sparse event wings. ⚠️

### Headline

Breadth is **~70% wiring + hardening** and **~30% new pricing math**:

- **Wiring/hardening (cheap, low-risk):** promote C8 / CStar / S3 / Wing through
  the `IVolCurve` / `VolCurveKind` seam; grow the 2-candidate selector to a full
  panel; feed the real term-structure `YieldCurve`; close the 3 calibration
  robustness holes; thin-board regularization + one-sided anchoring.
- **New pricing math (gated, only if a segment gate fails):** a
  discrete-dividend early-exercise engine (tree or PDE) and negative-carry AL
  handling — needed by `HtbDividendName`, nothing else.

The unification/dispersion sprint already reserved this exact seam (its P3-4
"Analytics-layer extensibility"); this goal is the **superset** — close every
`ProfileKind`, not just the ~10-member dispersion pilot.

---

## Part IV — The extensibility seam (how growth stays bounded)

Every addition is bounded by an existing abstraction, so breadth grows without
sprawl. A new live curve kind = **six mechanical touch-points**:

1. `VolCurveKind` enum value (`vol_curve.hpp:64`).
2. An `IVolCurve` adapter over the concrete params (thin — `w`/`iv`/`dof`/`clone`).
3. A `fit_slice_curve` dispatch arm (`vol_curve.hpp:245`) — the calibrator
   already exists for C8/CStar/S3.
4. A `SurfaceArchive` slice serializer — `ArchiveSliceHeader.kind` +
   `ArchiveDirEntry.kind_bits` (`surface_archive.hpp:150,219`) are already
   kind-tagged; adding a kind **bumps the schema hash by construction**, so old
   corpora are cleanly rejected (re-fit is the migration), never mis-read.
5. A `CurveSelector` candidate entry (`curve_selector.cpp:21`).
6. Config through `CurveConfig` / `CalibOpts` / `FitPreset` — no ad-hoc knobs.

Virtual dispatch sits **only at the per-slice query layer**, so the arithmetic
hot path (Black-76 + correction) is untouched. Every addition fits from the
shared de-Americanized European observation set (`build_observations_european`)
and must hold the standing gates (SPY 99.5% in-band; `value_chain` determinism;
`/W4 /permissive- /WX`).

---

## Part V — Phased roadmap (hybrid: A-led, C-framed)

Each phase is independently shippable and holds every cross-cutting invariant.
TDD per repo norm (write the gate test first). Phases B1–B2 are the A-led
activation; B3 stands up the C-framed per-segment acceptance boards; B4–B5 fill
new pricing math *conditionally*, driven by the B3 gate data.

### B0 — Baseline + coverage instrumentation

- Build `build-rel`; full atx-vol ctest green (re-verify, don't trust the claim).
- Record baseline metrics: SPY/XOM in-band, cold fit ms, `value_chain` inv/s.
- **Coverage harness:** a per-`ProfileKind` fixture board (synthetic known-truth
  + any cached real slice) and a `fit_quality_by_profile` report (vega-weighted
  vol-RMSE, in-band, calendar-arb-free, χ²). This is the scoreboard every later
  phase reads. *Gate:* suite green; scoreboard emits a number per profile.

### B1 — Wire the built curves into the unified seam (A)

- Promote **C8**, **CStar**, **S3** (and **Wing** if its evaluator lands cheaply)
  to live `VolCurveKind` values via the six touch-points in Part IV.
- Each new kind round-trips **bit-identically** through the archive and is
  reachable from `VolaSession` / `PricedSurface` / `PricerFitter`.
- *Gate:* a fit through the unified path using each new kind reproduces the
  standalone module's evaluator bit-for-bit; archive round-trip bit-identical;
  schema hash bumped; SPY 99.5% and determinism unregressed.

### B2 — Full candidate selector + real term structure (A)

- Grow `default_selector_candidates()` from 2 to the full panel
  (ConvexDense / eSSVI / SVI / C8 / CStar / S3), scored by vega-weighted OOS
  in-band with the parsimony tie-break already implemented. Selection stays
  data-driven — a name only pays for DoF its board's holdout justifies.
- Feed the multi-pillar `YieldCurve` from ingest (close sprint P2-3's flat
  `{T=1,r}`), correcting LEAP forwards/discounts.
- *Gate:* on the B0 scoreboard, MegaCapEvent event-smile boards select C8/CStar
  and beat the eSSVI-only baseline in χ²; LEAP forwards match a hand-checked
  term-structure case; no selection regression on SPY/XOM.

### B3 — Per-`ProfileKind` acceptance boards + fit hardening (C)

- Stand up an acceptance board + fit/price policy per `ProfileKind` (the 7
  gates). Each gate: vega-weighted vol-RMSE ≤ floor, calendar-arb-free
  near-money, in-band (tick-aware, per the SPY metric-trap lesson).
- **Fit hardening for thin/degenerate boards** (fires on IlliquidSmallCap /
  one-sided names): close the 3 known robustness holes (essvi theta-band clamp,
  svi neg-variance reject, c8 gradient-failure guard); add one-sided-board
  anchoring + Tikhonov regularization toward a profile prior; near-expiry FD
  stencil fix (clamp probes into `[0,T]`).
- *Gate:* every `ProfileKind` board is **green except** those whose failure is
  provably a pricing-math gap (HtbDividendName) — those are quarantined and hand
  the exact failing case to B4/B5. Engineered sparse/degenerate slices reproduce
  then fix each robustness hole.

### B4 — Discrete-dividend early-exercise engine (new math, conditional)

*Run only if the B3 HtbDividendName / heavy-dividend gate fails on escrowed-fwd
AL.*

- A native discrete-cash-dividend American pricer (CRR/tree or Crank–Nicolson
  PDE with jump-at-ex-div), routed via a new `AmericanMethod` value and a
  `PricingRoute` in the profile. Cross-check against the existing CN PDE oracle
  and against escrowed-fwd AL where they should agree (small/late dividends).
- *Gate:* on a known-truth heavy-dividend board (early-exercise-optimal call just
  before ex-div), the discrete-div engine prices within tolerance of the CN
  oracle and closes the HtbDividendName gate that escrowed-fwd AL misses; SPY
  path unchanged (it does not route here).

### B5 — Negative-carry / hard-to-borrow corner (new math, conditional)

*Run only if a hard-to-borrow gate hits the `NotImplemented` corner.*

- Handle the `xmax ≤ 0` negative-carry corner in Andersen–Lake (or route such
  contracts to the tree/PDE of B4, which has no asymptotic-boundary assumption).
  Extend the borrow term structure so a per-expiry borrow curve (not a single
  scalar) feeds the forward.
- *Gate:* deep-HTB known-truth board prices without `NotImplemented`; borrow term
  structure reproduces an injected per-term borrow.

---

## New modules / files (cumulative, indicative)

- B0: `include/atx/vol/coverage.hpp` + `src/coverage.cpp` (per-profile scoreboard);
  `tests/coverage_by_profile_test.cpp`.
- B1: `VolCurveKind` + adapters extended in `vol_curve.hpp` / `.cpp`; archive
  serializers in `surface_archive.cpp`; new-kind round-trip tests.
- B2: `curve_selector.cpp` candidate panel; ingest term-curve wiring in
  `opra_panel.cpp` / `data.cpp`.
- B3: `include/atx/vol/detail/thin_board.hpp` (anchoring/regularization);
  per-profile acceptance tests.
- B4 (conditional): `src/american_discrete_div.cpp` + `AmericanMethod` extension.
- B5 (conditional): negative-carry handling in `american.cpp`; borrow-curve
  extension in `dividend.hpp` / `curve.hpp`.

## Cross-cutting invariants (every phase)

- SPY 99.5% price-in-band held on the cached real board; standing quality gate
  not regressed.
- `value_chain` / portfolio output bit-identical across thread counts.
- Full atx-vol suite green; `/W4 /permissive- /WX` clean.
- Every new curve kind round-trips bit-identically through the archive and bumps
  the schema hash by construction.
- Every performance claim measured with the interleaved A/B throttle-canceling
  discipline; negative results documented as such.
- New pricing math (B4/B5) is **gated**: shipped only when a B3 acceptance gate
  proves the existing approximation fails, and it never re-routes the validated
  SPY/liquid path.

## Non-goals

- **Model-implied dynamics** (local vol, Heston/Bates, LSV, rough vol — Part I
  class D). These are exotic-pricing / smile-dynamics tools, not board-fitting
  tools; the de-Am → arb-free static fit → re-Am pipeline is the frame.
- **VolProduct (VXX/UVXY) native dynamics.** Vol-ETP smiles reflect a
  mean-reverting term structure with its own microstructure; a dedicated model is
  a separate project, not part of the equity-option breadth pass. Kept classified
  by the profile taxonomy but routed to the nearest curve until scoped.
- **SIMD vector transcendentals** (toolchain-blocked under clang-cl; measured
  negative in prior sessions).
- **A full learned / neural surface.** The parametric + dense family closes the
  listed-board fit; a neural de-Am or surface is research, not breadth.

## Open decisions (resolve before B1)

- **Wing evaluator:** promote to a live kind in B1, or leave as a `Parametrization`
  tag? Decide by whether any B0 board needs the exchange-wing shape that C8 does
  not already cover.
- **Selector cost budget:** the full 6-candidate panel is ~3× the fit cost of the
  2-candidate default. Cap the panel per `ProfileKind` (e.g. index → ConvexDense
  only; event names → eSSVI/C8/CStar) so the selector stays cheap where the shape
  is known.
- **Discrete-div engine choice (B4):** CRR tree (simple, robust, slower) vs
  Crank–Nicolson PDE (faster, matches the existing oracle) — pick when B4 is
  triggered, against the accuracy the failing gate demands.

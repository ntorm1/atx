# Surface library deep dive — month-1 production fit + SOTA roadmap

Trigger: first full production month (611 symbols × 15 sessions, 2025-08-11..29)
of the 616-name xsec universe fit into `surface-db/xsec-2025`. This doc analyzes
that month's fit performance (speed + accuracy), maps every failure family to
its mechanism in code, surveys the state of the art (vendor + academic,
2020-2026), and lays out the build-out required to reach a state-of-the-art
American equity vol surface library across ALL surfaces — not just the liquid
half.

## 1. Month-1 scorecard

| Metric | Value |
|---|---|
| Cells attempted | 9,165 (611 syms × 15 sessions) |
| Cells stored | 8,576 (**93.6%**) |
| Cells failed | 589 (6.4%) |
| Wall clock | ~18.7 min (concurrent with 6 OPRA pull workers) |
| Speed | 0.117 s/cell wall @ 16 cores ≈ **1.9 core-s/cell** |
| Full-year projection (616 names × ~250 sessions) | ≈ 5 h wall |
| Storage | 4.3 KB/cell → ~450 MB/full-year universe. Trivial. |
| Cells that can serve T=1.0 natively | **73.3%** (2,292 truncated below 1y) |

Failure split: 140 NotFound `carry_failed` on every chain (low-priced names),
51 `Unavailable` risk rejections (18 inversion-certification, rest
quality-floor/admission), remainder spread across chunk reruns. 3 symbols
zero-ok all month (AMCR, CMPS, SPCE); 5 universe names absent from the
2025-08 hive entirely (BNY, ECHO, FISV, MRSH, P — renames/new listings).

## 2. Accuracy findings

### 2.1 Tenor truncation is a DATA phenomenon, and it is big

`tenor-audit` over the month: **26.7% of stored cells have max_T < 1.0**
(16.6% < 0.6y). Attribution by symbol (exact, via ok-count block split):

- **80 names never fit a ≥1y slice all month** (median liquidity rank 459)
- **288 names flicker** (some days yes, some no; median rank 367)
- **240 names always ≥1y** (median rank 169)

Ground truth (hive quote counts at the 15:55 snapshot, 2025-08-15): GWW's
longest listed expiry is 2026-04 (T=0.67); TDG quotes **238 two-sided** on its
2026-05 board (T=0.75) and lists nothing longer; SNDK (Feb-2025 spinoff, rank
2 by liquidity) tops out at T=0.59. **The fits are honest — the 1y board does
not exist for ~40% of names in the early window.** Jan-2027 LEAPS list ~Sept
2025 on the exchange cycle; long-cycle names (top ~240) already carry 1.4y+.

### 2.2 Silent extrapolation defect in the vega panel (library bug, ours)

The served surface **flat-extrapolates beyond max_T** — the library's own
comment calls the result "finite but FABRICATED" (`analytics_aggregate.cpp:85`).
`backtest.cpp` gates marks with `PricedSurface::extrapolates_tenor`;
**`vega_panel.cpp` never calls it.** Consequence: every 1y ATMF strangle entry
or daily mark on a truncated cell prices on a fabricated flat 1y smile,
silently. This contaminates the SP100 pilot labels (unquantified there — SP100
truncation is lower but not zero) and would contaminate 26.7% of the xsec
panel. Fix is small and mandatory (see W1).

### 2.3 In-band repricing (band-audit)

`band-audit` reprices the full listed chain against the stored surface and
scores the fraction of quotes whose re-Americanized fair value lands inside
NBBO. Stratified 25-name sample, all 15 sessions, 4,047 expiry-slices /
545k quotes:

| Tier | slices | quotes | in-band (quote-wtd) | median slice in-band | slices <0.35 floor |
|---|---|---|---|---|---|
| liquid (AAPL MSFT NVDA TSLA SPY QQQ) | 2,234 | 431k | **0.891** | 0.932 | 0.3% |
| mid (GWW TDG GEHC EQT … ZBH) | 1,334 | 74k | **0.924** | 0.975 | 1.1% |
| cheap/hard (BYND PLUG CLOV TLRY …) | 479 | 9.4k | **0.819** | 0.885 | 10.4% |

By tenor: flat 0.876→0.913 short→long — no tenor cliff; the long end is
actually the best-repriced region within its fitted domain. Notables: single-name
megacaps 0.92-0.94; SPY/QQQ lower (0.845/0.873 — ConvexDense index seed,
huge weekly chains with deep-ITM/far-wing tails); TLRY is the failure poster
(0.524, 42% of slices below the floor); mids are the sweet spot. Versus the
only published industry bar (SpiderRock: worst-violation zero on ~90% of
fits): only 10-40% of our slices have every quote within one half-spread of
the band — but our audit scores the FULL chain including intrinsic-dominated
deep ITM, so the comparison is directional, not exact. The gap is
concentrated exactly where W2/W5/W6 aim.

## 3. Failure anatomy (mechanism, from code)

Full mechanism map with file:line anchors from the code deep-dive; the three
production failure strings:

**FS-1 `carry_failed` on every chain (dominant, low-priced names).**
`run_surface_parity` hard-drops any expiry whose robust carry solve fails the
confidence gate (`deamer.cpp:665-667`: n_retained ≥ 3, dispersion ≤ 0.02,
leave-one-out ≤ 0.005 — all in absolute annualized-rate units). One tick of
C−P quantization on a $2 stock at T=30d ≈ **6 rate points** of borrow error
(tick/(S·T)) — 3× the dispersion gate; the same tick on a $200 stock is 6e-4.
The gate's units make it price-level- and tenor-dependent by construction.
Compounding: `penny_floor=0.05` marks most near-ATM bids on $1-3 names as
`Penny` → no valid co-terminal pair at all. Critically, the eSSVI lane has
**no board-level carry fallback** — `fit_curve_surface` (the non-eSSVI lane)
defers to term-structure repair (Decision-B), `run_surface_parity` does not.
Same policy, same board, opposite treatment.

**Standing falsification (respect this):** commit `547a466` reverted a
slice-width-invariant re-basing of the gate after a pre-registered test showed
the newly admitted slices were statistically indistinguishable from the ones
still refused (median de-Am round trip 0.02619 vs 0.02311), and the
term-structure fallback borrow "is doing harm on thin boards." **Loosening the
gate thresholds buys coverage with no quality discrimination. The fix must
change the estimator's information set (pooling), not the thresholds.**

Also measured (lane C, `01040c0`): on boards that *serve*, slice loss is
**30.2% prep starvation vs 4.7% carry failure** — one-sided-quote discard
(`opra_panel.cpp:867` drops bid-missing rows; 9.5% of lqbench rows, all
ask-only) is the bigger slice killer on surviving boards.

**FS-2 `inversion=failed`.** Certification requires every accepted de-Am
proposal audited against the cold Andersen-Lake reference and drop fraction
≤ 0.10. Latent pathology: deep-ITM inversion is ill-posed (two roots;
Burkovska et al.), and `american_implied_vol` clamps at-intrinsic prices to a
0.5%-vol floor that then **passes every downstream audit**. Fixed on unmerged
`364a8b8` (well-posedness refusal before the ledger).

**FS-3 `QualityBelowFloor`.** Admission floor `worst_frac_within_bidask ≥
0.35` — the *worst single expiry* on the board must reprice ≥35% of its quotes
in-band. `carry=failed` co-occurs because the non-eSSVI lane admitted a
non-confident slice on fabricated fallback borrow, which shifts F, shifts every
k=ln(K/F), and drives fair values out of band — the floor then correctly
refuses. The floor is also an *absolute* yardstick: on $2-10 names NBBO width
in vol terms is enormous, and a spread-normalized χ² would admit fits that are
as good as the data allows (see W5).

**Latent board-killer:** `surface_parity.cpp:585` hardcodes
`n_curve_params=3` into chi² scoring; a fitted slice with ≤3 scorable quotes
fails the whole board. Fixed on unmerged lane-B `124d537`/`ead7079`.

## 4. Speed anatomy

`timing.populate_s` dominates (428.8s vs 2.6s load per 6-session chunk).
Hot loops, ranked: (1) per-strike de-Am IV inversion (one AL solve per OTM
strike; shared-boundary batch + adjacent warm start already on); (2) carry
fixed-point (≤5 pairs × nested AL solves × 2 legs); (3) fit-inversion audit
(risk builds reprice **every** row against the cold reference — ≥2× multiplier);
(4) full-chain re-Am parity scoring; (5) failing cells cost 4-5× a clean one
(fallback ladder + strict-recovery refit). Ledger exists
(`ATX_VOL_SOLVE_LEDGER=1`, `ATX_VOL_PROFILE=1` per-stage ms) — use before
optimizing. Published comparisons: direct SVI slice fits run ~0.1 ms/slice;
SpiderRock full-chain live calibration ~45 s/underlier; our 1.9 core-s/cell is
comfortably mid-pack and **speed is not the binding constraint — robustness
ladders are affordable everywhere** (a Mingone global refit pass is noise at
this budget).

## 5. SOTA survey (condensed; links in the research notes)

- **De-Americanization / carry (Vola Dynamics, SpiderRock, Cboe, OptionMetrics):**
  production practice is *American-consistent* forward extraction — imply
  borrow per term inside the American pricer (Vola "American PCP"; SpiderRock
  solves `sdiv` to align call/put American-model IVs near ATM, keeps an EMA of
  it). Cboe uses CRR + forecast discrete dividends and does NOT imply forwards
  for American names. De-Am error grows with rates, maturity, ITM-ness
  (Burkovska et al.) — at 4-5% rates the "EEP negligible" shortcut is unsafe
  ITM/long-dated. OptionMetrics' new implied-borrow methodology has a
  published defect (Wallmeier 2024/2025): single-snapshot call-put
  misalignment read as borrow inherits all timestamp noise — exactly our FS-1.
- **Robust forward extraction:** per-expiry OLS on P−C+S vs K is the template
  (Cboe), but the robust line (Söderbäck 2022 WLS; median-based 2023) and
  pooling across expiries is where the field went. Box spreads give the rate
  input (BDG *JFE* 2022); per-name only borrow remains to estimate.
- **Parametric surfaces:** Mingone (*QF* 2022) maps the entire eSSVI
  no-arbitrage domain to a box → global calibration where every optimizer
  iterate is arb-free by construction; in production at TASE via Zeliade;
  <500 objective evals per surface warm-started. Pasquazzi 2023: warm start
  matters (naive inits hit bad local minima); metrics F1 (in-band fraction) /
  vega-weighted MSE. Vola's curve family: 3-param S3/SSVI "fits the vast
  majority of US equity names"; liquid names need richer C5-C12 (W-shapes,
  negative ATM curvature near events); exact S3 no-arb region known (Klassen).
  ML surface fitting (deep smoothing, neural SDE, VolGAN): not documented in
  production for daily single-name construction anywhere we found.
- **Quality metrics:** SpiderRock publishes the only concrete bar: fits inside
  the bid-ask channel, **worst-violation zero on ~90% of fits**. Klassen: χ²/dof
  ≲ 1 with per-quote error bars from spreads; absolute vol-RMSE floors are the
  wrong yardstick for cheap names. Ulrich et al. 2023: per-expiry smoothers
  dominate 3-D smoothing (supports our slice architecture).
- **Thin chains (Klassen deck pp.54-61 = the production blueprint):** 95%+ of
  US underliers are illiquid; fit 3-param curves with soft penalties so terms
  with less data than parameters still fit; spread information across strikes,
  expiries, and time (Bayesian); temporal filtering with prior fits as prior.
  Cross-NAME hierarchical shrinkage on a 600-name panel is ahead of the public
  literature — nearest analogs are multitask-GP priors (2026) and Klassen's
  observation that (skew, curvature) are comparable across names for decades.

## 6. Build-out roadmap

Ordered by (label-integrity first, then coverage per unit risk):

**W1 — Panel/label integrity gate (immediate, small).**
`vega_panel`: refuse entry resolution and daily marks when
`extrapolates_tenor(T)` for the leg tenor; emit `tenor_domain.max_T` and an
`extrapolated` flag per row. Re-run SP100 pilot to quantify contamination.
Decide label policy for tenor-limited names (see W7). *Everything downstream
of the panel is untrustworthy until this lands.*

**W2 — Pooled American-consistent carry (the FS-1 fix).**
Replace per-expiry independent borrow solves with ONE borrow curve per
(symbol, date) estimated jointly across all expiries: robust weighted
regression (weights ∝ 1/spread², Huber/median), hard model-free American PCP
band clip before entry, external rate curve input (CMT spline or index box
rate), temporal EMA shrinkage toward yesterday's borrow (SpiderRock pattern),
zero-borrow prior beyond. Per-expiry regression on 1-3 wide pairs is
ill-posed by construction — pooling restores identifiability *without touching
the confidence thresholds* (respects the 547a466 falsification: change the
information set, not the gate). Sequencing: supersedes lane-A `6dd888c`
(Decision-B port), whose fabricated-borrow harm was measured.

**W3 — Merge the lane program (mostly-written coverage/correctness).**
Priority inside the roll-up (`feat/vol-sbd-integration`):
1. `e8f09c5` one-sided quotes as bounds (30.2% starvation >> 4.7% carry on
   serving boards; 9.5% of rows currently discarded);
2. `124d537`/`ead7079` dynamic dof (kills a latent whole-board failure);
3. `364a8b8` deep-ITM well-posedness refusal (closes the 0.5%-vol-floor
   false-pass);
4. lane-B arb exactness (exact quartic calendar detection, Lee wing bound 2,
   Davis-Hobson feasibility) — correctness of the checks the admission gate
   relies on.

**W4 — Global arb-free eSSVI refit pass (Mingone box parametrization).**
When slice-sequential fit + projection fails (FS-2/FS-3 families), refit the
whole board globally in the transformed (ρᵢ, θ₁, aᵢ, cᵢ) box domain,
warm-started from the surviving slices — output arb-free by construction, so
projection/repair and its rejections vanish for that cell. Bounded
trust-region LSQ, <500 evals, compute-noise at our budget. Production
precedent: TASE.

**W5 — Admission metric reform (spread-normalized).**
Add per-quote error bars derived from spreads; admission on χ²/dof ≲ 1
(Klassen) alongside — eventually instead of — the absolute
`worst_frac_within_bidask ≥ 0.35`. On $2-10 names the NBBO in vol terms is
huge; the current floor refuses fits that are as good as the data allows,
and (per FS-3) punishes carry-fallback distortions with the wrong signal.
Store per-cell quality (in-band frac, worst violation in half-spreads, χ²/dof)
IN the DB so band-audit-grade accuracy is queryable without a repricing pass.
Targets: liquid names ≥90% in-band / worst-violation ≈0 on ~90% of cells
(SpiderRock bar); cheap names χ²/dof ≲ 1.

**W6 — Shrinkage ladder for thin chains (Klassen blueprint).**
Per-expiry model ladder: rich curve → S3(3-param) → {ATM level only, shape
from prior} → prior surface. Ridge penalties toward (i) neighboring expiries,
(ii) yesterday's params, (iii) cross-sectional name-bucket prior (sector ×
vol-level), penalty weight ∝ 1/(effective quote count). Converts "fail" into
"shrunk fit with honest error bars." The cross-name prior is beyond published
practice — differentiator territory.

**W7 — Tenor strategy for the 1y vega program (design decision).**
The 1y ATMF label is unserveable natively for ~40% of names in the early
window (LEAPS listing cycle — data, not fittable). Options: (a) gate rows on
native tenor domain (shrinks early-window panel breadth to ~360-450 names);
(b) re-target label to min(1y, longest fitted tenor) with tenor as a feature;
(c) keep 1y with the `extrapolated` flag as an explicit feature and let the
ranker learn the discount. Recommend (a) for label purity in the paper run +
(b) as the production book definition — a monthly-rebalanced vega book holds
what is listed.

**W8 — Speed (non-binding; do only what falls out).**
Run `ATX_VOL_SOLVE_LEDGER=1` + `ATX_VOL_PROFILE=1` once over a failing-heavy
chunk for attribution. Cheap wins if wanted: cap `max_deam_strikes_per_expiry`
for risk builds, dedupe the 4-5× failing-cell ladder cost (fail-fast after
carry refusal instead of full fallback ladder), skip resume-time serial config
regen. Do not spend on GPU/SIMD — 100× headroom vs published budgets already.

## 7. Verification plan

- Re-run month-1 after W2+W3: target failed cells 6.4% → ≤2%, with per-family
  attribution (carry vs inversion vs floor).
- Band-audit before/after: in-band fraction distribution per liquidity tercile;
  target liquid ≥90%, universe median ≥70% within-domain.
- W1 regression: panel row count on truncated cells must drop to zero
  extrapolated entries; pilot IC re-measured on clean labels.
- Keep `opra_parity_bench CARRY_MODE=default|risk` (kept by `547a466`) as the
  pre-registered harness for any carry change: admitted-slice round-trip
  distribution must separate from refused-slice distribution — that test
  failing is what killed the last gate change.

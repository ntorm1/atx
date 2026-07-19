# SOTA: Earnings Implied-Move & Event-Variance Modeling (American equity options)

**Date:** 2026-07-18. Organizing identity (SpiderRock/Dubinsky-Johannes, verbatim):
**σ²·T = σ_E²·n + σ_C²·T** = total ATM variance = (per-event variance)×(count) +
(censored diffusive variance)×time. i.e. `w_total(T) = n(T)·eMove² + σ_C²·T`.

## 1. Foundational academic models
- **Patell & Wolfson (1979,1981)** — empirical IV-crush: IV rises into announcement,
  collapses after; strongest at short maturities → signature of a fixed variance lump.
- **Dubinsky & Johannes (2006)** "Fundamental Uncertainty, Earnings Announcements and
  Option Prices" — direct parent: diffusion + deterministic-time jump on each scheduled
  date; risk-neutral total variance additive `σ_BS²(T)·T = σ_diff²·T + n(T)·σ_E²`;
  estimator differences two maturities straddling a print. σ_diff≡σ_C, σ_E²≡eMove².
  https://business.columbia.edu/sites/default/files-efs/pubfiles/6051/DJ_2006.pdf ,
  https://papers.ssrn.com/sol3/papers.cfm?abstract_id=600593
- **Scheduled-jump pricing** (generalizes Merton 1976 to a KNOWN jump date; Kou 2002
  double-exponential): https://arxiv.org/pdf/1412.8414 , https://www.columbia.edu/~sk75/MagSci02.pdf
- **Zhong (2026)** "Non-Spanning Identification of Scheduled Event Risk" — SOTA on
  identification: calibrate the diffusive surface only from NON-spanning expiries, fit the
  jump on spanning quotes, validate on held-out spanning quotes; two-component Gaussian
  mixture jump beats single Gaussian. https://arxiv.org/abs/2606.12872
- **Risk premium** (implied>realized): Barth & So (2014) TAR 89(5) non-diversifiable
  earnings variance risk; Kelly-Pástor-Veronesi (2016) JF political-uncertainty pricing;
  AGKR (2025) Review of Finance — scheduled jump ⇒ **concave/W-shaped IV SMILE** near event
  (strike-dim identification). https://academic.oup.com/rof/article/29/4/963/8079062
- Expected absolute move = √(2/π)·σ_E ≈ 0.7979·σ_E. Pitfalls: implied≠realized (premium);
  equal-event assumption (next print richer); mean-zero jump understates directional moves;
  BMO/AMC timing shifts n(T) by one.

## 2. Straddle-implied move vs term-structure decomposition
- **Straddle (single-expiry)**: Straddle_ATM ≈ √(2/π)·S·σ√T = MAD move; EM ≈ Straddle/S;
  "0.85 rule" shrink for ~68% coverage. Biased up by diffusive time value + vol premium.
- **Two-expiry differencing**: `eMove² = σ_after²·T_after − σ_C²·T_after` (σ_C from a
  non-spanning expiry / smooth fit); forward-variance form `eMove²=(σ₂²T₂−σ₁²T₁)−σ_C²(T₂−T₁)`.
  Moontower worked example (business-day clock). Correct basis for `n·eMove²+σ_C²T`.
  https://moontowermeta.com/how-an-option-trader-extracts-earnings-from-a-vol-term-structure/
- Pitfalls: differencing amplifies quote noise (fit σ_C from MANY expiries, not one pair);
  MAD(0.7979σ) vs 1-SD vs 0.85 are different numbers — iEMove is a per-event VOL (σ_E),
  don't mix; straddle ≈ MAD only at ATM & zero-rate → use true forward-ATM.

## 3. Event-variance term-structure interpolation (censor → interp smooth → re-add)
Recipe: (1) strip n(T)·eMove² from each spanning expiry → censored ATM point; (2)
interpolate censored TOTAL VARIANCE, monotone/arb-free; (3) re-add n(T*)·eMove² at target.
**Never interpolate dirty IV — it smears the bump.**
- **SpiderRock**: iEMove; grid 5,10,21,42,63,84,126,252 (+); ATM strike where C≈P; fit
  eMove by deviation from smooth term curve; "similar to forward-vol in total variance."
- **Vola Dynamics**: Event Var Fitter + Event Modeling (clean surface + discrete jumps);
  C* curves fit W-shaped smiles SVI can't. https://voladynamics.com/examples/amzn-around-earnings
- **ORATS**: solves implied earnings effect for "rational" ex-earnings term structure, then
  constant-maturity interp with earnings removed. https://orats.com/university/volatility-around-earnings
- Operator: `σ_C(T)²=[σ_dirty²T−n·eMove²]/T`; fit monotone w_C(T); re-add. Pitfalls:
  interpolating dirty IV; miscounting n near BMO/AMC/expiry-day; NEGATIVE censored forward
  variance (calendar arb — clamp/penalize in joint solve); non-earnings date-certain events
  (FDA/PDUFA/investor-day/macro) misattributed to eMove.

## 4. Vol-time / business-time clocks + event-day add-on
Variance accrues on a non-calendar clock: calendar (365, oscillates), business/trading
(252, √252≈16 "divide by 16"), stochastic (per-session/bucket/overnight/weekend/event
weights). Event add-on eMove² is ON TOP of the print day's normal session weight.
- **Bondarenko (2026)** "The Clock Matters" — ultra-short IV term-structure irregularity is
  largely a time-measurement artifact; smoothness needs the right clock.
  https://papers.ssrn.com/sol3/papers.cfm?abstract_id=6705119
- **Wright** "Event-Day Options" NBER w28306 (macro analog). Ultra-short surfaces arXiv 2603.29430.
- Pitfalls: mixing 365/252/256; √252 annualize but 365 for T (reproducibility bug);
  overnight/weekend gap weighting (AMC print realizes in next open); don't double-count the
  event day (SpiderRock eMove is instantaneous → additive; match exactly).

## 5. Smooth-surface models for the censored component
- **SSVI** (Gatheral-Jacquier 2014): `w(k,T)=(θ_T/2){1+ρφk+√[(φk+ρ)²+(1−ρ²)]}`; no
  calendar arb ⇔ θ_T non-decreasing; **linear in ATM total variance θ_T ⇒ censor/re-add =
  clean θ-shift** (major structural advantage). https://arxiv.org/pdf/1204.0646
- **eSSVI** (Hendriks-Martini 2019; global no-arb Corbetta/Mingone arXiv 2204.00312,
  arXiv 2304.02106) — maturity-dependent ρ/φ, better short-maturity fit (earnings regime).
- Alternatives: arb-free SABR (Hagan 2014 PDE); stochastic collocation repair; Vola C* for
  the W-shaped event SMILE that SVI/eSSVI CANNOT represent.
- Censor at ATM: θ_T^cen=θ_T^dirty−n·eMove²; fit eSSVI to θ^cen (monotone); re-add for
  pricing. Butterfly checked on dirty θ, calendar on censored monotone θ. Pitfall: eSSVI
  smooths away the event smile concavity — fine for ATM term structure (our goal), not for
  full off-ATM pricing.

## 6. American IV inversion at scale (front-end before censoring)
- **Bjerksund-Stensland 2002**: closed-form, max err ≈0.03, fast — bulk default.
  https://derivativesacademy.com/.../bjerksund_stensland_2002...pdf
- **Andersen-Lake-Offengenden (2016)**: spectral collocation, 10-11 digits, ~100k
  prices/s/CPU — accuracy benchmark. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027
- **CRR binomial**: the VENDOR-convention pricer (OptionMetrics IvyDB, **SpiderRock**) with
  discrete-dividend projections — needed for bit-for-bit vendor agreement.
- Root-finder: **Jäckel "Let's Be Rational"** (machine-precision European, ≤2 Householder-4);
  de-Americanize then apply. FlashIV branch-light fixed-iteration for SIMD (arXiv 2605.29102).
- **Dividends dominate ATM accuracy**: discrete (tree, adjust S at ex-date) vs escrowed;
  Vellekoop-Nieuwenhuis splicing. Wrong divs → wrong forward → wrong ATM strike → aliases
  into eMove differencing. Match the VENDOR's pricer/day-count/ATM/div model for reproduction.

## 7. Performance / SOTA pipeline
- **fastvol** (SIMD AVX + OpenMP + CUDA, batched American IV): https://github.com/vgalanti/fastvol
- **FlashIV** (arXiv 2605.29102): cheap seed + branch-light fixed Householder, SIMD-friendly.
- eSSVI global calibration = thousands of names/day. QuantLib (QdFpAmericanEngine ALO,
  BS2002, trees), FinancePy (BS2002/CRR/BAW).
- Convention table: SpiderRock (proprietary American CRR-family, discrete div, trading-time,
  fwd-ATM C≈P, σ²T=σ_E²n+σ_C²T iEMove); ORATS (SMV tree, business days, ex-earnings rational);
  OptionMetrics IvyDB (CRR, numerical discrete div, calendar, delta-CM); Vola (event-aware).
- Pitfalls: per-lane branch divergence kills SIMD; American pricer (not root-finder) is the
  bottleneck (batch by moneyness/T buckets); reproducibility ≠ accuracy — keep vendor-compat
  mode distinct from best-accuracy mode.

## (A) Best-practice pipeline for earnings-censored ATM term structure
1. Fix conventions up front (trading-time clock, instantaneous eMove² add-on, day-weight
   vector, forward-ATM C≈P). 2. American→IV front-end: discrete-div forward, BS2002 bulk +
   ALO/CRR short-DTE event expiries, fixed-iteration Householder invert (or de-Am+Jäckel).
3. Extract dirty ATM total variance θ_dirty(T_i) on the fixed grid. 4. Joint event
   calibration (non-spanning identification): build n(T) w/ BMO/AMC + print-on-expiry; fit
   {eMove, σ_C term curve} minimizing deviation of censored θ_C=θ_dirty−n·eMove² from a
   smooth MONOTONE curve, σ_C from non-spanning tenors, eMove pinned by spanning; hard
   constraint θ_C monotone (no negative censored fwd variance). 5. eSSVI carrier (θ-linear).
   6. Re-add & publish censored curve + iEMove; validate on held-out spanning quotes.
   7. QA: censored fwd variance ≥0; re-added surface reprices input ATM straddles; eMove
   stable day-over-day except across print.

## (B) Ranked highest-leverage improvements (for a n·eMove²+σ_C²T + eSSVI system)
1. Correct dividend/forward in the American front-end (aliases into eMove). HIGHEST.
2. Non-spanning identification of σ_C + monotone-θ_C hard constraint.
3. Lock/match time clock & day-count end-to-end (252 vs 365, √252, overnight/weekend/event).
4. Right-size pricer per tenor (BS2002 bulk, ALO/CRR short-DTE) + branch-light vectorized IV.
5. Relax equal-event (per-event eMove_i or decay; next print richer).
6. Model the event STRIKE structure (W-smile) if pricing off-ATM (Gaussian-mixture jump).
7. Add non-earnings date-certain events to n(T).
8. Premium-aware caveats (implied vs realized).
9. Vendor-compat reproducibility harness (exact grid/ATM/eMove-instantaneous) w/ round-trip.

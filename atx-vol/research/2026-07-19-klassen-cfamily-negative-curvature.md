# Klassen "C-curves", negative ATF curvature, and the earnings W-shape

Research brief grounding the AMZN-around-earnings sprint. Primary sources cited inline.

## Headline: does the 3-param base admit negative c2? **No.**
Klassen's base **S3 ≡ SSVI** (Gatheral–Jacquier "Simple SVI") shape is
```
σ²(z) = σ0²·[ ½(1+s2·z) + sqrt( ¼(1+s2·z)² + ½·c2·z² ) ],   valid only c2 ≥ 0
```
For c2<0 the radicand `~ z²(¼s2² + ½c2)` goes negative in the wings → curve undefined.
Klassen deck states verbatim: S3 is *"Simplest sensible curve with 3 parameters (c2 ≥ 0)"*
and 5-param SVI *"Certainly can not fit W-shaped curves around events (still c2 ≥ 0)."*
**Negative ATF curvature is representable only by the richer nested C-curves
(C5/C6/C7/C8/C10/C12).** The AMZN example is fit with **C8**, hitting **c2 ≈ −1.1** on
the 1.06-day expiry.
Source: Klassen, *Arbitrage-Free Parametric Volatility Surfaces and Real-Time Fitting*,
Global Derivatives Chicago 2017 — https://voladynamics.com/pdf/Klassen_GD_Chicago_2017.pdf
(slides 9,17,23–24); https://voladynamics.com/examples/amzn-around-earnings/

## Curve hierarchy
| Curve | Params | Adds | neg c2? |
|---|---|---|---|
| S3/SSVI | 3 | σ0 + skew s2 + curvature c2; tied wings `C±=¼s2²+½c2±½s2` | **No (c2≥0)** |
| SVI/L5 | 5 | independent wings C−,C+ | **No (still c2≥0)**; curvature max forced at ATF |
| C5..C12 | 5–12 | shoulder/wing/ATM modes decoupled from ATF curvature | **Yes** |
Klassen's motivation (slide 24): *"Curvature has unique maximum around ATF, but that's
not what the market wants!"* — events want convexity pushed into the shoulders, leaving a
**frown** (neg curvature) at ATF. His χ² on one ES slice: S5 χ²=6.458 (avE5 23.9bp) →
**C8 χ²=0.599 (3.2bp)** → C12m χ²=0.021 (1.4bp).

## Normalization + shape (implementer sheet)
```
σ0 = ATF vol = σ(T,K=F);  y = log(K/F);  z = y/(σ0√T);  σ̂0 = σ0√T
σ(z)² = σ0²·f(z),  f(0)=1;   w(y) = T·σ² = σ̂0²·f(z)
f(z) = 1 + s2·z + ½·c2·z² + …      (s2 skew, c2 curvature)
```
atx CStar base is exactly `atm = 1 + 2·s2·z + c2·z²` (polynomial → neg c2 native),
C8 tier = base{theta,s2,c2,C_left,C_right} + modes{2,5,8}.

## Butterfly no-arb (density ≥ 0) — Roper/Gatheral–Jacquier
```
g(y) = (1 − y·w'/(2w))² − (w'²/4)(1/w + ¼) + w''/2 ≥ 0  ∀y
shape form: g(z) = (1 − z f'/(2f))² − ¼ f'²/f − (σ̂0²/16) f'² + ½ f''
ATF:  g(0) = 1 + ½·c2 − ¼·s2²(1 + ¼σ̂0²)
⇒ LOCAL feasibility:  c2 ≥ −2 + ½·s2²(1 + ¼σ̂0²)      (c2=−2 max frown at s2=0)
Lee wing bound (necessary): σ̂0·C± ≤ 2
```
A W-shape stays arb-free because g(z)≥0 is **pointwise**: the negative `+½f''` at ATF is
offset by positive shoulder curvature. The jump-driven RND is **bimodal** — thin (but
positive) under the forward, two side lobes keep g≥0. `g(0)≥0` is necessary NOT
sufficient; must check `min_z g(z) ≥ 0` (atx `cstar_min_roper_g`, 240 knots on z∈[−5.5,5.5]).
Sources: Klassen SSRN 2725700 (S3/SSVI necessary+sufficient no-arb);
Gatheral–Jacquier https://arxiv.org/pdf/1204.0646 ; Roper 2010; Lee 2004.

## Calendar no-arb
Total variance `w(y,T)` non-decreasing in T at every fixed y (`∂w/∂T ≥ 0`). VD visual
test on the AMZN page: total-variance-vs-log-moneyness lines don't cross, ordered by T.
Enforce by fitting one term at a time then an **error-bar-aware cross-term no-arb pass**
(spread shape info across expiries; don't hard-clamp). The earnings expiry carries a steep
but monotone `w` jump (the n·eMove² lift) — that is fine; prevent the fitter from
*overshooting* a near term so its line crosses a later one. atx: seed CStar from the
calendar-projected eSSVI surface (`arb_project_calendar_essvi`), verify, θ-floor if needed.

## Earnings decomposition (SpiderRock, confirmed verbatim)
```
σ²·T = σE²·n + σC²·T   ≡   w_total(T) = n·eMove² + σ_continuous²·T
```
n = #earnings before expiry; eMove = per-event instantaneous jump vol (Dirac variance);
σC = censored/continuous baseline (smooth term structure). Discrete `n·eMove²` is a
strike-independent parallel lift of front total variance; in price space a discrete jump
makes the terminal density bimodal → forces c2<0. A strict W (local min at ATM + 2 humps)
needs ≥3 mixture components (2 jump outcomes + diffusion). = atx `event_vol.hpp` model.
Sources: SpiderRock LiveVolSurfaces docs (`σ²T=σE²n+σC²T`); Alexiou et al.,
*Pricing Event Risk: Evidence from Concave Implied Volatility Curves*, Review of Finance
2025; *W-shaped IV curves and the Gaussian mixture model*, QF 23(4) 2023.

## Fitting practicalities (Klassen slide 25)
Reduced-χ² + soft (Tikhonov) shape/term-structure priors; weight quotes by 1/vega and
bid-ask half-width; IRLS/Huber down-weighting of outliers; fit one term at a time then
transfer info across terms for smoothness + calendar no-arb. Priors let a thin front
expiry carry more params than effective quotes. = atx CStar calib (IRLS-Huber k=1.345,
per-mode ridge) already implements this shape.

## Build implications (locked)
1. Fit CStar **C8** per near-term slice; extract BOTH the 3-param view {σ0,s2,c2} and the
   8-param view from that single fit. No standalone S3 fit for negative c2.
2. Validate every slice with `min_z g(z) ≥ 0` (pointwise), not just g(0).
3. Calendar: seed from calendar-projected eSSVI; verify non-crossing near-money.
4. Earnings term structure via `run_earnings_repro` (iEMove, censored ATM curve).

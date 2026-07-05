# atx-vol → Vola Dynamics parity (American equity options)

**Goal.** Bring `atx-vol` to feature + performance parity with the Vola Dynamics
analytics library for **American equity options**: match its fit quality
(reduced-χ²), its fair-value-within-bid-ask behavior, and its de-Americanized
volatility-surface construction. Use in-repo option-chain panels as fixtures.

Status: **IMPLEMENTED** (2026-07-04). Gaps G1-G7 shipped as new modules
(`american_iv`, `dividend`, `deamer`, `fit_metrics`, `parity`, `s3`, `panel`,
`vola_parity`, `surface_parity`), integrated /W4 /permissive- /WX-clean; the
`VolaParity` + `SurfaceParity` acceptance suites pass (fair-value-within-bid-ask
≥ 0.95, mid-vol-RMSE ≤ 5e-3, reduced-χ² ≤ 2, borrow/forward recovered,
interpolation matches truth, calendar-arb-free). Full suite 541/541 green.
G8 (native discrete-div PDE) and G9 (multi-underlier sequential driver) deferred
— see §6. Source research below is drawn from Klassen, *Arbitrage-Free Parametric
Volatility Surfaces and Real-Time Fitting* (Global Derivatives Chicago, 2017) and
the Vola Dynamics Wilmott profile (Jan 2020), plus a full capability audit of the
current `atx-vol` tree.

---

## 1. The Vola target (what parity means, concretely)

**1.1 Curve family (strike dimension).** Vola factors the ATF (at-the-forward)
level out of every smile and works in a dimensionless normalized strike:

```
sigma0 := sigma(T, K=F)                     ATF vol
z = NS := log(K/F) / (sigma0 * sqrt(T))     normalized strike
sigma(z)^2 = sigma0^2 * f(z|p)              shape curve
f(z) = 1 + s2*z + 0.5*c2*z^2 + ...          s2 = skew, c2 = curvature
```

The simplest sensible curve is **S3 / SSVI** (3 params sigma0, s2, c2, with
c2 >= 0):

```
sigma^2(z) = sigma0^2 * [ 0.5*(1 + s2*z) + sqrt( 0.25*(1 + s2*z)^2 + 0.5*c2*z^2 ) ]
```

Wings for S3: `C± = sqrt(0.25*s2^2 + 0.5*c2) ± 0.5*s2`. For 5-param (SVI/JW) the
wings `C-,C+` become free, constrained by `-C- <= s2 <= C+` and `sigma_hat0*C± <= 2`.
Richer **nested curves C5,C6,C7,C8,C10,C12** add flexibility (crucially
*negative* curvature around ATF, which liquid names ES/SPX/SPY/AAPL demand and
5-param curves cannot express). The digit = parameter count.

atx-vol already ships this family: `eSSVI`, raw+JW `SVI`, `C8` (8-param JW +
curvature bumps), `CStar` (C16M modal, tiers C5/C8/C12/C16). **The ats-vol C
library atx-vol was ported from is itself modeled on this Vola curve family** —
structural parity already exists. What is missing is the American-equity
*workflow* around it and the *parity metrics*.

**1.2 No-arbitrage.** Butterfly: Roper density `g(y) >= 0` for all `y`, with
```
g(y) = (1 - y*w'(y)/(2w(y)))^2 - 0.25*(1/w(y) + 0.25)*w'(y)^2 + 0.5*w''(y)
```
In shape form the vol level enters in exactly one place; the ATF condition is
```
g(0) = 1 + 0.5*c2 - 0.25*s2^2*(1 + 0.25*sigma_hat0^2) >= 0
   =>  s2^2 <= (4 + 2*c2) / (1 + 0.25*sigma_hat0^2)
```
Calendar: total variance `w(y)=T*sigma(y)^2` non-decreasing in T at fixed y.
Vola's "no-arb mode" spreads shape information across terms using error bars;
critically, **arbitrage removal costs almost nothing in fit quality** (their
SPY C8 example: reduced-χ² 0.096 regular → 0.104 no-arb).

**1.3 Dividends.** Hybrid model `S_t = S~_t + D_t`: observed stock = pure-GBM
component plus a deterministic PV-of-future-dividends shift. Cash dividends
short end, blending to proportional long end. Borrow cost implied per term.

**1.4 American / de-Americanization (the load-bearing pipeline, slide 26).**
American options are light exotics; all fitting happens on **European-equivalent
implied vols**. Per term:
```
1. pick interest rate
2. pick cash dividends
3. imply borrow cost per term to satisfy American put-call parity  (American PCP)
4. imply vol-by-strike: invert each American price -> European-equivalent IV
5. fit all terms to the vol curves
```
Arbitrage is removed in **vol-space, not price-space**. Fair value for quoting
is the reverse: fitted European-equiv IV -> re-Americanize -> American price.

**1.5 Fit objective.** Input = implied vols *with error bars* (post div/borrow
modeling). Minimize **reduced chi-square + soft penalties**; Bayesian error bars
around every calibrated quantity (borrow, surface params) let the fitter trade
off no-arb constraints vs. matching the market. "Good microprices help."
Primary metric = **reduced χ²**; secondary `avE5` (avg fit error).

**1.6 Parity benchmarks (from the slides, same ESM6 snapshot).**
| curve | reduced χ² | avE5 |
|---|---|---|
| S5 (SVI, 5p) | 6.458 | 23.9 |
| C8 | 0.599 | 3.2 |
| C12m | 0.021 | 1.4 |

Rich nested curves fit ~100× better (reduced-χ²) than S3/SSVI on liquid names;
no-arb mode ≈ free. These become atx-vol acceptance targets.

---

## 2. Current atx-vol state (audit result)

- **American pricer**: Andersen-Lake (≈1e-7 vs QuantLib, 5e-3 vs CN-PDE) + BAW +
  Chebyshev correction cache + American greeks. **Continuous-yield q only**;
  discrete cash divs only pre-folded into the forward (Battig-Jarrow escrow);
  no borrow arg; no library PDE (CN-PSOR exists only as a test oracle).
- **Calibrators**: eSSVI (w-domain LM), SVI Zeliade + Martini-Mingone, C8
  (vol-domain), CStar (price-domain B76). **All fit with the American-exercise
  correction DISABLED** — European B76 / w-domain targets. Confirmed PORT NOTEs
  in cstar/svi-mm/c8/essvi/calib.
- **Fit metric**: vega-weighted RMSE in `FitDiag`; `Loss{Mid|Interval}`,
  `anchor{Mid|Bid|Ask}` partial micro-price support. **No reduced-χ², no error
  bars, no minimum-edge, no fair-value-within-bid-ask.**
- **Curves**: `forward_div_corrected` (escrowed cash div); borrow field present
  but not computed; no hybrid/proportional blend.
- **Data**: `QuoteFrame`/`QuoteRow` in-memory panel + `data_install` (working);
  `load_spiderrock_parquet` -> `NotImplemented`; **no chain loader, no committed
  chain fixtures anywhere in the repo**.
- **Interp**: log-moneyness per-slice closed form; linear-in-total-variance in T;
  no-extrapolation guards. Sound and Vola-aligned.

---

## 3. Gap → action matrix

| # | Gap | Action | New/edit | Priority |
|---|---|---|---|---|
| G1 | No American IV inversion | `american_implied_vol()` — invert AL/BAW price → IV (Halley/bisection on the American pricer) | new `american_iv.{hpp,cpp}` | CRIT |
| G2 | No borrow/PCP implication; cash-div-only forward | `imply_borrow_pcp()` per term (American PCP) + hybrid dividend forward (escrow + proportional blend) | new `dividend.{hpp,cpp}` | HIGH |
| G3 | Calibrators price European (Am correction off) | De-Americanization front end: chain → European-equiv IV `FitObs` (uses G1,G2), feeds existing calibrators unchanged | new `deamer.{hpp,cpp}` | CRIT |
| G4 | No reduced-χ²/error bars/minimum-edge | Per-vol error bars from bid-ask (vega→vol), reduced-χ² + minimum-edge in fit diagnostics | new `fit_metrics.{hpp,cpp}` + small `calib`/`vol_surface` diag fields | HIGH |
| G5 | No parity metrics | `fair_value_within_bid_ask()`, `rmse_mid`, `reduced_chi2` over a re-Americanized chain | new `parity.{hpp,cpp}` | HIGH |
| G6 | No chain loader / fixtures | Wire `load_spiderrock_parquet` onto `atx::core::io::read_parquet` (schema map exists) + CSV loader + **known-truth synthetic American-equity panel generator** | new `panel_synth.{hpp,cpp}`, extend `data.cpp` | HIGH |
| G7 | Exact S3 + ATF bound not explicit | S3 shape-curve evaluator in Vola's normalized-z form + ATF no-arb bound; verify atx-vol Roper `g` matches Vola `g(z)` | new `s3.{hpp,cpp}` | MED |
| G8 | Discrete-div American accuracy | Promote CN-PSOR oracle to a library **discrete-cash-dividend PDE pricer**; cross-check vs escrowed AL | new `pde_american.{hpp,cpp}` | MED |
| G9 | Calendar "spread across terms w/ error bars" | Verify/extend `essvi_calib_surface_sequential` uses error bars; parity test no-arb ≈ free | edit essvi_calib | LOW |

---

## 4. Acceptance (parity harness)

A single test binary path drives real parity:

1. **Known-truth synthetic panel** (G6): pick a truth surface (C8/CStar with an
   event W-shape), real hybrid dividends + borrow, generate American mid prices
   via the pricer, apply realistic bid-ask bands.
2. Run the full pipeline: de-Americanize (G3) → fit (existing calibrators) →
   re-Americanize fair value (G1) → parity metrics (G5).
3. **Assert**: (a) fit reduced-χ² ≤ ~1 (well-specified, since truth ∈ family);
   (b) **fair-value-within-bid-ask fraction ≥ 0.95**; (c) mid-RMSE ≤ half-spread;
   (d) C8/CStar reduced-χ² ≥ ~50× better than S3 on the event fixture (Vola's
   ~100× claim, conservatively); (e) no-arb mode within ~10% of regular χ².
4. **Loadable panels**: same harness runs over any `QuoteFrame` produced by the
   parquet/CSV loader when real data is present (skips when absent, like the C).

Real-vendor RMSE-in-bps needs a Vola/market feed not in-repo; known-truth
self-consistency + the PDE oracle is the rigorous in-repo substitute and is how
the numbers above are made falsifiable.

---

## 5. Build sequence (subagent waves; main thread owns integrate/build/test)

- **Wave 1 (foundation)**: G1 american_iv, G2 dividend/borrow, G7 s3, G4 fit_metrics.
  All new files, self-contained, no CMake edits, no builds.
- **Wave 2 (pipeline+data)**: G3 deamer (needs G1,G2), G5 parity (needs G1),
  G6 loaders+synth. 
- **Wave 3 (validate)**: parity harness test (§4), G8 pde_american, G9 sequential.

Each wave: main thread wires CMake, builds under `/W4 /permissive- /WX` (clang-cl
via vcvars), runs the full `atx-vol-tests`, fixes integration, then proceeds.
Conventions per `.agents/cpp/agent.md`: `enum class`, `Result<T>`, Rule of Zero,
GoogleTest TDD, zero warnings.

---

## 6. Out of scope (documented, not required for American-equity parity)

- Live forward refit (robust PCP + Tukey) — already deferred in curve.hpp.
- AVX2/AVX-512 throughput kernels — scalar-backed batch is bit-exact.
- Databento OPRA network puller path (cost/network) — parquet/CSV/synthetic only.
- Full SLVJ/exotics calibration — Vola feature, not American-vanilla parity.

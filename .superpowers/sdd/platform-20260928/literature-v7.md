# v7 literature review: from alpha library to production equity L/S platform

**Scope.** Extends `mega-alpha-20260926/v6-literature.md` (per-theme review §3, missing families §4, basic combination §5 and cost mitigation §6 are not repeated) along the seven questions of `task-P2-brief.md`.
**Book facts used** (handoff-5 §2; data request):
- v6.1 final cell: S2 net SR +1.239 on TRAIN 2020-2022; DSR at N 29 is .911 < .95; not validated.
- 32 admitted sub-alphas, `ew-theme-v1`.
- Construction `aim-partial-v5`: theta .05, dust .1, delta orders, exit .05, L 1.247.
- S2 cost model: linear 6 bps + sqrt impact + 1% ADV cap + `swap-fin-v1`.
- Calibration arithmetic uses the documented v6 final cell: gross SR 1.547, vol 4.57%, tau .0375/day, 13.55 bps/$ at L 1.247 vs 12.82 at L 1.

**Conventions.** Evidence grades as in v6: **A** replicated top-journal; **B** single-study or mixed; **C** thin or practitioner-only. *[est]* = my arithmetic from cited magnitudes (±50%). "(practitioner)" = not peer reviewed. Inline citations are Author Year Venue; §9 lists every one with its URL. Read-only research; nothing was run.

---

## S0. One-page summary

| # | topic | top recommendation (implement X because Y) | expected effect | conf. | effort |
|---|---|---|---|---|---|
| 1 | Alpha combination | Keep prior-based 1/N themes. Estimate only second moments and decay: w_k ∝ (prior SR_k / σ̂_k) × 1/(1+19φ_k), shrunk toward EW with δ ≈ .2, capped at [.5, 2]×EW. Because plug-in means, IC weights or ML lose 35-60% of SR at N 9-32 and T 3 y [est]. | SR 0 to +.05; turnover −5 to −15% | B+ | S |
| 2 | Construction | Replace the aim-partial heuristic with a daily cost-aware optimiser around the Garleanu-Pedersen aim: risk model, name-level impact, cost^{1/3} no-trade band, borrow, ADV caps. Because theta .05 is GP-optimal only if costs ∝ Σ, and ours are not. | SR +.05 to +.15 at $1bn, or 2-4x capacity at equal SR | B+ (direction A) | L |
| 3 | Risk model | Build a USE4-style fundamental model (market + ~50 industries + ~10 styles; EWMA 84/504; Newey-West; eigenfactor adjustment; VRA; Bayesian specific risk), plus a nonlinear-shrinkage statistical cross-check and a bias-statistic harness. | ex-ante/realised bias within 1±√(2/T); vol −10 to −20% at equal gross; prerequisite for #2 | A- | M-L |
| 4 | Capacity | Replace the fixed sqrt coefficient with a Kyle-Obizhaeva / FIM name-level model, and publish a replayed capacity curve. S2 extrapolation: net SR 1.0 at ≈ $5bn, break-even ≈ $85bn [est]. | sizing decision; cost realism | B | S-M |
| 5 | Backtest integrity | Machine-readable trial ledger; effective-N DSR (ONC clustering); CSCV PBO on every grid; CPCV for fitted combiners; per-alpha report card (margin, fitness, decay curve, sub-universe, PnL correlation ≤ .7). | fewer false positives at 100+ alphas; faster admission | A (method) | M |
| 6 | New families | Wave 1 needs no new data (XBRL + bars): q5 expected growth, nincr, QMJ-safety. Wave 2 is data-gated: opportunistic insider buying, earnings-date delay, borrow-utilisation screen, 13F confidential holdings / best ideas. | book SR +.05 to +.10 (diversification) | B-/C+ | M each |
| 7 | Live operations | Monitor daily: bias statistic, sleeve IC/turnover CUSUM, implementation shortfall vs model, crowding. Retire only on a CUSUM alarm plus a reason, margin < cost, or a data break. Bayesian reweight with ~10 y of prior weight. | protects capital; avoids noise-driven churn | B | M |

---

## S1. Alpha combination beyond equal weight

**State of the art.** Combining alphas is a portfolio problem over alpha returns, so the binding constraint is how precisely their means are estimated.
- **Optimal-weight theory.**
  - Grinold-Kahn 2000 (book): alpha = IC × σ × score.
  - Qian-Hua-Sorensen 2007 (book): the IR-optimal linear combination is w ∝ Σ_IC⁻¹ E[IC].
  - Qian-Hua 2004 JOIM: IC volatility ("strategy risk") adds risk that the risk model does not see.
- **Estimated weights rarely beat equal weights out of sample.**
  - DeMiguel-Garlappi-Uppal 2009 RFS: sample mean-variance needs ~3000 months to beat 1/N with 25 assets.
  - Kan-Zhou 2007 JFQA: plug-in weights are never optimal under estimation risk.
  - Claeskens-Magnus-Vasnev-Wang 2016 IJF: estimated combination weights are biased and add variance, which explains the forecast-combination puzzle.
- **What does beat 1/N uses little mean information.**
  - Tu-Zhou 2011 JFE: an optimal mix of 1/N and a sophisticated rule beats 1/N in most scenarios.
  - Kirby-Ostdiek 2012 JFQA: volatility timing and reward-to-risk timing beat 1/N even under high costs, because their turnover is low.
- **ML results need long panels or ignore costs.**
  - Gu-Kelly-Xiu 2020 RFS: 18 y training, 12 y validation, 30 y test. Their dominant predictors (momentum, liquidity, volatility) are the costly ones.
  - Chen-Pelger-Zhu 2024 MS: out-of-sample annual SR 2.1 on a 50-year monthly panel, with no cost model.
  - Avramov-Cheng-Metzker 2023 MS: ML profits concentrate in microcaps and distressed firms, and fade after costs.
  - Kelly-Malamud-Zhou 2024 JF ("virtue of complexity") is about *market timing*. Nagel 2025 NBER shows that its short-window version reduces to volatility-timed momentum, which happened to work historically.
  - The only net-of-cost ML evidence is Jensen-Kelly-Malamud-Pedersen 2026 RFS, which learns portfolio weights directly on a cost-aware objective.
- **Platform scale.** Kakushadze-Yu 2017 JAM (practitioner-academic): regress normalised alpha returns on risk factors, then weight the residual means by residual variance. The cost is linear in N.
- **Selection bias.** Novy-Marx 2016 NBER: combining the best k of n signals carries a bias nearly as large as picking the best of n^k.

**When does fitting beat 1/N here? [est]** Setup:
- N sleeves, T years, true optimal SR θ.
- The plug-in tangency portfolio has OOS SR ≈ θ²/√(θ² + N/T). Mean error dominates (Kan-Zhou logic).
- If 1/N captures a fraction f of θ, fitting wins only if T > N f² / (θ² (1 − f²)).

| N | T (years) | plug-in OOS SR (θ = 1.5) | 1/N at f = .85 | break-even T (f = .85 / .70) |
|---|---|---|---|---|
| 32 sleeves | 3 (TRAIN) | .63 | 1.28 | 37 y / 14 y |
| 32 sleeves | 13 (2010-22 via U2) | 1.04 | 1.28 | same |
| 9 themes | 3 | .98 | 1.28 | 10 y / 4 y |
| 9 themes | 13 | 1.31 | 1.28 | same |

Covariances are a different matter: 32 sleeves on 750 days is N/T ≈ .04, which Ledoit-Wolf shrinkage handles well (§3).

**Recommendations**
- **R1.1 Keep `ew-theme-v1` as the production default** until 10-15 y of PIT history exist (owner gate U2).
  - *Because:* plug-in MV or IC weights lose 35-60% of SR at T = 3 y [est]. v3 went from TRAIN 1.81 to VAL −1.27.
  - *Effect:* avoids an OOS loss of −.3 to −.6 SR.
- **R1.2 Estimate only what is estimable.**
  - Theme weight: w_theme ∝ (prior_SR_k / σ̂_k) × GP_k, capped at [.5, 2]×EW.
    - σ̂ comes from an LW-shrunk 252-day sleeve covariance.
    - GP_k is the member mean of 1/(1 + 19φ_k), where φ_k is the daily decay of the member's rank autocorrelation. §2 derives the 19 from theta .05.
  - Within a theme, average members; do not prune (v6 §5.3).
  - *Because:* second-moment weighting beats 1/N net of costs (Kirby-Ostdiek 2012 JFQA), and fast signals deserve less aim weight (Garleanu-Pedersen 2013 JF).
  - *Effect:* SR 0 to +.05; turnover −5 to −15% [est].
  - *Note:* `ew-theme-aim-v1` uses g_k = θ Σ_j (1−θ)^j ρ_k(j). When ρ_k(j) = (1−φ_k)^j this equals 1/(1+19φ_k) exactly, so it already *is* GP Proposition 4. Its TRAIN loss (−.126, SE .112) is 1.1 SE and is not evidence against the theory. Re-test it only on longer history.
- **R1.3 Shrink any mean-based fit toward EW** (Tu-Zhou 2011 JFE): w = (1−δ) w_EW + δ w_fit, with δ = T/(T + T0).
  - T0 comes from the table: ≈ 10 y for themes, ≈ 37 y for sleeves. At T = 3 y that gives δ ≈ .23 and ≈ .08.
  - Pre-register δ; it counts as one trial.
- **R1.4 Use IC weights only as a within-theme tie-breaker**, with at most 25-50% data weight.
  - On non-overlapping 21-day horizons, SE(mean IC) ≈ σ_IC/√36 ≈ .015, against typical ICs of .02-.04 [est]. Raw IC weights are mostly noise.
- **R1.5 ML stacking is a research track only.**
  - Run it on the U2 panel (2005 onward) with a cost-aware objective (JKMP 2026 RFS).
  - Judge it by CPCV (R5.4): paired net dSR > 0 in at least 80% of paths.
  - Not before 10 y of data.

---

## S2. Portfolio construction with costs and turnover control

**State of the art.**
- **Closed-form dynamics.** Garleanu-Pedersen 2013 JF solve dynamic trading with quadratic costs Λ and AR(1) signals:
  - trade a fixed fraction a/λ toward an aim portfolio;
  - the aim is the Markowitz portfolio with each signal scaled by (1 + φ_k a/γ)⁻¹.
- **Liquidity regimes.** Collin-Dufresne-Daniel-Saglam 2020 JFE let the trading rate vary with liquidity: trade faster in liquid, persistent states.
- **No-trade bands.** Linear (spread) costs create a no-trade region (Constantinides 1986 JPE).
  - For small costs its width scales as cost^{1/3} (Janeček-Shreve 2004 FS).
  - de Lataillade-Deremble-Potters-Bouchaud 2012 JIS derive the band for mean-reverting signals.
- **General formulation.** Boyd et al. 2017 FnT-Opt give the convex single-period (SPO) and model-predictive multi-period (MPO) problem, with risk, trading, holding and borrow costs.
- **Evidence that optimisation pays:**
  - Frazzini-Israel-Moskowitz 2015 WP: cost-optimised size/value/momentum at ≤ 1% tracking error raise break-even size 3-6x. US UMD goes from $56bn to ~$100bn at 50 bps TE and to $159bn at 100 bps; HML from $214bn to $558bn.
  - Novy-Marx-Velikov 2016 RFS: the buy/hold spread is the most effective simple mitigation.
  - DeMiguel-Martin-Utrera-Nogales-Uppal 2020 RFS: combining characteristics nets trades; the number of significant characteristics rises from 6 to 15 once costs are included.
  - JKMP 2026 RFS: cost-aware learned portfolios dominate cost-agnostic ones.
- **Constraints cost alpha.** Clarke-de Silva-Thorley 2002 FAJ: every binding constraint lowers the transfer coefficient.

**Mapping our heuristic onto GP [est; daily discount ρ ≈ 0].**
- Take cost ½Δx'ΛΔx with Λ = λΣ.
- The trading rate τ = a/λ solves τ² + kτ − k = 0, where k = γ/λ.
- Hence k = τ²/(1−τ) and a/γ = (1−τ)/τ.

| theta | γ/λ | a/γ | aim weight 1/(1+(a/γ)φ) at signal half-life 5 / 10 / 21 / 63 / 252 d |
|---|---|---|---|
| .05 (current) | .0026 | 19.0 | .29 / .44 / .62 / .83 / .95 |
| .03 (TRAIN: −.001, SE .133) | .0009 | 32.3 | .19 / .32 / .49 / .74 / .92 |

**Already GP-consistent:**
- uniform partial trading (theta);
- delta orders, i.e. trading relative to drifted holdings;
- exit .05, i.e. trading toward a zero aim at the same rate.

**Departures from GP:**
- **(a) Costs are not ∝ Σ.** GP's single rate needs Λ ∝ Σ. Ours are linear + sqrt with name-level ADV, so the optimal rate matrix Λ⁻¹A_xx is name-specific *and* the aim holds smaller positions in costly names. The v6 per-name theta test (−.195) changed only the trading speed.
- **(b) The target is not a Markowitz portfolio.** It is a rank vector neutralised by projection, not (γΣ)⁻¹Bf built on a risk model.
- **(c) The band is uniform.** Dust .1/N is the same for every name. Cost^{1/3} scaling implies that a name with 8x the spread needs a band 2x wider.
- **(d) Fast sleeves get full aim weight** in `ew-theme-v1`. R1.2 fixes this.

**Recommendations**
- **R2.1 Daily single-period optimiser (Boyd SPO) around the GP aim.**
  - *Objective:* max_w α'w − (γ/2) w'Σ_risk w − Σ_i [s_i|Δw_i| + η_i|Δw_i|^{3/2}] − Σ_i b_i max(−w_i, 0).
  - *Inputs:*
    - α_i = Σ_k W_k (1+19φ_k)⁻¹ IC_k σ_i z_ik (Grinold-Kahn form, with prior IC_k, not fitted);
    - η_i from the §4 cost model;
    - b_i from the `swap-fin` tiers (30/100/500 bps), later from vendor fees (data ask D4).
  - *Constraints:*
    - 1'w = 0; |β'w| ≤ .02; gross ≤ L;
    - |w_i| ≤ min(w_max, q·ADV_i/NAV);
    - |Δw_i| ≤ p·ADV_i/NAV, with p = 1% today;
    - w_i ≥ 0 where there is no locate.
  - *Calibration:* γ targets ex-ante vol of 4-5%/yr. Industries and styles are penalised through Σ_risk rather than projected out (R3.4).
  - *Solver:* 3000 names daily is a small SOCP/power-cone problem (Clarabel, ECOS, or OSQP with a piecewise-linear 3/2 term). cvxportfolio is a reference implementation (Boyd et al. 2017).
  - *Because:* FIM 2015 find 3-6x capacity at ≤ 1% TE; also Novy-Marx-Velikov 2016 and JKMP 2026.
  - *Effect:* SR +.05 to +.15 at $1bn, or equal SR at 2-4x AUM [est].
  - *Effort:* L; needs R3.1 and R4.1.
- **R2.2 Cheap interim step (M):** target_i = aim_i / (1 + κ c_i/c̄) and band_i = (b/N)(c_i/c̄)^{1/3}.
  - Declare one κ and one b before any read. This replaces the uniform dust band.
  - *Effect:* cost −25 to −45% for gross −3 to −8% (v6 lever 2) [est].
- **R2.3 Regime-dependent trading rate** (Collin-Dufresne-Daniel-Saglam 2020 JFE): θ_t = θ·(c̄_ref/c̄_t)^{1/2}, clipped to [.5, 1.5]θ.
  - c̄_t is the cross-sectional median modelled cost, driven by VIX and spreads.
  - Grade B-; effort S.
- **R2.4 Multi-period MPC** (5-21 d horizon with decaying alpha forecasts) only after SPO is live. The GP aim already embeds decay, so the increment should be small [est; C].
- **R2.5 Report the transfer coefficient daily**, TC = corr(α_i/σ_i², w_i) (Clarke-de Silva-Thorley 2002 FAJ). If TC < .5 [est], constraints (locate, ADV caps, neutrality) are eating the alpha; investigate.

---

## S3. Risk models: what a small shop should build for ~3000 names daily

**State of the art.**
- **Fundamental models.** Barra USE4 (Menchero-Orr-Wang 2011, practitioner methodology notes):
  - Daily cross-sectional regressions on country + 60 industries + style factors.
  - Factor covariance by EWMA. USE4S settings: vol half-life 84 d, correlation half-life 504 d, Newey-West lags 5/2, volatility-regime adjustment half-life 42 d.
  - An eigenfactor adjustment fixes the sample covariance's underprediction of optimised-portfolio risk, a bias that grows with K/T (Menchero-Wang-Orr 2012 FAJ; Shepard 2009 as cited there).
  - Specific risk is shrunk with a Bayesian and a structural model.
  - Accuracy is judged by the bias statistic. Its 95% band is 1 ± √(2/T) under normality, but it covers only ~86% at kurtosis 5 with T = 120 (USE4 notes, App. A).
- **Statistical models.**
  - Asymptotic PCA: Connor-Korajczyk 1988 JFE.
  - POET, i.e. factors plus thresholded sparse residuals: Fan-Liao-Mincheva 2013 JRSSB.
- **Shrinkage:**
  - Ledoit-Wolf 2004 JPM: linear.
  - Ledoit-Wolf 2017 RFS: nonlinear, optimal for portfolio selection when N ~ T.
  - Ledoit-Wolf 2020 AoS: analytical nonlinear, ~1000x faster, works to N ≥ 10,000.
  - Ledoit-Wolf 2022 Bernoulli: quadratic.
  - Bun-Bouchaud-Potters 2017 Phys. Rep.: RMT rotationally invariant cleaning.
  - Engle-Ledoit-Wolf 2019 JBES: DCC-NL for time-varying covariance.
  - De Nard-Ledoit-Wolf 2021 JFEc: a factor model plus (dynamic) NL shrinkage of residuals, tested on 1000 stocks.
- **Hedging.**
  - Daniel-Mota-Rottke-Santos 2020 RFS: removing unpriced risk from characteristic portfolios raises the squared SR of the FF5 combination from 1.17 to 2.13.
  - Ehsani-Harvey-Li 2023 FAJ: sector neutrality helps long-short investors.

**Our position.**
- The only risk control is `price-risk-v1`, a projection on beta252, vol63 and ladv63.
- The C5 industry projection cut vol from 3.62% to 2.92% (−19%), with gross SR 1.304 → 1.316, but net SR fell by .072 (SE .156).
- Hard projection also strips alpha content, e.g. industry momentum.

**Recommendations**
- **R3.1 Build `atx-risk-v1` (fundamental).**
  - Daily WLS with √mcap weights of returns on market + FF49 industries (merge industries with < 10 names) + ~10 styles already in the field set: size, beta252, residual vol, 12-1 momentum, B/P-E/P value, profitability, asset growth, leverage, ladv63 liquidity, SI/DTC.
  - USE4S half-lives (84/504, Newey-West 5/2, VRA 42) and a Monte-Carlo eigenfactor adjustment.
  - Specific risk: EWMA-84 residual variance, Bayesian-shrunk toward the size-decile mean, with a structural fallback for names with < 252 d of history.
  - *Effect:* correct ex-ante risk, and it enables R2.1. Compute is trivial at 3000 × 65. *Effort:* M-L.
- **R3.2 Statistical cross-check `atx-risk-stat`.**
  - APCA/POET with 10-20 factors on a 252-504 d window, plus LW-2020 analytical NL shrinkage of residuals.
  - Flag names or portfolios where the two models disagree by more than 25% [est].
  - Use LW linear or quadratic shrinkage for the 32-40-sleeve alpha covariance (N/T ≈ .05).
  - *Effort:* M.
- **R3.3 Bias-statistic harness.**
  - Rolling 63 d and 252 d bias statistics on random portfolios, factor-mimicking portfolios and the optimised book.
  - Accept if within 1 ± √(2/T), with the band widened by simulation for the book's kurtosis (13.5), as in the USE4 notes.
  - *Effort:* S.
- **R3.4 Neutralisation policy.**
  - Hard constraints only on dollar and beta.
  - Industry and style risk enter via γ w'Σw in R2.1. Priced tilts then survive (Clarke-de Silva-Thorley 2002) while unpriced risk is hedged (Daniel-Mota-Rottke-Santos 2020).
  - *Effect:* vol −10 to −20% at equal gross (from the C5 result); SR +0 to +.1 if gross is preserved [est; B].

---

## S4. Capacity and market impact

**State of the art.**
- **Live institutional costs.** Frazzini-Israel-Moskowitz 2018 WP, $1.7tn of AQR trades 1998-2016:
  - Market impact: mean 9.97 bps, median 6.18, value-weighted 15.14. Implementation shortfall: mean 11.02 bps.
  - Large caps 8.90 bps vs small caps 18.95.
  - Average trade is 0.9% of daily volume (range < 0.1% to 13.1%), worked over 2.7 days.
  - ~85% of impact is permanent.
  - Impact is concave in size (log-log slope .35): the model gives 13.73 bps at 2% of DTV and 32.34 bps at 10%.
  - Across stocks at 1% DTV the model's median is 14.55 bps (p10 8.68, p90 38.1).
  - Traded firms are large (mean $15.4bn).
- **Invariance.** Kyle-Obizhaeva 2016 Econometrica, 400k portfolio-transition orders 2001-05:
  - Cost C = σ W^{−1/3} [κ0 + κI (W^{2/3} X/V)^{1/2}], with W = σ·P·V.
  - Square-root calibration: κ0 = 2.08 bps, κI = 12.08 bps for a benchmark stock ($40 × 1M shares/day, σ = 2%/day), giving 14.16 bps at 1% ADV.
  - Transition orders average 16.79 bps: 6.16 in the top-volume decile, 44.95 in the bottom. Mean order 4.2% of ADV, median 0.57%.
- **Functional form of impact:**
  - Almgren-Thum-Hauptmann-Li 2005 Risk: temporary-impact exponent ≈ .6 (as summarised by Said 2022 arXiv).
  - Toth et al. 2011 PRX: the square-root law.
  - Bucci-Benzaquen-Lillo-Bouchaud 2019 PRL, 8M ANcerno trades: a crossover from linear impact for small orders to square-root for larger ones.
  - Zarinelli-Treccani-Farmer-Lillo 2015 MML: square-root fits about 2 decades of order size; log fits about 5.
- **Capacity:**
  - FIM 2015 WP, US break-even: HML $214bn, UMD $56bn.
  - Ratcliffe-Miranda-Ang 2017 JII, 1-day horizon: from momentum $65bn to size $5tn.
  - Landier-Simon-Thesmar 2015 WP: a GP-based performance-scale frontier. SR decays more slowly for liquid stocks, persistent signals and high frictionless SR.
  - Pastor-Stambaugh-Taylor 2015 JFE: decreasing returns to scale at industry level.

**Where our model sits [est].** Two-point calibration on the v6 final cell (12.82 bps/$ at L 1, 13.55 at L 1.247, same book) gives c ≈ 6.6 + 6.3√g bps/$, where g is gross notional in $bn. Holding turnover fixed at .0375/day (9.45×/yr), with gross μ 7.07%, financing ≈ .39%/yr and vol 4.57%, net μ(N) ≈ 6.06% − .66%·√N for NAV N in $bn:

| NAV | $1bn | $2bn | $5bn | $10bn | $25bn | break-even | $-profit maximum |
|---|---|---|---|---|---|---|---|
| S2 net SR | 1.18 | 1.12 | 1.00 | .87 | .60 | ≈ $85bn | ≈ $37bn (SR ≈ .44) |

What the extrapolation ignores:
- the 1% ADV cap binding, which lowers turnover and alpha capture;
- a re-tuned theta at scale, which raises capacity (Landier-Simon-Thesmar);
- a possibly worse-than-sqrt tail at high participation (Zarinelli et al.).

Treat "SR 1.0 at ≈ $5bn" as ±50%.

At $1bn the average name trades ~$15-20k/day, about 0.1% of ADV for a median member [est]. There, the KO square-root formula gives ~6 bps for the benchmark stock. So S2's 13.55 bps/$ is conservative against AQR-grade realised costs and close to the transition average. S1 (flat 6 bps, net +1.324) is the optimistic bound.

The 1% ADV per-name-day cap is tighter than FIM's practice (0.9% average over 2.7 days, up to 13%) and binds only in the illiquid tail.

**Recommendations**
- **R4.1 Name-level cost model v2.**
  - impact_i(x) = (σ_i/.02) (W_i/W*)^{−1/3} [κ0 + κI √(x (W_i/W*)^{2/3} / .01)], with the KO constants as the prior.
  - Half-spread from quotes, or Corwin-Schultz from daily bars (data ask D9).
  - Keep S2 primary. Add stresses S2-KO, S2-FIM (a + b·x + c·√x at FIM medians) and S2-log.
  - Calibrate a and κI to live fills once trading, using the FIM method.
  - *Effort:* S-M.
- **R4.2 Capacity curve by replay, not extrapolation.**
  - NAV ∈ {.5, 1, 2, 5, 10, 20} $bn, with caps binding and theta reset per NAV by the GP rule τ ≈ √(γ/λ), λ ∝ √NAV.
  - Report net SR, $ PnL, % of trades capped, and participation p50/p95.
  - Choose AUM where marginal net alpha per $ still clears the hurdle.
  - *Effort:* M.
- **R4.3 Participation policy [est].**
  - Keep ≤ 1% ADV per name-day up to $2bn NAV.
  - Above that, let the optimiser schedule up to 3-5% ADV over several days (the cost function prices it) rather than hard-truncating.
  - Cap any position at 25% of ADV, i.e. an exit within 5 days at 5% participation.

---

## S5. Backtest integrity for a growing alpha platform

**State of the art.**
- **Selection bias:**
  - Deflated SR: Bailey-López de Prado 2014 JPM.
  - PSR and minimum track-record length (MinTRL): Bailey-López de Prado 2012 JoR.
  - Probability of backtest overfitting (PBO) via combinatorially symmetric CV (CSCV): Bailey-Borwein-López de Prado-Zhu 2017 JCF.
  - Combinatorial purged CV (CPCV) with embargo: López de Prado 2018 (book).
  - Effective number of trials by clustering trial returns: López de Prado-Lewis 2019 QF.
- **Multiple-testing hurdles:**
  - t > 3: Harvey-Liu-Zhu 2016 RFS.
  - 3.8 for time-series and 3.4 for cross-sectional tests; otherwise ~45% false rejections: Chordia-Goyal-Saretto 2020 RFS.
  - SR haircuts are non-linear, so a flat 50% is wrong: Harvey-Liu 2015 JPM.
  - FDR-calibrated hurdles by double bootstrap: Harvey-Liu 2020 JF.
- **Literature priors are informative.**
  - Chen-Zimmermann 2022 CFR reproduce 98% of 161 clearly significant predictors at t > 1.96 (slope .88).
  - Jensen-Kelly-Pedersen 2023 JF.
- **Signal timing.** Anomaly returns concentrate in the first month after the information release (Bowles-Reed-Ringgenberg-Thornock 2024 JF).
- **Platform practice (WorldQuant):**
  - Kakushadze 2016 Wilmott: 101 formulaic alphas; holding periods 0.6-6.4 d; 15.9% mean pairwise correlation; returns track volatility, not turnover.
  - Kakushadze-Tulchinsky 2016 JIS: across 4,000 live alphas, R ~ V^0.8-0.85 and cents-per-share ~ 1/turnover.
  - Tulchinsky et al. 2019 (book).
  - BRAIN platform (practitioner):
    - fitness = SR·√(|ret| / max(turnover, .125));
    - margin = PnL / $ traded, in bps;
    - submission requires SR ≥ 1.25 (target 1.5), fitness ≥ 1, turnover ≤ 70% (1-20% preferred), daily-PnL correlation to the pool < .7 unless SR is ≥ 10% higher, a sub-universe check, and per-name weight < 10%.
  - Novy-Marx-Velikov 2023 WP ("Assaying Anomalies") standardise a per-signal report benchmarked to 200+ anomalies.

**Our position.**
- DSR at N 29 is .911 (Lo null .50).
- NAV cells correlate at ~.95. The 76 admission trials and the library-design choices are outside N.
- **MinTRL [est].** Formula (Bailey-López de Prado 2012): 1 + [1 − γ3·SR + (γ4−1)/4·SR²]·(z/(SR − SR*))². For the final cell (SR 1.18, skew −1.32, kurtosis 13.5):
  - ≈ 550 days (2.2 y) to reject SR* = 0 at 95%;
  - ≈ 1,640 days (6.5 y) to reject SR* = .5.
  - So a 2-year VAL can falsify the cell but cannot confirm it.

**Recommendations**
- **R5.1 Machine-readable trial ledger** (JSON lines). Log every admission, composition, construction, universe and data-version run with its daily net series and pins. It becomes the source of N for every statistic. *Effort:* S.
- **R5.2 Effective-N DSR from v7 onward.** Pre-register it; do not re-score the v6 gate.
  - ONC-cluster all trial series (López de Prado-Lewis 2019); N_eff = number of clusters; V[SR] across cluster representatives.
  - Report it beside the cell-count DSR and the Lo null.
  - *Effort:* M.
- **R5.3 PBO by CSCV** on every grid of ≥ 4 cells: 16 blocks of ~47 sessions, C(16,8) = 12,870 splits. Accept a grid winner only if PBO ≤ .2 [est threshold]. *Effort:* S-M.
- **R5.4 CPCV for any fitted combiner or ML.** 6 groups of ~125 sessions with 2 test groups gives 15 splits and 5 paths. Purge 21 sessions (the signal horizon); embargo 5. Report the path distribution of net SR. *Effort:* M.
- **R5.5 Admission rules at 100+ alphas:**
  1. Prior-signed canonical definitions keep the current veto rule.
  2. Data-mined or parameter-searched candidates need HAC t ≥ 3 (Harvey-Liu-Zhu 2016; Chordia-Goyal-Saretto 2020) and BY-FDR ≤ 10% per batch.
  3. Daily-PnL correlation with the book < .7, or a spanning t ≥ 2 of candidate PnL on book PnL.
  4. Sub-universe: same sign, and ≥ 50% of the full-universe IC, in the top-1000 ADV names [est].
  5. Margin ≥ 2× the modelled cost/$ at standalone turnover [est].
  6. Form the signal at information time, with no blanket smoothing lag (Bowles et al. 2024).

  Every candidate counts as a trial.
- **R5.6 Per-alpha report card**, auto-generated in the style of Assaying Anomalies. *Effort:* M. Contents:
  - IC and IR by year, by size tercile and by FF12 industry;
  - the IC(h) decay curve for h = 1..63 and its half-life, which feeds φ_k;
  - turnover, margin, fitness;
  - capacity at $1bn;
  - correlation to the 32 members and the 9 themes;
  - coverage per year.

---

## S6. Alpha families not yet in the library (extends v6 §4)

| family | signal | data (ask id) | turnover | evidence | gross SR [est] | priority |
|---|---|---|---|---|---|---|
| q5 expected growth | fitted 1-3 y investment growth from Tobin's q, CFO/A and ΔROE (rolling FM slopes) | have (XBRL + bars) | low | 0.84%/mo, t 10.3, 1967-2018 (Hou-Mo-Xue-Zhang 2021 RoF) | .4-.7 | P1 |
| nincr | number of consecutive quarters with a YoY quarterly EPS increase (0-8) | `ni_q` history (D7 for 8 quarters) | low | one of only 2 characteristics independent after 2003 in non-microcaps (Green-Hand-Zhang 2017 RFS) | .2-.4 | P1 |
| QMJ safety / payout | low leverage, low earnings volatility, payout (Asness-Frazzini-Pedersen 2019 RAS) | have; replaces `fscore` (54% coverage) | low | A- (global) | +.05-.1 on top of quality | P2 |
| Opportunistic insider buying | net $ buys by non-routine insiders / mcap, held 1-6 months | Form 4 (D11; free on EDGAR) | low | VW 82 bps/mo, routine trades ≈ 0 (Cohen-Malloy-Pomorski 2012 JF); thinner evidence after 2007 | .3-.6 | P2 |
| Earnings-date delay | scheduled date later than expected (same quarter last year) → short into the announcement | expected and scheduling dates (D2) | event, 2-6 weeks | later dates precede worse news; equity prices react slowly (Johnson-So 2018 JFQA) | .3-.5 | P2 |
| Borrow / utilisation | short-side screen on fee and utilisation; long low-fee names | vendor fee + utilisation (D4) | low-mid | CME portfolio 1.31%/mo gross, .78% net (Drechsler-Drechsler 2016 WP); supply binds when shorting is most attractive (Beneish-Lee-Nichols 2015 JAE); fees erase the average anomaly (Muravyev-Pearson-Pollet 2025 JF) | as a screen: +.02-.06 net | P2 |
| Announcement premium | long names expected to announce next month | D2 | high (monthly rotation) | 9.9%/yr (Savor-Wilson 2016 JF); global (Barber et al. 2013 JFE) | timing tilt only | P3 |
| 13F confidential holdings | stocks disclosed late through 13F-HR/A amendments | D1 + amendments | quarterly | outperform for up to 12 months (Agarwal-Jiang-Tang-Yang 2013 JF) | .2-.4 | P3 |
| 13F best ideas | highest-conviction tilt across active managers | D1 (weights) | quarterly | 1-4%/quarter (Cohen-Polk-Silli 2010 WP); crowded names carry tail risk (Brown-Howard-Lundblad 2022 RFS) | .2-.5 | P3 |
| Fund trade direction | net institutional buying vs selling | D1 | quarterly | stocks funds buy beat stocks they sell (Chen-Jegadeesh-Wermers 2000 JFQA); institutions trade against anomalies (Edelen-Ince-Kadlec 2016 JFE) | .1-.3 | P3 |
| Options skew / IV spread / ΔIV | 25Δ put IV − ATM IV; call−put IV; 1-month change in call IV | D8 | mid | 10.9%/yr (Xing-Zhang-Zhao 2010 JFQA); 50 bps/wk, declining (Cremers-Weinbaum 2010 JFQA); ~1%/mo decile spread for 6 months (An-Ang-Bali-Cakici 2014 JF); ≥ 2/3 is a borrow-fee proxy (Muravyev-Pearson-Pollet 2025 JFE) | ≈ 0 net without fee data | P3 |
| chempia | industry-adjusted % change in employees | employee counts (10-K; not in the data request) | annual | the other post-2003 survivor (Green-Hand-Zhang 2017) | .2-.3 | P3 |

**Post-2004 survivors buildable from SEC XBRL + CRSP-like bars.** The evidence:
- The US is the only market with a reliable post-publication decline: −62% equal-weighted, −66% value-weighted (Jacobs-Müller 2020 JFE). McLean-Pontiff 2016 JF find −58%.
- Multivariate Fama-MacBeth regressions since 2003 leave only nincr and chempia independent in non-microcaps (Green-Hand-Zhang 2017).
- Cluster-level evidence survives: 13 themes, most of them part of the tangency portfolio, across 93 countries (Jensen-Kelly-Pedersen 2023).
- Average net post-publication returns are ~4 bps/mo per anomaly and ~20 bps/mo for combinations (Chen-Velikov 2023 JFQA).

**Synthesis [est].** At ≤ monthly turnover in a liquid US universe:
- *Defensible:* profitability (GP/A, CbOP, ROE), quality/safety, investment and composite issuance, profit growth (ΔROE, nincr), multi-lag seasonality, residual momentum, composite value.
- *Weak:* accruals, asset growth.
- *Rarely survive costs:* short-term reversal, IVOL, MAX.
- *Regime-unstable:* distress.

Library v6.1 already holds most of the defensible set. The new cheap members are nincr, q5 Eg and QMJ-safety. Refresh fundamentals on the XBRL acceptance clock, as the platform already does (Bowles et al. 2024).

- **R6.1** Build wave 1 (q5 Eg, nincr, QMJ-safety) as one pre-registered library revision (3 admission trials). *Effect:* book SR +.02 to +.05 from new low-turnover, low-correlation members [est].
- **R6.2** Gate wave 2 on data, in the order D11, D2, D4. Insider data is free; borrow data brings the largest realism gain.

---

## S7. Live operation: what to monitor and when to retire

**State of the art.**
- **Execution.**
  - Measure implementation shortfall against the decision price (Perold 1988 JPM).
  - Split it into temporary and permanent impact: ~85% is permanent in FIM 2018.
- **Risk.**
  - Check ex-ante risk with bias statistics (USE4 notes).
  - Realised active risk exceeds risk-model risk by the IC volatility (Qian-Hua 2004).
- **Performance monitoring.**
  - CUSUM is the fastest detector at a given false-alarm rate. It flags flat-to-benchmark performance in ~40 months for typical managers (Philips-Yashchin-Stein 2003 JPM).
  - For a given SR, normal drawdowns are longer and deeper than managers expect (Rej-Seager-Bouchaud 2018 Wilmott).
  - Stop-loss rules add value only when returns have momentum (Kaminski-Lo 2014 JFM).
- **Decay.**
  - −26% out of sample and −58% after publication (McLean-Pontiff 2016); stronger for liquid anomalies (Chordia-Subrahmanyam-Tong 2014 JAE).
  - Decay is non-stationary, with a ~1.4%/yr gap between true and measured alpha (Penasse 2022 MS).
  - Systematic strategies lose about half their in-sample SR (Falck-Rej-Thesmar 2022 QF).
  - Live factor returns disappoint versus backtests, and correlations jump in stress (Arnott-Harvey-Kalesnik-Linnainmaa 2019 JPM).
- **Crowding.**
  - Comomentum flags crash-prone momentum (Lou-Polk 2022 RFS).
  - Crowded hedge-fund names carry tail risk (Brown-Howard-Lundblad 2022).
- **Pooling.** Estimating skill across many strategies at once sharpens each individual estimate (Harvey-Liu 2018 RFS).

**Daily monitor (thresholds [est])**
- **M1 Risk.**
  - Book bias statistic on 63 d: band 1 ± √(2/63) = [.82, 1.18], widened for kurtosis per R3.3.
  - Ex-ante vs realised factor exposures.
  - Realised / ex-ante active risk (Qian-Hua multiplier), used to set γ.
- **M2 Alpha health, per sleeve.**
  - 63 d and 252 d IC and turnover against TRAIN p5-p95.
  - Signal half-life against the φ_k in use.
  - Field coverage and staleness against data-request Appendix A.
  - CUSUM of standardised sleeve returns, testing SR_prior vs 0, with in-control ARL ≥ 5 y.
- **M3 Execution.**
  - Implementation shortfall per order and per day, against the pre-trade model.
  - Slippage ratio (realised / model): a 21 d mean outside [.7, 1.3] triggers recalibration of R4.1.
  - Participation p50/p95, % of trades capped, locate failures and recalls.
- **M4 Crowding and regime.**
  - Comomentum for the momentum sleeves.
  - SI and 13F hedge-fund crowding in our longs and shorts.
  - VIX regime for reversal-type sleeves (reversal ∝ VIX, v6 §7).
- **M5 Reconciliation.** Positions, cash and corporate actions against the broker (P1 scope).

**Retire / reweight**
- **Reweight at most quarterly**, with a prior-dominated update: SR_post = (T0·SR_prior + T·SR_live)/(T0 + T).
  - T0 = 10 y for literature-canonical sleeves [est], so one live year moves the weight by 9%.
  - Pool sleeves in a random-effects model (Harvey-Liu 2018).
- **Retire only on one of:**
  - (a) a CUSUM alarm *plus* an identified economic or data reason;
  - (b) realised margin below realised cost/$ for 12 months;
  - (c) a data-source break.

  Never on drawdown alone (Rej-Seager-Bouchaud 2018; Kaminski-Lo 2014). Expected post-publication decay (~50% haircut) is already in the priors.

---

## S8. Ranked build list (all topics)

| rank | build | topic | why / expected effect | effort | depends on |
|---|---|---|---|---|---|
| 1 | Trial ledger + effective-N DSR + CSCV PBO + MinTRL in `nav_summ` | 5 | every later lever is judged with it; cheap | S-M | — |
| 2 | Cost model v2 (KO/FIM name-level) + replayed capacity curve | 4 | resolves the largest modelling uncertainty (S1 +1.32 vs S2 +1.18) and the AUM decision | S-M | — |
| 3 | `atx-risk-v1` fundamental model + bias harness (+ statistical cross-check) | 3 | correct ex-ante risk; prerequisite for rank 5 | M-L | — |
| 4 | Combination v7: prior SR / σ̂ × GP-decay theme weights, Tu-Zhou δ ≈ .2 | 1 | SR 0 to +.05; turnover −5 to −15% | S | 1 |
| 5 | Cost-aware SPO optimiser around the GP aim (R2.1), with the R2.2 interim step first | 2 | SR +.05 to +.15 at $1bn, or 2-4x capacity | L | 2, 3 |
| 6 | Per-alpha report card + admission rules R5.5 | 5 | scales the library to 100+ without false positives | M | 1 |
| 7 | Library wave 1: q5 Eg, nincr, QMJ-safety | 6 | SR +.02 to +.05 | M | 6 |
| 8 | Daily monitor M1-M4 + retire/reweight rules | 7 | protection; production prerequisite | M | 2, 3 |
| 9 | Wave 2 families on new data (Form 4 → D2 → D4 → D1 → D8) | 6 | SR +.03 to +.08; D4 also improves realism | M each | data asks |
| 10 | ML / cost-aware learned combiner on U2 history, judged by CPCV | 1 | uncertain; only with ≥ 10 y of data | L | U2, 1, 5 |

**Do not build:**
- IC-, MV- or ML-fitted weights on 3 years of data.
- Drawdown stop-losses.
- Hard industry projection once the optimiser exists.
- Ever-finer theta or dust grids on TRAIN: each cell raises N and the PBO.

---

## S9. Bibliography (author, year, title, venue, URL)

- Agarwal, V., Jiang, W., Tang, Y., Yang, B. (2013). Uncovering Hedge Fund Skill from the Portfolio Holdings They Hide. JF 68(2). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12012
- Almgren, R., Thum, C., Hauptmann, E., Li, H. (2005). Direct Estimation of Equity Market Impact. Risk 18(7). https://www.researchgate.net/publication/228754794_Direct_Estimation_of_Equity_Market_Impact
- An, B.-J., Ang, A., Bali, T., Cakici, N. (2014). The Joint Cross Section of Stocks and Options. JF 69(5). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12181
- Arnott, R., Harvey, C., Kalesnik, V., Linnainmaa, J. (2019). Alice's Adventures in Factorland. JPM 45(4). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3331680
- Asness, C., Frazzini, A., Pedersen, L.H. (2019). Quality Minus Junk. RAS 24(1). https://link.springer.com/article/10.1007/s11142-018-9470-2
- Avramov, D., Cheng, S., Metzker, L. (2023). Machine Learning vs. Economic Restrictions. Mgmt Sci 69(5). https://pubsonline.informs.org/doi/abs/10.1287/mnsc.2022.4449
- Bailey, D., Borwein, J., López de Prado, M., Zhu, Q. (2017). The Probability of Backtest Overfitting. J. Computational Finance 20(4). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2326253
- Bailey, D., López de Prado, M. (2012). The Sharpe Ratio Efficient Frontier (PSR, MinTRL). J. of Risk 15(2). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1821643
- Bailey, D., López de Prado, M. (2014). The Deflated Sharpe Ratio. JPM 40(5). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551
- Barber, B., De George, E., Lehavy, R., Trueman, B. (2013). The Earnings Announcement Premium Around the Globe. JFE 108(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X12002188
- Beneish, M., Lee, C., Nichols, D.C. (2015). In Short Supply: Short-Sellers and Stock Returns. JAE 60(2). https://www.sciencedirect.com/science/article/abs/pii/S0165410115000580
- Bowles, B., Reed, A., Ringgenberg, M., Thornock, J. (2024). Anomaly Time. JF 79. https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13372
- Boyd, S., Busseti, E., Diamond, S., Kahn, R., Koh, K., Nystrup, P., Speth, J. (2017). Multi-Period Trading via Convex Optimization. Found. & Trends in Optimization 3(1). https://web.stanford.edu/~boyd/papers/pdf/cvx_portfolio.pdf
- Brown, G., Howard, P., Lundblad, C. (2022). Crowded Trades and Tail Risk. RFS 35(7). https://academic.oup.com/rfs/article-abstract/35/7/3231/6371884
- Bucci, F., Benzaquen, M., Lillo, F., Bouchaud, J.-P. (2019). Crossover from Linear to Square-Root Market Impact. PRL 122. https://link.aps.org/doi/10.1103/PhysRevLett.122.108302
- Bun, J., Bouchaud, J.-P., Potters, M. (2017). Cleaning Large Correlation Matrices: Tools from RMT. Physics Reports 666. https://ui.adsabs.harvard.edu/abs/2017PhR...666....1B/abstract
- Chen, H.-L., Jegadeesh, N., Wermers, R. (2000). The Value of Active Mutual Fund Management. JFQA 35(3). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=224417
- Chen, L., Pelger, M., Zhu, J. (2024). Deep Learning in Asset Pricing. Mgmt Sci 70(2). https://pubsonline.informs.org/doi/10.1287/mnsc.2023.4695
- Chen, A., Velikov, M. (2023). Zeroing In on the Expected Returns of Anomalies. JFQA. https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/zeroing-in-on-the-expected-returns-of-anomalies/945133D5A3ECEEAF466AEE91551FD225
- Chen, A., Zimmermann, T. (2022). Open Source Cross-Sectional Asset Pricing. CFR 11(2). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3875093
- Chordia, T., Goyal, A., Saretto, A. (2020). Anomalies and False Rejections. RFS 33(5). https://academic.oup.com/rfs/article-abstract/33/5/2134/5739455
- Chordia, T., Subrahmanyam, A., Tong, Q. (2014). Have Capital Market Anomalies Attenuated? JAE 58(1). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2029057
- Claeskens, G., Magnus, J., Vasnev, A., Wang, W. (2016). The Forecast Combination Puzzle: A Simple Theoretical Explanation. IJF 32(3). https://www.sciencedirect.com/science/article/abs/pii/S0169207016000327
- Clarke, R., de Silva, H., Thorley, S. (2002). Portfolio Constraints and the Fundamental Law of Active Management. FAJ 58(5). https://www.tandfonline.com/doi/abs/10.2469/faj.v58.n5.2468
- Cohen, L., Malloy, C., Pomorski, L. (2012). Decoding Inside Information. JF 67(3). https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2012.01740.x
- Cohen, R., Polk, C., Silli, B. (2010). Best Ideas. WP (LSE). https://eprints.lse.ac.uk/24471/1/Best%20ideas(published).pdf
- Collin-Dufresne, P., Daniel, K., Saglam, M. (2020). Liquidity Regimes and Optimal Dynamic Asset Allocation. JFE 136(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X19302302
- Connor, G., Korajczyk, R. (1988). Risk and Return in an Equilibrium APT (asymptotic PCA). JFE 21(2). https://www.sciencedirect.com/science/article/abs/pii/0304405X88900621
- Constantinides, G. (1986). Capital Market Equilibrium with Transaction Costs. JPE 94(4). https://doi.org/10.1086/261386
- Cremers, M., Weinbaum, D. (2010). Deviations from Put-Call Parity and Stock Return Predictability. JFQA 45(2). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=968237
- Daniel, K., Mota, L., Rottke, S., Santos, T. (2020). The Cross-Section of Risk and Returns. RFS 33(5). https://academic.oup.com/rfs/article-abstract/33/5/1927/5803086
- de Lataillade, J., Deremble, C., Potters, M., Bouchaud, J.-P. (2012). Optimal Trading with Linear Costs. J. Investment Strategies 1(3). https://arxiv.org/abs/1203.5957
- De Nard, G., Ledoit, O., Wolf, M. (2021). Factor Models for Portfolio Selection in Large Dimensions. J. Financial Econometrics 19(2). https://academic.oup.com/jfec/article-abstract/19/2/236/5285483
- DeMiguel, V., Garlappi, L., Uppal, R. (2009). Optimal Versus Naive Diversification. RFS 22(5). https://academic.oup.com/rfs/article-abstract/22/5/1915/1592901
- DeMiguel, V., Martin-Utrera, A., Nogales, F., Uppal, R. (2020). A Transaction-Cost Perspective on the Multitude of Firm Characteristics. RFS 33(5). https://academic.oup.com/rfs/article-abstract/33/5/2180/5821387
- Drechsler, I., Drechsler, Q.F. (2014, rev. 2016). The Shorting Premium and Asset Pricing Anomalies. NBER w20282. https://www.nber.org/papers/w20282
- Edelen, R., Ince, O., Kadlec, G. (2016). Institutional Investors and Stock Return Anomalies. JFE 119. https://www.sciencedirect.com/science/article/abs/pii/S0304405X16000039
- Ehsani, S., Harvey, C., Li, F. (2023). Is Sector Neutrality in Factor Investing a Mistake? FAJ 79(3). https://www.tandfonline.com/doi/abs/10.1080/0015198X.2023.2196931
- Engle, R., Ledoit, O., Wolf, M. (2019). Large Dynamic Covariance Matrices (DCC-NL). JBES 37(2). https://www.tandfonline.com/doi/abs/10.1080/07350015.2017.1345683
- Falck, A., Rej, A., Thesmar, D. (2022). When Systematic Strategies Decay. Quantitative Finance 22. https://arxiv.org/abs/2105.01380
- Fan, J., Liao, Y., Mincheva, M. (2013). Large Covariance Estimation by Thresholding Principal Orthogonal Complements (POET). JRSS-B 75(4). https://arxiv.org/abs/1201.0175
- Frazzini, A., Israel, R., Moskowitz, T. (2015). Trading Costs of Asset Pricing Anomalies. WP. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2294498
- Frazzini, A., Israel, R., Moskowitz, T. (2018). Trading Costs. WP. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3229719
- Garleanu, N., Pedersen, L.H. (2013). Dynamic Trading with Predictable Returns and Transaction Costs. JF 68(6). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12080 (WP text: https://nbgarleanu.github.io/DynTrad.pdf)
- Green, J., Hand, J., Zhang, X.F. (2017). The Characteristics that Provide Independent Information about Average U.S. Monthly Stock Returns. RFS 30(12). https://academic.oup.com/rfs/article-abstract/30/12/4389/3091648
- Grinold, R., Kahn, R. (2000). Active Portfolio Management, 2nd ed. McGraw-Hill (book). https://www.mheducation.com/highered/mhp/product/active-portfolio-management-quantitative-approach-producing-superior-returns-selecting-superior-returns-controlling-risk.html
- Gu, S., Kelly, B., Xiu, D. (2020). Empirical Asset Pricing via Machine Learning. RFS 33(5). https://academic.oup.com/rfs/article/33/5/2223/5758276
- Harvey, C., Liu, Y. (2015). Backtesting. JPM 42(1). https://papers.ssrn.com/abstract=2345489
- Harvey, C., Liu, Y. (2018). Detecting Repeatable Performance. RFS 31(7). https://academic.oup.com/rfs/article-abstract/31/7/2499/4841739
- Harvey, C., Liu, Y. (2020). False (and Missed) Discoveries in Financial Economics. JF 75(5). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12951
- Harvey, C., Liu, Y., Zhu, H. (2016). ...and the Cross-Section of Expected Returns. RFS 29(1). https://academic.oup.com/rfs/article/29/1/5/1843824
- Hou, K., Mo, H., Xue, C., Zhang, L. (2021). An Augmented q-Factor Model with Expected Growth. RoF 25(1). https://academic.oup.com/rof/article-abstract/25/1/1/5727769
- Jacobs, H., Müller, S. (2020). Anomalies across the Globe: Once Public, No Longer Existent? JFE 135(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X19301618
- Janeček, K., Shreve, S. (2004). Asymptotic Analysis for Optimal Investment and Consumption with Transaction Costs. Finance & Stochastics 8. https://link.springer.com/article/10.1007/s00780-003-0113-4
- Jensen, T.I., Kelly, B., Malamud, S., Pedersen, L.H. (2026). Machine Learning and the Implementable Efficient Frontier. RFS. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4187217
- Jensen, T.I., Kelly, B., Pedersen, L.H. (2023). Is There a Replication Crisis in Finance? JF 78(5). https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13249
- Johnson, T., So, E. (2018). Time Will Tell: Information in the Timing of Scheduled Earnings News. JFQA 53(6). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2480662
- Kakushadze, Z. (2016). 101 Formulaic Alphas. Wilmott 2016(84). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2701346
- Kakushadze, Z., Tulchinsky, I. (2016). Performance v. Turnover: A Story by 4,000 Alphas. J. Investment Strategies 5(2). https://arxiv.org/abs/1509.08110
- Kakushadze, Z., Yu, W. (2017). How to Combine a Billion Alphas. J. Asset Management 18. https://link.springer.com/article/10.1057/s41260-016-0004-9
- Kaminski, K., Lo, A. (2014). When Do Stop-Loss Rules Stop Losses? J. Financial Markets 18. https://www.sciencedirect.com/science/article/abs/pii/S138641811300030X
- Kan, R., Zhou, G. (2007). Optimal Portfolio Choice with Parameter Uncertainty. JFQA 42(3). https://www-2.rotman.utoronto.ca/~kan/papers/erisk8.pdf
- Kelly, B., Malamud, S., Zhou, K. (2024). The Virtue of Complexity in Return Prediction. JF 79(1). https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13298
- Kirby, C., Ostdiek, B. (2012). It's All in the Timing: Simple Active Portfolio Strategies that Outperform Naive Diversification. JFQA 47(2). https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/abs/its-all-in-the-timing-simple-active-portfolio-strategies-that-outperform-naive-diversification/05D18E04B226C6E9B8441482CC92F943
- Kyle, A., Obizhaeva, A. (2016). Market Microstructure Invariance: Empirical Hypotheses. Econometrica 84(4). https://onlinelibrary.wiley.com/doi/abs/10.3982/ECTA10486 (PDF with supplement: https://pages.nes.ru/aobizhaeva/Kyle-Obizhaeva-ECTA-2016-Invariance-with-Supplement.pdf)
- Landier, A., Simon, G., Thesmar, D. (2015). The Capacity of Trading Strategies. WP (later with Bonelli). https://ssrn.com/abstract=2585399
- Ledoit, O., Wolf, M. (2004). Honey, I Shrunk the Sample Covariance Matrix. JPM 30(4). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=433840
- Ledoit, O., Wolf, M. (2017). Nonlinear Shrinkage of the Covariance Matrix for Portfolio Selection: Markowitz Meets Goldilocks. RFS 30(12). https://academic.oup.com/rfs/article-abstract/30/12/4349/3863121
- Ledoit, O., Wolf, M. (2020). Analytical Nonlinear Shrinkage of Large-Dimensional Covariance Matrices. Annals of Statistics 48(5). https://projecteuclid.org/journals/annals-of-statistics/volume-48/issue-5/Analytical-nonlinear-shrinkage-of-large-dimensional-covariance-matrices/10.1214/19-AOS1921.full
- Ledoit, O., Wolf, M. (2022). Quadratic Shrinkage for Large Covariance Matrices. Bernoulli 28(3). https://projecteuclid.org/journals/bernoulli/volume-28/issue-3/Quadratic-shrinkage-for-large-covariance-matrices/10.3150/20-BEJ1315.full
- López de Prado, M. (2018). Advances in Financial Machine Learning (CPCV). Wiley (book). https://philpapers.org/rec/LPEAIF
- López de Prado, M., Lewis, M. (2019). Detection of False Investment Strategies Using Unsupervised Learning Methods. Quantitative Finance 19(9). https://www.tandfonline.com/doi/abs/10.1080/14697688.2019.1622311
- Lou, D., Polk, C. (2022). Comomentum: Inferring Arbitrage Activity from Return Correlations. RFS 35(7). https://academic.oup.com/rfs/article-abstract/35/7/3272/6412574
- McLean, R.D., Pontiff, J. (2016). Does Academic Research Destroy Stock Return Predictability? JF 71(1). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12365
- Menchero, J., Orr, D.J., Wang, J. (2011). The Barra US Equity Model (USE4) Methodology Notes. MSCI (practitioner). https://www.top1000funds.com/wp-content/uploads/2011/09/USE4_Methodology_Notes_August_2011.pdf
- Menchero, J., Wang, J., Orr, D.J. (2012). Improving Risk Forecasts for Optimized Portfolios. FAJ 68(3). https://www.tandfonline.com/doi/abs/10.2469/faj.v68.n3.5
- Muravyev, D., Pearson, N., Pollet, J. (2025a). Anomalies and Their Short-Sale Costs. JF 80(6). https://onlinelibrary.wiley.com/doi/10.1111/jofi.13501
- Muravyev, D., Pearson, N., Pollet, J. (2025b). Why Does Options Market Information Predict Stock Returns? JFE. https://www.sciencedirect.com/science/article/pii/S0304405X25001618
- Nagel, S. (2025). Seemingly Virtuous Complexity in Return Prediction. NBER w34104. https://www.nber.org/papers/w34104
- Novy-Marx, R. (2016). Backtesting Strategies Based on Multiple Signals. NBER w21329. https://www.nber.org/papers/w21329
- Novy-Marx, R., Velikov, M. (2016). A Taxonomy of Anomalies and Their Trading Costs. RFS 29(1). https://academic.oup.com/rfs/article-abstract/29/1/104/1844518
- Novy-Marx, R., Velikov, M. (2023). Assaying Anomalies. WP. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4338007
- Pastor, L., Stambaugh, R., Taylor, L. (2015). Scale and Skill in Active Management. JFE 116(1). https://www.nber.org/papers/w19891
- Penasse, J. (2022). Understanding Alpha Decay. Mgmt Sci 68(5). https://pubsonline.informs.org/doi/10.1287/mnsc.2022.4353
- Perold, A. (1988). The Implementation Shortfall: Paper versus Reality. JPM 14(3). https://www.pm-research.com/content/iijpormgmt/14/3/4
- Philips, T., Yashchin, E., Stein, D. (2003). Using Statistical Process Control to Monitor Active Managers. JPM 30(1). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=371121
- Qian, E., Hua, R. (2004). Active Risk and Information Ratio. J. Investment Management 2(3). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=569281
- Qian, E., Hua, R., Sorensen, E. (2007). Quantitative Equity Portfolio Management. Chapman & Hall/CRC (book). https://www.routledge.com/Quantitative-Equity-Portfolio-Management-Modern-Techniques-and-Applications/Qian-Hua-Sorensen/p/book/9781584885580
- Ratcliffe, R., Miranda, P., Ang, A. (2017). Capacity of Smart Beta Strategies: A Transaction Cost Perspective. J. Index Investing 8(3). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2861324
- Rej, A., Seager, P., Bouchaud, J.-P. (2018). You Are in a Drawdown. When Should You Start Worrying? Wilmott 93. https://arxiv.org/abs/1707.01457
- Said, E. (2022). Market Impact: Empirical Evidence, Theory and Practice. arXiv (survey). https://arxiv.org/abs/2205.07385
- Savor, P., Wilson, M. (2016). Earnings Announcements and Systematic Risk. JF 71(1). https://onlinelibrary.wiley.com/doi/10.1111/jofi.12361
- Toth, B. et al. (2011). Anomalous Price Impact and the Critical Nature of Liquidity. Phys. Rev. X 1. https://journals.aps.org/prx/abstract/10.1103/PhysRevX.1.021006
- Tu, J., Zhou, G. (2011). Markowitz Meets Talmud. JFE 99(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X10001893
- Tulchinsky, I. et al. (2019). Finding Alphas, 2nd ed. Wiley (book). https://onlinelibrary.wiley.com/doi/book/10.1002/9781119571278
- WorldQuant BRAIN submission criteria (practitioner, third-party): https://github.com/QuantML-Research/wq-alpha-research/blob/main/SKILL.md ; metric definitions (turnover, fitness, margin): https://github.com/alexisdpc/WorldQuant-alpha-trading/blob/main/ImprovingAlphas.md
- Xing, Y., Zhang, X., Zhao, R. (2010). What Does the Individual Option Volatility Smirk Tell Us About Future Equity Returns? JFQA 45(3). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1107464
- Zarinelli, E., Treccani, M., Farmer, J.D., Lillo, F. (2015). Beyond the Square Root: Logarithmic Dependence of Market Impact on Size and Participation Rate. Market Microstructure & Liquidity 1(2). https://arxiv.org/abs/1412.2152

*Checked this session by search or fetch: all entries except the following, carried from v6-literature §8 with their URLs: Constantinides 1986, Chen-Velikov 2023, Chordia-Subrahmanyam-Tong 2014, Edelen-Ince-Kadlec 2016, Ehsani-Harvey-Li 2023, Falck-Rej-Thesmar 2022, Ledoit-Wolf 2004, McLean-Pontiff 2016, Barber et al. 2013, Harvey-Liu-Zhu 2016, Kan-Zhou 2007, DeMiguel et al. 2009 / 2020, Novy-Marx-Velikov 2016, Avramov et al. 2023, Asness-Frazzini-Pedersen 2019. Numbers quoted from FIM 2018, FIM 2015, Kyle-Obizhaeva 2016, USE4 and Garleanu-Pedersen were read in the full texts.*

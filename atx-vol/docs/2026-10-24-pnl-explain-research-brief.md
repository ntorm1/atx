# Research brief: high-throughput P&L explain for American equity option books

Prepared 2026-10-24 from external sources only. Tags like [S7] point to the Sources list. **UNVERIFIED** means not confirmed against primary text in this session. **(inference)** marks my own reasoning, which no cited source states.

## 1. Attribution methodologies

**(a) Taylor / risk-based.** The explain line is P&L ≈ Θ·dt + Δ·dS + ½Γ·dS² + V·dσ + Vanna·dS·dσ + ½Volga·dσ² + ρ·dr + φ·dq, and the residual is actual P&L minus that sum. Green calls this "risk-based explain"; Andersen–Piterbarg call it "second-order P&L predict" [S6].

SpiderRock's production risk cube is a concrete template [S7]:
- Delta P&L = start Δ × dS ("BS delta; NOT hedgeDe").
- Gamma = ½·avg(Γ_start, Γ_end)·dS², and volga is averaged the same way.
- Vega uses the vol move "includes spot-induced vol change". A separate vol-slope term (`opnPnlSl`, "overlaps with opnPnlVe") shows what a sticky-delta hedge would have added.
- Rho, φ (dividend yield), discrete dividend (−Δ·dDDiv) and vanna are separate lines.
- Theta = `decomposedTheta(dYearsV, dYearsC)`. My reading is that these are vol-time and calendar-time year fractions; the docs do not define them.
- Residual = `opnPnlErr`.

Daviaud gives the continuous-time version (Theta+Gamma, Vega, Vanna, Volga integrals). He also notes that summing daily Greek explains "loses the memory" of the trade-inception vol over long horizons [S8]. Carr–Wu build option pricing on the same local attribution [S9].

**(b) Sequential revaluation ("step-wise", "bump and do not reset", "waterfall").** Effects are applied one at a time and stay in place, so N steps cost N+1 valuations. The result is exact by construction, but "cross-effects are introduced, which cannot be allocated properly, as well as an order dependence" [S6].

- BNP/KTH example: for an ATM option pushed OTM by a rate move, running the vega step before the rate step gives vega a "quite significant" P&L. Run after the rate step, vega's share is much smaller [S10].
- "Star" mode (bump-and-reset from D-1) has no order dependence but leaves unexplained P&L even when every factor is covered. Monte Carlo pricing noise pollutes step differences; PDE error does not, because it does not depend on pricing context [S10].
- Whether time goes first or last is "often the biggest methodological decision" [S11].
- A common order is lifecycle/new trades → time → rates/dividends → spot → vol. No public vendor text confirms it as the standard (**UNVERIFIED**).
- The order-free option is to average all d! orders, which is the Shapley value. Flaig & Junike find that one-at-a-time leaves significant unexplained P&L and that the sequential result depends significantly on order; they recommend the order-averaged version [S12].
- In continuous time, the preferred decomposition (exact, symmetric, normalized) is a limit of recursive Shapley values and is consistent across reporting grids [S13].

**(c) Hybrid.** One bank-style PAA standard [S14]:
- Allows either Greek-based explain or full-revaluation sequential perturbation.
- Splits unexplained P&L into a "sensitivity approach" gap (full reval − Taylor) and a "risk factor selection" gap.
- Triggers review at 10%/$50K of P&L, and a methodology review if unexplained P&L exceeds 10% of total.

Product-control norms are below 1% (or below $5k) for vanilla books and roughly 5% for non-linear books [S15]. Chebyshev-tensor proxies were the only method to stay accurate through 2012's large moves in one backtest against 1st- and 2nd-order Taylor [S16].

**Failure modes (analysis).**
- Gamma error grows like dS³ (illustrative blog numbers: 1.3% error at a 2% move vs 144.7% at a 20% move [S17]).
- Smile dynamics split the move differently between delta and vega (§2).
- An American option inside the exercise region has Δ=−1 and Γ=0, so a Taylor expansion across the boundary is wrong.
- Ex-dividend steps on calls.
- Pin risk: Γ grows like τ^-½ near expiry.

*Design implications*
- Treat full reval of start-of-step positions as truth. Report the Taylor residual (reval − Taylor) separately from unexplained P&L (actual − reval) [S14].
- Use the trapezoid rule (start/end average) for Γ, Volga and Vanna [S7]. It costs nothing if end-of-step Greeks are computed anyway as the next step's base.
- Ship a documented fixed-order waterfall plus an exact Shapley mode over 4 groups (time, carry, spot, vol). That needs 2⁴ = 16 revals per option (inference).
- Auto-switch to reval for positions near the boundary, near expiry, or across an ex-date.

## 2. Vol-move conventions

**Smile dynamics rules.**
- Sticky strike: σ(K) is fixed as S moves, so Δ equals the BS delta.
- Sticky moneyness: σ = f(K/S), so Δ is above the BS delta under negative skew.
- Sticky implied tree: Δ is below the BS delta [S18].
- Bergomi's skew-stickiness ratio: R=1 for sticky strike, R=0 for sticky delta, R≈2 for local vol with weak skew [S19]. Empirical index SSR is about 1.5 [S20].
- Coremont tested a "sticky surface" rule: shape held constant in z = ln(K/F)/(σ_ATM√T), with the ATM level moving along the skew. It performed best from 1w to 2y on almost all indices, with sticky strike a close second and sticky delta poor outside APAC [S21].
- SpiderRock's adjusted delta is Δ + VegaSlope·Vega, where VegaSlope = dVol/dS [S22]. Its cube records fixed-strike IV change (`iVolMove`) and the explain uses BS delta, which amounts to sticky-strike explain [S7].

**Level / skew / curvature.** Cont & da Fonseca find 2–3 Karhunen–Loève factors (level, skew, curvature) explain most daily IV-surface variance [S23]. That is the empirical basis for parallel/twist/butterfly vega. SpiderRock defines slope as vol(x=−0.5) − vol(x=+0.5) in standardized log-moneyness and exposes skew buckets (`skewDD/DN/AT/UP/DU`), with no curvature field [S22][S7]. I found no public methodology for Bloomberg MARS, Murex or Numerix (**UNVERIFIED**). ORE added "P&L and P&L explain" analytics and AAD in its May 2024 release, but I did not read the methodology [S24].

**Theta vs carry.** Basel says time effects "include the theta effect and carry or funding costs" and must be treated consistently in HPL and RTPL (MAR32.27, fn 2) [S1]. Product control books coupon accrual, theta and financing spread under carry [S15]. For dividends, SpiderRock keeps a yield line (φ·dSDiv), a discrete-dividend line and cash dividend P&L (`opnDivPnL`) separate [S7].

*Design implications*
- Make the dynamics rule a parameter: sticky strike (default), sticky moneyness, sticky surface. Always report the skew-delta overlap term.
- Define the vol move as the fixed-strike ΔIV. Project it per expiry onto the surface's own ATM / slope / curvature parameters and keep a "shape residual".
- Split theta into diffusive decay and earnings-variance release (σ²T = σE²·n + σC²·T [S22]).
- Share legs: qty·dS, cash dividend on ex-date (netted against the price drop), and financing/borrow accrual as carry.

## 3. FRTB P&L attribution test (MAR32, in force 1 Jan 2023)

- HPL revalues prior end-of-day positions with today's market data and excludes intraday trades and new deals (32.25). RTPL comes from the risk model's valuation engine (32.22) [S1].
- Window: the most recent 250 trading days (32.35).
- Metrics: Spearman rank correlation (32.36–38), and a KS statistic equal to the maximum absolute gap between the empirical CDFs, with each CDF step 0.004 = 1/250 (32.39–41).
- **Green:** Spearman > 0.80 **and** KS < 0.09 (p = 0.264). **Red:** Spearman < 0.70 **or** KS > 0.12 (p = 0.055). **Amber:** everything else (32.42, Table 2) [S1]. MathWorks uses the same cutoffs [S3].
- Amber: capital surcharge k·max(0, SA − IMA) over green and amber desks (MAR33.43/45). The value k = 0.5 is **UNVERIFIED** because the formula is published only as an image [S2].
- Red: the desk loses the internal models approach (IMA) and moves to the standardized approach [S4].
- RTPL inputs may be aligned to HPL only if documented. HPL may never be aligned to RTPL, and noise adjustments are banned (32.30–33) [S1].
- Well-hedged desks fail more easily because the P&L signal is small relative to the errors [S4].
- The EU RTS 2022/2059 Arts. 7–9 implement the same metrics, amended by 2025/878. I did not check its thresholds (**UNVERIFIED**) [S5].

*Design implications*
- Keep per-day RTPL (Taylor) and HPL (reval) series per underlier/book, and compute rolling 250-obs Spearman + KS as built-in explain QA.
- Implement KS exactly: an ECDF over 250 points with 0.004 steps.
- Running PLA on 30-min increments is a useful internal diagnostic, not a regulatory number. Watch for intraday seasonality (inference).

## 4. Constant-maturity, constant-moneyness series

- **Cboe VIX:** 30-day variance is a linear interpolation in total variance T·σ² between two bracketing expiries, with time counted in minutes (N30 = 43,200, N365 = 525,600) [S25]. It uses expiries between 23 and 37 days [S26].
- **OptionMetrics IvyDB v5.0** [S27]:
  - Grid: 10, 30, 60, 91, 122, 152, 182, 273, 365, 547 and 730 calendar days; deltas 0.10–0.90 in 0.05 steps (negative for puts).
  - Method: vega-weighted Gaussian kernel over log(days), call-equivalent delta (Δ for calls, 1+Δ for puts) and a call/put flag. Bandwidths h1 = 0.05, h2 = 0.005, h3 = 0.001; options with vega < 0.5 are dropped.
  - American options are priced with a CRR binomial. Standardized ATM-forward vols are interpolated linearly from the surface.
- **SpiderRock** [S22][S28][S29]:
  - Fixed grid at 5, 10, 21, 42, 63, 84, 105, 126, 189, 252, 378 and 504 days, with delta columns `volD45…volA00…volU45`. The schema labels suggest 5-delta steps but the narrative doc says 10-delta steps, so the docs conflict.
  - Vols are "censored" (earnings removed). The axis is ln(K/F)/(σ_ATM√T), and strike vols are stored as multiples of ATM vol.
  - The interpolation method across tenors is not documented (**UNVERIFIED**).
- **Delta conventions:** FX distinguishes spot, forward and premium-adjusted delta [S30]. Black-76 forward delta inverts to K = F·exp(½σ²T − σ√T·N⁻¹(Δ)), solved by fixed point because σ = σ(K). With American model deltas, 1+Δ_put ≠ Δ_call, so the call-equivalent mapping is asymmetric (inference).
- **Interpolation:** There is no calendar arbitrage iff ∂_t w(k,t) ≥ 0 at fixed k = ln(K/F_t), assuming proportional dividends. Lemma 5.1: choose a monotone ATM total variance θ_t, then blend C/K linearly at constant k with weight α_t = (√θ₂ − √θ_t)/(√θ₂ − √θ₁). This guarantees no static arbitrage [S31]. The shape-preserving alternative holds the vol-multiple shape fixed in standardized moneyness [S21][S22].

*Design implications*
- Track (τ*, x*), with x* = forward log-moneyness or Black-76 forward delta. Each step, price a fixed synthetic contract, then re-strike. Never book the re-strike jump as P&L.
- Interpolate in total variance at constant k with a monotone ATM θ. Count intraday time in minutes, as VIX does.
- Store both Black-76 forward delta (for strike selection) and the American model delta (for hedging).
- Interpolate censored vol and add event variance by earnings count.

## 5. High-throughput architecture

**Published timings.**
- Andersen–Lake–Offengenden: about 100,000 American prices/s per CPU at accuracy comparable to an FD grid with several hundred steps, and 10–11 digits in under 0.1 s (abstract, via secondary source) [S32].
- Healy, Andersen–Lake (m=5, n=4, l=11, p=21): RMSE 4.1e-5 at 39,040 options/s, or 179,705/s in batch mode, where spots share one exercise boundary.
- Healy, (m=7, n=8, l=15, p=31): RMSE 4.9e-6 at 12,507/s, or 70,881/s batched.
- Healy, TR-BDF2 (m=20): 4,708/s. Healy does not state hardware [S33].
- QuantLib's QdFp engine "fast" set is (l,n,m,p) = (7,2,7,27). Its timings are given only in figures (**UNVERIFIED**) [S34].
- GPU: a dated GTX 295 binomial run (512 steps) peaked at 24,852 options/s [S39]. STAC-A2 Monte Carlo basket Greeks took 0.77 s on 2×K20Xm, which is not comparable per contract [S40].
- I found no modern published per-contract SIMD/GPU timing for American Greeks (**UNVERIFIED**).

**Greeks method.**
- AD cost relative to pricing: tangent mode ≤ ω_T·n inputs with ω_T ∈ [2, 2.5]; adjoint ≤ ω_A·m outputs with ω_A ∈ [3, 4] [S36]. Correlation Greeks cost ≤ 4× [S37].
- Savine puts all sensitivities at about 5× one valuation [S38].
- tf-quant-finance vectorizes Andersen–Lake over `[num_options]` [S35].
- SoA vs AoS: a gap under 1% on modern Intel (secondary, **UNVERIFIED**) [S41].

*Design implications*
- **Budget (inference):** 10k options × 13 snapshots per day. At 40k–180k options/s/core, one price pass takes 0.06–0.25 s per core, and about 8 bump reprices stay under ~2 s per core per snapshot. Bump-and-reprice is affordable, and AAD is an optimization rather than a necessity.
- The exercise boundary does not depend on S [S33]. Spot bumps and spot ladders reuse one boundary, which gave about 4.6× throughput in Healy's batch mode. (S,K)-homogeneity may let normalized boundaries be shared per (σ, r, q, τ) (inference; verify).
- Use an SoA layout grouped by underlier→expiry, so forward, r, q, τ and smile parameters broadcast across vectorized strikes.
- **Streaming explain:** compute Greeks once per snapshot; the end-of-step Greeks become the next step's base; second-order terms use the trapezoid average [S7]. The day's explain is the sum of the 30-min steps, which approximates the grid-consistent continuous-time decomposition [S13] (inference).
- Warm-start the boundary iteration from the previous snapshot. A good initial guess cuts iterations [S33]; carrying it across snapshots is inference.

## Sources
- [S1] https://www.bis.org/basel_framework/chapter/MAR/32.htm?inforce=20230101&published=20200327
- [S2] https://www.bis.org/basel_framework/chapter/MAR/33.htm?inforce=20230101&published=20200605
- [S3] https://www.mathworks.com/help/risk/profit-and-loss-attribution-workflow.html
- [S4] https://zandersgroup.com/en/insights/blog/frtb-profit-and-loss-attribution-pla-analytics ; https://zandersgroup.com/en/insights/blog/to-hedge-or-not-to-hedge-navigating-the-catch-22-of-frtbs-pla-test
- [S5] https://eur-lex.europa.eu/eli/reg_del/2022/2059/oj
- [S6] https://arxiv.org/html/2005.10504
- [S7] https://docs.spiderrockconnect.com/docs/next/MessageSchemas/Schema/Topics/risk-v5/RiskCubeDetailV5/
- [S8] https://www.risk.net/investing/7959307/rethinking-pl-attribution-for-options
- [S9] https://engineering.nyu.edu/sites/default/files/2020-09/option-profit-carr-jofi-12894.pdf
- [S10] https://www.diva-portal.org/smash/get/diva2:1477940/FULLTEXT02
- [S11] https://www.quantinsider.io/blogs/pnl-explain-vs-pnl-predict-why-this-distinction-actually-matters
- [S12] https://arxiv.org/abs/2309.07667
- [S13] https://arxiv.org/abs/2212.06733
- [S14] https://cfrm17.github.io/PnLExplain.pdf
- [S15] https://ibproductcontrolacademy.com/knowledge/pnl-explain.html
- [S16] https://arxiv.org/pdf/1911.10948
- [S17] https://riskhub.org/blogs/greeks-based-portfolio-risk-decomposition
- [S18] https://emanuelderman.com/wp-content/uploads/2013/09/smile-lecture9.pdf
- [S19] https://www.risk.net/derivatives/equity-derivatives/1564129/smile-dynamics-iv
- [S20] https://arxiv.org/html/2602.05241v1
- [S21] https://www.coremont.com/wp-content/uploads/2022/05/Coremont-Equity-Implied-Volatility-Dynamics-September-2020.pdf
- [S22] https://docs.spiderrockconnect.com/docs/next/Documentation/PlatformFeatures/Analytics/LiveVolSurfaces/
- [S23] https://ideas.repec.org/a/taf/quantf/v2y2002i1p45-60.html
- [S24] https://www.lseg.com/en/media-centre/press-releases/acadia/2024/acadia-12th-update-open-source-risk-engine
- [S25] https://www.kellogg.northwestern.edu/faculty/mcdonald/ftp/d67/vix_cboe_wp.pdf
- [S26] https://www.sec.gov/file/rel-no-34-75120
- [S27] https://lib.tsinghua.edu.cn/PDF/OptionMetricsshiyongzhinan.pdf
- [S28] https://docs.spiderrockconnect.com/docs/MessageSchemas/Schema/SRSE%20Products/SRAnalytics/LiveSurfaceFixedGrid/
- [S29] https://docs.spiderrockconnect.com/docs/MessageSchemas/Schema/SRSE%20Products/SRAnalytics/LiveSurfaceFixedTerm/
- [S30] https://econpapers.repec.org/RePEc:zbw:cpqfwp:20
- [S31] https://arxiv.org/pdf/1204.0646
- [S32] https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027
- [S33] https://arxiv.org/html/2109.15157v1
- [S34] https://hpcquantlib.wordpress.com/2022/10/09/high-performance-american-option-pricing/
- [S35] https://tf-quant-finance.readthedocs.io/en/latest/experimental/american_option_pricing/andersen_lake/andersen_lake.html
- [S36] https://people.maths.ox.ac.uk/gilesm/files/AAD_Review.pdf
- [S37] https://arxiv.org/pdf/1004.1855
- [S38] https://turtlmedia.informa.com/story/quantminds-emag-2019-q1/page/5
- [S39] https://www.researchgate.net/publication/290969012_GPU_implementation_of_binomial_tree_model_for_option_pricing
- [S40] https://www.hpcwire.com/aiwire/2014/06/04/fat-xeons-best-accelerated-systems-option-pricing/
- [S41] https://arxiv.org/abs/2204.13740

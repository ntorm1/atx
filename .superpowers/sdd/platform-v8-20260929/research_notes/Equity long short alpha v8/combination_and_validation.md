# Alpha combination and statistical validation for a multi-signal US equity L/S book (v8, state of knowledge 2026-09)

Scope note. These notes extend `literature-v7.md` S1/S5 and `v6-literature.md` section 5 and do not repeat them. Items already covered there (DeMiguel-Garlappi-Uppal, Kan-Zhou, Tu-Zhou, Kirby-Ostdiek, Gu-Kelly-Xiu, Chen-Pelger-Zhu, Kelly-Malamud-Zhou, JKMP, Kakushadze-Yu, Novy-Marx 2016, DSR/PBO/CSCV, ONC) are referenced only where a new number or a contradiction is added. Everything tagged **[est]** is my own arithmetic (daily data, 252 sessions/year, normal quantiles) and is an estimate, not a cited result. The web search budget for the session ran out before a few primary sources could be checked; those are listed under Gaps rather than asserted.

---

## 1. History extension as the main statistical lever (T = 3y -> T = 11y)

### Takeaway
Going from 3 to 11 years cuts every Sharpe-type standard error by a factor sqrt(3/11) = 0.52: SE(SR) falls from about 0.58-0.62 to 0.30-0.32, and SE of a paired Sharpe difference at rho = .98 falls from about 0.12 to 0.06 **[est]**. That is enough to make the level of the book credible (the DSR gate becomes reachable at SR 1.0-1.4) but it is still not enough to accept single +0.08 increments one at a time; those need about 17-25 years at rho = .98, so improvements must be tested as bundles or at the signal/IC level **[est]**.

### Cited Findings
- Under IID returns the Sharpe estimator is asymptotically normal with variance (1 + SR^2/2)/T in per-period units; monthly Sharpe ratios cannot be annualised by sqrt(12) except in special cases, and serial correlation can overstate annual hedge-fund Sharpe ratios by as much as 65% — [Lo 2002, FAJ 58(4)](https://www.tandfonline.com/doi/abs/10.2469/faj.v58.n4.2453)
- Non-normal generalisation used in PSR / MinTRL: Var(SR) = [1 - g3*SR + (g4 - 1)/4 * SR^2]/(T - 1), and MinTRL = 1 + [1 - g3*SR + (g4 - 1)/4 * SR^2] * (z_alpha/(SR - SR*))^2, SR in per-period units — [Bailey-Lopez de Prado 2012](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1821643)
- Deflated Sharpe ratio: PSR evaluated against SR0 = sqrt(V[SR_trials]) * ((1 - gamma) * Phi^-1(1 - 1/N) + gamma * Phi^-1(1 - 1/(N e))) — [Bailey-Lopez de Prado 2014](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551)
- The Jobson-Korkie (1981) test of equal Sharpe ratios, corrected by Memmel (2003), is the most used test; its covariance matrix assumes IID bivariate normal returns — [Memmel 2003](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=412588); [Ledoit-Wolf 2008](https://www.econ.uzh.ch/dam/jcr:ffffffff-935a-b0d6-0000-00007214c2bc/jef_2008pdf.pdf)
- Ledoit-Wolf simulation conclusions (T = 120, 5,000 simulations, bootstrap M = 499): "JKM works well for i.i.d. bivariate normal data but is not robust against fat tails or time series effects, where it becomes liberal"; HAC inference "is often liberal in finite samples"; the studentized time-series (circular block) bootstrap "works well both for i.i.d. and time series data". They recommend always using the time-series bootstrap in practice because of volatility clustering, with block size chosen by calibration (candidate block sizes {1,2,4,6,8,10}; their applications selected b = 4 and b = 6) — [Ledoit-Wolf 2008](https://www.econ.uzh.ch/dam/jcr:ffffffff-935a-b0d6-0000-00007214c2bc/jef_2008pdf.pdf)
- Opdyke (2007) corrects Lo's and Memmel's limiting variances for general IID (non-normal) data and for stationary time series; the two-sample PSR is Phi(SR_diff / SE(SR_diff)) and the two-sample MinTRL is (V_a + V_b + V_ab) * (z_{1-alpha}/SR_diff)^2, where the V terms contain skewness, kurtosis and cross-moments of orders (1,1), (1,2), (2,1), (2,2) — [Portfolio Optimizer blog, summarising Opdyke 2007 and Bailey-Lopez de Prado 2012](https://portfoliooptimizer.io/blog/the-probabilistic-sharpe-ratio-hypothesis-testing-and-minimum-track-record-length-for-the-difference-of-sharpe-ratios/); [Ledoit-Wolf 2008, Remark 3.1](https://www.econ.uzh.ch/dam/jcr:ffffffff-935a-b0d6-0000-00007214c2bc/jef_2008pdf.pdf)
- 2025 update: Lopez de Prado, Lipton and Zoonekynd derive a closed-form approximation to the Sharpe sampling distribution under joint non-normality and serial correlation, and propose a reporting standard covering minimum sample length, power, FWER and FDR; Monte Carlo shows more reliable inference than classical t-statistics; replication code is public — [Lopez de Prado-Lipton-Zoonekynd 2025, SSRN 5520741](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=5520741); [code](https://github.com/zoonek/2025-sharpe-ratio)
- Anomaly decay evidence relevant to older data: portfolio returns are 26% lower out of sample and 58% lower post-publication — [McLean-Pontiff 2016](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12365) (URL carried from the v6/v7 bibliography)
- On 72 published factors (CRSP 1963-2014, mean in-sample Sharpe 0.98), Sharpe decays by about one half after publication; the discount ratio falls by about 0.05 per publication year (the haircut grows for more recently published factors); publication year alone explains 30% of the variance of Sharpe decay, overfitting proxies (number of operations, sensitivity to outliers) add 15% — [Falck-Rej-Thesmar 2022](https://arxiv.org/abs/2105.01380)
- Kozak-Nagel-Santosh note that their 50 anomaly portfolios "experienced significant deterioration in the latest (not data-mined) part of the sample", and they run a pure out-of-sample test estimating on pre-2005 data and evaluating 2005-2016 — [Kozak-Nagel-Santosh, NBER w24070](https://www.nber.org/system/files/working_papers/w24070/w24070.pdf)
- Short-horizon ML strategies were mainly profitable before 2004; net of costs performance is close to zero after 2004 — [Blitz-Hanauer-Hoogteijling-Howard 2023](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4474637); [Robeco summary](https://www.robeco.com/en-us/insights/2023/07/the-term-structure-of-machine-learning-alpha)
- Weighting recent vs old data: under continuous structural breaks the MSFE-optimal observation weights are exponential-smoothing weights; under discrete breaks they are a step function (constant within regime); exponential smoothing forecasts are highly sensitive to the down-weighting parameter, so the authors propose robust optimal weights when break information is uncertain — [Pesaran-Pick-Pranovich 2013, J. Econometrics 177(2)](https://www.sciencedirect.com/science/article/abs/pii/S0304407613000687)
- Protocol view on sample choice: "The training sample needs to be justified in advance. The sample should never change after the research begins"; a better out-of-sample test is "freshly uncovered historical data", with the caveat that researchers know how history unfolded — [Arnott-Harvey-Markowitz 2019](https://people.duke.edu/~charvey/Research/Published_Papers/P138_A_backtesting_protocol.pdf)

### Inferences
All numbers below are **[est]**, daily data, annualised; the non-normal case uses the v7 cell moments (skew -1.32, kurtosis 13.5) as a stand-in because the current book's moments were not supplied.

**(a) SE of the Sharpe ratio.** SE(SR_ann) = sqrt(V/Y), V = 1 - g3*SR_d + (g4-1)/4*SR_d^2, SR_d = SR_ann/sqrt(252).

| SR | Y = 3 | Y = 5 | Y = 8 | Y = 11 |
|---|---|---|---|---|
| 1.0 normal / non-normal | .578 / .604 | .448 / .468 | .354 / .370 | .302 / .316 |
| 1.2 | .578 / .610 | .448 / .473 | .354 / .374 | .302 / .319 |
| 1.405 | .578 / .617 | .448 / .478 | .354 / .378 | .302 / .322 |

t-stat of SR 1.405 vs zero: 2.4 at 3y, 4.7 at 11y (normal). With daily data the non-normality correction is small (+5-7% on SE); serial correlation and volatility clustering matter more, hence the Ledoit-Wolf bootstrap.

**(b) SE of a paired Sharpe difference.** Closed form from the JK-Memmel delta method: Var(dSR) = (1/n) * [2(1 - rho) + 0.5*(SR1^2 + SR2^2 - 2*SR1*SR2*rho^2)] in per-period units. With daily data the second term is negligible, so SE(dSR_ann) ~ sqrt(2(1 - rho)/Y).

| rho | SE at 3y | SE at 11y | t of true +.08 (3y / 11y) | years for t = 1.645 on +.08 | years for t = 2 on +.08 | years for 80% power, +.08 |
|---|---|---|---|---|---|---|
| .95 | .183 | .096 | 0.44 / 0.84 | 43 | 63 | 97 |
| .98 | .116 | .061 | 0.69 / 1.32 | 17 | 25 | 39 |
| .99 | .082 | .043 | 0.98 / 1.87 | 8.5 | 12.6 | 19.5 |
| .995 | .058 | .030 | 1.38 / 2.64 | 4.3 | 6.3 | 9.8 |

- The book's observed SE of .09-.12 on 3y is what rho = .98-.99 predicts; the formula reproduces the platform's numbers.
- Minimum detectable difference at 11y (one-sided 5%): .10 at rho = .98, .07 at rho = .99, .05 at rho = .995. At 3y: .19 / .135 / .095.
- Consequence: single +.07 to +.09 steps remain under 2 SE even at 11y unless rho >= .995. Test the cumulative bundle against the frozen baseline: for example five steps totalling +.40 with rho = .93 to baseline gives SE .216 at 3y (t 1.85) and .113 at 11y (t 3.5).
- Paired tests should use the Ledoit-Wolf studentized circular block bootstrap with a pre-registered seed, block size and replication count so that the run is byte-reproducible; report JK-Memmel beside it as the liberal bound.

**(c) MinTRL (95% one-sided).**

| SR | vs SR* = 0 (normal / non-normal) | vs SR* = 0.5 (normal / non-normal) |
|---|---|---|
| 1.0 | 2.7y / 3.0y | 10.9y / 11.9y |
| 1.2 | 1.9y / 2.1y | 5.5y / 6.2y |
| 1.4 | 1.4y / 1.6y | 3.4y / 3.8y |

Three years can reject SR = 0 for SR >= 1.0 but cannot reject SR* = .5 unless SR >= ~1.45; eleven years can reject SR* = .5 for any true SR >= 1.0.

**(d) DSR.** E[max z] over N trials: 1.58 (N = 10), 2.16 (N = 37), 2.28 (50), 2.53 (100), 2.77 (200). Backing out from the book fact (SR 1.405, 3y, N = 37, DSR .82): SR0 ~ .84-.88 and cross-trial sd of SR ~ .39-.41.

| Scenario at 11y | SR0 | DSR at SR 1.0 / 1.2 / 1.405 | SR needed for DSR >= .95 |
|---|---|---|---|
| N = 37, cross-trial variance unchanged | .84-.88 | .66-.69 / .86-.87 / .96 | 1.37 |
| N = 37, cross-trial variance scales 1/T (pure sampling noise) | .44-.46 | .96 / .99 / 1.00 | 0.95 |
| N = 100, variance unchanged | .99-1.03 | .47-.52 / .72-.75 / .90 | 1.52 |
| N = 100, variance scales 1/T | .51-.54 | .94 / .98-.99 / 1.00 | 1.03 |
| (reference) 3y, N = 37 | .84-.88 | .59-.60 / .71-.72 / .82 | 1.83-1.88 |

The truth lies between the two variance rows: part of the dispersion across the 37 trials is real design difference and part is noise. V[SR] must be re-estimated on the 11y trial series, not carried over. At 3y the .95 gate needed SR ~1.85, which the book could not reach; at 11y it needs roughly 1.0-1.4.

**(e) Feasibility of fitted weights (extends the v7 S1 table to T = 11 and to 10 themes).** Plug-in OOS SR ~ theta^2/sqrt(theta^2 + N/T).

| theta | N | plug-in OOS SR at 3y | at 11y | 1/N at f = .85 | break-even T (f = .85 / .70) |
|---|---|---|---|---|---|
| 1.0 | 10 themes | .48 | .72 | .85 | 26y / 9.6y |
| 1.5 | 10 themes | .95 | 1.27 | 1.27 | 11.6y / 4.3y |
| 1.5 | 38 sleeves | .58 | .94 | 1.27 | 44y / 16y |
| 2.0 | 10 themes | 1.48 | 1.81 | 1.70 | 6.5y / 2.4y |
| 2.0 | 38 sleeves | .98 | 1.47 | 1.70 | 25y / 9y |

At 11y, unshrunk theme-level fitting is at break-even with equal weight (theta 1.5, f .85); sleeve-level fitting still loses. The Tu-Zhou style blend weight delta = T/(T + T0) rises from ~.2 to ~.5 for themes (T0 ~ 10-12y) and from ~.06 to ~.2 for sleeves (T0 ~ 44y).

**Usable length is less than 11 years.** Prices start 2012-03, so any 252-session lookback (12-1 momentum, 1y beta/ivol) first exists about 2013-03; fundamentals from 2010 give multi-year growth/stability signals only from 2013-2015. A full-book sample of 2013-04 to 2022-12 is about 9.75y: SE(SR) ~ .32, SE(dSR, rho .98) ~ .064. A sprint plan should state the per-signal first-valid date and the date from which the full book exists.

**Risks of 2012-2019 and mitigation.**
- Regime: 2012-2019 is post-decimalisation, post-Reg NMS, post-2004 (the break date in the Robeco evidence) and after the publication date of most library signals, so it is structurally closer to live conditions than the academic samples. Expect lower gross alpha than 2020-2022, not higher.
- Pre-register the expectation: if SR(2013-2019) < SR(2020-2022) this is decay in the direction the literature predicts, not a bug.
- Recency weighting: exponential weights with a 5y half-life over 11y keep 9.3 effective years and put 43% of weight on the last 3y; a 3y half-life keeps 7.4 effective years (54% on the last 3y); equal weights put 27% on the last 3y. The half-life is a free parameter to which results are sensitive (Pesaran et al.), so fix it ex ante (suggestion: equal weights for inference, 5y half-life only for fitted quantities) and count any change as a trial.
- Survivorship and PIT: inference on the extension is only valid if the 2012-2019 universe includes delisted names and fundamentals are as-first-reported.

### Gaps
- The book's own skewness, kurtosis and autocorrelation were not provided; the non-normal columns use v7's cell moments.
- No primary source was found on how practitioners weight recent vs old data in alpha research (half-life conventions); only the econometric result of Pesaran-Pick-Pranovich.
- The full text of Lopez de Prado-Lipton-Zoonekynd 2025 could not be retrieved (SSRN returned 403); the exact serial-correlation variance formula is not reproduced here. The public code repository is the fallback.
- No source was found quantifying data-quality differences (coverage, PIT accuracy, survivorship) for US equities 2012-2019 vs 2020+; this must be measured on the platform's own data.

---

## 2. Robust combination at small T beyond equal weight

### Takeaway
The methods that are defensible at T = 3-11y are the ones that estimate only second moments or a single shrinkage intensity: correlation-cluster equal weight, inverse-volatility, prior-mean ridge MVE in the Kozak-Nagel-Santosh form, and normal-normal (empirical Bayes) shrinkage of sleeve Sharpe ratios toward theme priors. Hierarchical risk parity has no reliable out-of-sample edge over simpler risk-based rules in a recent independent test, and ML/IPCA/nonparametric methods remain research-track because their evidence comes from multi-decade panels and their net-of-cost results after 2004 are weak unless the target horizon is lengthened.

### Cited Findings
- HRP algorithm: distance d_ij = sqrt(0.5*(1 - rho_ij)), hierarchical tree clustering, quasi-diagonalisation, recursive bisection with inverse-variance split; it needs no covariance inversion. In the original Monte Carlo (10,000 iterations) out-of-sample variance was 0.1157 for CLA minimum variance, 0.0928 for inverse variance and 0.0671 for HRP — [Lopez de Prado 2016, JPM 42(4)](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2708678); numbers as reported in [Wikipedia: Hierarchical Risk Parity](https://en.wikipedia.org/wiki/Hierarchical_Risk_Parity)
- Contradicting evidence (2026): on US and Brazilian equities, June 2011-May 2025, 756-day rolling windows, monthly rebalancing, comparing HRP, HCAA, HERC, CHRP against minimum variance, risk parity, maximum diversification, maximum decorrelation, inverse volatility and equal weight: "there is no evidence that hierarchical risk clustering techniques outperform established risk-based approaches in the datasets analysed, even when alternative covariance matrix estimators are used"; hierarchical methods showed higher transaction costs — [Trucios 2026, Empirical Economics 70:58](https://link.springer.com/article/10.1007/s00181-026-02900-x)
- HRP is sensitive to the estimation window; one study finds the best out-of-sample Sharpe with five years of daily data — [Estimation Windows in HRP Methods, Springer](https://link.springer.com/chapter/10.1007/978-3-031-78241-1_5)
- Raffinot's variants (HCAA, HERC) allocate across clusters and then within; Raffinot's conclusion is quoted as "Hierarchical 1/N is very difficult to beat" — [Hudson & Thames summary of Raffinot](https://hudsonthames.org/beyond-risk-parity-the-hierarchical-equal-risk-contribution-algorithm/)
- Kozak-Nagel-Santosh: a Bayesian prior on SDF coefficients that shrinks the contributions of low-variance principal components; the estimator is an L2-penalised MVE portfolio and "maps into L2-norm constrained MVE portfolio weights obtained by DeMiguel et al. (2009)". Within their prior family, eta = 0 implies near-arbitrage in low-eigenvalue PCs; eta = 1 is Pastor-Stambaugh (level shrinkage only); they use eta = 2, which shrinks low-eigenvalue PCs more. Penalty strength is expressed as the prior root expected squared Sharpe ratio kappa and chosen by K = 3 fold cross-validation on daily data (withheld folds of about 23 years in the FF25 example) — [Kozak-Nagel-Santosh, NBER w24070](https://www.nber.org/system/files/working_papers/w24070/w24070.pdf); published [JFE 135(2) 2020](https://ideas.repec.org/a/eee/jfinec/v135y2020i2p271-292.html)
- KNS results: for 50 anomaly portfolios OOS R2 is maximised near kappa ~ 0.30; "L2-shrinkage delivers much higher OOS R2 than a pure L1-penalty Lasso-style approach"; a characteristics-sparse SDF does not exist ("to adequately capture the pricing information in the 50 anomalies one needs to include basically all of these 50 factors"), but a PC-sparse one does (4 PCs do well, 10 PCs get close to the maximum); Pastor-Stambaugh level-only shrinkage achieves OOS R2 below 5%, well short of the eta = 2 prior; t-statistics of individual SDF coefficients "are quite low" even for the best anomalies — [Kozak-Nagel-Santosh, NBER w24070](https://www.nber.org/system/files/working_papers/w24070/w24070.pdf)
- Empirical Bayes hierarchy (Jensen-Kelly-Pedersen): alpha_i = common + cluster component c_j ~ N(0, tau_c^2) + factor component w_i ~ N(0, tau_w^2); the posterior mean is a shrinkage factor times alpha_hat, the factor approaching 1 as T grows and falling as the prior tightens; "having data on many factors is helpful for estimating the alpha of any of them"; posterior variance using all factors is lower than in isolation; replication rate 84.0% under the Bayesian model vs 77.3% under Benjamini-Yekutieli; 10 of 13 themes enter the ex post tangency portfolio with significantly positive weight — [Jensen-Kelly-Pedersen, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf); published [JF 78(5) 2023](https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13249)
- Random-effects "noise-reduced alpha": pooling information from the cross-sectional alpha distribution improves out-of-sample alpha forecasts relative to unit-by-unit estimates — [Harvey-Liu 2018, RFS 31(7)](https://academic.oup.com/rfs/article-abstract/31/7/2499/4841739)
- Fama-MacBeth on long panels: with ten-year rolling FM slopes and 15 low-frequency characteristics, expected-return estimates have a cross-sectional sd of 0.87% monthly and a predictive slope of 0.74 (s.e. 0.07) for subsequent returns, i.e. forecasts are overdispersed by about a quarter even with 10y windows — [Lewellen 2015, Critical Finance Review 4](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2511246)
- Adaptive group LASSO with nonparametric characteristic effects has higher out-of-sample explanatory power than linear panel regressions and raises Sharpe ratios by 50% (gross, academic universe) — [Freyberger-Neuhierl-Weber 2020, RFS 33(5)](https://academic.oup.com/rfs/article-abstract/33/5/2326/5821383)
- IPCA: five latent factors with characteristic-instrumented loadings; only ten characteristics are significant at 1% and account for nearly 100% of model accuracy — [Kelly-Pruitt-Su 2019, JFE 134(3)](https://www.nber.org/papers/w24540)
- ML in practice (Robeco review): ML is useful for nonlinearities and interactions, but recent results rely on high-turnover signals, which raises implementation concerns — [Blitz-Hoogteijling-Lohre-Messow 2023, JPM 49(7)](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4321398)
- ML term structure: models trained on 3-, 6- or 12-month forward returns trade less and produce significant positive net returns in 2004-2021, where 1-month models net to about zero; efficient buy/hold trading rules are part of the result — [Blitz-Hanauer-Hoogteijling-Howard 2023, JFDS 5(4)](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4474637)
- Design-choice dispersion: across 1,056 ML model variants (7 design choices: algorithm, target, target transformation, post-publication inputs, feature compression, rolling vs expanding window, micro-cap inclusion) monthly returns range 0.13%-1.98% and annualised Sharpe 0.08-1.82; ensembles beat individual algorithms; market-relative return targets are recommended — [Chen-Hanauer-Kalsbach, SSRN 5031755](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=5031755); [Robeco summary](https://www.robeco.com/en-int/insights/2024/12/better-by-design-why-human-choices-matter-for-return-predictions-via-machine-learning)
- Large/mid-cap evidence: gradient boosting beats linear models statistically on global large and mid caps (MSCI/FTSE/S&P/STOXX constituents, 1991-2018, 20 characteristics), but "the economic gains tend to be more limited and critically dependent on the ability to take risk and implement trades efficiently" — [Leung-Lohre-Mischlich-Shea-Stroh 2021, JFDS 3(2)](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3546725)
- Forecast combination: the simple average of individual-predictor forecasts beats the kitchen-sink model out of sample — [Rapach-Strauss-Zhou 2010, RFS 23](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1257858)
- Conditional (volatility-managed) multifactor weights can beat the unconditional portfolio out of sample and net of costs when the factors are managed jointly — [DeMiguel-Martin-Utrera-Uppal 2024, JF 79(6)](https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13395)

### Inferences
Method sheet for the sprint plan. "Years to beat EW" is an **[est]** judgement combining the break-even arithmetic in section 1(e) with the cited evidence; "fitted params" counts quantities chosen from return data (pre-registered constants count as zero fitted parameters but one trial each).

| # | Method | What is estimated | Fitted params | Years of data before it plausibly beats `ew-theme-v1` | Turnover effect | Expected dSR vs EW at 11y |
|---|---|---|---|---|---|---|
| M0 | `ew-theme-v1` (baseline) | nothing | 0 | - | baseline | 0 |
| M1 | Correlation-cluster EW (hierarchical 1/N) | sleeve correlation matrix, tree | 0 means; 2 structural choices (linkage, K) | 1-3y of daily sleeve returns; usable now | neutral; weights change only at re-cluster dates | -.03 to +.05 |
| M2 | Inverse-vol / inverse-variance of sleeves | N sleeve vols | N vols, 1 lookback | 1y; usable now | slightly negative if slow sleeves have lower vol | 0 to +.05 (v7 R1.2) |
| M3 | HRP / HERC | covariance, tree | as M1 + M2 | 3-5y window per cited study | higher costs than simple risk-based rules (Trucios 2026) | -.05 to +.05; not preferred |
| M4 | Maximum diversification / min variance on sleeves | covariance, inversion | N(N+1)/2 shrunk | 3y+ with LW shrinkage | concentrates in low-correlation sleeves; unstable weights raise turnover | -.10 to +.05 |
| M5 | Prior-mean MVE, LW covariance (v6 recipe) | covariance only | 0 mean params | usable now | neutral | 0 to +.05 |
| M6 | KNS ridge MVE: w = (Sigma + gamma*I)^-1 mu_hat | sample means plus one penalty | 1 (kappa), or 0 if kappa is pre-registered | themes: ~10y; sleeves: not before 15-20y | tilts to high-variance PCs, which are the slow style themes: neutral to lower | themes +.00 to +.10; sleeves negative |
| M7 | Normal-normal / empirical-Bayes shrinkage of sleeve SR to theme prior | sleeve SR, tau_c, tau_w | 2 if taus estimated; 0 if taus fixed from literature | usable now with fixed taus; ~10y to estimate taus from 38 sleeves | neutral | 0 to +.08 |
| M8 | Black-Litterman style blend of prior SR and sample SR | same as M7, different parameterisation | 1 (confidence scalar) | as M7 | neutral | as M7 |
| M9 | Ridge / elastic-net Fama-MacBeth on 3-12 month forward returns | 38 slopes + 1-2 penalties | 38 shrunk + 2 | 10y minimum (Lewellen used 10y windows for 15 characteristics) | raises turnover if target is 1-month return; lowers it if target is 6-12 month return | -.10 to +.10 |
| M10 | IPCA (K = 3-5) | Gamma matrix K x L | 100-200 | 20y+ | unknown net | research only |
| M11 | Boosted trees / NN, long-horizon target, cost-aware | thousands | many | 15-20y+ (GKX windows; v7 S1) | high unless horizon >= 3 months and buy/hold rule | research only |
| M12 | Stacking / model averaging of M1, M2, M5, M7 outputs | none if equal-weighted | 0 | usable now | neutral | 0 to +.05, lower variance of outcome |
| M13 | Online exponentially weighted updating of theme weights | running means | 1 (half-life) | 10y+ and only with shrinkage to EW | adds weight drift; small | -.05 to +.05 |

Concrete recipes (all deterministic; every constant pre-registered).

- **M1 correlation-cluster EW.**
  1. Daily net returns of the 38 admitted sleeves, vol-scaled; correlation via Ledoit-Wolf shrinkage on the development window.
  2. Distance sqrt(0.5*(1 - rho)); average or Ward linkage; ties broken by sleeve id so the tree is reproducible.
  3. Cut at K = 10 to compare with the hand-made themes; report the adjusted Rand index between data clusters and themes.
  4. Weight 1/K per cluster, equal within cluster.
  5. Use: primarily a diagnostic of whether the theme map is right. Adopt only if the paired bundle test passes.
- **M6 KNS ridge at theme level.**
  1. Sigma = LW-shrunk covariance of 10 theme returns (daily, full development window); mu_hat = sample means.
  2. w = (Sigma + gamma*I)^-1 mu_hat, with gamma set from a pre-registered kappa (prior on the book's maximum squared Sharpe) rather than cross-validated on 11y.
  3. Blend toward EW with delta = T/(T + T0), T0 = 11.6y for themes (section 1e): delta ~ .45-.49 at T = 9.75-11y.
  4. Cap at [0.5, 2] x EW (v7 R1.2 cap).
  5. Validate with CPCV over calendar-year groups (section 7).
- **M7 hierarchical shrinkage of sleeve Sharpe ratios.**
  1. Prior mean per theme m_k from literature post-publication Sharpe with haircut (v6 section 5 item 2).
  2. Posterior SR_i = k*SR_hat_i + (1 - k)*m_k, k = tau^2/(tau^2 + 1/Y) in annualised Sharpe units; tau fixed ex ante (suggestion: tau_w = .3 within theme).
  3. Weight sleeves proportionally to max(posterior SR, 0) within theme; themes stay EW or M6.
  4. Numbers: at Y = 3, k = .21; at Y = 11, k = .50 (tau = .3) **[est]**.
- **M9 ridge Fama-MacBeth (only after the runner supports 2013+).**
  1. Monthly cross-sectional regressions of forward 3-, 6- and 12-month industry-adjusted returns on the 38 z-scored signals, top-3000 universe, ridge penalty pre-registered.
  2. Signs constrained to the literature prior (non-negative slopes after sign alignment); this removes wrong-signed fits and is a zero-parameter constraint.
  3. Average slopes over an expanding window with at least 8y; shrink the slope vector 50% toward equal slopes.
  4. Compare to EW by CPCV; expect Lewellen-type overdispersion (slope ~ .74), so scale forecasts by 0.75 before construction.

Ranking for a sprint: M1 (diagnostic) and M7 with fixed taus are usable on the 3y sample; M6 at theme level and M9 become testable once the 2013-2022 panel exists; M3, M4, M10, M11 are not recommended for production under the current data budget.

### Gaps
- Primary sources for maximum diversification (Choueifaty-Coignard 2008), Black-Litterman (1992), Pastor-Stambaugh (2000) and Cong et al. (AlphaPortfolio) could not be fetched after the search budget was exhausted; M4 and M8 rest on the comparative evidence in Trucios 2026 and on the KNS description of the Pastor-Stambaugh prior. No claims about AlphaPortfolio or Avramov et al. beyond what v7 already states are made here.
- No study was found that applies HRP, KNS shrinkage or empirical Bayes to combining alpha sleeves (as opposed to assets or academic factor portfolios) with 3-11 years of data and realistic costs; the expected-dSR column is judgement.
- The KNS optimal kappa (~0.30 for anomalies, ~1 for financial-ratio portfolios) is reported in the paper's own scaling; its mapping to an annualised prior Sharpe for this book, and the exact gamma formula, need to be taken from the published version before pre-registration.
- Net-of-cost ML results specifically for US top-3000 at daily rebalance were not found; the Robeco evidence is monthly.
- The HRP Monte Carlo variances are quoted via Wikipedia, not from the paper text.

---

## 3. Signal-level vs portfolio-level combination (mixing vs integrating) and trade netting

### Takeaway
For long-only portfolios the literature mostly favours integrating signals before construction, but the advantage is disputed under robust tests and depends on tracking-error level. For a linear long/short book the two are identical unless construction is nonlinear, so the practical gain of signal-level combination is trade netting and lower turnover rather than higher gross alpha.

### Cited Findings
- AQR: the "portfolio mix" combines stand-alone style portfolios; the "integrated portfolio" aggregates style scores per asset first. Secondary summaries of the paper report about +1% per year excess return and about +40% information ratio for integration in long-only portfolios, with benefits growing in the number of styles — [Fitzgibbons-Friedman-Pomorski-Serban 2017, J. of Investing 26(4)](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2802849); [AQR page](https://www.aqr.com/Insights/Research/White-Papers/Long-Only-Style-Investing)
- Portfolio blending gives higher information ratios at low-to-moderate tracking error; signal blending is better at high tracking error — [Ghayur-Heaney-Platt 2018, FAJ 74(3)](https://rpc.cfainstitute.org/research/financial-analysts-journal/2018/faj-v74-n3-5)
- Contradiction: using robust performance tests, no statistically significant difference between mixed and integrated approaches; earlier findings are described as a statistical fluke; the integrated approach loads more on the low-risk anomaly without improving performance — [Leippold-Ruegg 2018, European Financial Management 24(5)](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2887117)
- Combining four long-only factor sub-portfolios (low beta, size, value, momentum) captures less than half (about 40%) of the potential Sharpe improvement over the market, while a long-only portfolio built from individual securities captures most (about 80%) — [Clarke-de Silva-Thorley 2016, FAJ](https://ssrn.com/abstract=2616071)
- Bottom-up multifactor construction is superior to top-down because top-down ignores security-level interaction of factors (Bender-Wang 2016, JPM 42(5)), as summarised in — [Bottom-up versus top-down factor investing, J. Asset Management 2020](https://link.springer.com/article/10.1057/s41260-020-00188-9)
- Trade netting: when characteristics are combined, trades in the underlying stocks cancel, so with transaction costs the number of jointly significant characteristics rises from 6 to 15 — [DeMiguel-Martin-Utrera-Nogales-Uppal 2020, RFS 33(5)](https://academic.oup.com/rfs/article-abstract/33/5/2180/5821387) (URL carried from the v7 bibliography)

### Inferences
- If position = linear function of the combined score and no constraint binds, the sum of sleeve portfolios equals the portfolio of the summed score; mixing and integrating only differ through rank truncation, per-name caps, the optimiser's risk and turnover terms, and the aim/trade-rate filter. `ew-theme-v1` combines at score level, so it is already "integrated".
- The Ghayur result and the Clarke result point the same way: the more binding the constraints, the more the integrated approach wins. For a low-turnover L/S book with caps and a trade-rate filter, keep combining at signal level, and compute per-sleeve PnL by attribution rather than by running separate books.
- Netting value can be measured without new parameters: turnover of the combined score versus the weight-averaged turnover of stand-alone sleeves. The ratio is a reportable diagnostic per release (not measured here).
- A Leippold-Ruegg style robust test (Ledoit-Wolf bootstrap) should be the default for any mixed-vs-integrated comparison; the expected difference is within noise at 3y.

### Gaps
- The +1% and +40% figures for Fitzgibbons et al. come from secondary summaries returned by search, not from the paper text; verify before quoting in a report.
- No long/short, daily-rebalanced evidence on mixing vs integrating was found; all cited studies are long-only and monthly or quarterly.

---

## 4. Turnover-aware combination

### Takeaway
Turnover of a score-driven portfolio is governed by the forecast's cross-sectional autocorrelation, so signal weights and smoothing should be chosen on lagged/horizon IC net of cost rather than on one-period IC. The analytic frameworks (Qian-Sorensen-Hua, Grinold, Garleanu-Pedersen) all imply more weight on persistent signals and different trade rates for fast and slow signals.

### Cited Findings
- Turnover of a quantitative signal rises with target tracking error, with the square root of the number of stocks, with lower forecast autocorrelation and with lower average specific risk; slide formula (as extracted) T = sqrt(N/pi) * sigma_model * sqrt(1 - rho_f) * E(1/sigma), rho_f = cross-sectional correlation of consecutive forecasts — [Qian-Sorensen-Hua, Northfield 2006 slides for JPM 2007](https://www.northinfo.com/Documents/215.pdf)
- Momentum factors have the lowest forecast autocorrelation (highest turnover), value the highest, quality in between — [Qian-Sorensen-Hua slides](https://www.northinfo.com/Documents/215.pdf)
- A moving average of current and lagged factor values raises forecast autocorrelation; in their example with rho_f(1) = .90 and rho_f(2) = .81 the autocorrelation can be raised to about .95, and since turnover scales with sqrt(1 - rho_f) the ratio sqrt((1 - .95)/(1 - .90)) is about 71% — [Qian-Sorensen-Hua slides](https://www.northinfo.com/Documents/215.pdf)
- Optimal alpha model under a turnover constraint: maximise IR = v'IC / sqrt(v' Sigma_IC v) over weights on current and lagged factors subject to a target forecast autocorrelation; lagged IC is corr(F_{t-l}, R_t); in their illustration (N = 3000, model risk 4%, specific risk 30%) the net-return-maximising autocorrelation depends on the assumed cost (0.5%, 1.0%, 1.5%); implications listed: more value/quality exposure as AUM grows, "more important for large cap stocks" — [Qian-Sorensen-Hua slides](https://www.northinfo.com/Documents/215.pdf); journal version [JPM 34(1)](https://www.pm-research.com/content/iijpormgmt/34/1/27)
- Signal weighting is "better described as risk budgeting": Grinold gives a portfolio-based approach to choosing signal weights in the presence of trading costs, to maximise after-cost effectiveness — [Grinold 2010, JPM 36(4)](https://jpm.pm-research.com/content/36/4/24)
- Dynamic trading: aim in front of the target and trade partially toward the aim; persistent signals receive more weight in the aim portfolio — [Garleanu-Pedersen 2013, JF 68(6)](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12080) (URL carried from the v7 bibliography)
- ML evidence consistent with the above: lengthening the prediction horizon to 3-12 months reduces turnover and restores positive net returns post-2004 — [Blitz-Hanauer-Hoogteijling-Howard 2023](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4474637)

### Inferences
- "Alpha-decay-adjusted IC" recipe (zero fitted weights; inputs are second-moment-like and precisely estimable):
  1. For each sleeve compute IC(h) for h = 1..63 sessions and rho_f(1) (daily rank autocorrelation). These use N_stocks x T observations and are far more precise than sleeve mean returns.
  2. Define horizon IC over the expected holding period H_k = 1/(daily turnover of the sleeve): IC_H = sum over h <= H of IC(h).
  3. Net score = prior IC scale x (IC_H/IC(1) ratio) - cost x turnover, where turnover ~ c*sqrt(1 - rho_f). Only the shape of decay is taken from data; the level stays on the prior.
  4. Weight = prior weight x GP aim gain (v7 R1.2); this is the same object as v7's 1/(1 + 19*phi_k) and need not be re-derived.
- Fast and slow signals at different trade rates: run two aims (slow themes: value, quality, investment, low risk; fast themes: reversal, short flow, earnings drift) and trade the fast aim only outside a no-trade band set by its cost per unit of IC. This has 1 new parameter (band width) and should be pre-registered as a single trial.
- The v7 finding that `ew-theme-aim-v1` lost -.126 (SE .112) on 3y is 1.1 SE; at 9.75-11y the same test has SE about .06 (rho .98), so a true effect of +/-.10 becomes detectable. Re-testing aim weighting on the extended sample is one of the few combination experiments with adequate power **[est]**.
- Smoothing choice per signal (MA length) changes both IC and rho_f; selecting the MA by maximising in-sample net IR over many lengths is a multiple-testing exercise. Restrict to 3 pre-registered lengths per speed class.

### Gaps
- The full text of Grinold 2010 was not accessible; only the abstract-level description is cited, so the exact weighting formula is not reproduced.
- The Qian-Sorensen-Hua numbers come from conference slides whose text extraction is partly garbled; the turnover formula, the 71% figure and the IR/turnover trade-off chart should be checked against the JPM article or the 2007 book before implementation.
- No published evidence was found on horizon-IC weighting for daily-rebalanced top-3000 US books net of realistic costs in 2015-2025.

---

## 5. Weak or wrong-signed members: what shrinkage says

### Takeaway
With a literature prior and only 3 years of data, a negative in-sample result barely moves the posterior: a sleeve with prior Sharpe .3 and sample Sharpe -.3 keeps roughly 60-80% of its prior weight. At 11 years the same sample result removes nearly all of the weight, so the extension is what makes data-driven demotion of members legitimate **[est]**.

### Cited Findings
- Posterior alpha is a shrinkage factor times the OLS alpha, lying between the estimate and the prior mean of zero; "The more data we have (higher T), the less shrinkage there is"; a lower post-publication alpha "is the expected outcome from the Bayesian perspective" and the McLean-Pontiff decay is consistent with the posterior a Bayesian would have formed — [Jensen-Kelly-Pedersen, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- In the hierarchical model a factor's posterior depends on the other factors: the average observed alpha is informative about the common component and the factor's out-performance relative to the average is shrunk toward zero — [Jensen-Kelly-Pedersen, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- Spike-and-slab prior for the factor zoo: weak and spurious factors get posteriors for their price of risk that are diffuse and centred at zero, so they are detectable, and inference is robust to their presence; flat priors make model selection invalid — [Bryzgalova-Huang-Julliard 2023, JF 78(1)](https://onlinelibrary.wiley.com/doi/10.1111/jofi.13197)
- Publication bias is small relative to decay: bias-adjusted returns are 12.3% smaller than in-sample returns (s.e. 1.7 pp) across 156 published predictors, because the dispersion of returns across predictors is too large to be noise — [Chen-Zimmermann 2020, RAPS 10(2)](https://academic.oup.com/raps/article-abstract/10/2/249/5640503)
- Predictors of decay that can grade a prior ex ante: publication year (more recent means larger haircut), number of operations needed to build the signal, sensitivity of in-sample Sharpe to removing stocks or outliers — [Falck-Rej-Thesmar 2022](https://arxiv.org/abs/2105.01380)
- Strategy complexity predicts live deterioration: the most complex strategies lose over 30 percentage points more Sharpe than the simplest — [Suhonen-Lennkh-Perez 2017, JPM 43(2)](https://www.pm-research.com/content/iijpormgmt/43/2/90)
- Even the best anomalies have low individual t-statistics in a joint SDF; what matters is the joint contribution — [Kozak-Nagel-Santosh, NBER w24070](https://www.nber.org/system/files/working_papers/w24070/w24070.pdf)

### Inferences
Normal-normal posterior in annualised Sharpe units, prior mean m = .3, SE^2 = 1/Y: k = tau^2/(tau^2 + 1/Y); posterior = k*sample + (1 - k)*m. All **[est]**.

| tau (prior sd) | Y | k | sample -.3 -> posterior (share of prior) | sample 0 | sample -.6 | sample Sharpe needed to flip the sign |
|---|---|---|---|---|---|---|
| .2 | 3 | .11 | .24 (79%) | .27 | .20 | -2.5 |
| .3 | 3 | .21 | .17 (57%) | .24 | .11 | -1.1 |
| .5 | 3 | .43 | .04 (14%) | .17 | -.09 | -0.4 |
| .2 | 11 | .31 | .12 (39%) | .21 | .03 | -0.7 |
| .3 | 11 | .50 | .00 (1%) | .15 | -.15 | -0.3 |
| .5 | 11 | .73 | -.14 | .08 | -.36 | -0.1 |

- Decision rule that follows: at 3y, never drop or flip a prior-admitted member on in-sample IC sign alone; the most the data supports is a weight haircut to 60-80% of prior. At 11y a sleeve with sample Sharpe <= 0 has a posterior of at most half its prior and can be demoted one tier.
- The hierarchical structure matters: if the member's theme average is positive in sample, the member's own negative deviation is shrunk toward the theme, so a wrong-signed member inside a working theme should keep more weight than the table implies.
- Negative weights are never justified by this evidence level: constrain to non-negative weights on the prior sign.
- Evidence-tiered weights as a zero-parameter prior. Assign tiers before looking at platform returns, from the cited decay predictors:
  - Grade A (weight 1.0): replicated in both the Chen-Zimmermann and JKP data, original t >= 3, published before 2005, few operations, economically motivated.
  - Grade B (weight 0.67): replicated but one criterion missing.
  - Grade C (weight 0.33): recent publication, complex construction, or replicated only in small caps.
  - The tier map is 1 trial; the three weights are pre-registered constants, not fitted. Re-grading after seeing platform results is "iterated out of sample" in the Arnott-Harvey-Markowitz sense and must be logged as a new trial.
- The tau to use is the key judgement. tau = .3 for a single sleeve's net Sharpe is consistent with the Falck-Rej-Thesmar distribution (mean in-sample .98, about half retained, wide dispersion) but was not estimated here.

### Gaps
- JKP's estimated tau_c and tau_w values and the exact closed form of the shrinkage factor could not be extracted from the PDF text (encoding); they would give a literature-based tau for the recipe above. The k formula used here is the textbook normal-normal result.
- No source addresses shrinkage of information coefficients (as opposed to factor alphas) with theme-level priors; an IC version of the table would be a direct analogue, not a cited result.
- Bryzgalova-Huang-Julliard works with monthly data over long samples; its applicability at T = 3-11y with 38 sleeves is untested.

---

## 6. Orthogonalisation before combining

### Takeaway
Symmetric (Lowdin) orthogonalisation is order-independent and treats all inputs equally, which makes it preferable to sequential residualisation for attribution. As a combination device it implicitly up-weights low-variance directions of the signal set, which the Kozak-Nagel-Santosh prior says should carry little Sharpe, so at small T simple averaging within themes remains the safer default.

### Cited Findings
- Klein and Chow apply the Schweinler-Wigner / Lowdin symmetric transformation to factor returns; the procedure treats all inputs equally and "is not sensitive to the order in which vectors are arranged", and enables a variance decomposition of systematic risk — [Klein-Chow 2013, QREF 53(2)](https://www.sciencedirect.com/science/article/abs/pii/S1062976913000185); method summary in [NUS RMI working paper](https://rmi.nus.edu.sg/wp-content/uploads/2022/12/RMI-IRP-2022-01.pdf)
- Absence of near-arbitrage implies Sharpe ratios of low-eigenvalue principal components should be smaller than those of high-eigenvalue components; a prior that gives equal expected Sharpe to all PCs (eta = 1) is not economically plausible and performs worse out of sample — [Kozak-Nagel-Santosh, NBER w24070](https://www.nber.org/system/files/working_papers/w24070/w24070.pdf)
- Combining characteristics lets trades net against each other — [DeMiguel et al. 2020](https://academic.oup.com/rfs/article-abstract/33/5/2180/5821387)

### Inferences
- Algebra: with signal correlation matrix S, symmetric orthogonalisation is F_orth = F * S^(-1/2). Equal-weighting the orthogonalised signals equals weighting the originals by S^(-1/2) * 1. This sits between equal weight (S^0) and the minimum-variance-like S^(-1) * 1. Sequential (Gram-Schmidt) residualisation gives all shared variance to whichever signal is ordered first, so the result depends on an arbitrary ordering, which is itself a hidden degree of freedom.
- Noise amplification: S^(-1/2) divides by the square root of small eigenvalues. For a theme with members correlated at .8-.9 the smallest eigenvalues are .1-.2, so orthogonalised members are 2-3x amplified differences between near-duplicates, which are mostly measurement noise, and have higher turnover **[est]**.
- Pros: clean attribution, removes double counting of a dominant common component (for example size or beta exposure shared across sleeves), order independence. Cons: up-weights idiosyncratic signal noise, raises turnover, requires estimating S (fine at N = 38, T = 750+), and changes every sleeve when one is added, which breaks the audit trail of per-sleeve definitions.
- Recommended use:
  1. Orthogonalise to risk factors, not to each other: residualise every signal cross-sectionally on industry, size and beta (one deterministic regression per day), which removes the largest shared component without amplifying noise.
  2. Use symmetric orthogonalisation across the 10 themes only for attribution reports.
  3. If a decorrelated combination is wanted, use the shrunk version S_delta = (1 - delta)*S + delta*I with delta pre-registered at .5, then weight by S_delta^(-1/2) * 1. One trial, zero fitted parameters.
- Cross-sectional orthogonalisation of scores and time-series orthogonalisation of sleeve returns are different operations; the first changes holdings, the second only changes weights. The plan should say which is meant.

### Gaps
- No empirical study was found comparing symmetric orthogonalisation with simple averaging for alpha combination out of sample; Klein-Chow is about risk decomposition of Fama-French factors, not alpha combination.
- The Klein-Chow full text was not accessed (paywalled); properties are taken from abstract-level summaries.

---

## 7. Validation design: splits, holdout reuse, multiple testing, trial accounting

### Takeaway
With zero-fitted-parameter rules the whole development sample is the test and the binding problem is the number of looks, not the split. The never-used 2012-2019 block is the most valuable asset: run the frozen v7 book on it once, pre-registered, before any tuning. The twice-read 2023-2024 block should be folded into development and the 2025+ holdout queried only through a pass/fail threshold with a fixed budget.

### Cited Findings
- Seven-point protocol (categories): research motivation; multiple testing and statistical methods; data and sample choice; cross-validation; model dynamics; complexity; research culture. Checklist items include: "Did the researcher keep track of all models and variables that were tried (both successful and unsuccessful)"; "Are the researchers aware that true out-of-sample tests are only possible in live trading?"; "Are steps in place to eliminate the risk of out-of-sample 'iterations'" — [Arnott-Harvey-Markowitz 2019, JFDS 1(1)](https://people.duke.edu/~charvey/Research/Published_Papers/P138_A_backtesting_protocol.pdf)
- "Suppose a model is successful in the in-sample period but fails out of sample... The researcher modifies the initial model so it then works both in sample and out of sample. This is no longer an out-of-sample test. It is overfitting." — [Arnott-Harvey-Markowitz 2019](https://people.duke.edu/~charvey/Research/Published_Papers/P138_A_backtesting_protocol.pdf)
- Trial counting depends on correlation: "if the 20 strategies tested had a near 1.0 correlation, then the process is equivalent to trying only one strategy"; interactions count in full (20 variables imply 190 pairwise interactions); data transformations such as volatility scaling or the winsorisation level are "analogous to trying extra variables" and must be chosen in advance — [Arnott-Harvey-Markowitz 2019](https://people.duke.edu/~charvey/Research/Published_Papers/P138_A_backtesting_protocol.pdf)
- A non-randomised split can validate a false strategy: the ticker-letter strategy mined on 1963-1988 "validated" with stronger results on 1989-2015 — [Arnott-Harvey-Markowitz 2019](https://people.duke.edu/~charvey/Research/Published_Papers/P138_A_backtesting_protocol.pdf)
- Disclosure template for multiple tests when presenting a strategy to clients or management — [Fabozzi-Lopez de Prado 2018, JPM 45(1)](https://jpm.pm-research.com/content/45/1/141)
- Reusable holdout: Thresholdout compares the training-set and holdout-set value of each query; if they agree within a noisy threshold it returns the training value (the analyst learns one bit), otherwise it returns the holdout value plus Laplace noise and decrements an overfitting budget B; it stops when B is exhausted. "The number of queries that Thresholdout can answer is exponential in n as long as the number of times that the analyst overfits is at most quadratic in n." Theorem 9 requires n >= O( ln(m/beta)/tau^2 * min{B, sqrt(B * ln(ln(m/beta)/tau))} ) for m queries at accuracy tau — [Dwork et al. 2015, NeurIPS](https://proceedings.neurips.cc/paper_files/paper/2015/file/bad5f33780c42f2588878a9d07405083-Paper.pdf); [Science 349:636](https://www.science.org/doi/10.1126/science.aaa9375)
- Naive reuse experiment: n = 10,000, d = 10,000 pure-noise features, variables kept when train and holdout correlations agree; the resulting classifier reports over 63% accuracy at k = 500 on both train and holdout although true accuracy is 50%; Thresholdout (T = 0.04, tau = 0.01) prevents this — [Dwork et al. 2015, NeurIPS](https://proceedings.neurips.cc/paper_files/paper/2015/file/bad5f33780c42f2588878a9d07405083-Paper.pdf)
- Under cryptographic assumptions no efficient algorithm can answer more than quadratically many adaptively chosen statistical queries (Hardt-Ullman; Steinke-Ullman), as cited in — [Dwork et al. 2015, NeurIPS](https://proceedings.neurips.cc/paper_files/paper/2015/file/bad5f33780c42f2588878a9d07405083-Paper.pdf)
- CPCV vs walk-forward in a controlled synthetic environment: CPCV shows lower probability of backtest overfitting and a better deflated-Sharpe test statistic than K-fold, purged K-fold and walk-forward; walk-forward "exhibits notable shortcomings in false discovery prevention", with higher temporal variability — [Arian-Norouzi-Seco 2024, Knowledge-Based Systems](https://www.sciencedirect.com/science/article/abs/pii/S0950705124011110); [SSRN 4778909](https://www.ssrn.com/abstract=4778909)
- Data-specific hurdles by double bootstrap: for 484 S&P Capital IQ strategies, with prior share of true strategies p0 = 10%, a 5% Type I error rate needs t = 2.4 (18% of strategies survive); a target of five misses per false discovery needs t = 2.6 (15% survive); with p0 = 0 the cutoff is 3.2 for CAPIQ and 4.9 for 18,113 data-mined anomalies; the authors state the 3.0 threshold was never intended as a universal rule; the cutoff falls as p0 rises; existing methods lack power — [Harvey-Liu 2020, JF 75(5)](https://arxiv.org/pdf/2006.04269)
- In the same data, 22.1% of CAPIQ strategies have t > 2.0 against 5.5% of the 18,113 mined anomalies — [Harvey-Liu 2020](https://arxiv.org/pdf/2006.04269)
- Bootstrap-based stepwise procedures (Romano-Wolf 2005 for FWER; Romano-Shaikh-Wolf 2008 for FDR) control error under arbitrary dependence but are computationally heavy (B x O(M^2) regressions) — as discussed in [Harvey-Liu 2020](https://arxiv.org/pdf/2006.04269)
- Bayesian alternative to frequentist corrections: a zero-alpha prior shrinks all estimates, "raising p-values with similar conservatism as a frequentist MT correction", while borrowing strength across factors; evidence is strengthened rather than weakened by the number of factors — [Jensen-Kelly-Pedersen](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf); spike-and-slab version — [Bryzgalova-Huang-Julliard 2023](https://onlinelibrary.wiley.com/doi/10.1111/jofi.13197)
- Sequential programmes: alpha-investing (Foster-Stine 2008) and generalised alpha-investing control false discoveries for a stream of tests; later online FDR rules include Javanmard-Montanari (Annals of Statistics 2018), decaying-memory FDR (Ramdas et al. 2017) and SAFFRON (2018) — [SAFFRON, arXiv 1802.09098](https://arxiv.org/pdf/1802.09098); [Online multiple hypothesis testing survey, arXiv 2208.11418](https://arxiv.org/pdf/2208.11418); [Foster-Stine alpha-investing](https://www.researchgate.net/publication/4993364_Alpha-Investing_A_Procedure_for_Sequential_Control_of_Expected_False_Discoveries)
- Non-standard errors from design freedom: 1,056 defensible ML designs span Sharpe 0.08-1.82 — [Chen-Hanauer-Kalsbach](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=5031755)

### Inferences
**Data budget (proposal).**

| Block | Status | Proposed role in v8 |
|---|---|---|
| 2013-04 to 2019-12 (first valid full-book date to end 2019) | never used | One-shot backward holdout for the frozen v7 book and the frozen accepted improvements; afterwards becomes development data |
| 2020-2022 | TRAIN, 37+ trials | Development |
| 2023-2024 | read twice as validation | Fold into development; stop calling it validation |
| 2025-01 to date (~1.7y) | reserved holdout | Single-shot release gate, pass/fail only |
| Live / paper from release | true out of sample | The only real test (Arnott-Harvey-Markowitz) |

- **Backward holdout first.** Before any engineering choice is tuned on 2012-2019, pre-register: book = v7 as frozen, metric = net Sharpe and paired dSR of each previously accepted improvement versus its baseline, test = Ledoit-Wolf bootstrap with fixed seed. With ~6.75y the SE of SR is ~.39 and SE(dSR, rho .98) ~ .077 **[est]**. This is the only place where the +.07 to +.09 improvements can be re-tested on data that did not select them. Pooled with 2020-2022 afterwards, they become part of a 9.75y development sample. The engineering work on the runner must not be validated by looking at PnL on this block; validate it on 2020-2022 reproduction (byte-identical to the existing TRAIN result) instead.
- **Cost of the two validation reads.** Selection among k looks inflates the expected best by E[max of k standard normals] x SE: 0.56 SE for k = 2, 0.85 for k = 3, 1.16 for k = 5. For a 2y block SE(SR) ~ .71 and SE(dSR, rho .98) ~ .14, so two adaptive reads can carry an optimistic bias of up to ~.40 Sharpe in level or ~.08 in a paired difference, the same size as the improvements being accepted **[est]**. That is why 2023-2024 can no longer certify a +.08 step.
- **Holdout protocol (Thresholdout adapted).**
  1. The 2025+ block is queried only by a gate job that returns a bit: pass if net SR_holdout >= SR_dev - c*SE and paired dSR versus the incumbent >= -c'*SE; constants c, c' pre-registered.
  2. Budget B = 2 failures per year; every query is logged in the trial ledger whether it passes or fails.
  3. No holdout PnL series, drawdown chart or per-theme attribution is shown to researchers until the book is retired or the holdout is rolled forward.
  4. Determinism: the Laplace noise in Thresholdout conflicts with byte-reproducibility unless seeded; use the deterministic boolean query (SparseValidate-style), or record the seed in the pre-registration if noise is used.
  5. With 1.7y the holdout SE(SR) is ~.77: it can falsify (for example SR < 0) but cannot confirm SR >= 1. State this in the gate definition.
- **CPCV layout for the extended sample (fitted combiners only).** Groups = calendar years 2013-2022 (N = 10), test groups k = 2: C(10,2) = 45 splits, 9 backtest paths; purge 21 sessions for monthly-horizon labels (63 for quarterly), embargo 5. Pass if paired net dSR > 0 in >= 80% of paths (v7 R5.4) and the median path dSR exceeds 1 SE. Walk-forward (expanding window, annual refit) is reported alongside as the realistic deployment path, but not used as the acceptance test because of its higher false-discovery rate in Arian et al.
- **Multiple-testing control for improvements.**
  - Level of the book: DSR with effective N (v7 R5.2).
  - Stream of improvement tests: alpha-investing with pre-registered initial wealth (for example W0 = .05), a fixed bidding rule and a fixed payout on each rejection. Prior-motivated changes (literature-signed, zero fitted parameters) and data-mined changes are separate streams with separate wealth, reflecting the Harvey-Liu point that the hurdle falls as the prior share of true ideas rises (t ~ 2.4-2.6 for curated ideas at p0 = 10%, 3.2-4.9 for mined ones).
  - Because single steps are underpowered (section 1b), register bundles: a bundle is accepted if its cumulative paired dSR passes, and each member must individually have non-negative dSR and the prior-consistent sign.
- **Defect and bug cells in the trial count (proposal; no literature found).**
  - Invalid cell (crash, look-ahead, wrong universe): logged, excluded from N and from V[SR], with the defect reference.
  - Bug-fix rerun where the fix was decided without seeing PnL (code review, unit test): replaces the defective cell, N unchanged.
  - Bug-fix rerun where the defect was noticed because the PnL looked wrong, or where keeping the fix depended on the result: counts as a new trial, since a filter that only investigates disappointing results is a selection mechanism.
  - Data-vendor restatement reruns: new data version, one trial per affected cell cluster (ONC collapses them).
- **Reporting standard per release.** SR with Lo/Bailey-Lopez de Prado SE; Ledoit-Wolf bootstrap CI for paired differences; MinTRL against SR* = 0 and .5; DSR at cell count and effective N; PBO for grids; number of trials by stream; holdout queries used; Fabozzi-Lopez de Prado disclosure table; the seven Arnott-Harvey-Markowitz categories answered in one line each.

### Gaps
- No published treatment was found of how to count defect or bug-fix reruns in a trial ledger; the rule above is a proposal.
- Thresholdout theory assumes IID samples and bounded queries; Sharpe ratios on autocorrelated, heavy-tailed daily returns satisfy neither, so the formal guarantee does not transfer and the protocol is a heuristic borrowing its logic.
- Benjamini-Hochberg / Storey primary sources and Romano-Wolf primary papers were not fetched (search budget exhausted); they are cited only through Harvey-Liu 2020 and Jensen-Kelly-Pedersen.
- Arian et al. 2024 is a synthetic-data study; no empirical comparison of CPCV vs walk-forward on real equity L/S combiners was found.
- Chordia-Goyal-Saretto 2020 and Harvey-Liu-Zhu 2016 are already in v7 S5 and were not re-read.
- The alpha-investing parameter values are illustrative; no finance-specific calibration was found.

---

## 8. Realistic expectations: achievable Sharpe and backtest-to-live haircut

### Takeaway
Published evidence puts the typical backtest-to-live Sharpe haircut at about one half for academic anomalies and about three quarters for bank-marketed systematic strategies, with larger haircuts for recent, complex or heavily mined designs. A prior-signed, simply-constructed book should sit at the mild end, so a TRAIN net Sharpe of 1.4 on 3 years maps to a planning value near 0.7-1.0 **[est]**.

### Cited Findings
- 215 alternative-beta strategies from global investment banks: median 73% deterioration in Sharpe between backtest and live; the most complex strategies deteriorate over 30 percentage points more than the simplest — [Suhonen-Lennkh-Perez 2017, JPM 43(2)](https://www.pm-research.com/content/iijpormgmt/43/2/90)
- 72 academic factors: mean in-sample Sharpe 0.98 (first quartile 0.43); post-publication Sharpe about half of in-sample; out-of-sample Sharpe is well approximated by a linear function of in-sample Sharpe; the haircut grows with publication year — [Falck-Rej-Thesmar 2022](https://arxiv.org/abs/2105.01380)
- 26% lower out of sample, 58% lower post-publication — [McLean-Pontiff 2016](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12365)
- Pure publication bias accounts for only about 12% — [Chen-Zimmermann 2020](https://academic.oup.com/raps/article-abstract/10/2/249/5640503)
- The 50% rule-of-thumb haircut is wrong because the multiple-testing haircut is non-linear: the highest Sharpe ratios are moderately penalised and marginal ones heavily — [Harvey-Liu 2015, "Backtesting"](https://papers.ssrn.com/abstract=2345489); summary at [Man Group](https://www.man.com/insights/backtesting)
- Hedge fund evidence: aggregate hedge fund performance declined after 2008; equity hedge fund alpha was about 4 percentage points per year lower than pre-crisis and not statistically significant; fund-selection models did not overcome the trend — [Bollen-Joenvaara-Kauppila 2021, FAJ 77(3)](https://www.tandfonline.com/doi/abs/10.1080/0015198X.2021.1921564)
- One-month ML strategies net to about zero after 2004; longer-horizon versions remain positive — [Blitz-Hanauer-Hoogteijling-Howard 2023](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4474637)
- "The greater the complexity and the reliance on nonintuitive relationships, the greater the likely slippage between backtest simulations and live results" — [Arnott-Harvey-Markowitz 2019](https://people.duke.edu/~charvey/Research/Published_Papers/P138_A_backtesting_protocol.pdf)

### Inferences
- Haircut ladder applied to TRAIN net SR 1.405 **[est]**:
  - Chen-Zimmermann bias only (12%): 1.24. Lower bound on the haircut; ignores arbitrage and regime.
  - McLean-Pontiff out-of-sample (26%): 1.04.
  - Falck-Rej-Thesmar / McLean-Pontiff post-publication (50-58%): 0.59-0.70.
  - Suhonen median (73%): 0.38. Applies to marketed, complex, selected strategies; upper bound for this book.
- Bayesian version: prior for a diversified net-of-cost multi-signal market-neutral book SR ~ N(.7, .4^2) (assumption), sample 1.405 with SE .6 at 3y gives k = .31 and posterior ~ .92; the same sample Sharpe held over 11y (SE .31) gives k = .62 and posterior ~ 1.14. The extension is worth about +.2 of credible Sharpe even if the point estimate does not change **[est]**.
- The signals in the book are mostly already published, so the McLean-Pontiff publication decay is already in the 2020-2022 data; the remaining haircut is for selection (37 trials), regime luck of 2020-2022, and cost-model error. This argues for the 26-50% range rather than 73%.
- Planning values: expected live net SR 0.7-1.0; treat anything above 1.2 in an 11y backtest as needing explanation. Acceptance of a release should be phrased as "posterior SR >= .7 with 80% probability" rather than "backtest SR >= x".
- Signs that the haircut should be larger: improvements that raise turnover, rely on 2020-2022 liquidity-provision returns, involve fitted weights, or whose paired dSR turns negative on the 2013-2019 backward holdout.

### Gaps
- No reliable primary source was found for the realised net Sharpe of professional multi-signal equity market-neutral books (HFRI/BarclayHedge equity-market-neutral index statistics were not retrievable; search results were marketing or secondary pages). The prior SR ~ .7 used above is an assumption, not a cited figure.
- Falck-Rej-Thesmar and McLean-Pontiff measure single-factor decay gross of costs in academic universes; decay of a combined, cost-aware book in the top 3000 may differ in either direction.
- Harvey-Liu 2015 numeric haircut examples could not be fetched (timeout); only the qualitative result is cited.
- Jensen-Kelly-Malamud-Pedersen net Sharpe figures are in v6/v7 and were not re-verified.

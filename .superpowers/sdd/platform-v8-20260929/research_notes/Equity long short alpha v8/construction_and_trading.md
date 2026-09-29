# Portfolio construction, risk targeting and trading for a US equity market-neutral L/S book (v8, as of 2026-09)

Scope note. These notes EXTEND `literature-v7.md` S2/S3/S4/S7 and `v6-literature.md` s.6. They do not repeat Garleanu-Pedersen, Boyd SPO/MPO, no-trade bands, Frazzini-Israel-Moskowitz, Novy-Marx-Velikov 2016, DeMiguel 2020, USE4, Ledoit-Wolf, Kyle-Obizhaeva or the transfer coefficient, except where a new source updates or contradicts them.

Conventions.
- `[est]` = my own arithmetic or derivation, not a sourced number.
- `[secondary]` = figure taken from a non-primary source and not traced to the original.
- Book facts used in estimates are those supplied in the brief: aim-partial-v5 has net SR 1.405, gross SR 1.809, vol 4.13%/yr, turnover 3.8%/day of gross, cost 13.5 bps per traded dollar, gross leverage 1.247, 1,850 names, capacity curve net SR 1.45/1.41/1.36/1.24/1.08 at $0.5/1/2/4/8bn.

Research limitation. The session's web-search quota was exhausted part-way through the work. Items listed under Gaps were therefore not searched to completion. A gap here means "not retrieved", not "absent from the literature".

Two results from the brief change which earlier recommendations remain open:
- Experiment 1 (target / (1 + kappa x cost)) is v7 recommendation R2.2. It was tried and rejected, so it is not recommended again below.
- Experiment 2 is a version of v7 recommendation R2.1. Section 1 diagnoses why it failed and what would have to change before a retry.

---

## 1. Why mean-variance optimisers underperform rank heuristics, and the documented fixes

### Takeaway
The literature attributes optimiser underperformance to three interacting causes rather than to optimisation itself: alpha that the risk model does not span gets over-weighted, binding constraints (above all a gross-leverage cap) change what is being maximised, and forecast error is amplified. Experiment 2's failure pattern is the expected result of a binding gross cap combined with alpha = IC x sigma x z, and the best-evidenced fixes are joint regularisation (risk target instead of gross cap, soft leverage and turnover penalties, robust return and risk terms) or tracking the heuristic target instead of maximising alpha.

### Cited Findings

Alpha and risk-factor misalignment
- Lee and Stefek (2008, JPM 34(4):12-25): using different factor models for risk and alpha creates unintended exposures in optimised portfolios. Misalignment arises when the alpha vector is not completely spanned by the risk factors, and the optimiser emphasises the unspanned portion. They state that aligning the risk and alpha models may lead to better portfolios even if that worsens the overall risk forecasts, and they compare four remedies. — [JPM abstract](https://jpm.pm-research.com/content/34/4/12)
- MSCI's later summary gives the mechanism concretely. If alpha uses 13-month momentum and the risk model uses 12-month momentum, the optimiser "will take a disproportionately large bet on stocks that did well 13 months ago, because that bet has alpha but bears no systematic risk"; yet the month-13 return "has little power to forecast future returns". — [Lee, Ruban, Stefek, Yao 2012, MSCI Research Insight](https://www.msci.com/documents/10199/8426a871-688b-4705-ae65-aee7c4c9a0c5)
- MSCI describes two remedies. (a) Penalise the residual alpha in the objective (Bender-Lee-Stefek 2009); this "requires the manager to estimate the missing volatility, and it assumes that the returns to the residual alpha and the risk model factors are uncorrelated". (b) Add the alpha to the risk model as a custom factor, which captures correlation between the alpha and the model factors. — [MSCI 2012](https://www.msci.com/documents/10199/8426a871-688b-4705-ae65-aee7c4c9a0c5)
- MSCI's empirical verdict is lukewarm: both methods "tend to improve the risk forecasts for optimized portfolios. The impact on information ratios, however, is mixed - in our preliminary work, we find both improvements and degradations in information ratios". MSCI is "highly skeptical" of "broad claims that any direction outside the risk factors should always be regarded as source of systematic risk that needs to be penalized". — [MSCI 2012](https://www.msci.com/documents/10199/8426a871-688b-4705-ae65-aee7c4c9a0c5)
- Saxena and Stubbs (2013, Journal of Risk 15(3):3-37) argue that risk under-estimation of optimised portfolios comes from the interaction of expected returns, constraints and the risk model inside the optimiser. They add a dynamic Alpha Alignment Factor (AAF) to the risk model during optimisation and report, on actual manager backtests, that the under-estimation is pervasive and that the AAF corrects it. — [Saxena-Stubbs 2013](https://www.researchgate.net/publication/259384627_The_Alpha_Alignment_Factor_A_Solution_to_the_Underestimation_of_Risk_for_Optimized_Active_Portfolios)
- Ceria, Saxena and Stubbs (2012, JPM 38(2):29-43) frame the problem as a three-way interaction of the expected-return model, the risk model and the constraints, magnified by the optimiser. — [JPM record](https://www.pm-research.com/content/iijpormgmt/38/2/29)
- Saxena lists the symptoms of factor alignment problems: "risk underestimation of optimized portfolios, undesirable exposures to factors with hidden and unaccounted systematic risk, consistent failure in achieving ex-ante performance targets, and inability to harvest high quality alphas into above-average IR". — [Saxena, "The Alignment Trio"](https://www.linkedin.com/pulse/alignment-trio-anureet-saxena-ph-d-cfa)
- The two vendors disagree. Axioma authors present the AAF as a general fix; MSCI authors report mixed IR effects and warn against always penalising the unspanned direction. — [Saxena-Stubbs 2013](https://www.researchgate.net/publication/259384627_The_Alpha_Alignment_Factor_A_Solution_to_the_Underestimation_of_Risk_for_Optimized_Active_Portfolios); contradicted in part by [MSCI 2012](https://www.msci.com/documents/10199/8426a871-688b-4705-ae65-aee7c4c9a0c5)

Sampling error in the covariance matrix
- Sampling error does not bias the risk forecast of a randomly chosen portfolio but does cause under-estimation of the risk of optimised portfolios: "portfolios with the lowest forecast risk are those whose risk is underestimated the most". A factor structure reduces this bias but does not remove it. — [MSCI 2012](https://www.msci.com/documents/10199/8426a871-688b-4705-ae65-aee7c4c9a0c5)

Alpha scaling
- Barra Aegis applies Grinold's rule alpha = IC x volatility x score. The volatility is residual volatility, which MSCI defines as CAPM-residual risk and explicitly NOT the model's specific risk. MSCI lists "larger adjustments for assets with high volatility" as a property and gives IC 0.05 as "good" and 0.10 as "very good". — [Gleiser-McKenna 2010, MSCI Barra](https://www.msci.com/documents/10199/1645561/PI_Converting_Scores_Into_Alphas.pdf/7adf1f42-10aa-40eb-9e8c-ecc11eeba2d4)
- Northfield's critique bears directly on Experiment 2: "Expect lower IC's for volatile securities (harder to predict) than for less volatile ones (easier to predict). Using a single IC exaggerates volatile securities' alphas." IC may be estimated within groups (same cap, industry or volatility). — [Shah 2007, Northfield "Alpha Scaling Revisited"](https://www.northinfo.com/Documents/247.pdf)
- Northfield's "cross-sectional Grinold" alternative is alpha_k = IC x (one cross-sectional volatility for the whole universe) x score_k. The score comes from mapping ranks onto a standard normal. The cross-sectional volatility is taken from the risk model as average stock variance minus market variance. Under this form alpha per dollar is proportional to the score and does not rise with the name's own volatility. — [Shah 2007](https://www.northinfo.com/Documents/247.pdf)
- Northfield adds that horizon and signal-decay adjustments "are important, particularly in low-turnover portfolios". The forecast should be the time-weighted average over the reference holding period; their example is 8% annualised for 6 months then 1% for 18 months, giving 2.75% over 2 years. — [Shah 2007](https://www.northinfo.com/Documents/247.pdf)

Leverage and short-sale constraints
- In Frazzini and Pedersen's model, investors who cannot use leverage "bid up high-beta assets": a leverage constraint pushes a return-seeking investor toward riskier securities. — [Frazzini-Pedersen, NBER w16601](https://www.nber.org/papers/w16601)
- Jacobs, Levy and Markowitz: real long-short constraints (Reg T, per-name short limits, broker and client limits) change over time and by broker, and all are expressible as linear equalities or inequalities. The assumption that one can short without limit and use the proceeds is "mathematically convenient, but it is unrealistic". Long-only fast algorithms carry over to long-short portfolios when the portfolio is "trimable", which "usually holds in practice". — [Jacobs-Levy-Markowitz 2006, FAJ 62(2)](https://jlem.com/documents/FG/jlem/articles/580188_Trimability.pdf); see also [JLM 2005](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2447041)

Robust optimisation and regularisation
- Ceria and Stubbs (2006, J. Asset Management 7(2):109-127): mean-variance weights are very sensitive to small changes in expected returns. They separate the true, estimated and actual frontiers and propose a robust formulation that minimises the gap between the estimated and actual frontiers. — [Ceria-Stubbs 2006](https://link.springer.com/article/10.1057/palgrave.jam.2240207)
- Boyd, Johansson, Kahn, Schiele and Schmelzer (2024) give closed forms for both robustifications. Worst-case return = mu'w - rho'|w|, an "uncertainty-weighted leverage" penalty. Worst-case variance = w'Sigma w + kappa (sum_i sigma_i |w_i|)^2, a "volatility-weighted leverage" penalty. Defaults: rho = 20th percentile of |return forecast|, kappa = 0.02. — [Boyd et al. 2024, "Markowitz Portfolio Construction at Seventy"](https://arxiv.org/pdf/2401.05080)
- Boyd et al. back-test: 74 surviving S&P 100 stocks, daily, 2000-2023, 4,436 out-of-sample days, SYNTHETIC forecasts with IC 0.15 on weekly returns, 10% volatility target, shorting cost 5%/yr over fed funds. Table 1 follows. The PDF text extraction shifted the turnover column by one row; I realigned it and checked each row against the paper's prose (for example, the turnover-limited policy has turnover near its target of 25).

| Policy | Return | Vol | Sharpe | Turnover (x/yr) | Max leverage | Max drawdown |
|---|---|---|---|---|---|---|
| Equal weight | 14.1% | 20.1% | 0.66 | 1.2 | 1.0 | 50.5% |
| Basic Markowitz | 3.7% | 14.5% | 0.19 | 1145.2 | 9.3 | 78.9% |
| + weight limits (10% / -5%) | 20.2% | 11.5% | 1.69 | 638.4 | 5.1 | 30.0% |
| + leverage limit 1.6 | 22.9% | 11.9% | 1.86 | 383.6 | 1.6 | 14.9% |
| + turnover limit 25 | 19.0% | 11.8% | 1.54 | 26.1 | 6.5 | 25.0% |
| + robust return and risk | 15.7% | 9.0% | 1.64 | 458.8 | 3.2 | 24.7% |
| Markowitz++ (all terms, softened) | 38.6% | 8.7% | 4.32 | 28.0 | 1.8 | 7.0% |
| Markowitz++ tuned yearly | 41.8% | 8.8% | 4.65 | 38.6 | 1.6 | 6.4% |

  — [Boyd et al. 2024, Table 1](https://arxiv.org/pdf/2401.05080). The authors warn that the data have survivorship bias and the forecasts are synthetic, so the levels are not implementable and only differences between policies carry meaning.
- Boyd et al. soften the risk target, leverage limit and turnover limit into penalties. Priority parameters come from the Lagrange multipliers of the hard-constrained problem over the previous five years (70th percentile for risk and turnover; 25% of the maximum for leverage). Yearly tuning on the previous two years accepts a parameter change only if in-sample Sharpe rises AND turnover <= 50, leverage <= 2 and volatility <= 15%. — [Boyd et al. 2024](https://arxiv.org/pdf/2401.05080)

Evidence from a realistic large-scale test
- Jensen, Kelly, Malamud and Pedersen (RFS, published 15 March 2026). US stocks, monthly rebalancing, out of sample 1981-2020, investor with $10bn by 2020, risk aversion 10. Impact is 0.1% for trading 1% of daily dollar volume (quadratic cost, lambda = 0.2 / volume, calibrated to Frazzini et al. 2018). Table 2:

| Method | Gross SR | Net SR | Vol | Turnover (x NAV per month) | Gross leverage |
|---|---|---|---|---|---|
| Portfolio-ML (learns weights directly, dynamic) | 1.43 | 1.38 | 14% | 0.32 | 3.60 |
| Multiperiod-ML, one tuning layer | 0.95 | 0.41 | 34% | 1.47 | 12.70 |
| Static-ML (single-period cost-aware), one tuning layer | 1.06 | 0.94 | 27% | 0.76 | 11.21 |
| Multiperiod-ML, two tuning layers | 1.33 | 1.16 | 8% | 0.40 | 2.50 |
| Static-ML, two tuning layers | 1.36 | 1.11 | 10% | 0.61 | 3.22 |
| Decile portfolio sort | 1.10 | -11.87 | 15% | 2.60 | 2.00 |
| Markowitz-ML (cost-agnostic) | 2.00 | negative | 156% | 56.33 | 53.15 |

  — [JKMP working-paper version, Table 2](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz); published as [RFS 2026](https://doi.org/10.1093/rfs/hhag022); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4187217)
- JKMP on the single-period cost-aware optimiser: with one tuning layer it "delivers a positive net Sharpe, but delivers a lower utility than putting all the money in the risk-free asset". The second layer shrinks the expected-return vector and modifies the covariance and cost inputs. The first-stage forecasts are "dominated by short-term signals, and this method does not take into account which predictors are persistent and which have quick alpha decay". — [JKMP](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)
- JKMP on the cost-agnostic optimiser: the culprit is "excessive reliance on fleeting small-scale characteristics (e.g., 1-month reversal for small stocks)". After costs, value and quality earn the highest economic feature importance. Portfolio-ML beats the tuned static alternative "by roughly 20% in Sharpe ratio terms and 60% in utility terms". — [JKMP](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)

Practitioner view
- AQR (Israel, Jiang, Ross 2017, JPM): an optimisation can be used to allow deviation from the ideal portfolio, and "the effect of varying the deviation is similar to that of changing the rebalance frequency: a higher deviation induces greater style drift and therefore greater performance degradation, but also less turnover". — [AQR "Craftsmanship Alpha"](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)

### Inferences

Diagnosis of Experiment 2 `[est; my derivation]`
- Without a binding constraint and with diagonal specific risk, alpha_i = IC sigma_i z_i gives w_i proportional to z_i / sigma_i. That tilts AWAY from volatile names relative to rank weights. Experiment 2 tilted TOWARD them, so alpha scaling alone is not the cause.
- Add a binding gross cap sum|w_i| <= L with multiplier lambda. The first-order condition becomes w_i = sign(z_i) x max(IC sigma_i |z_i| - lambda - marginal cost_i, 0) / (gamma sigma_i^2).
- The cap is therefore a soft threshold on alpha PER DOLLAR. Low-sigma names with moderate scores fall below lambda and are dropped. High-sigma names pass.
- As lambda grows relative to gamma, the problem approaches a linear programme that puts all gross into the largest IC sigma |z|, stopped only by position and ADV caps. High specific volatility and low liquidity go together, so cost per dollar rises as well.
- This reproduces all three reported symptoms: gross SR down 28%, cost per dollar up 27%, tilt to high-specific-vol illiquid names.
- The economics are those of Frazzini-Pedersen's leverage-constrained investor. The effect is worse if true IC falls with volatility (Northfield), because the alpha being chased is over-stated exactly where the optimiser concentrates.
- Reverse optimisation of the accepted heuristic: rank weights w proportional to z imply alpha proportional to sigma^2 z under diagonal risk. The heuristic is more volatility-loaded in implied alpha than Grinold-Kahn, yet it works, because nothing in it SELECTS on sigma. The optimiser and heuristic differ in selection, not in scaling.

Recipe 1A. Target-tracking optimiser (recommended as the first optimiser experiment)
- Problem: minimise (w - w_tgt)' Sigma (w - w_tgt) + sum_i [s_i |dw_i| + eta_i |dw_i|^1.5] + borrow_i max(-w_i, 0), subject to 1'w = 0, |beta'w| <= 0.02, |w_i| <= min(w_max, q ADV_i / NAV), |dw_i| <= p ADV_i / NAV.
- There is no alpha vector and no separate gross cap. Gross is inherited from w_tgt.
- Why: it removes alpha scaling and the gross-cap threshold from the problem. The optimiser can only choose WHICH gaps to close first given risk and cost. With Sigma from the existing factor model, a gap in a name whose risk is already hedged by other holdings is cheap to leave open.
- Parameters to pre-register: Sigma source (factor model unchanged); one cost-versus-tracking scalar at three values; p in {1%, 2%}; w_tgt = aim-partial-v5 target.
- Acceptance rule to pre-register: net SR not below the heuristic, cost per dollar down >= 10%, gross SR loss <= 3%, evaluated at $1bn AND $4bn.
- Expected effect `[est]`: cost per dollar -10 to -20% for gross SR -0 to -3%, which is net SR +0.03 to +0.08 at $1bn and more at $4-8bn. The basis is AQR's equivalence between deviation and rebalance frequency, not a direct published test.
- Failure modes: (i) Sigma lets the optimiser substitute liquid for illiquid names inside a factor bucket, which loses alpha if the alpha is name-specific (monitor correlation of w with w_tgt, require > 0.9); (ii) a hidden tilt to low-specific-risk names; (iii) solver instability at 3000 names with the 1.5-power term.

Recipe 1B. If an alpha-maximising optimiser is retried
- Use all four Boyd regularisers together and softened: a risk target (not a gross cap) as the scale control, a leverage penalty, a turnover penalty, and the robust terms rho'|w| and kappa (sigma'|w|)^2.
- Set the leverage penalty so the gross cap binds on fewer than 10% of days `[est]`.
- Pre-register three alpha-scaling arms: (a) IC sigma_i z_i (Grinold); (b) IC sigma_xs z_i (Northfield cross-sectional); (c) IC_g sigma_i z_i with IC_g estimated on TRAIN within specific-volatility terciles.
- Run the custom alpha factor or an AAF-style penalty as a separate arm, because MSCI reports mixed IR effects.
- Parameters to pre-register: rho = 20th percentile of |alpha|; kappa = 0.02; leverage and turnover priorities from TRAIN Lagrange multipliers as in Boyd; risk target = the heuristic's ex-ante volatility.
- Diagnostics to log daily: share of gross in the top specific-volatility quintile and bottom liquidity quintile versus the heuristic; correlation of w with w_tgt; shadow price of each constraint; bias statistic of the OPTIMISED book.
- Failure modes: those of Experiment 2, plus overfitting of tuning layers. JKMP needed two tuning layers and 40 years of data; TRAIN here is three years.

Expected size of optimiser gains `[est]`
- The large gains in the literature are measured against naive monthly sorts (JKMP decile sort: net SR -11.87). aim-partial-v5 already has partial adjustment, a band and an ADV cap.
- The remaining gain is therefore closer to the gap between two cost-aware methods. In JKMP that gap is about 20% of Sharpe between a tuned static optimiser and the dynamic method, and that is with 40 years of training data.

### Gaps
- Lee-Stefek's quantitative comparison of the four remedies is behind the JPM paywall. Only the abstract and MSCI's later summary were read.
- Saxena-Stubbs numerical results (size of risk under-estimation, IR change with the AAF) were not retrieved; the Springer and JPM pages require authentication.
- Bender-Lee-Stefek 2009 and Stefek-Lee-Yao 2012 were not retrieved.
- No published head-to-head test of "track a heuristic target" against "maximise alpha" was found. Recipe 1A rests on inference.
- Book content from Paleologo (2021, 2025), Isichenko (2021), Qian-Hua-Sorensen, Chincarini-Kim and Grinold-Kahn (2019) could not be read; only publisher descriptions were available. No claims from those books are made here.
- Robeco, Axioma/SimCorp and Two Sigma notes comparing heuristic and optimised construction were not found before the search quota ran out.
- Shrinkage toward 1/N or rank weights (for example DeMiguel-Garlappi-Uppal, Kan-Zhou) was not searched.

---

## 2. Position sizing heuristics, liquidity-aware sizing, concentration versus breadth

### Takeaway
No retrieved study tests equal, rank-linear, z/sigma and z/sigma^2 weights head to head for a long-short stock book, so the sizing choice has to be settled on TRAIN. A simple derivation shows z/sigma is the minimax choice across plausible alpha-scaling hypotheses, and the evidence that factor premia are roughly twice as strong in small caps explains why cost-shrinking targets (Experiment 1) destroyed gross Sharpe.

### Cited Findings
- MSCI on weighting schemes: score-tilt weighting captured target factors and limited unintended exposure, while equal and inverse-volatility weighting "resulted in implicit exposure to size and low-volatility, regardless of the targeted factor". — [MSCI blog, "How Portfolio-Weighting Schemes Affected Factor Exposures"](https://www.msci.com/research-and-insights/blog-post/how-portfolio-weighting-schemes-affected-factor-exposures)
- AQR's illustrative long-short portfolios weight stocks "by value signal strength" (or momentum signal strength), are market- and industry-neutral ex ante, and are then scaled to a portfolio volatility target (5% in the B/P example). — [AQR "Craftsmanship Alpha"](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Ding and Martin (2017, J. Empirical Finance 43:91-114): the optimal portfolio's IR rises with the mean of IC(t) and with N, falls with the volatility of IC(t), and has an absolute upper bound as N tends to infinity. IC volatility is an intrinsic "strategy risk". — [Ding-Martin 2017](https://www.sciencedirect.com/science/article/pii/S0927539817300543); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2730434)
- Factor premia by size (Blitz, Baltussen, van Vliet 2020, FAJ; US, July 1963 - December 2018). The equal-weighted five-factor long-short portfolio has Sharpe 1.08 in small caps against 0.53 in large caps. — [Blitz-Baltussen-van Vliet 2020, Table 4](https://repub.eur.nl/pub/130144/Repub_130144_O-A.pdf)
- Landier, Simon and Thesmar: for all four strategies studied, performance is higher in mid caps (ranks 501-1500) than in large caps (top 500). Annual volume is about $3.5tn in mid caps against $15tn in large caps, so on volume alone adding mid caps raises capacity by "about 20%". — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4)
- JKMP: a larger investor "internalizes price impact from their trades, and this leads the investor to tilt away from highly predictive but costly-to-trade stocks and signals"; "a larger investor must trade more slowly and focus more on liquid stocks". — [JKMP](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)
- Robeco restricts its short-term signal study to MSCI World constituents and argues that "it may be more efficient to apply short-term strategies only to those [stocks] where the expected gains outweigh the expected costs". — [Hanauer, Blitz, van Vliet, Honarvar, Huisman 2022, Robeco](https://cdn.uc.assets.prezly.com/dbe30eb4-6b9f-4e86-9633-a127867241f6/-/inline/no/20220530%20Beyond%20Fama-French%20-%20alpha%20from%20short-term%20signals%20May%202022%20-%20GB.pdf)
- Northfield expects lower IC for volatile securities. — [Shah 2007](https://www.northinfo.com/Documents/247.pdf)

### Inferences

Sharpe retained by each weighting rule under three alpha hypotheses `[est]`
- Assumptions: diagonal specific risk, no costs, z independent of sigma, sigma lognormal across names with coefficient of variation 0.5 (an assumption, not measured on the book).
- Entries are the fraction of the best achievable Sharpe.

| Weight rule | If alpha ~ z (flat in sigma) | If alpha ~ sigma z (Grinold) | If alpha ~ sigma^2 z (implied by rank weights being optimal) |
|---|---|---|---|
| w ~ z (rank or score weights, current) | 0.64 | 0.89 | 1.00 |
| w ~ z / sigma | 0.89 | 1.00 | 0.89 |
| w ~ z / sigma^2 | 1.00 | 0.89 | 0.64 |

- z/sigma never loses more than about 11% under any of the three hypotheses. Rank weights lose 36% if alpha does not scale with volatility, which is the direction Northfield and the low-volatility literature point to.
- Inverse-volatility weights also tilt to larger, more liquid names (MSCI), so the cost side probably favours z/sigma as well. That tilt must be reported, since it is an implicit size and low-volatility exposure.

Recipe 2A. Volatility exponent test
- Target: w_i proportional to z_i / sigma_i^k, then the existing projection on beta, volatility and log dollar volume, then rescale to the same ex-ante volatility (not the same gross).
- Parameters to pre-register: k in {0, 0.5, 1}; sigma = specific volatility from the factor model, floored at the 5th and capped at the 95th cross-sectional percentile; comparison at equal ex-ante risk.
- Expected effect `[est]`: gross SR +0 to +10% and cost per dollar -5 to -10% for k = 1. The theoretical ceiling from the table is +12% (0.89 to 1.00) under Grinold scaling.
- Failure modes: the volatility projection already in the construction may absorb most of the effect; a low-volatility tilt raises crowding with low-risk funds; equal-risk comparison requires gross leverage to rise for k > 0.

Recipe 2B. IC by bucket (diagnostic, no trading change)
- Measure on TRAIN the mean and standard deviation of daily IC within terciles of specific volatility and of ADV.
- This settles which column of the table applies and whether Northfield's claim holds for this library. It also gives Ding-Martin's mean(IC)/sd(IC) by bucket, which says how much IR is lost by concentrating in liquid names.

Liquidity-aware sizing `[est; my derivation]`
- With square-root impact, annual cost on a position w_i traded at rate tau is proportional to sigma_i tau^1.5 w_i^1.5 (NAV / ADV_i)^0.5.
- Where cost dominates risk, the optimal position is proportional to alpha_i^2 ADV_i / (sigma_i^2 NAV). Where risk dominates, it is alpha_i / (gamma sigma_i^2) and independent of ADV.
- A cap of the form |w_i| <= q ADV_i / NAV is therefore the correct functional form for the cost-dominated limit, and it binds on more names as NAV grows. A square-root-of-ADV weighting has no such derivation behind it.
- Why Experiment 1 failed: shrinking targets by cost at $1bn removes weight from small and illiquid names, where factor Sharpe is about twice as high (Blitz et al.), while cost at $1bn is not yet large enough to pay for that. The literature's liquidity tilt (JKMP, Landier) is a large-AUM result.
- Consequence for the sprint: evaluate every liquidity-aware rule at $4bn and $8bn as the primary cells, not at $1bn.

Breadth
- With 1,850 names and non-zero IC volatility, the book is probably on the flat part of Ding-Martin's IR curve, so dropping the least liquid few hundred names should cost little IR from breadth alone `[est]`. Any loss would come from the higher per-name alpha in small caps, which Recipe 2B measures.

### Gaps
- No empirical study comparing equal, rank-linear, signal-proportional, z/sigma and z/sigma^2 weighting for long-short stock portfolios was retrieved.
- No source was found for square-root-of-ADV weighting or for the numerical value of q in ADV-based position caps.
- Ding-Martin's closed-form IR formula and the limit value were not read in full text; only the abstract was retrieved.
- Grinold (1989) and Clarke-de Silva-Thorley (2006) were not re-read; they are covered in the earlier reviews.

---

## 3. Volatility targeting and leverage

### Takeaway
The evidence does not support a Sharpe gain from time-series volatility scaling of a diversified market-neutral book: out of sample and after costs the gain survives only for the market and momentum. The case for ex-ante risk targeting here is different: it converts library improvements into return instead of lower volatility, and it thins the left tail. Leverage availability is not the binding constraint at gross 1.25; impact cost and deleveraging risk are.

### Cited Findings

Volatility-managed portfolios
- Cederburg, O'Doherty, Wang and Yan (2020, JFE 138:95-117), 103 equity strategies. In direct comparisons the volatility-managed version has the higher Sharpe in 53 cases and the original in 50. Only 8 strategies show a statistically significant Sharpe difference in favour of volatility management, and these "are concentrated among momentum-related strategies". — [Cederburg et al. 2020](https://www.lehigh.edu/~xuy219/research/COWY.pdf)
- Spanning-regression alphas are positive for 77 of 103 volatility-scaled portfolios (23 significantly positive, 3 significantly negative), confirming Moreira-Muir in sample. The implied strategy is not implementable in real time, because it needs ex-post optimal weights on the scaled and unscaled portfolios. — [Cederburg et al. 2020](https://www.lehigh.edu/~xuy219/research/COWY.pdf)
- Out of sample, the real-time combination earns a lower certainty-equivalent return than the original portfolio in 72 of 103 cases. The cause is "structural instability in the underlying spanning regressions". Exceptions with out-of-sample gains are momentum, profitability (ROE) and betting-against-beta. — [Cederburg et al. 2020](https://www.lehigh.edu/~xuy219/research/COWY.pdf)
- Barroso and Detzel (2021, JFE 140(3):744-767): after transaction costs, volatility management of asset-pricing factors other than the market "generally produces zero abnormal returns and significantly reduces Sharpe ratios", using five cost-mitigation strategies. The managed market portfolio stays profitable after costs. — [Barroso-Detzel 2021](https://www.sciencedirect.com/science/article/abs/pii/S0304405X21000775)
- DeMiguel, Martin-Utrera and Uppal (2024, Journal of Finance): a conditional MULTIFACTOR portfolio, whose factor weights vary with market volatility, outperforms its unconditional counterpart out of sample and net of costs. — [DeMiguel et al. 2024](https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13395)
- Harvey, Hoyle, Korgaonkar, Rattray, Sargaison and Van Hemert (2018, JPM 45(1):14-33), 60+ assets, daily data from 1926. For US equities the Sharpe ratio improves from 0.40 unscaled to between 0.48 and 0.51 scaled, and is not sensitive to the volatility estimator. The gain is confined to risk assets (equity and credit) and is attributed to the leverage effect; for bonds, currencies and commodities the Sharpe effect is negligible. — [Harvey et al. 2018](https://people.duke.edu/~charvey/Research/Published_Papers/P135_The_impact_of.pdf)
- Harvey et al.: volatility targeting reduces the likelihood of extreme returns and the volatility of volatility across all asset classes. Scaling by volatility gives lower turnover than scaling by variance. Default estimator: exponentially weighted daily returns with a 20-day half-life; half-lives from 10 to 90 days were tested. — [Harvey et al. 2018](https://people.duke.edu/~charvey/Research/Published_Papers/P135_The_impact_of.pdf)
- Bongaerts, Kang and van Dijk (2020, FAJ 76(4)): Moreira-Muir and Harvey et al. contain look-ahead bias through an ex-post scaling constant. An implementable "conventional" version does not consistently improve risk-adjusted performance, can overshoot the volatility target, and has turnover "often more than 200% per year". For factors it raises Sharpe for momentum but not for size, value, profitability or investment. — [Bongaerts-Kang-van Dijk 2020](https://repub.eur.nl/pub/130215/Bongaerts-Kang-van-Dijk-Conditional-volatility-targeting-2020-FAJ.pdf)
- Their conditional rule: sort all past monthly realised volatilities into quintiles; scale exposure down only when last month was in the top quintile, scale up (to a leverage cap) only when in the bottom quintile, otherwise leave exposure unscaled. — [Bongaerts-Kang-van Dijk 2020](https://repub.eur.nl/pub/130215/Bongaerts-Kang-van-Dijk-Conditional-volatility-targeting-2020-FAJ.pdf)
- Conditional-rule results. US momentum Sharpe rises by 0.17 from 0.14, against +0.16 for the conventional rule, at about half the turnover (1.1 versus 2 per year). Across regional momentum factors (1995-2019) the average Sharpe gain is 0.23, turnover is 1.2 against 2.4, maximum leverage 2.0 against 5.5, and maximum drawdown falls by an average of 20.1 points from 54.1%. There is no significant improvement for other US factors. — [Bongaerts-Kang-van Dijk 2020](https://repub.eur.nl/pub/130215/Bongaerts-Kang-van-Dijk-Conditional-volatility-targeting-2020-FAJ.pdf)
- AQR: a consistent level of risk through time is "a defensible approach" absent a view on time-varying attractiveness, provided volatility is forecastable and the trading cost of targeting does not remove the benefit. For long-short portfolios leverage can be managed by "reducing leverage when volatility increases", capping it at an absolute level, trading liquid instruments and holding enough cash. — [AQR "Craftsmanship Alpha"](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)

Leverage does not leave Sharpe unchanged once impact is counted
- JKMP Proposition 1: the net Sharpe ratio declines along the implementable efficient frontier, because "investors cannot freely leverage their portfolio to the desired risk in the presence of trading costs because more leveraged positions are larger and incur larger trading costs". Example: with $10bn and risk aversion 10 the investor earns 19% net at 14% volatility; with $1bn the same volatility earns 22%. — [JKMP](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)

Deleveraging and tail events
- Khandani and Lo: in the week of 6 August 2007 quantitative long-short equity funds had unprecedented losses. The hypothesis supported by their data is forced liquidation of one or more large market-neutral portfolios followed by price impact on similarly constructed portfolios. Unwinding began in July 2007. — [Khandani-Lo, NBER w14465](https://www.nber.org/system/files/working_papers/w14465/w14465.pdf)
- Value-type portfolios (book-to-market, cashflow-to-market, earnings-to-price) were being unwound on 2-3 August; price and earnings momentum were unaffected until 8-9 August. On 10 August "sharp reversals in all five strategies erased nearly all of the losses of the previous four days". — [Khandani-Lo](https://www.nber.org/system/files/working_papers/w14465/w14465.pdf)
- At 8:1 leverage the book-to-market portfolio would have lost a cumulative 24% from 1 August to the close of 7 August, and a price-momentum portfolio about 31% over 8-9 August. Leverage of 4:1 to 10:1 "was quite common among quantitative equity market-neutral strategies". A manager who cut leverage after the loss "would not have been able to recoup all of its losses" despite the price reversal. — [Khandani-Lo](https://www.nber.org/system/files/working_papers/w14465/w14465.pdf)
- A withdrawal of market-making capital starting 8 August is the second component: simulated returns of a simple market-making (contrarian) strategy were significantly negative that week and positive before and after. — [Khandani-Lo](https://www.nber.org/system/files/working_papers/w14465/w14465.pdf)

Industry leverage and financing (2024-2026)
- `[secondary]` Prime-brokerage-book gross leverage: quantitative funds 645.3% and multi-strategy funds 444.3% (November 2025); long/short equity 285.2% globally at end-2025; US long/short 208.1% gross with 52.8% net. The article does not name its original sources. — [Young and Calculated (Substack)](https://youngandcalculated.substack.com/p/what-leverage-actually-is-in-a-hedge)
- `[secondary]` An S&P Global report (May 2026) puts industry gross leverage at about eight times NAV, up from five times a decade earlier. The top four prime brokers hold about 68% of the market, and prime-brokerage revenue is about $37bn in 2025. — [Resonanz Capital, 4 May 2026](https://resonanzcapital.com/insights/four-banks-eight-times-leverage-what-the-sp-warning-means-for-hedge-fund-counterparty-risk)
- BIS: secured borrowing by hedge funds moves with equity valuations; prime brokers expand margin lending in good conditions and tighten in stress, so leverage supply is procyclical. — [BIS Quarterly Review, March 2024](https://www.bis.org/publ/qtrpdf/r_qt2403y.htm)

### Inferences

What the book's numbers imply `[est]`
- Net return = 1.405 x 4.13% = 5.80%. Gross return = 1.809 x 4.13% = 7.47%. Total drag = 1.67%/yr.
- Traded notional = 0.038 x 1.247 x 252 = 11.9 x NAV per year. At 13.5 bps that is 1.61%/yr, which accounts for almost all of the drag.
- The capacity curve is fitted within 0.03 by net SR = 1.58 - 0.175 x sqrt(NAV in $bn). The implied split at $1bn is about 0.23 Sharpe units of size-independent cost (spread and fixed) and 0.175 of impact. At $8bn impact is about 0.49.
- Levering by a factor k at NAV N trades the same dollars as NAV kN at base leverage. So net SR(N, k) is approximately 1.58 - 0.175 sqrt(kN), which is JKMP's declining frontier.

| NAV | Target vol | Gross leverage | Net SR `[est]` | Net return `[est]` |
|---|---|---|---|---|
| $1bn | 4.13% (current) | 1.25 | 1.41 | 5.8% |
| $1bn | 6% | 1.81 | 1.37 | 8.2% |
| $1bn | 8% | 2.42 | 1.34 | 10.7% |
| $4bn | 4.13% | 1.25 | 1.24 | 5.1% |
| $4bn | 6% | 1.81 | 1.16 | 6.9% |

- The earlier library upgrade cut volatility from 4.71% to 4.13% at unchanged return because gross was fixed. Under a fixed 4.71% risk target the same upgrade would have raised gross to about 1.42 and net return to about 6.6%, at a Sharpe about 0.01 lower than reported `[est]`.
- The extrapolation ignores financing spread on the added gross, the ADV cap binding more often, and any non-linearity beyond square root.

Recipe 3A. Ex-ante risk targeting with a slow update
- Rule: L_t = clip(L_(t-1) adjusted only when |sigma_target / sigma_hat_t - 1| exceeds a band, L_min, L_max), where sigma_hat_t is the factor model's ex-ante volatility of current holdings.
- Parameters to pre-register: sigma_target (one value, for example 5%); band 15%; L_max (for example gross 2.0); review frequency weekly; cross-check sigma_hat against EWMA realised volatility with 20-day half-life (Harvey) and act on the larger of the two.
- Expected effect: Sharpe unchanged to slightly lower (about -0.01 to -0.04 at $1bn per the table `[est]`); return proportional to target; thinner left tail and lower volatility of volatility (Harvey).
- Cost of adjustment `[est]`: a 10% leverage change trades 10% of gross, which is 2.6 days of normal turnover and about 1.7 bps of NAV at 13.5 bps per dollar. Rescaling the target and letting the 5%-per-day partial adjustment close the gap nets most of this against alpha trades.
- Failure modes: risk model under-forecasts optimised or concentrated books (Section 1), so leverage is too high exactly when it matters; procyclical deleveraging into a crowded unwind locks in losses (Khandani-Lo); conventional targeting can overshoot and adds more than 200%/yr turnover (Bongaerts).

Recipe 3B. Conditional scaling of momentum-type sleeves only
- Apply the Bongaerts quintile rule at sleeve level to price-momentum sleeves, where every study finds a benefit. Leave other sleeves unscaled, since Cederburg and Barroso-Detzel find none after costs.
- Parameters to pre-register: quintile breakpoints from an expanding window of the sleeve's own monthly realised volatility; scale-down only (no leverage-up) as the base arm; sleeve list fixed in advance.
- Expected effect: sleeve Sharpe +0.15 to +0.25 on stand-alone momentum (Bongaerts); book-level effect much smaller because momentum is a minority of risk `[est]`.
- Failure mode: sleeve volatility inside a neutralised multi-signal book is lower and less clustered than stand-alone factor volatility, so the published effect may not carry over.

Stress scenario from 2007 `[est]`
- Khandani-Lo's 31% two-day loss at 8:1 is 3.9% per unit of gross. At gross 1.247 a single-factor momentum book would lose about 4.8% in two days, roughly 13 two-day standard deviations of a 4.13% volatility book.
- A diversified, neutralised multi-signal book would lose less, but all five of their factors fell together, so diversification across signals gave little protection in that week.
- The practical rule the episode supports is a pre-committed policy that does NOT cut leverage mechanically on a loss of this type, since losses reversed within days and the deleveraged manager did not recover.

### Gaps
- Moreira-Muir (2017) was not read in full; only its results as restated by Cederburg et al. and Harvey et al. are used.
- No study of March 2020, the November 2020 momentum crash, or later crowding episodes was retrieved.
- Financing spreads, portfolio-margin terms and balance-sheet charges for a $1-8bn market-neutral book were not found in a primary source. The leverage figures above are secondary and should be re-sourced (OFR Hedge Fund Monitor, Form PF) before use in a plan.
- No study of volatility targeting applied to a diversified multi-signal market-neutral stock book was found; all evidence is for single factors or asset classes.

---

## 4. Factor and industry exposure control

### Takeaway
Removing unpriced risk from characteristic portfolios and neutralising sectors both raise long-short Sharpe in the published evidence, mainly by cutting volatility at unchanged return, and the cost is higher turnover. That matches the book's own industry-projection result, so the decision turns on turnover cost and on whether the risk reduction is wanted for tail and crowding reasons.

### Cited Findings
- Daniel, Mota, Rottke and Santos (2020, RFS): characteristic-sorted portfolios capture unpriced as well as priced risk. Hedging the unpriced part with covariance information from past returns raises the squared Sharpe of the optimal combination of the five Fama-French characteristic portfolios to 2.16 from 1.16. — [Daniel-Mota-Rottke-Santos](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3083143). The v7 review quotes 2.13 and 1.17; the difference is between paper versions.
- Ehsani, Harvey and Li (2023, FAJ 79(3)): keeping the sector component produced better LONG-SHORT factors in only 20% of trials, but better LONG-ONLY factors in 78% of trials. The within-sector component of a characteristic carries more information than the across-sector component; size, value and investment gain most. — [Ehsani-Harvey-Li 2023](https://www.tandfonline.com/doi/abs/10.1080/0015198X.2023.2196931); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3959116)
- AQR: a B/P portfolio that removes incidental market and industry risk "has delivered higher risk-adjusted returns". Industry neutrality "often reduces volatility while keeping long-run returns broadly unaffected, which implies higher portfolio Sharpe ratios", but "this adjustment typically results in higher turnover". — [AQR "Craftsmanship Alpha"](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- AQR also notes that a naive momentum sort implicitly times the market (overweighting it after an up market), a choice that "should be consciously chosen". — [AQR "Craftsmanship Alpha"](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Equal and inverse-volatility weighting carry implicit size and low-volatility exposures regardless of the targeted factor. — [MSCI blog](https://www.msci.com/research-and-insights/blog-post/how-portfolio-weighting-schemes-affected-factor-exposures)
- Long and short legs differ. Across five Fama-French-style factors, combined long legs reach Sharpe 1.1 against less than 0.7 for combined short legs, because long legs are less correlated with each other. No short leg adds significant alpha over the long legs. The authors propose hedging the market beta of a long-only factor portfolio "with liquid derivatives on broad market indexes". — [Blitz-Baltussen-van Vliet 2020](https://repub.eur.nl/pub/130144/Repub_130144_O-A.pdf)
- Crowding: in August 2007 similarly constructed portfolios lost together as one or more large portfolios were unwound. — [Khandani-Lo](https://www.nber.org/system/files/working_papers/w14465/w14465.pdf)
- Misalignment applies to neutralisation too: constraints interact with alpha and the risk model, and misaligned constraints are one of the three sources of factor alignment problems. — [Ceria-Saxena-Stubbs 2012](https://www.pm-research.com/content/iijpormgmt/38/2/29)

### Inferences
- The book's own result (industry projection: volatility 3.62% to 2.92%, gross SR 1.304 to 1.316, net SR -0.072 with SE 0.156) is what AQR describes: lower volatility, same gross Sharpe, more turnover. The net change is inside one standard error, so it does not reject industry neutrality.
- Because gross was fixed in that test, the 19% volatility reduction became lower return. Under Recipe 3A the same construction would run at higher gross, so its cost in return terms would be the turnover cost at the higher gross, not the lost volatility `[est]`.
- Three levels of control, from weakest to strongest:
  - Penalise: industry and style risk enter through Sigma in a tracking or alpha objective (Recipe 1A). This keeps priced tilts and is the v7 default.
  - Partially neutralise: subtract a fraction of the projection.
  - Hard neutralise: full projection, as done now for beta, volatility and log dollar volume.

Recipe 4A. Partial industry neutralisation at equal risk
- Target: w = w_raw - phi x (projection of w_raw on FF49 industry dummies), then the existing projection, then rescale to the ex-ante volatility of the base construction.
- Parameters to pre-register: phi in {0, 0.5, 1}; comparison at equal ex-ante risk; turnover and cost per dollar reported beside Sharpe.
- Expected effect `[est]`: net SR -0.05 to +0.05 at $1bn (the prior test could not distinguish it from zero); maximum drawdown and factor-crowding exposure lower. Accept on tail metrics only if net SR is not worse by more than one standard error.
- Failure modes: removes industry-momentum alpha; turnover rises; with 49 industries and 1,850 names, small industries create noisy offsets.

Recipe 4B. Hedge-portfolio test in the DMRS style
- For each sleeve, form a hedge portfolio from names with near-zero sleeve score but high covariance loading on the sleeve's return (loadings from the factor model or past returns), and subtract it.
- Parameters to pre-register: covariance window and estimator; hedge ratio from TRAIN only; applied to the five slowest sleeves first.
- Expected effect: the published gain (squared Sharpe 1.16 to 2.16, a 37% rise in Sharpe `[est]`) is for unhedged academic factor portfolios. The book is already neutralised on beta, volatility and liquidity, so the remaining gain is likely much smaller `[est]`.
- Failure mode: hedge positions add gross and turnover without alpha; the gain depends on covariance stability.

Beta-neutral versus dollar-neutral
- The book is both. If shorts are partly replaced by index or sector hedges (Section 7), dollar neutrality in single names is lost and beta and sector neutrality must be enforced at the combined level.

### Gaps
- Evidence on which style exposures are priced versus unpriced for a combined multi-signal score (as opposed to single Fama-French factors) was not found.
- Residual size and liquidity exposure after neutralisation, and its return contribution, was not covered by any retrieved source.
- Exposure to common hedge-fund factors and crowding measures (13F-based or short-interest-based) was not searched; the v7 review's S7 items (Lou-Polk, Brown-Howard-Lundblad) remain the reference.
- DMRS and Ehsani-Harvey-Li were read at abstract level only.

---

## 5. Turnover control that preserves alpha

### Takeaway
Published large-scale cost-aware portfolios trade far more slowly than the book: JKMP's $10bn portfolio turns over about 9% of gross per month against the book's roughly 80%. The methods with evidence for cutting turnover at small alpha loss are rank-space hysteresis (buy/hold bands) and horizon-aware aims; uniform cost-based shrinkage of targets and uniform slowing of illiquid names were both rejected on TRAIN at $1bn and should be retested only at higher AUM.

### Cited Findings
- JKMP turnover: Portfolio-ML 32% of NAV per month at gross leverage 3.60; two-layer Multiperiod-ML 40% at leverage 2.50; two-layer Static-ML 61% at leverage 3.22. The dynamic method's advantage comes partly from lower turnover. — [JKMP Table 2](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)
- JKMP: the optimal predictor of near-term returns differs from the optimal predictor of returns further ahead, and "with transaction costs, whatever you buy today you will likely own for a while", so expected returns at all horizons matter. — [JKMP](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)
- Landier, Simon and Thesmar, in a Garleanu-Pedersen model with quadratic costs. Capacity has elasticity 1 with respect to the price-impact coefficient and elasticity 2 with respect to the speed at which the signal fades. An investor who accepts a 30% Sharpe reduction loses about 20% of P&L to transaction costs. — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4)
- Landier et al. back-test: small portfolios trade about 12% of positions per month; the optimal trader's turnover falls to about half that when annual dollar volatility exceeds $10bn; cost per dollar of P&L reaches about one third at $10bn, where realised Sharpe is halved. — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4)
- Signal persistence in their data: book-to-market half-life about 4.6 years in mid caps, low volatility about 2 years, repurchases about 1.5 years. — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4)
- Robeco, five short-term signals on MSCI World constituents, December 1985 - December 2021. Individual signals have turnover of 1,300-2,000% per year and break-even costs below 25 bps. The composite has six-factor alpha above 12% and break-even cost above 30 bps, but with naive quintile rebalancing costs still remove more than two thirds of it at 25 bps per trade. — [Robeco 2022](https://cdn.uc.assets.prezly.com/dbe30eb4-6b9f-4e86-9633-a127867241f6/-/inline/no/20220530%20Beyond%20Fama-French%20-%20alpha%20from%20short-term%20signals%20May%202022%20-%20GB.pdf)
- Robeco's rule: hold the top (bottom) X% plus previously selected names still in the top (bottom) Y%. Moving from buy 20 / hold 20 to buy 10 / hold 50 gave "a slight decrease in the gross alpha" and lifted net alpha above 6% per year. — [Robeco 2022](https://cdn.uc.assets.prezly.com/dbe30eb4-6b9f-4e86-9633-a127867241f6/-/inline/no/20220530%20Beyond%20Fama-French%20-%20alpha%20from%20short-term%20signals%20May%202022%20-%20GB.pdf)
- The published version (FAJ 79(4), 2023) reports that the alpha is robust to implementation lags of several days and holds out of sample, post-publication and across regions. — [Blitz, Hanauer, Honarvar, Huisman, van Vliet](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4115411)
- Isichenko (2021, "Costly Trading"): with a stable forecast and quadratic risk aversion the no-trade zone width scales as cost^(1/2), in contrast to cost^(1/3) in stochastic settings. The zone is not symmetric about the cost-free target: risk aversion pulls toward zero, and the cost-free target is the zone boundary with the larger absolute value. — [Isichenko, arXiv 2110.15239](https://arxiv.org/abs/2110.15239). This qualifies the cost^(1/3) scaling used in the v7 review.
- AQR: allowing deviation from the ideal portfolio has an effect similar to lowering rebalance frequency; "low, or high, turnover by itself should not be seen as a virtue", and net return over a long sample is the comparison that matters. — [AQR "Craftsmanship Alpha"](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Ratcliffe, Miranda and Ang (2017): moving from a one-day to a five-day trading horizon raises estimated momentum capacity from $65bn to $320bn and size capacity from $5tn to over $10tn. — [Ratcliffe-Miranda-Ang](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2861324)
- Briere, Lehalle, Nefedova and Raboun (ANcerno institutional trades): annual trading costs of 16 bps for size, 23 bps for value, 31 bps for investment and for profitability, and 222 bps for momentum; all five remain profitable net, but impact "substantially" reduces profitability for large portfolios. — [Briere et al. 2019](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3380239)
- Northfield: for low-turnover portfolios the alpha should be the time-weighted average forecast over the intended holding period. — [Shah 2007](https://www.northinfo.com/Documents/247.pdf)
- Volatility targeting adds turnover: conventional targeting exceeds 200% per year; the conditional rule halves it. — [Bongaerts-Kang-van Dijk 2020](https://repub.eur.nl/pub/130215/Bongaerts-Kang-van-Dijk-Conditional-volatility-targeting-2020-FAJ.pdf)

### Inferences

How slow is "slow" `[est]`
- The book trades 3.8% of gross per day, about 80% of gross per month and 9.6 times gross per year. If that counts buys plus sells, the average holding period is about 53 trading days.
- JKMP Portfolio-ML trades 0.32 / 3.60 = 8.9% of gross per month, about 1.07 times gross per year. That is about one ninth of the book's rate.
- Landier's frictionless benchmark for slow fundamental signals trades 12% per month and the optimal trader at scale about 6%.
- Caveats: those studies rebalance monthly, use quadratic costs, and have signal sets dominated after costs by value and quality. The comparison shows the direction and rough size of the gap, not a target.
- Landier's elasticity of 2 implies that halving the effective decay speed of the traded signal quadruples capacity. Slowing the SIGNAL (weighting persistent sleeves, smoothing) is therefore a stronger capacity lever than slowing the TRADE.

Why the two rejected experiments are consistent with the literature
- Experiment 1 shrank targets by cost. It cut cost per dollar 13-16% and gross Sharpe 10%. At $1bn the drag is 1.67%/yr, so a 15% cost saving is worth about 0.25%/yr, while 10% of gross return is about 0.75%/yr `[est]`. The trade could not pay at $1bn.
- By the fitted capacity curve, impact cost at $8bn is about 2.8 times that at $1bn, so the same saving would be worth about 0.7%/yr against the same 0.75%/yr loss `[est]`. The experiment is near break-even at $8bn and should be re-read there rather than discarded.
- The v6 per-name trade-rate test slowed illiquid names without changing the aim. It failed for the same reason: illiquid names carry more alpha per dollar (Section 2).

Recipe 5A. Rank-space hysteresis on top of partial adjustment
- Rule: a name enters the target only when its combined-score rank is inside the top (bottom) X%; once held, its target stays until the rank leaves the top (bottom) Y%, with Y > X. Partial adjustment and the ADV cap continue to apply.
- Parameters to pre-register: (X, Y) in {(20, 20) as control, (20, 35), (10, 50)}; weights within the held set by score or z/sigma per Recipe 2A.
- Expected effect: turnover -20 to -40% and gross SR -2 to -5% `[est]`. Robeco's result is a "slight" gross decrease for a net alpha moving from under one third of gross to about half of it, but that is for signals with 1,800% turnover; the book's signals are slower and already smoothed by theta, so gains will be smaller.
- Failure modes: fewer names held reduces breadth; interacts with the dust band; with a continuous rank target the hysteresis creates weight discontinuities at the band edge.

Recipe 5B. One-sided band (trade to the near edge)
- Rule: define a band around the cost-free target; when the holding is outside the band trade to the NEAR edge, not to the target (Isichenko's asymmetry).
- Parameters to pre-register: band half-width b_i = b0 x (c_i / c_bar)^e with e in {1/3, 1/2}; b0 at two values. The two exponents are the stochastic-forecast and stable-forecast scalings.
- Expected effect `[est]`: cost -5 to -15% at gross SR -0 to -3%.
- Failure mode: for fast sleeves the forecast is not stable and e = 1/2 over-widens bands.

Recipe 5C. Horizon-weighted aim (extends v7 R1.2)
- Replace each sleeve's score in the aim by its expected cumulative alpha over the realised holding period (about 50 days now), as Northfield prescribes. Fast sleeves then carry weight only in proportion to the alpha they deliver over that horizon.
- Parameters to pre-register: holding period H in {21, 63} days; sleeve decay half-lives from TRAIN autocorrelation of scores; no refitting of IC.
- Expected effect `[est]`: turnover -10 to -25% from lower weight on fast sleeves; gross SR -0 to -5%.
- Failure mode: mis-estimated half-lives on three years of data.

Recipe 5D. Best-trade-first under a turnover budget
- When the daily turnover budget or the ADV cap binds, rank candidate trades by (gap to target x expected remaining alpha over the holding horizon) / marginal cost and fill in that order.
- This matters only when constraints bind, which at $1bn is the illiquid tail and at $4-8bn is a large share of names.
- Parameters to pre-register: budget as a fraction of gross per day at three values; evaluation at $4bn and $8bn.
- Failure mode: systematically starves illiquid high-alpha names; no published evidence was found for this rule.

### Gaps
- Collin-Dufresne, Daniel and Saglam (2020) and Garleanu-Pedersen extensions were not re-read; the v7 review covers them.
- No published evidence was found for priority (best-trade-first) rules under a turnover budget.
- No study compares signal smoothing, position smoothing, trade-rate control, bands and turnover constraints on one data set.
- Novy-Marx and Velikov (2019) on banding versus rebalance frequency is covered in the v6 review and was not re-read.
- The Robeco implementation-lag figures were not retrieved in numbers; only the qualitative statement in the abstract was available.

---

## 6. Execution for a daily-rebalanced book without intraday data

### Takeaway
The closing auction has grown to about 9% of US daily volume and clears large size at roughly the half-spread, which makes it usable for part of the book's flow, but a 1%-of-ADV order sent entirely to the close would be about 11% of auction volume. Measured institutional costs rise from about 17 bps at 2% of daily volume to 28 bps at 6%, so spreading over days dominates concentrating in one print.

### Cited Findings

Closing auction size and cost
- Bogousslavsky and Muravyev (2023, J. Financial Markets 66): the closing auction was 7.48% of aggregate daily dollar volume in 2018, up from 3.11% in 2010; $15.2bn traded in the auction on a typical 2018 day. Volume in 3:30-3:55pm declined as a share of total, so volume moved to the last five minutes and the auction. — [Bogousslavsky-Muravyev, June 2021 version](https://static1.squarespace.com/static/6310c0b9bb63a25599f4418c/t/634ffc92f81e226b2c30654f/1666186387645/who-trades-at-the-close_June2021.pdf); [SSRN](https://www.ssrn.com/abstract=3485840)
- Auction price deviation from the 4pm midquote averages 8.12 bps: 20.6 bps for small stocks and 2.66 bps for large stocks. It exceeds 26 bps on 5% of stock-days, 63 bps on 1% and 195 bps on 0.1%. — [Bogousslavsky-Muravyev](https://static1.squarespace.com/static/6310c0b9bb63a25599f4418c/t/634ffc92f81e226b2c30654f/1666186387645/who-trades-at-the-close_June2021.pdf)
- Decomposition: average half-spread 7.56 bps and price impact 0.55 bps. For large stocks the two are 1.47 and 1.19 bps. The closing price matches the pre-close bid or ask in 68.5% of auctions. — [Bogousslavsky-Muravyev](https://static1.squarespace.com/static/6310c0b9bb63a25599f4418c/t/634ffc92f81e226b2c30654f/1666186387645/who-trades-at-the-close_June2021.pdf)
- Deviations rise with auction turnover (0.81 bps per 1% increase in turnover in their panel regression, larger for small stocks) and with volatility. They "reverse almost fully overnight", with one third to one half of the reversal in the first 30 minutes after the close where after-hours liquidity exists. — [Bogousslavsky-Muravyev](https://static1.squarespace.com/static/6310c0b9bb63a25599f4418c/t/634ffc92f81e226b2c30654f/1666186387645/who-trades-at-the-close_June2021.pdf)
- Auction volume is driven by ETF and passive ownership and spikes on index rebalances, month-ends and option expirations; it is LOWER around earnings announcements. Closing volume rises 20% relative to intraday volume after S&P 500 addition. — [Bogousslavsky-Muravyev](https://static1.squarespace.com/static/6310c0b9bb63a25599f4418c/t/634ffc92f81e226b2c30654f/1666186387645/who-trades-at-the-close_June2021.pdf)
- Side effect at the open: over the sample, turnover in the first 15 minutes fell 22% for S&P 500 stocks, effective spread rose about 10 bps and depth at the best quotes fell 63%. — [Bogousslavsky-Muravyev](https://static1.squarespace.com/static/6310c0b9bb63a25599f4418c/t/634ffc92f81e226b2c30654f/1666186387645/who-trades-at-the-close_June2021.pdf)
- Update to 2024: exchanges matched about $50bn per day in closing auctions, roughly 9% of daily volume, and about 20% on index-rebalance or expiration days. On Russell reconstitution day (28 June 2024) more than 34% of daily notional traded in the auction. — [BMLL, 24 June 2025](https://www.bmlltech.com/news/market-insight/into-the-close-unpacking-u-s-closing-auction-dynamics-and-the-impact-of-the-russell-reconstitution)
- Update to 2026: the NYSE closing auction averaged 605.5 million shares (about $43bn) per day in Q1 2026, with a single-day record of $230.5bn on 20 March 2026. — [NYSE Research, 8 April 2026](https://www.nyse.com/research/insights/behind-the-record-volumes-a-hidden-opportunity)
- Fill risk by size segment: unfilled interest at the closing price is about 3.3% of executed auction volume for Russell 1000 names and over 6.3% for names outside the Russell 1000; small and mid caps account for nearly two thirds of unfilled shares. — [NYSE Research, 8 April 2026](https://www.nyse.com/research/insights/behind-the-record-volumes-a-hidden-opportunity)

Cost by participation
- AQR live trades (August 1998 - September 2013): trading 2% of a stock's daily volume costs about 17 bps of market impact; trading 6% costs about 28 bps. AQR's example splits a 6% order over three days at about 2% per day. The 11 bps saving "is likely an overestimate". — [AQR "Craftsmanship Alpha", Exhibit 8](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- AQR also states that spreading trades over the day "rather than trading in a short period (e.g., around market close)" will likely lower impact. — [AQR "Craftsmanship Alpha"](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf). This is in tension with Bogousslavsky-Muravyev's conclusion that "the auction matches large volumes cheaply".
- The AQR figures (17 bps at 2%) are higher than the model value quoted in the v7 review from the 2018 version of Frazzini et al. (13.73 bps at 2%). The samples and definitions differ (binned averages to 2013 versus a fitted model to 2016).
- JKMP's calibration to Frazzini et al.: 0.1% impact for trading 1% of daily dollar volume, expected volume taken as the six-month average. — [JKMP](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)
- Other researchers' cost assumptions: Robeco uses 25 bps per trade as a conservative figure for developed large and mid caps; Bongaerts et al. use 25 bps for factor trades, against the 18 bps average shortfall in Frazzini et al. (2015). — [Robeco 2022](https://cdn.uc.assets.prezly.com/dbe30eb4-6b9f-4e86-9633-a127867241f6/-/inline/no/20220530%20Beyond%20Fama-French%20-%20alpha%20from%20short-term%20signals%20May%202022%20-%20GB.pdf); [Bongaerts-Kang-van Dijk 2020](https://repub.eur.nl/pub/130215/Bongaerts-Kang-van-Dijk-Conditional-volatility-targeting-2020-FAJ.pdf)
- ANcerno-based anomaly costs: 16 to 31 bps per year for slow factors and 222 bps for momentum. — [Briere et al. 2019](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3380239)

Signal delay
- Robeco's short-term composite is reported as robust to implementation lags of several days. — [Blitz et al. 2023](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4115411)
- A practitioner blog reports that next-open execution degrades a short-horizon strategy because trades "suffer from overnight moves". This is a single-strategy, non-peer-reviewed source and is weak evidence. — [Concretum Group](https://concretumgroup.com/when-execution-delays-erode-short-term-alpha/)

### Inferences

Participation arithmetic `[est]`
- At $1bn the book trades 0.038 x 1.247 x $1bn = $47m per day, about $26k per name across 1,850 names. At $8bn it is about $380m per day.
- With the auction at 9% of daily volume, an order of 1% of ADV sent entirely market-on-close is about 11% of auction volume. At 0.25% of ADV it is about 3%.
- Bogousslavsky-Muravyev's averages describe the marginal small participant. They do not measure the cost of being 11% of the auction. For small caps the average deviation is already 20.6 bps.
- The book's 13.5 bps per dollar at $1bn sits between the auction's all-in average (8.1 bps) and AQR's 17 bps at 2% participation, which is consistent with low average participation plus an illiquid tail.

Recipe 6A. Split execution by liquidity bucket
- Russell 1000-type names: send up to a fixed share of the daily order to the close, capped at a fraction of expected auction volume; work the rest as full-day VWAP.
- Smaller names: no market-on-close; VWAP over one to three days, capped at p% of ADV per day.
- Parameters to pre-register: close share in {0, 25%, 50%}; auction participation cap 3% of expected auction volume `[est]`; multi-day horizon 1 to 3 days; p in {1%, 2%}.
- Research constraint: with daily OHLCV only, the backtest can model the close print but not VWAP or intraday impact. Both arms must therefore be run under two cost models (the current one and a participation-based one at 17 bps per 2% ADV) and accepted only if the ranking agrees.
- Avoid closes on index-rebalance, month-end and expiration days for discretionary flow unless providing liquidity, since deviations rise with auction turnover.
- Failure modes: the backtest assumes the book's own order does not move the close; unfilled limit-on-close interest (3.3% to 6.3%) leaves residual positions; trading only at the close correlates the book's flow with passive flow.

Signal delay cost `[est]`
- Under partial adjustment at 5% per day the average age of information in positions is about 19 days, so one extra day of delay is a small fraction of total staleness.
- For a signal with half-life H days, the alpha lost to one day of delay is about 1 - 2^(-1/H): 13% for H = 5, 3.2% for H = 21, 1.1% for H = 63, 0.5% for H = 126.
- The cost of trading at the next close instead of the next open is therefore material only for the fast sleeves, and these are also the capacity-bound sleeves.
- Test to pre-register: rerun the accepted construction with signals lagged 1, 2 and 3 extra days and report gross SR by sleeve. A sleeve losing more than 10% per day of lag should be classed as execution-sensitive.

### Gaps
- Almgren et al. (2005), Bucci et al. (2019), Toth et al. (2011) and Frazzini-Israel-Moskowitz (2018) were not re-read; the v6 and v7 reviews carry their numbers.
- No study was found that measures implementation shortfall by ADV participation separately for US large, mid and small caps after 2020.
- No study measures the cost of market-on-close orders as a function of the order's share of auction volume.
- No peer-reviewed estimate of overnight gap cost (decision at close t, execution at open or close t+1) for medium-horizon equity signals was retrieved.
- Auction share by market-cap segment after 2018 was not found.

---

## 7. Short side: borrow cost, proxies, exclusion rules and index hedges

### Takeaway
Borrow fees are concentrated: about 80% of US stocks cost under 32 bps per year to short, while the top fee decile averaged 568 bps, and recent work finds that anomaly long-short returns vanish once fees are charged or once the high-fee 12% of stock-dates is dropped. Without fee data the best-documented proxy is short interest divided by institutional ownership, which the book can build from data it already has.

### Cited Findings

Fee distribution
- D'Avolio (2002): 91% of stocks are general collateral with average fees of 17 bps per year; 9% (about 200 stocks per day) are specials averaging 4.30% per year; the 16% of CRSP stocks that may be impossible to borrow are under 0.6% of market value. — [D'Avolio 2002, via AQR](https://www.aqr.com/Insights/Research/Journal-Article/The-Market-for-Borrowing-Stock)
- D'Avolio: institutional ownership explains about 55% of the cross-sectional variation in loan supply. The probability of being special falls with size and institutional ownership and rises with disagreement proxies (analyst dispersion, message-board activity). — [D'Avolio 2002, via AQR](https://www.aqr.com/Insights/Research/Journal-Article/The-Market-for-Borrowing-Stock)
- Drechsler and Drechsler (2004-2012, over 95% of CRSP stocks): each of the eight cheapest fee deciles averages below 32 bps per year; the ninth averages 75 bps; the tenth averages 568 bps. Market capitalisation of the ninth and tenth deciles averaged about $1.1tn and $435bn. — [Drechsler-Drechsler, NBER w20282](https://www.nber.org/system/files/working_papers/w20282/w20282.pdf)

Fees and anomaly returns
- Drechsler-Drechsler: the cheap-minus-expensive portfolio earns 1.43% per month gross (t = 4.99), 0.91% net of fees, with a four-factor alpha of 1.53% (t = 7.06). The tenth fee decile returns -0.68% per month gross and -0.16% per month net of the fee. — [Drechsler-Drechsler](https://www.nber.org/system/files/working_papers/w20282/w20282.pdf)
- Eight anomalies (value, momentum, idiosyncratic volatility, composite equity issuance, financial distress, max return, net issuance, gross profitability) "effectively disappear" within the 80% of stocks with low fees, except gross profitability. Idiosyncratic volatility earns 87 bps per month unconditionally, -5 bps in the low-fee bucket and 176 bps in the high-fee bucket. — [Drechsler-Drechsler](https://www.nber.org/system/files/working_papers/w20282/w20282.pdf)
- For every anomaly the extreme short decile has an average fee above 140 bps per year. Fee decile is positively related to idiosyncratic volatility, distress, max return, net issuance and the magnitude of momentum return; book-to-market and gross profitability fall in the high-fee deciles. — [Drechsler-Drechsler](https://www.nber.org/system/files/working_papers/w20282/w20282.pdf)
- Size and liquidity do not explain the pattern: low-fee portfolios matched to the high-fee ones on size and anomaly characteristic, or on liquidity, behave like the low-fee bucket. — [Drechsler-Drechsler](https://www.nber.org/system/files/working_papers/w20282/w20282.pdf)
- Muravyev, Pearson and Pollet (2025, Journal of Finance 80(6):3639-3694), 162 anomalies: the average long-short return is 0.14% per month before short-sale costs, "and the returns are due to the short leg"; it is -0.01% after borrow fees. "Anomalies are not profitable even before fees if the high-fee observations, representing 12% of stock dates, are excluded." — [Muravyev-Pearson-Pollet 2025](https://onlinelibrary.wiley.com/doi/10.1111/jofi.13501); [abstract](https://ideas.repec.org/a/bla/jfinan/v80y2025i6p3639-3694.html)
- Kim and Lee (2023, Accounting and Finance), 14 anomalies, January 2006 - December 2017: the combined shorting cost (loan fees plus the implicit cost of short-leg stocks being unavailable) is almost 40% of gross long-short returns. — [Kim-Lee 2023](https://onlinelibrary.wiley.com/doi/abs/10.1111/acfi.12953); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3519706)

Proxy without fee data
- Drechsler-Drechsler build SIRIO = short interest / institutional ownership, "a rough measure of the demand for shorting (short interest) relative to available lending supply (institutional ownership)". Over 1980-2012, the SIRIO-sorted cheap-minus-expensive portfolio earns 1.48% per month with a four-factor alpha of 1.54%, close to the fee-sorted result. It is "a noisy signal", and with this proxy the low-bucket anomaly returns remain mostly significant. — [Drechsler-Drechsler](https://www.nber.org/system/files/working_papers/w20282/w20282.pdf)
- Japanese evidence (centralised lending market, six months of daily data): hard-to-borrow stocks have higher short interest ratios, borrowing costs, price-to-book and turnover, and lower institutional ownership; institutional ownership is negatively related to borrow cost in both groups. This is one country and a short sample. — [Khan 2025, IJFS](https://ideas.repec.org/a/gam/jijfss/v13y2025i1p16-d1581921.html)

Long and short leg asymmetry, and index hedges
- Blitz, Baltussen and van Vliet (US, 1963-2018): single-factor Sharpe is about 0.5 on both legs, but combining five factors gives 1.1 for long legs and under 0.7 for short legs. By size, five-factor long legs reach 0.72 in large caps against 0.36 for short legs, and 1.13 against 0.94 in small caps. — [Blitz-Baltussen-van Vliet 2020](https://repub.eur.nl/pub/130144/Repub_130144_O-A.pdf)
- In spanning tests the alpha of each long leg is positive and mostly significant, while short-leg alphas range "from nearly zero to significantly negative"; spanning of the short legs by the long legs cannot be rejected. In the maximum-Sharpe combination the weight on long legs is 17.8% for large caps and 64.9% for small caps. — [Blitz-Baltussen-van Vliet 2020](https://repub.eur.nl/pub/130144/Repub_130144_O-A.pdf)
- Their implementation: legs are measured against a market-index hedge, so the "long leg" is long stocks hedged with liquid index derivatives. Short positions are also less liquid (short volume about 24% of NYSE and 31% of Nasdaq volume, citing Diether-Lee-Werner) and subject to recall, squeeze and legal limits. — [Blitz-Baltussen-van Vliet 2020](https://repub.eur.nl/pub/130144/Repub_130144_O-A.pdf)
- The two literatures disagree on where the return sits. Muravyev-Pearson-Pollet find that gross anomaly returns "are due to the short leg"; Blitz et al. find that most added value comes from the long legs. — [Muravyev-Pearson-Pollet 2025](https://onlinelibrary.wiley.com/doi/10.1111/jofi.13501); contradicted by [Blitz-Baltussen-van Vliet 2020](https://repub.eur.nl/pub/130144/Repub_130144_O-A.pdf)
- Modelling convention elsewhere: Boyd et al. charge 5% per year over fed funds on shorts in simulation and 7.5% in the optimiser's forecast. — [Boyd et al. 2024](https://arxiv.org/pdf/2401.05080)

### Inferences

Reconciling the two findings `[est]`
- Muravyev-Pearson-Pollet average over a universe that includes microcaps, where fees are extreme. Blitz et al. use Fama-French 2x3 portfolios, value-weighted within size halves, where high-fee names carry little weight. Both can hold: gross short-leg alpha is concentrated in names that cost most to short.
- For a top-3000 universe the relevant question is how much of the book's short alpha comes from names in roughly the top fee decile. This is measurable with the SIRIO proxy.

Fee drag estimate for the book `[est]`
- Short notional is about 0.62 x NAV. If shorts were spread evenly over Drechsler's fee distribution, the average fee would be 0.8 x 17 + 0.1 x 75 + 0.1 x 568 = about 78 bps, which is 0.49% of NAV per year or 0.12 Sharpe units at 4.13% volatility.
- Excluding the top fee decile lowers this to about 23 bps on short notional, 0.15% of NAV, 0.04 Sharpe units. The saving is about 0.08 Sharpe units before counting lost alpha.
- Bias in both directions: a top-3000 universe has fewer specials than all of CRSP (estimate too high), but anomaly shorts over-sample high-fee names, with every anomaly's short decile above 140 bps (estimate too low).
- The current backtest's financing assumption should be compared with these figures. If it charges a flat general-collateral rate, net Sharpe is over-stated by up to about 0.1.

Recipe 7A. Build a hard-to-borrow proxy and report short alpha by bucket (diagnostic first)
- Proxy: SIRIO_i = short interest_i / institutional shares held_i, from exchange short interest and 13F. Institutional ownership below a floor (for example 5% of shares) is treated as top bucket.
- Report on TRAIN: share of short notional, gross alpha and turnover by SIRIO decile; the same for short interest / float as a second proxy.
- Parameters to pre-register: decile breakpoints within the top-3000 universe; 13F lag (45 days after quarter-end); short-interest publication lag.
- Failure mode: 13F is quarterly and lagged, and SIRIO is itself a known return predictor, so bucket returns mix fee effects and signal effects.

Recipe 7B. Stress the backtest with a fee schedule
- Schedule by SIRIO decile: deciles 1-8 at 25 bps, decile 9 at 75 bps, decile 10 at 570 bps, from Drechsler's averages. Add a second stress at twice the decile 9-10 fees.
- Accept construction changes only if they hold under both schedules.
- Failure mode: fees vary through time and spike in squeezes; a static schedule understates tail cost.

Recipe 7C. Exclusion or down-weighting of expensive shorts
- Rule arms: (a) no new shorts in SIRIO decile 10; (b) short weight multiplied by 0.5 in decile 10 and 0.75 in decile 9; (c) control.
- Evaluate NET of the 7B fee schedule, not gross. On gross returns this rule will look harmful, because gross short alpha is concentrated in these names.
- Expected effect `[est]`: gross SR -3 to -8%; fee drag -0.05 to -0.08 Sharpe units; net effect near zero to slightly positive, plus lower squeeze and recall risk.
- Failure mode: the removed names are also high specific-volatility names, so the book's short side becomes lower-volatility than the long side and neutralisation must be re-checked.

Recipe 7D. Partial index or sector hedge in place of single-name shorts
- Rule: run single-name shorts at a fraction s of long notional and cover the remaining beta and sector exposure with index futures or sector ETFs.
- Parameters to pre-register: s in {1.0, 0.75, 0.5}; hedge instruments fixed in advance; beta and sector neutrality enforced at the combined level.
- Expected effect `[est]`: single-name turnover, impact and borrow cost fall roughly in proportion to (1 - s) on the short side; short alpha falls by the same proportion; residual factor risk rises because index hedges do not offset style exposures. Blitz et al. imply the Sharpe loss from dropping short legs is small for standard factors in large caps; Muravyev-Pearson-Pollet imply the lost alpha was mostly not collectable net of fees anyway.
- Capacity effect `[est]`: futures capacity is far above single-stock capacity, so the binding side of the book becomes the long side only, which raises capacity most at $4-8bn.
- Failure modes: the combined book is no longer dollar-neutral in single names; long-only factor exposure brings size and style risk that the index hedge does not remove; volatility rises, so compare at equal ex-ante risk.

### Gaps
- Fails-to-deliver, the Reg SHO threshold list and options-implied borrow cost as hard-to-borrow proxies were not researched before the search quota ran out.
- The accuracy of SIRIO or short interest / float as a classifier of high-fee names (hit rate, false positives) was not found; Drechsler-Drechsler report portfolio returns, not classification accuracy.
- Muravyev-Pearson-Pollet's fee data source, sample period and results by size segment were not retrieved; only the abstract was read.
- No study was found that measures Sharpe and capacity for a market-neutral book with a partial futures or ETF hedge against a full single-name short book.
- Fee levels after 2017 and their distribution within the largest 3000 names were not found.
- Recall risk and locate failure rates were not quantified by any retrieved source after D'Avolio.

---

## 8. Capacity: measurement, levers and expected decay with AUM

### Takeaway
Theory and calibration agree that capacity is driven by three things in a known order of strength: signal persistence (elasticity 2), liquidity of the traded names (elasticity 1), and frictionless Sharpe. The book's own capacity curve fits a square-root law closely and implies that dollar profit is still rising at $8bn, so the capacity question is which Sharpe floor to accept rather than where profit peaks.

### Cited Findings
- Landier, Simon and Thesmar: closed-form performance-to-scale frontier in a Garleanu-Pedersen model. Sharpe decay with scale is slower for strategies that trade more liquid stocks, use signals that fade slowly, and have strong frictionless performance. For a 30% Sharpe reduction, reachable scale (in dollar volatility) rises with the square of frictionless Sharpe and falls with impact and with the square of the signal's fading speed. — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2585399)
- Calibration: "quality" (operating cash flow to assets) has capacity an order of magnitude above value, low volatility and repurchases, with reachable dollar volatility of about $80bn in large caps and $10bn in mid caps. High-frequency signals have very small capacity compared with fundamental signals. — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4)
- Liquidity regime matters: the Sharpe loss from trading at $5bn of dollar volatility could be as large as 0.8 with early-1990s liquidity and no more than 0.3 with early-2000s liquidity. Mid-cap price impact fell by about a factor of 10 between the 1990s and the 2000s. — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4)
- Mis-specified liquidity is costly: a trader who believes mid caps are as liquid as large caps suffers "a huge impact on performance", while smaller errors matter less. Under-estimating the number of competitors trading the same signal strongly reduces performance. — [Landier-Simon-Thesmar 2015](https://www.aeaweb.org/conference/2016/retrieve.php?pdfid=21020&tk=BGQnasd4)
- JKMP: the implementable frontier is strictly worse for larger investors, for two reasons: higher impact, and the opportunity cost of trading less. At 14% volatility, net excess return is 22% at $1bn and 19% at $10bn. — [JKMP](https://www.aeaweb.org/conference/2024/program/paper/SYTF3aaz)
- Ratcliffe, Miranda and Ang: momentum capacity $65bn at a one-day trading horizon and $320bn at five days. — [Ratcliffe-Miranda-Ang 2017](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2861324)
- Briere et al.: measured institutional costs for the Fama-French factors and momentum are well below earlier academic estimates, but market impact substantially reduces profitability for large portfolios. — [Briere et al. 2019](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3380239)
- Hedge fund diseconomies of scale. Bollen, Joenvaara and Kauppila (2024): for each standard deviation of lagged skill, doubling fund size lowers alpha by 46 bps per year before 2008 and by 17 bps after. More skilled funds lose more from scale. — [Bollen-Joenvaara-Kauppila 2024](https://cfr.ivo-welch.org/forthcoming/papers/bollen2024decreasing.pdf)
- A separate study finds that hedge fund gross alpha falls with both fund size and the aggregate size of the fund's style, and that fund-level models understate the scale effect by 55 bps. Funds trading a larger share of their portfolio or holding less liquid securities face greater diseconomies. — [Decreasing returns to scale and skill in hedge funds, J. Banking and Finance 2023](https://www.sciencedirect.com/science/article/pii/S0378426623002005)

### Inferences

Reading the book's capacity curve `[est]`
- Fit: net SR = 1.58 - 0.175 x sqrt(NAV in $bn). Fitted values 1.46 / 1.41 / 1.33 / 1.23 / 1.09 against reported 1.45 / 1.41 / 1.36 / 1.24 / 1.08.
- Dollar profit at 4.13% volatility: $30m / $58m / $112m / $205m / $357m at $0.5 / 1 / 2 / 4 / 8bn. Profit is still rising at $8bn.
- The marginal $4bn from $4bn to $8bn earns a net Sharpe of about 0.92 (incremental profit $152m on incremental risk $165m).
- Net SR 1.0 is reached at about $11bn by the fit. This extrapolates beyond the tested range and the v7 caveats apply (ADV caps, worse-than-square-root tail).
- Compared with Bollen et al.'s 17 bps of alpha per doubling (post-2008), the book loses about 0.05 to 0.16 Sharpe units per doubling, which at 4.13% volatility is 20 to 66 bps per year per doubling. The book's modelled decay is at or above the hedge fund average, so the capacity curve is not obviously optimistic.

Levers ranked by the evidence `[est for the ranking]`
1. Signal persistence. Elasticity 2. Weight the aim toward slow sleeves by horizon (Recipe 5C). This is the only lever with a quadratic payoff.
2. Trading speed. Re-tune the trade rate per NAV instead of holding theta at 0.05. Landier's optimal trader halves turnover by the scale at which Sharpe has halved; Ratcliffe et al. gain about five times capacity from a five-times slower horizon.
3. Liquidity of names. Elasticity 1. ADV-proportional position caps (Section 2) and, at $4-8bn, liquidity-conditional targets.
4. Short-side substitution. Partial index hedge (Recipe 7D) removes the short side's impact and borrow constraint.
5. Frictionless Sharpe. Capacity rises with its square, so library improvements raise capacity as well as Sharpe.

Recipe 8A. Capacity replay with re-tuned speed
- Extends v7 R4.2. For NAV in {1, 2, 4, 8} $bn, run theta in {0.05, 0.035, 0.025, 0.0175} and report net SR, dollar profit, share of trades capped and participation p50 / p95.
- Parameters to pre-register: the theta grid; the rule for choosing theta per NAV (maximum net SR on TRAIN, then frozen); the cost model and one stress model.
- Expected effect `[est]`: no gain at $1bn (the earlier theta 0.03 test was -0.001); +0.03 to +0.10 net SR at $8bn, where impact is about 0.49 Sharpe units and slower trading reduces it roughly with the square root of turnover.
- Failure mode: slower trading raises staleness in fast sleeves; combine with Recipe 5C so fast sleeves are down-weighted rather than traded late.

Recipe 8B. Report capacity as a frontier, not a point
- For each construction variant report net SR at $1bn and $8bn and the fitted square-root slope. A change that lowers the slope from 0.175 to 0.14 is worth +0.10 Sharpe at $8bn and +0.035 at $1bn `[est]`.
- Pre-register the decision rule: variants are ranked on net SR at the intended AUM, with a floor on net SR at $1bn.

Liquidity model risk
- Landier's warning on mis-specified impact applies to the illiquid tail, where the book's cost model is least constrained by data. Until live fills exist, accept capacity claims only if they hold under the stress cost model.

Cross-question recipe index for the sprint plan

All effect sizes are `[est]` unless a source is named in the section. "Primary cell" is the AUM at which the experiment should be judged.

| ID | Technique | Primary cell | Expected net SR effect | Main pre-registered parameters | Main failure mode |
|---|---|---|---|---|---|
| 1A | Track heuristic target under risk and cost | $1bn and $4bn | +0.03 to +0.08 | cost-vs-tracking scalar (3 values), ADV rate p | substitutes liquid for illiquid names, loses name-specific alpha |
| 1B | Regularised alpha optimiser (risk target, soft leverage and turnover, robust terms) | $4bn | uncertain; prior attempt 0.54 vs 1.33 | alpha scaling arm, rho, kappa, penalties from TRAIN multipliers | repeats Experiment 2; tuning overfit |
| 2A | Weights z / sigma^k | $1bn | gross +0 to +10%, cost -5 to -10% | k in {0, 0.5, 1}, equal ex-ante risk | implicit size and low-vol tilt |
| 2B | IC by volatility and liquidity bucket | diagnostic | none | tercile definitions | none |
| 3A | Ex-ante risk target with band | $1bn | -0.01 to -0.04 SR, return scales with target | sigma target, band 15%, L max | procyclical deleveraging; risk under-forecast |
| 3B | Conditional vol scaling, momentum sleeves | $1bn | small at book level | quintile rule, scale-down only | effect may not survive neutralisation |
| 4A | Partial industry neutralisation at equal risk | $1bn | -0.05 to +0.05 | phi in {0, 0.5, 1} | removes industry momentum; turnover |
| 4B | Hedge unpriced risk per sleeve | $1bn | small | covariance window, hedge ratio | adds gross and turnover |
| 5A | Rank-space buy/hold hysteresis | $1bn and $4bn | turnover -20 to -40%, gross -2 to -5% | (X, Y) pairs | breadth loss |
| 5B | One-sided cost-scaled band | $4bn | cost -5 to -15% | exponent 1/3 or 1/2, b0 | over-wide bands for fast sleeves |
| 5C | Horizon-weighted aim | $4bn | turnover -10 to -25% | H, sleeve half-lives | half-life estimation error |
| 5D | Best-trade-first under budget | $8bn | unknown | budget levels | starves illiquid high-alpha names |
| 6A | Split execution, close plus VWAP | $4bn | depends on cost model | close share, auction cap | backtest cannot see own impact |
| 7A | SIRIO proxy and short alpha by bucket | diagnostic | none | decile breakpoints, data lags | proxy is also a return signal |
| 7B | Fee-schedule stress | all | -0.04 to -0.12 versus flat GC | fee by decile | static fees understate squeezes |
| 7C | Down-weight expensive shorts | $1bn | about 0 net | multipliers by decile | short side becomes lower-vol |
| 7D | Partial index hedge for shorts | $8bn | capacity up, SR uncertain | s in {1, 0.75, 0.5} | residual style risk |
| 8A | Re-tune trade rate per NAV | $8bn | +0.03 to +0.10 | theta grid per NAV | staleness in fast sleeves |
| 8B | Capacity frontier reporting | all | none | decision rule | none |

### Gaps
- No study of capacity or Sharpe decay specifically for diversified equity market-neutral funds in the $1-8bn range was found.
- The hedge fund scale papers were read at abstract or single-table level; strategy-level estimates for equity market neutral were not retrieved.
- Landier et al.'s exact formula could not be extracted cleanly from the PDF text; only its stated elasticities and calibrated results are used.
- Evidence on how crowding (many funds trading the same signals) changes capacity in 2020-2026 was not retrieved.
- Frazzini-Israel-Moskowitz break-even sizes and Pastor-Stambaugh-Taylor are in the v7 review and were not re-read.


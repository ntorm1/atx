# Improving existing equity alpha signals (signal engineering) for a US long/short, low-turnover, high-capacity book, as of September 2026

Scope and conventions. These notes extend `mega-alpha-20260926/v6-literature.md` (sections 3, 6) and `platform-20260928/literature-v7.md` (S0, S1, S6); findings already stated there are not repeated except where a newer source contradicts them (flagged **CONTRADICTS v6/v7** or **EXTENDS v6/v7**). Read-only research: nothing was run on book data. `[est]` marks my own arithmetic or judgement, roughly +/-50%, not a sourced number. "Free parameters" counts numbers that would have to be chosen (or fitted) beyond the house conventions (rank or FF12 group rank, 21-day linear decay, windows 21/63/252). "Overfit risk" is graded Low / Medium / High by how many researcher degrees of freedom the change opens and how thin the out-of-sample evidence is. Numbers quoted from PDFs were read from the primary text unless marked "secondary summary".

Headline for the sprint planner (detail in the sections):

| # | Change (no new data) | Expected effect on the sleeve | Params | Overfit risk | Status vs v6/v7 |
|---|---|---|---|---|---|
| 1 | Rank every value, profitability and investment member within industry (FF12 now, FF49 as the literature's best case) | Sharpe up via lower vol; up to +0.42 SR for value-weighted value in the source study; turnover slightly up | 0 (1 discrete choice) | Low | Extends v6 3.9 |
| 2 | Replace "latest EAR" as theme core by the 12-month sum of earnings-announcement-window returns | Top-1000 US 2010-2024: 4.90%/yr, t 2.94, vs 1.15%, t 0.67 for latest EAR; turnover 430% vs 976%/yr one-way | 0-1 | Low-Medium | **Contradicts** v6 3.1 |
| 3 | Profitability numerator = operating profit with R&D added back; stop adding quality composites | Subsumes cash-based OP in cross-section regressions (t 5.69 vs 2.57), fully after 2000 | 0 | Low | **Contradicts** v6 3.6 |
| 4 | Treat residual momentum as a volatility/beta clean-up, not as new alpha | Firm-specific momentum 10.10%/yr before 2000, 3.45%/yr (t 1.53) after | 0 | Medium | **Contradicts** v6 3.5 |
| 5 | DTC with a 126-252 day turnover denominator, ranked within size bucket | Large-cap DTC edge over SI ratio about 0.25-0.29%/mo in source; coefficient unchanged with 6-12 month turnover | 0 | Low | Extends v6 3.8 |
| 6 | Reversal: industry-relative, earnings-window returns removed from the lookback; use as trade-timing screen | Value-weighted: 31 bps/mo (t 1.68) raw vs 108 bps/mo (t 9.35) for the cleaned version, gross | 0-1 | Medium (cost) | Extends v6 3.7 |
| 7 | Low-risk theme: keep only members that survive beta/vol projection; upgrade the beta used in the neutraliser | Alpha of value-weighted BAB vs FF5 is 24 bps/mo, t 1.63 | 0 | Low | Extends v6 3.3 |
| 8 | Do not time themes (value spread or factor momentum) on 3-13 years | Timing needs standalone SR > 0.1 to earn any weight; SR 0.5 justifies only +/-12% tilts | n/a | High if done | Confirms v6 lever X |

---

## 1. Value: industry-relative, composite, timely price, intangibles, enterprise multiples, value conditioned on quality

### Takeaway
The two changes with the strongest and most replicated support are (i) measuring value within industry and (ii) averaging several price ratios built on the current price; both add zero fitted parameters. Intangible-adjusted book and enterprise multiples improve value mostly by importing a profitability tilt the book already holds in its own theme, so their incremental value at book level is small.

### Cited Findings
- Asness, Porter and Stevens (2000) split book-to-market, cash-flow-to-price, size, change in employees and past-return measures into a within-industry component (firm minus industry average) and an across-industry component (the industry average), on all NYSE/AMEX/NASDAQ stocks June 1963 to November 1998, and find the split gives better proxies for expected returns than the raw characteristic — [AQR working paper page](https://www.aqr.com/Insights/Research/Working-Paper/Predicting-Stock-Returns-Using-IndustryRelative-Firm-Characteristics); [SSRN](https://dx.doi.org/10.2139/ssrn.213872)
- Cohen and Polk (1998) were the first to build intra- and inter-industry book-to-market portfolios; their motivation is that book-to-market depends on accounting conventions that differ by industry, so the raw ratio is a noisy proxy — [SSRN 7483](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=7483)
- Ehsani, Harvey and Li (2023, FAJ), US 1963-2020, five factors (size, value, profitability, investment, momentum), equal/rank/value weights, 5 to 49 Fama-French industries: keeping the sector component produces a better long-short factor in only 20% of bootstrap trials (analytical prediction 29%), but a better long-only factor in 78% of trials — [paper PDF](https://people.duke.edu/~charvey/Research/Published_Papers/P165_Is_sector_neutrality.pdf)
- Same paper: "the largest improvement of 0.42 units in annualized Sharpe ratio occurs for the value factor that hedges out exposure to 49 industries" (value-weighted long-short); the entire predictive power of market-wide book-to-market comes from its within-sector component — [paper PDF](https://people.duke.edu/~charvey/Research/Published_Papers/P165_Is_sector_neutrality.pdf)
- Same paper, the decision rule: the across-industry component is redundant when SR_across / SR_within < correlation(across, within); for rank-weighted momentum and value-weighted profitability, investment and momentum the ratio exceeds the correlation (so some industry exposure is optimal in theory), yet neutralising still did not lower their Sharpe ratios because actual sector exposure far exceeds the optimal amount — [paper PDF](https://people.duke.edu/~charvey/Research/Published_Papers/P165_Is_sector_neutrality.pdf)
- Israel, Jiang and Ross (2017, "Craftsmanship Alpha", JPM): for a naive top-third/bottom-third B/P long-short portfolio, "only 32% of the risk" comes from stock selection within industries; the rest is market and industry exposure. Industry neutrality "often reduces volatility while keeping long-run returns broadly unaffected, which implies higher portfolio Sharpe ratios", but "typically results in higher turnover" — [AQR PDF](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Same paper: a composite of five value measures (all on current prices) is compared with B/P alone on rolling five-year Sharpe ratios, US long-short 1990-2015; changing the selection cutoff from 50% to 33% gives "virtually the same Sharpe ratio" (more return, more risk) — [AQR PDF](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Asness and Frazzini (2013): standard HML uses prices that are 6 to 18 months old; value portfolios using the most timely price earn alphas of 305 to 378 bps per year against a five-factor model that already contains standard HML, market, size, momentum and short-term reversal — [SSRN 2054749](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2054749)
- Eisfeldt, Kim and Papanikolaou (2022): the intangible value factor outperformed Fama-French HML by 2.4% per year with a standard deviation of 5.9% (information ratio about 0.40), was 78% correlated with HML, and the outperformance was larger after the financial crisis (numbers from a secondary summary of the paper) — [SSRN 3720983](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3720983); [NBER w28056](https://www.nber.org/papers/w28056)
- Novy-Marx and Medhat (2025, NBER w33601) test twelve "alternative value" measures (E/P, CF/P, FCF/P, S/P, EBITDA/EV, net payout yield, clean-surplus payout, retained earnings, three intangible-adjusted book measures, profits-to-price) and conclude that profitability tilts "explain all the abnormal performance" of alternative value, including intangible-adjusted versions; these are "factor rotations" that "do not yield any actual improvements in the investment opportunity set" once profitability is traded directly — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Same paper: profitability explains half of value's post-2007 underperformance; their profitability factor earned 31 bps/month from July 1963 to December 2006 (t 3.96) and 60 bps/month over the following 17 years (t 4.84) — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Loughran and Wellman (2011, JFQA): enterprise multiple = (equity + debt + preferred - cash) / EBITDA; their EM factor earns 5.28% per year, July 1963 to 2009, and remains significant against the four-factor model — [JFQA](https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/abs/new-evidence-on-the-relation-between-the-enterprise-multiple-and-average-stock-returns/5CD22A12A06AFCDC5233E477757FB659); [SSRN 1481279](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1481279)
- Blitz and Hanauer (2021, JPM) "resurrect" value with a composite of EBITDA/EV, cash-flow-to-price, net payout yield and book-to-market with capitalised R&D, each converted to a robust cross-sectional z-score capped at +/-3 and averaged; financials are excluded because EBITDA/EV and CF/P are not meaningful for them; both the enhanced factor and HML "suffer in recent years" (2018-2020) — [Quantpedia summary](https://quantpedia.com/resurrecting-the-value-premium/); [SSRN 3705218](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3705218)
- Piotroski and So (2012, RFS): value/glamour returns are concentrated among firms where the price multiple is incongruent with fundamental strength (FSCORE): long value with high FSCORE, short glamour with low FSCORE — [RFS](https://academic.oup.com/rfs/article-abstract/25/9/2841/1589567)
- Kessler, Scherer and Harries (2020, JPM) evaluate 3,168 alternative implementations of a value portfolio and use the dispersion of Sharpe ratios to rank design choices and to count the degrees of freedom consumed in strategy development — [SSRN 3593557](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3593557)
- Asness, Chandra, Ilmanen and Israel (2017): valuation-spread timing of value, momentum and low beta, when implemented in a multi-style portfolio that already contains value, gives "somewhat disappointing" results with no robust outperformance — [AQR](https://www.aqr.com/Insights/Research/Journal-Article/Contrarian-Factor-Timing-is-Deceptively-Difficult)

### Inferences
Technique cards (what to change; evidence; effect; overfit risk; parameters):

| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| V1 | Write every value member as `group_rank(decay_linear(x,21), grp_ff12)`; pre-register FF12. FF49 is the literature's best case but thin groups in a 3000-name universe make ranks coarse | Ehsani-Harvey-Li: long-short value gains in about 80% of bootstraps; best case +0.42 SR (VW, 49 industries, 1963-2020). Craftsmanship: same return, lower vol, more turnover. `[est]` +0.05 to +0.15 SR on the value sleeve, +0.01 to +0.03 at book level because the composite is already partly industry-projected | Low | 0 (one discrete choice, the classification) |
| V2 | One composite = mean of within-industry ranks of 4-5 ratios (CF/P, E/P, S/EV or S/P, EBITDA/EV, B/P) | AQR and Robeco both use 4-5 measure composites; no single-ratio dominance is documented; Novy-Marx-Medhat show all alt measures are value plus profitability | Low (pre-declare the list; do not select members on TRAIN) | 0 |
| V3 | Current price in the denominator, as-of fundamentals in the numerator; if smoothing is needed for turnover, smooth the final rank, not the fundamental | Asness-Frazzini: 305-378 bps/yr alpha over lagged-price HML | Low | 0 |
| V4 | Value conditioned on quality: a member equal to rank(value) x rank(profitability) or the mean of ranks restricted to "incongruent" names | Piotroski-So (US, through 2010); Novy-Marx-Medhat: value plus profitability traded directly is the efficient version | Medium: the product form is a new nonlinearity; the additive form is what equal-weighted themes already deliver | 0 (product) or 1 (threshold) |
| V5 | Intangible-adjusted book (capitalised R&D and part of SG&A) | EKP +2.4%/yr over HML, but fully explained by profitability per Novy-Marx-Medhat | Medium: depreciation rates and SG&A share are 3 numbers taken from the papers | 3 (borrowed, not fitted) |
| V6 | Value-spread timing of the value theme | Negative evidence in a multi-style book | High | >= 2 |

- Because the book holds profitability as its own equally weighted theme, V5 and enterprise multiples should be expected to raise the correlation between the value and profitability themes more than they raise book Sharpe `[est]`. The diversification arithmetic in v6 section 4 assumes theme correlations near 0.1; alternative-value definitions work against that assumption.
- Industry-relative ranking raises turnover (AQR). With 3.8% of gross per day already, V1 should be judged on net paired dSR, and the FF12 form is the cheaper variant `[est]`.
- Financials: for ratios that are undefined or distorted for financials (EBITDA/EV, CF/P, gross profit), ranking within the FF12 "Money" group keeps them comparable with each other; where the input is missing the neutral (median) rank is the default (see section 7).

### Gaps
- I could not extract the regression coefficients and t-statistics of Asness-Porter-Stevens (the AQR PDF did not convert); only the qualitative result is cited.
- Blitz-Hanauer's return, Sharpe and t-statistics were not available in the sources I could open.
- No post-2015 out-of-sample test of the timely-price effect was found.
- No source compared FF12 against FF49 or GICS specifically for a top-3000, daily-rebalanced, rank-weighted book.

---

## 2. Momentum: residual, volatility-scaled, industry-neutral, intermediate horizon, information discreteness, 52-week high, reversal interaction, comomentum

### Takeaway
The newest evidence weakens the case for residual momentum as a source of return: its alpha is largely a bet against beta plus omitted factor momentum, and the true firm-specific part has been weak since 2000. The best-supported upgrade that uses only prices and announcement dates is momentum measured from earnings-announcement-window returns over the prior 12 months, which works in the largest 1,000 US stocks through 2024 at about 70% of standard momentum's turnover.

### Cited Findings
- Ehsani and Linnainmaa (2022 working paper, "What does residual momentum tell us about firm-specific momentum?"): residual momentum is profitable even without firm-specific momentum "because it is also a bet against betas"; FF5 residual momentum has Sharpe ratio 0.59 in the US and its beta-neutral version 1.23; the improvement is larger among large stocks (t 5.22 vs 2.10) — [AEA conference PDF](https://www.aeaweb.org/conference/2023/program/paper/8Ah8THYY)
- Same paper: true firm-specific momentum earned 10.10% per year (t 10.48) from 1965 to 2000 and 3.45% per year (t 1.53) after 2000; "firm-specific momentum profits declined as arbitrageurs learned about the strategy" — [AEA conference PDF](https://www.aeaweb.org/conference/2023/program/paper/8Ah8THYY)
- Same paper: average returns of residual momentum fall as more factors are removed (CAPM residual 61 bps/month, t 4.33; FF5 residual 50 bps/month, t 4.39; total-return UMD 56 bps/month, t 3.56) while alphas rise, which the authors call a sign of mismeasurement — [AEA conference PDF](https://www.aeaweb.org/conference/2023/program/paper/8Ah8THYY)
- Hanauer and Windmueller (2023, JBF), US and 48 countries: constant-volatility scaling, semi-volatility scaling and dynamic scaling all reduce crashes and raise risk-adjusted returns, none is consistently superior; idiosyncratic momentum emerges as the best momentum strategy in multiple-model comparison, with improvements more than twice those of volatility scaling; the two alphas are distinct — [JBF](https://www.sciencedirect.com/science/article/abs/pii/S0378426622002928); [SSRN 3437919](https://www.ssrn.com/abstract=3437919)
- Gerard and Jehl (2025, FAJ), largest 1,000 US stocks, July 1992 to September 2024, holdings linear in the score: momentum from the sum of market-adjusted returns in +/-2-day windows around all earnings announcements of the prior 12 months earns 3.51%/yr at 7.91% risk (t 2.51), turnover 430%/yr one-way; standard 12-1 momentum earns 6.52%/yr at 21.79% risk (t 1.70), turnover 605% — [paper PDF](https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf)
- Same paper: industry-adjusting the formation returns cuts standard momentum to 3.63%/yr (t 1.53) but leaves the announcement-based measure at 2.83%/yr (t 2.29); the announcement-based measure does not reverse in the long run, whereas standard momentum with announcement days removed does; it survives controls for principal-component factor momentum, which subsumes standard stock momentum in the US — [paper PDF](https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf)
- Novy-Marx (2015): earnings surprise measures (SUE and the 3-day announcement return CAR3) subsume past performance in cross-section regressions, including among large caps; price momentum purged of earnings momentum earns two-thirds of UMD's spread with no less volatility; earnings momentum built to be neutral to past price performance has lower volatility and no crashes with similar return; net of costs 1975-2012 the SUE factor's Sharpe ratio is 0.55 and adding UMD and CAR3 raises it only to 0.57 — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/FMFM.pdf)
- Novy-Marx (2012) finds returns over months 12 to 7 predict better than months 6 to 2, by up to 0.50% per month; Goyal and Wahal (2015, JFQA) find no robust echo in 37 countries outside the US and attribute the US result largely to a carry-over of short-term reversal from month -2 — [Alpha Architect summary](https://alphaarchitect.com/when-academics-disagree-on-momentum-investing/); [JFQA](https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/abs/is-momentum-an-echo/5E4B893AFD2F110B7347F8F483D28ED3)
- Da, Gurun and Warachka (2014, RFS): momentum decreases monotonically from 5.94% for stocks with continuous information in the formation period to -2.07% for stocks with discrete information and the same cumulative formation return; continuation after continuous information does not reverse — [RFS](https://academic.oup.com/rfs/article-abstract/27/7/2171/1578455)
- George and Hwang (2004, JF): nearness to the 52-week high dominates and improves on past returns (individual and industry) as a predictor, and returns forecast by it do not reverse in the long run — [JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2004.00695.x)
- Lou and Polk (2022, RFS): when comomentum (abnormal return correlation among momentum stocks) is low, momentum is stabilising and does not revert; when it is high, momentum strategies "tend to crash and revert" — [RFS](https://academic.oup.com/rfs/article-abstract/35/7/3272/6412574)
- Arnott, Kalesnik and Linnainmaa (2023, RFS): momentum in industry-neutral factors explains industry momentum, while industry momentum explains none of factor momentum; factor momentum is transmitted to industries through their factor loadings — [RFS](https://academic.oup.com/rfs/article-abstract/36/8/3034/6988043)
- Cederburg, O'Doherty, Wang and Yan (2020, JFE), 103 equity strategies: volatility-managed portfolios do not systematically beat unmanaged ones in real-time implementations; out-of-sample certainty equivalents and Sharpe ratios are generally lower because the spanning regressions are unstable — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X2030132X)
- Barroso and Detzel (2021, JFE): after transaction costs, volatility management of factors other than the market generally gives zero abnormal return — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X21000775)
- Medhat and Schmeling (2022, RFS): sorting on last month's return and turnover gives reversal among low-turnover stocks and short-term momentum among high-turnover stocks; the latter survives costs and is strongest among the largest, most liquid stocks; reversal is present in the bottom seven turnover deciles and is indistinguishable from zero three months after formation, while short-term momentum persists for 12 months — [RFS](https://academic.oup.com/rfs/article-abstract/35/3/1480/6286969); [Verdad summary](https://verdadcap.com/archive/short-term-momentum-and-reversals)
- Israel, Jiang and Ross (2017): a plain 12-month momentum ranking implicitly times the market, holding high-beta names after up markets — [AQR PDF](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Israel and Moskowitz (2013, JFE): the long side contributes about half of momentum profits overall, 71% among small caps and only 38% among large caps — [JFE](https://www.sciencedirect.com/science/article/pii/S0304405X12002401)

### Inferences
| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| M1 | New member `ear_mom_12m`: sum over the last 252 sessions of market-excess (or FF12-excess) daily returns on sessions within +/-2 of an earnings flag; rank; no skip month | Gerard-Jehl: top-1000 US, t 2.51 (1992-2024) and t 2.94 (2010-2024), risk 7.9% vs 21.8% for 12-1 momentum, turnover 430% vs 605% | Low-Medium: one recent paper (practitioner authors, peer-reviewed), but consistent with Novy-Marx 2015 | 1 (window half-width 2; taken from the paper) |
| M2 | Keep residual/industry-excess momentum but expect its benefit to be risk reduction; do not raise its prior SR above raw momentum | Ehsani-Linnainmaa: post-2000 firm-specific momentum 3.45%/yr, t 1.53 | Medium | 0 |
| M3 | Information-discreteness interaction: multiply the momentum rank by (1 - rank of ID), ID = sign(12-1 return) x (% negative days - % positive days) over the formation window | Da-Gurun-Warachka: 5.94% vs -2.07% across ID groups | Medium: single paper, pre-2010 sample; interaction form is my choice | 0 |
| M4 | Skip period: keep 21 sessions; do not switch to 12-7 | Goyal-Wahal: echo not robust in 37 countries | Low (no change) | 0 |
| M5 | Volatility scaling of the momentum sleeve | Helps in-sample (Hanauer-Windmueller) but fails real-time tests across 103 strategies and after costs | Medium | 1-2 (vol window, target) |
| M6 | Comomentum or crowding switch | Needs an estimated time series and a threshold; about 1-2 crash episodes per decade to fit on | High | >= 2 |
| M7 | Keep `ind_mom_12_1` as its own member and treat it as a factor-momentum proxy | Arnott-Kalesnik-Linnainmaa 2023 | Low | 0 |

- **CONTRADICTS v6 3.5 and v6 section 4** (prior gross SR 0.4-0.7 for residual momentum). Two mechanisms reduce what the book can expect: (a) the book's composite is projected on beta, which removes the bet-against-beta component that Ehsani-Linnainmaa identify as a main source of residual-momentum alpha; (b) the remaining firm-specific part is weak after 2000. A prior SR of 0.2-0.4 for residual momentum inside this book looks more defensible `[est]`.
- M1 and the earnings theme overlap by construction. If M1 is admitted it should sit in the earnings-momentum theme (or replace a price-momentum member), not raise the number of themes.
- Because momentum's short leg matters more in large caps (Israel-Moskowitz: long side only 38%), and shorting is costly, the large-cap momentum sleeve is more exposed to borrow cost and squeeze risk than the value sleeve `[est]`.
- The 52-week-high member is already in the library; George-Hwang's no-reversal result argues for keeping it without a skip month, which the house form already does if written on the current close.

### Gaps
- Magnitudes (returns, t-statistics) for Medhat-Schmeling's high-turnover short-term momentum among megacaps could not be retrieved from an accessible primary source.
- I found no test of the information-discreteness interaction on post-2015 US data.
- No source quantified how much of residual momentum's improvement survives when the portfolio is already beta- and volatility-neutral; the inference above is mine.
- Comomentum values for 2020-2026 were not found.

---

## 3. Quality and profitability: cash-based vs operating, gross vs operating, R&D treatment, denominators, composite construction, financials

### Takeaway
The 2025 retrospective by the author of the gross-profitability paper finds that operating profit with R&D added back is the strongest single profitability measure and subsumes the cash-based version, fully so after 2000. It also finds that every "quality" composite is spanned by profitability, so adding more quality members mostly adds weight to the same bet.

### Cited Findings
- Ball, Gerakos, Linnainmaa and Nikolaev (2016, JFE): cash-based operating profitability outperforms measures that include accruals and subsumes the accrual anomaly; an investor raises the Sharpe ratio more by adding the cash-based factor alone than by adding both an accruals factor and an accrual-inclusive profitability factor; predictive power extends up to ten years — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X16300307); [SSRN 2587199](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2587199)
- A secondary summary reports maximum Sharpe ratios of 1.67 with the cash-based factor against 1.40 with operating profitability and 1.12 with accruals over 1963-2014; I did not verify these against the paper's tables — [search-surfaced summary of the JFE paper](https://www.sciencedirect.com/science/article/abs/pii/S0304405X16300307)
- Novy-Marx and Medhat (2025), value-weighted-least-squares Fama-MacBeth regressions, all measures scaled by book equity plus minority interest: operating profit unpunished for R&D (REVT - COGS - (XSGA - XRD) - XINT) "has the most power predicting returns"; when both are included, it has t 5.69 against 2.57 for cash-based operating profit with R&D added back; in the post-2000 subperiod it "fully subsumes" the cash-based measure; the same ordering holds in developed ex-US and emerging markets — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Same paper: undoing accruals "introduces significant mean reversion in the profitability measure"; the R&D-inclusive operating measure predicts its own 3- and 10-year growth (coefficient 0.19 vs 0.10 for standard operating profit), while the cash-based measure predicts its own growth with a negative sign — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Same paper: none of the quality measures tested (ROE, earnings stability, leverage, GMO-style Q-score, payout, ROIC, QMJ-style composites) has significant alpha relative to profitability, the other Fama-French factors and momentum; defensive (low-beta, low-volatility) strategies "derive all of their performance by tilting towards profitable companies, particularly those that invest conservatively" — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Same paper, overview table of definitions: gross profit and operating profit before R&D are scaled by total assets and exclude financials (Novy-Marx 2013; Ball et al. 2015); Fama-French operating profit is scaled by book equity; Ball et al. 2016 scale cash profit by average total assets — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Fama and French (2008, JF): among profitable firms higher profitability is associated with abnormally high returns, but there is little evidence that unprofitable firms have unusually low returns; the profitability and asset-growth anomalies are less robust across size groups than issuance, accruals and momentum — [JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2008.01371.x)
- Soebhag, van Vliet and Verwijmeren (2024, JEF), 256 construction variants per factor: including financial firms moves the average factor Sharpe ratio from 0.63 to 0.66; the effect of most choices that raise gross Sharpe (equal weights, microcaps, all-exchange breakpoints) reverses net of costs — [Quantpedia summary](https://quantpedia.com/the-importance-of-factor-construction-choices/); [JEF](https://www.sciencedirect.com/science/article/pii/S0927539824000525)

### Inferences
| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| Q1 | Profitability numerator: revenue - COGS - (SG&A - R&D) - interest; add R&D back wherever the XBRL SG&A tag includes it; scale by book equity (plus minority interest) for the equity-level member and by total assets for the asset-level member | Novy-Marx-Medhat: t 5.69 vs 2.57 against cash-based; subsumes it post-2000; US, developed ex-US and emerging | Low | 0 |
| Q2 | Keep one cash-based or accrual member at most; do not replace the operating member with it | Ball et al. 2016 (for), Novy-Marx-Medhat 2025 (against): conflict between two top sources | Low | 0 |
| Q3 | Freeze the number of quality members; treat QMJ-safety, F-score, ROIC etc. as the same bet | Novy-Marx-Medhat: all spanned by profitability | Low | 0 |
| Q4 | Financials: rank profitability within the FF12 Money group using a financial-appropriate numerator (operating income), or assign the median rank | Gerard-Jehl use "cash-based operating profitability, or operating income for financials" as their risk factor; Soebhag et al.: including financials is worth about +0.03 SR on average | Low | 0 |

- **CONTRADICTS v6 3.6**, which proposed replacing `accruals` and `cfoa` with cash-based operating profitability as the upgrade. The newer evidence says the accrual adjustment "has limited additional impact on return predictability" and the R&D add-back matters more. Both papers share an author pool linked to the same asset manager (Dimensional), so this is a within-school revision, not an independent refutation.
- In fields-v7 there is no COGS or SG&A (library-v7-draft W1-3 notes CFO/AT was used as a stand-in). If that is still true, Q1 is blocked on a fields change (XBRL tags exist: CostOfRevenue, SellingGeneralAndAdministrativeExpense, ResearchAndDevelopmentExpense), which is a definition change, not new data.
- Profitability is the one theme with better returns after publication than before (60 vs 31 bps/month). A smaller haircut than the generic 50-60% is defensible for this theme `[est]`, but the late sample coincides with the large-cap growth regime, so part of it may be regime.
- Fama-French 2008's asymmetry (the effect is among profitable firms) suggests the profitability rank is more informative in its upper half; with costly shorting this favours the theme (long-leg driven) `[est]`.

### Gaps
- No source examined profitability trend (Akbas-Jiang-Koch) or QMJ construction choices on post-2015 data.
- I could not find a direct comparison of denominators (assets vs book equity vs market equity) holding the numerator fixed; Novy-Marx-Medhat scale everything by book equity plus minority interest.
- The Ball et al. Sharpe numbers are unverified.

---

## 4. Earnings momentum without analyst data: SUE, revenue surprise, announcement return, what survives in large caps

### Takeaway
Drift after the latest earnings surprise is statistically absent outside microcaps in recent US data, whichever surprise measure is used; two 2025 top-journal papers that claim otherwise do not filter microcaps. What still works in the largest 1,000 stocks is a slower measure that accumulates announcement-window returns over the past year, and earnings-momentum signals are cleaner when made neutral to price momentum.

### Cited Findings
- Martineau (2022, Critical Finance Review): prices now fully reflect earnings surprises on the announcement date; for large stocks drift has been non-existent since 2006 and it has disappeared only recently for microcaps — [SSRN 3111607](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3111607); [CFR](https://www.nowpublishers.com/article/Details/CFR-0122)
- UCLA Anderson Review (2025) on the dispute: Dickerson, Julliard and Mueller (JFE, in press) and Hirshleifer, Peng and Wang (RFS 2025, 38(3), t around 14) report that drift is alive; Subrahmanyam's working paper finds a drift t-statistic of 2.18 with all stocks and 1.43 excluding microcaps, which are about 3% of market value; neither of the two papers filtered microcaps, and the first uses an off-the-shelf earnings factor based on abnormal returns, not accounting surprises — [UCLA Anderson Review](https://anderson-review.ucla.edu/is-post-earnings-announcement-drift-a-thing-again/)
- Gerard and Jehl (2025, FAJ), largest 1,000 US stocks: a strategy on the latest announcement return earns 3.05%/yr (t 2.13) over July 1992 to September 2024 but 1.15%/yr (t 0.67) over January 2010 to September 2024, with one-way turnover of 976-996% per year; the 12-month sum of announcement returns earns 3.51% (t 2.51) and 4.90% (t 2.94) in the same two periods with turnover 430% — [paper PDF](https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf)
- Livnat and Mendenhall (2006, JAR): drift is significantly larger when the surprise is defined from analyst forecasts than from a time-series model on Compustat data; the difference is due to the forecasts, not to restatements or special items — [JAR](https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1475-679X.2006.00196.x)
- Jegadeesh and Livnat (2006, JAE; FAJ): after controlling for earnings surprises, stocks with large revenue surprises earn significant abnormal returns after the announcement, and drift is stronger when the revenue surprise has the same sign as the earnings surprise — [JAE](https://www.sciencedirect.com/science/article/abs/pii/S0165410106000061); [SSRN 903767](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=903767)
- Meursault, Liang, Routledge and Scanlon (JFQA, "PEAD.txt"): a text-based surprise gives a drift larger than classic drift and considerable "even in recent years when classic PEAD is close to 0" (requires text, outside the no-new-data constraint) — [JFQA](https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/peadtxt-postearningsannouncement-drift-using-text/5EB217BB68B5FB054FE38541BAAC4679)
- Novy-Marx (2015): earnings momentum constructed to be neutral to past 12-2 returns "generated a similar, even more significant, spread" than the unconditional SUE factor, with lower volatility and no crashes — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/FMFM.pdf)
- Dai, Medhat, Novy-Marx and Rizova (2024, FAJ): a value-weighted PEAD factor earned 53 bps/month (t 5.45) over January 1973 to December 2021, full sample, no subperiod split reported in the text I read — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/RRLP.pdf)
- Ehsani and Linnainmaa (2022 WP, footnote 12): among 208 anomalies rebuilt from the open-source asset pricing data with the Fama-French 3x2 method, only three have a Sharpe ratio of 1.20 or more, one of them the earnings announcement return of Chan, Jegadeesh and Lakonishok (1996) (full sample, gross) — [AEA conference PDF](https://www.aeaweb.org/conference/2023/program/paper/8Ah8THYY)
- Engelberg, McLean and Pontiff (2018, JF): anomaly returns are about 6 times higher on earnings announcement days (see section 8 for magnitudes) — [paper PDF](https://rady.ucsd.edu/faculty/directory/engelberg/pub/portfolios/ANOMALIES_NEWS.pdf)
- Linnainmaa and Zhang (working paper): stocks earn significantly negative abnormal returns before earnings announcements and positive ones after; half of the pattern is attributed to a cycle in analyst optimism; stronger among high-uncertainty, hard-to-arbitrage stocks — [AEA conference PDF](https://www.aeaweb.org/conference/2019/preliminary/paper/YrbKK6Zn)

### Inferences
| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| E1 | Theme core becomes the 12-month sum of announcement-window excess returns (same construction as M1); the latest-EAR member is kept at reduced weight or dropped | Gerard-Jehl top-1000: t 2.94 vs 0.67 in 2010-2024; turnover less than half | Low-Medium | 1 (window) |
| E2 | Make SUE-type members neutral to 12-1 momentum before ranking (rank SUE within momentum terciles, or subtract the momentum rank) | Novy-Marx 2015: same return, lower vol, no crashes | Low | 0 |
| E3 | Revenue surprise as a confirming term: SUE member = mean of ranks of earnings surprise and revenue surprise (seasonal random walk, scaled by the std of the last 8 seasonal differences) | Jegadeesh-Livnat 2006; pre-2006 sample | Medium: old sample; a drift signal in a period where drift is near zero | 1 (8 quarters for the scale) |
| E4 | Collapse filing-clock surprise members (`sue`, `droe`, `chtax`) to one composite with a prior SR near zero in the top 1000 | Martineau; Subrahmanyam ex-microcap t 1.43 | Low | 0 |

- **CONTRADICTS v6 3.1**, which proposed making the latest 3-day EAR half of the theme. In the largest 1,000 stocks since 2010 the latest-EAR strategy is insignificant and turns over about 10 times a year. The slower 12-month aggregate is the better carrier for a low-turnover book.
- The evidence that drift survives in all-stock samples but not ex-microcaps implies the theme's TRAIN performance in a top-3000 universe will be driven by ranks 1000-3000 `[est]`; the report card's size-tercile IC split (v7 R5.6) is the right check.
- Time-series SUE is the weaker version of the signal (Livnat-Mendenhall), so without analyst data the prior for SUE should be below the analyst-based literature numbers.

### Gaps
- No subperiod (post-2010) result for the value-weighted PEAD factor or for revenue surprise was found.
- I could not open Martineau's paper to extract magnitudes by size group; only the abstract-level statement is cited.
- The Subrahmanyam working paper itself was not located; its numbers come from the UCLA Anderson Review article.
- No source tested the 12-month announcement-return measure on the 1000-3000 rank segment.

---

## 5. Low risk: correlation vs beta, beta estimation, idiosyncratic volatility, MAX, interaction with beta/vol neutralisation

### Takeaway
For an investable, value-weighted implementation the beta anomaly has no significant alpha once profitability and investment are controlled for, and the book additionally projects out beta and volatility. What can survive is the part of low risk that is orthogonal to both (correlation, and the shape of the return distribution) and the part that is really profitability. Better beta estimation is worth having for the neutraliser, not as an alpha.

### Cited Findings
- Novy-Marx and Velikov (2022, JFE): for each dollar in BAB the strategy commits on average $1.05 to stocks in the bottom 1% of total market capitalisation; rank weighting is "a backdoor to equal-weighting"; BAB and an equal-weighted tercile version are 99.6% correlated — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf)
- Same paper, January 1968 to December 2017: value-weighted BAB earns 56 bps/month (t 3.48) with Sharpe ratio 0.49 against 1.08 for BAB; its FF5 alpha is 24 bps/month (t 1.63) and its six-factor alpha 14 bps/month (t 0.93), with loadings on profitability and investment (t 6.59 and 4.86) — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf)
- Same paper: BAB's trading costs average 60 bps/month and reduce profitability by more than 55%; net return 48 bps/month (t 3.30), net generalised FF5 alpha 16 bps/month (t 1.20); excluding the bottom NYSE size decile cuts costs to 23 bps/month (12 bps over the last ten years of the sample) without changing net performance — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf)
- Same paper: the Frazzini-Pedersen beta (5-year correlation of 3-day returns times 1-year volatility ratio, shrunk 60/40) is not a market beta; it equals the 5-year beta times the ratio of the stock's 1-to-5-year volatility ratio to the market's; the market's own FP-beta averages 1.05 with standard deviation 0.09, and market volatility explains 47% of its variation — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf)
- Asness, Frazzini, Gormsen and Pedersen (2020, JFE): betting against correlation has FF5 alpha of 0.6%/month (t 5.25) in the US and 0.4%/month (t 3.47) globally; scaled MAX isolates lottery demand and also earns positive returns; BAC relates to margin debt, idiosyncratic risk factors to sentiment — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X1930176X); [AQR](https://www.aqr.com/Insights/Research/Working-Paper/Betting-Against-Correlation-Testing-Theories-of-the-Low-Risk-Effect)
- Novy-Marx and Medhat (2025): defensive strategies based on low beta or low volatility derive all their performance from tilts to profitable, conservatively investing firms — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Blitz, van Vliet and Baltussen (2020, JPM), a review from the opposite school: volatility rather than beta is the main driver, the effect is persistent across markets and "cannot be explained by other factors such as value, profitability, or exposure to interest rate changes" — [SSRN 3442749](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3442749)
- Stambaugh, Yu and Yuan (2015, JF): the idiosyncratic-volatility effect is negative among overpriced stocks and positive among underpriced stocks (mispricing from 11 anomalies); the negative side is stronger, especially for stocks that are hard to short, and stronger after high sentiment — [JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12286)
- Welch (2022, Critical Finance Review): winsorising each daily stock return at -2 and +4 times the contemporaneous market return and using exponentially decaying weights (half-life about four months) predicts future betas better than OLS, Vasicek and Bloomberg betas; RMSE improvement about 5-10% over OLS and 40-45% over Bloomberg betas — [paper PDF](https://cfr.ivo-welch.info/published/papers/welch2022simply.pdf)
- Chung (2024): the same estimator gives the best prediction of future betas in 45 markets and the best market hedge in 42 — [SSRN 4755442](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4755442)

### Inferences
| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| L1 | Audit each low-risk member by its IC after projection on beta252, vol63 and ladv63 (estimable precisely on 750 days); keep only members with non-trivial post-projection rank autocorrelation and IC | Novy-Marx-Velikov (VW alpha t 1.63); Novy-Marx-Medhat (all performance is profitability/investment) | Low | 0 |
| L2 | Compute correlation-based members within volatility buckets (BAC construction) so that the member is orthogonal to vol by construction, and rank within size bucket | AFGP: FF5 alpha 0.6%/mo, t 5.25 (rank-weighted, includes small caps) | Medium: the BAC evidence uses the same rank weighting that Novy-Marx-Velikov criticise | 1 (number of vol buckets; paper uses 5) |
| L3 | Conditional IVOL: sign the volatility signal by a mispricing score (negative IVOL only where the composite says overpriced) | Stambaugh-Yu-Yuan 2015 | Medium-High: interaction with the book's own composite; most of the profit is on hard-to-short names | 0-1 |
| L4 | Replace the OLS beta252 used in the neutraliser by the slope-winsorised, age-decayed beta | Welch 2022; Chung 2024 | Low (it is a risk estimate, judged on realised beta of the book, not on return) | 2 borrowed (bounds -2/+4; half-life about 84 sessions) |

- **EXTENDS v6 3.3 / lever 5.** Answer to "does neutralising beta and vol remove the low-risk premium?": for the level signals (low beta, low vol, low IVOL) essentially yes, by construction, since the projection removes the linear exposure the signal is made of; what is left is nonlinear residue plus turnover. The literature adds that even before projection the investable premium is explained by profitability and investment. The theme therefore competes with the profitability theme for the same return `[est]`.
- The two schools disagree (Robeco: not explained by profitability; Novy-Marx-Medhat: fully explained). For this book the disagreement matters little because the projection removes the volatility bet either way.
- If the theme is kept, equal theme weighting gives 1/10 of the book to a bet that is largely hedged out; folding survivors (BAC, SMAX) into another theme is the zero-parameter alternative `[est]`.

### Gaps
- No source measured BAC or SMAX performance value-weighted or in the top 1000 only.
- No source measured any low-risk signal after an explicit beta-and-volatility projection; the statement above is a logical inference from the construction, to be verified on TRAIN as a turnover/IC measurement.
- I found no post-2020 update of the beta or IVOL anomaly net of costs.

---

## 6. Short interest: days-to-cover vs ratio, institutional ownership conditioning, changes vs levels, short volume

### Takeaway
Days-to-cover beats the short-interest ratio, keeps its predictive coefficient after 2000 when the ratio loses significance, and is insensitive to whether turnover is averaged over 1, 6 or 12 months, so the slow denominator is free. The edge shrinks in large caps and the strategy has a specific tail (forced covering), seen in the 2008 short-sale ban.

### Cited Findings
- Hong, Li, Ni, Scheinkman and Yan (2015, NBER w21166), 1988-2012, decile sorts: equal-weighted long-short on the short ratio earns 0.71%/month (t 2.57, Sharpe 0.51); on days-to-cover 1.19%/month (t 6.67, Sharpe 1.33); four- and five-factor alphas about 1.3%/month (t about 8) — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Same paper, value-weighted: short ratio 0.29%/month (insignificant); days-to-cover 0.67%/month (t 2.24), DGTW-adjusted 0.59% (t 2.56); the DTC-minus-SR difference is 0.36%/month value-weighted (t 1.18) against 0.46% equal-weighted (t 2.65) — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Same paper, size filters: excluding the bottom 10% by NYSE size cut-off, SR earns 0.55% and DTC 0.84%/month (difference 0.29%, t 1.76); excluding the bottom 20% the difference is 0.25%/month — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Same paper, subperiods: DTC outperforms SR by 0.37%/month in 1988-1999 and 0.53%/month in 2000-2012; in Fama-MacBeth regressions the SR coefficient halves after 2000 and loses significance once DTC is included, while the DTC coefficient is the same in both halves — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Same paper, denominator horizon: DTC using turnover averaged over the prior month, the past 6 months or the past 12 months all forecast returns; with 6-month turnover the coefficient is -0.0004 (t -7.20) after controlling for Amihud illiquidity; the correlation of DTC with 1/turnover is only 0.09 — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Same paper: the DTC strategy had a significant drawdown during the US short-selling ban (17 September to 8 October 2008) as shorts were forced to cover; the SR strategy did not — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Same paper: mean lending fee in the 2003+ subsample is low (mean 1.39, sd 3.56, in the paper's fee units) and similar across size groups; DTC is as strong a predictor as lending fees and remains significant (t -2.48) when fees are controlled — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Nagel (2005, JFE): holding size fixed, the underperformance of stocks with high market-to-book, analyst dispersion, turnover or volatility is most pronounced among stocks with low institutional ownership, where short-sale constraints bind — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X05000735)
- Stambaugh, Yu and Yuan (2012, JFE): anomalies are stronger after high sentiment and the effect comes from the short leg; sentiment has no relation to long-leg returns — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X11002649)

### Inferences
| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| S1 | `dtc` = SI / (shares out x mean daily turnover over 126 or 252 sessions); no 21-day decay on top of a semi-monthly series | Hong et al.: result invariant to 1/6/12-month turnover; slow denominator lowers turnover for free | Low | 0 (window from the house set) |
| S2 | Rank DTC within size terciles (or project on log size) so the signal is not a liquidity sort | DTC-SR gap falls from 0.46 to 0.25-0.29%/mo as small caps are removed; the book already projects on log dollar volume | Low | 0-1 (number of buckets) |
| S3 | SI scaled by institutional ownership (SI / IO) as the lending-supply-utilisation member | Nagel 2005; Hong et al. use SIO as a fee proxy | Medium: 13F is quarterly with a 45-day lag; ratio blows up at low IO, needs a floor | 1 (IO floor) |
| S4 | Squeeze guard on the short leg: cap short weight in the top DTC percentile rather than rank-linear | 2008 ban drawdown; January 2021 (v6) | Medium | 1 (cap level) |

- For a top-3000 universe with most capital in liquid names, the value-weighted figures (0.67%/month gross, t 2.24, 1988-2012) are the relevant prior, before a post-publication haircut (publication 2015/2016) and before borrow fees `[est]`.
- Changes in short interest: I found no cross-sectional evidence that improves on levels; v6 already noted that the cited paper for `si_change` is an aggregate-market result.
- DTC is mechanically correlated with the log-dollar-volume factor the book projects out; S2 and the projection partly do the same job, so S2's incremental effect should be small `[est]`.

### Gaps
- No post-2015 out-of-sample evidence for days-to-cover was found.
- No new evidence on FINRA short volume beyond what v6 cites (Wang-Yan-Zheng 2020).
- No source reported DTC performance net of name-level borrow fees.

---

## 7. Signal processing: rank vs z-score, where to neutralise, size buckets, smoothing and half-life, missing data, nonlinearity, long/short asymmetry

### Takeaway
Construction choices move a factor's Sharpe ratio by as much as its sampling error, which makes every definitional choice a trial and argues for fixing house conventions once. The supported conventions are: bounded scores, within-industry comparison for long-short signals, weights that do not load on the smallest names, smoothing matched to each signal's own decay, and neutral (median) treatment of missing values.

### Cited Findings
- Soebhag, van Vliet and Verwijmeren (2024, JEF): across construction variants the CMA factor's Sharpe ratio ranges from 0.18 to 0.90 and UMD's from 0.37 to 0.78; the ratio of the "non-standard error" (dispersion across design choices) to the standard error exceeds one on average; all-exchange breakpoints raise average Sharpe from 0.54 to 0.74, equal weighting from 0.58 to 0.71, including microcaps from 0.59 to 0.70, and these gains reverse net of costs — [JEF](https://www.sciencedirect.com/science/article/pii/S0927539824000525); [Quantpedia summary](https://quantpedia.com/the-importance-of-factor-construction-choices/)
- Kessler, Scherer and Harries (2020): 3,168 implementations of value; dispersion in Sharpe ratios used to measure degrees of freedom consumed — [SSRN 3593557](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3593557)
- Novy-Marx and Velikov (2022): rank weighting "creates portfolios that are almost indistinguishable from simple, equal-weighted portfolios"; its effect on BAB comes "not by what it does ... but by what it does not do, i.e., weight stocks in proportion to their market capitalizations" — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf)
- Jensen, Kelly and Pedersen (2023, JF): factors built with "capped value weights" (market cap winsorised at the NYSE 80th percentile) and non-micro tercile breakpoints; capped value weighting adds 8.5 percentage points to their replication rate relative to pure value weighting — [NBER w28432 PDF](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- Israel, Jiang and Ross (2017): signal-strength weighting versus cut-off portfolios; risk targeting of the style sleeve (5% volatility target in their example) gives steadier risk; combining styles in one integrated score allows netting and lower turnover than mixing separate portfolios — [AQR PDF](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Blitz and Hanauer (2021) and Gerard and Jehl (2025) both use capped scores (robust z-scores capped at +/-3; "capped score values intersected with capped market-value weights") as their robust specification; in the latter, results for the announcement-momentum signal are unchanged — [Quantpedia summary](https://quantpedia.com/resurrecting-the-value-premium/); [paper PDF](https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf)
- Chen and McCoy (2024, JFE), 159 predictors: imputing missing values with cross-sectional means performs as well as expectation-maximisation, because missingness comes in large blocks by time and data source and cross-predictor correlations are small; sophisticated imputation adds estimation noise — [arXiv 2207.13071](https://arxiv.org/abs/2207.13071); [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X24000382)
- Freyberger, Neuhierl and Weber (2020, RFS): with adaptive group LASSO on rank-transformed characteristics, many predictors carry no incremental information, nonlinearities matter, and out-of-sample Sharpe ratios are about 50% higher than with linear models — [RFS](https://academic.oup.com/rfs/article-abstract/33/5/2326/5821383)
- Chen and Velikov (2023, JFQA) on that result: the nonlinear model nets 210 bps/month in 1991-2014 "if trading on microcaps is allowed"; their own combination methods earn about 20 bps/month net after 2005 — [JFQA PDF](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/945133D5A3ECEEAF466AEE91551FD225/S0022109022000874a.pdf/zeroing-in-on-the-expected-returns-of-anomalies.pdf)
- Patton and Timmermann (2010, JFE) give tests for monotonicity of expected returns across sorted portfolios; some characteristic-return relations fail monotonicity — [paper PDF](https://public.econ.duke.edu/~ap172/Patton_Timmermann_sorts_JFE_Dec2010.pdf)
- Blitz, Baltussen and van Vliet (2020, FAJ): most of the added value of factors comes from the long legs, the long legs diversify each other better, and the short legs are generally subsumed by the long legs; holds in large and small caps — [FAJ](https://www.tandfonline.com/doi/full/10.1080/0015198X.2020.1779560)
- Israel and Moskowitz (2013, JFE): long positions make up almost all of size, 60% of value and half of momentum profits — [JFE](https://www.sciencedirect.com/science/article/pii/S0304405X12002401)
- Stambaugh, Yu and Yuan (2012): in contrast, the sentiment-dependent part of anomaly profits sits in the short leg — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X11002649)
- Baldacci, Benveniste and Ritter (2021/2022): in a Gaussian-process model the steady-state optimal turnover is gamma x sqrt(n + 1), where gamma is a liquidity-adjusted risk-aversion parameter and n is the ratio of the signal's mean-reversion speed to gamma — [arXiv 2110.03810](https://arxiv.org/abs/2110.03810)
- Zhang, Wang and Cao (2021): a turnover-adjusted information ratio is always below the unadjusted one, and limiting turnover can raise the net information ratio, contrary to the fundamental law's breadth logic — [arXiv 2105.10306](https://arxiv.org/abs/2105.10306)
- Israelov and Katz (2011, FAJ): long-term investors can use short-term signals to decide when to execute trades they would make anyway, gaining exposure to the short-term signal "without having to pay additional transaction costs and without capacity limits" — [AQR](https://www.aqr.com/Insights/Research/Journal-Article/To-Trade-or-Not-to-Trade-Informed-Trading-With-Short-Term-Signals-for-LongTerm-Investors)
- Dai, Medhat, Novy-Marx and Rizova (2024, FAJ): "employing a reversal screen ... can improve expected returns without incurring additional turnover and trading costs" by delaying trades that demand costly liquidity — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/RRLP.pdf)

### Inferences
| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| P1 | Keep rank (bounded, uniform) as the house transform; if tails matter, use a capped robust z-score (+/-3) for that member only, declared before any read | Practitioner convention (Robeco, Quoniam); no study found showing z-scores beat ranks | Low | 0-1 |
| P2 | Neutralise by industry at the signal level for value, profitability, investment (group rank); leave momentum and low-risk members un-neutralised at signal level and let the composite projection handle industries | Ehsani-Harvey-Li decision rule: within component dominates for value; for momentum the across component has comparable Sharpe | Low | 0 |
| P3 | Rank within size terciles (or liquidity terciles) for members whose raw distribution is size-dependent (short interest, turnover-based, volatility-based, issuance) | JKP non-micro breakpoints and capped weights; Novy-Marx-Velikov on hidden small-cap loading of rank weights | Low-Medium | 1 (number of buckets) |
| P4 | Signal-specific smoothing: choose the decay length per member from its own rank autocorrelation and IC-decay curve (estimable on 750 days), restricted to the house set {none, 21, 63}; fundamentals updated on the filing clock need no 21-day decay on the numerator | Garleanu-Pedersen and Qian-Sorensen-Hua (v6/v7); Baldacci et al. turnover formula; the choice uses second-moment information only | Low if the rule is fixed in advance and uses autocorrelation, not TRAIN return | 1 per member from a 3-value menu |
| P5 | Missing values: assign the cross-sectional (or group) median rank, never drop the name from the composite | Chen-McCoy 2024 | Low | 0 |
| P6 | Nonlinear transforms (emphasising tails, or flattening the middle) | FNW +50% Sharpe, but concentrated where microcaps are tradable; Chen-Velikov show net gains vanish after 2005 | High | >= 1 per member |
| P7 | Long/short asymmetry: shrink the short-side weight of members whose short leg is fee-exposed, rather than symmetric rank weights | Blitz-Baltussen-van Vliet; Israel-Moskowitz; borrow-fee results in v6 | Medium: asymmetry factor is a free number unless tied to modelled borrow cost | 1 |
| P8 | Use fast signals (reversal, short-horizon volatility) to time execution of slow-signal trades rather than as sleeves | Israelov-Katz 2011; Dai et al. 2024 | Medium: gains depend on the cost model | 1-2 |

- Exponential versus linear decay: I found no empirical comparison. Analytically, a 21-day linear decay has a mean lag of about 7 sessions and a hard cut-off; an exponential filter with the same mean lag has half-life about 5 sessions and a longer tail. The difference between them is second-order relative to the choice of length `[est]`.
- Rank weighting across a top-3000 universe is close to equal weighting, so the book's gross performance leans on the 1000-3000 rank segment more than its capital capacity does. P3 and the capped-weight idea address the same issue.
- The evidence on which leg carries the premium conflicts: unconditional studies (Blitz et al., Israel-Moskowitz) favour the long leg, conditional studies (Stambaugh-Yu-Yuan) place the mispricing on the short leg after high sentiment. For a book with costly shorts, both point the same way operationally: short-leg alpha must clear borrow cost and should be sized down where it cannot.
- Soebhag et al. imply that a paired dSR test between two definitions of the same signal has an implicit multiple-testing problem; one variant per hypothesis (library-v7 section 0) is the correct discipline and should extend to processing choices.

### Gaps
- No direct empirical comparison of rank versus z-score versus winsorised raw values was found; the recommendation rests on convention.
- No source gave optimal half-lives by signal type; the v6 reference (Qian-Sorensen-Hua 2007) could not be re-opened (server error), and a claim surfaced in search that a 21-day moving average cut turnover by 82% and gross Sharpe by 31% could not be traced to a verifiable source, so it is not used.
- No evidence was found on signal-level versus composite-level neutralisation for a daily rank-weighted book specifically.
- Monotonicity test results for specific anomalies were not extracted.

---

## 8. Conditioning variables that modulate anomaly strength, and which are implementable as static interactions

### Takeaway
Anomaly returns are concentrated on information days, in smaller stocks, in stocks with low institutional ownership and high idiosyncratic volatility, and after high sentiment. Most of these conditions identify where arbitrage is costly, so for a large-cap, costly-short book they mostly explain where alpha is absent; only cross-sectional conditions built from price, volume and the calendar can be used without fitting.

### Cited Findings
- Engelberg, McLean and Pontiff (2018, JF), 97 anomalies: for a net anomaly score of 10 (about 1.5 standard deviations) expected return is 3.84 bps/day higher on ordinary days and 25.48 bps on earnings announcement days (6.3 times); on other corporate news days it is 5.62 bps (about 50% higher); effects are present on both long and short sides; anomaly returns are not higher on macro-news days — [paper PDF](https://rady.ucsd.edu/faculty/directory/engelberg/pub/portfolios/ANOMALIES_NEWS.pdf); [JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12718)
- Same paper, size split at the median: the interaction of the anomaly score with earnings days is 0.715 for large stocks and 3.067 for small stocks (about 4 times); the news-day interaction is insignificant among large stocks; "virtually all of the difference in anomaly returns between large and small stocks occurs on information days" — [paper PDF](https://rady.ucsd.edu/faculty/directory/engelberg/pub/portfolios/ANOMALIES_NEWS.pdf)
- Same paper: anomaly-long stocks have analyst forecast errors that are too pessimistic and anomaly-short stocks too optimistic, consistent with biased expectations corrected by news — [paper PDF](https://rady.ucsd.edu/faculty/directory/engelberg/pub/portfolios/ANOMALIES_NEWS.pdf)
- Linnainmaa and Zhang: abnormal returns are negative before and positive after earnings announcements — [AEA conference PDF](https://www.aeaweb.org/conference/2019/preliminary/paper/YrbKK6Zn)
- Nagel (2005): anomalies on the overpriced side are strongest at low institutional ownership, holding size fixed — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X05000735)
- Stambaugh, Yu and Yuan (2015): idiosyncratic volatility strengthens mispricing in both directions, more on the overpriced side — [JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12286)
- Stambaugh, Yu and Yuan (2012): every anomaly studied is stronger after high sentiment, through the short leg — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X11002649)
- Dai, Medhat, Novy-Marx and Rizova (2024): value-weighted one-month reversal earns 31 bps/month (t 1.68) over 1973-2021; among firms that announced earnings in the formation month it is 6 bps (insignificant), among non-announcers 51 bps; industry-relative reversal earns 74 bps/month (t 5.40) and the industry-relative, announcement-adjusted version 108 bps/month (t 9.35); standard reversal = 0.76 x cleaned reversal - 0.54 x PEAD - 0.53 x industry momentum (R-squared 87%) — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/RRLP.pdf)
- Same paper: higher volatility gives faster, initially stronger reversals and lower turnover gives more persistent ones; with 21 days of formation, reversal lasts about two weeks among high-volatility stocks and almost three months among low-volatility stocks; one month after formation the spread is 106 bps among low-volatility stocks against 39 bps among high-volatility stocks, while with 5-day formation high-volatility stocks reach 116 bps after two weeks; patterns hold before and after decimalisation (April 2001) and outside the US — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/RRLP.pdf)
- Medhat and Schmeling (2022): turnover switches one-month reversal into one-month momentum at the top turnover decile — [RFS](https://academic.oup.com/rfs/article-abstract/35/3/1480/6286969)
- Da, Gurun and Warachka (2014): information discreteness modulates momentum — [RFS](https://academic.oup.com/rfs/article-abstract/27/7/2171/1578455)
- Lou and Polk (2022): comomentum modulates momentum's sign of subsequent long-run return — [RFS](https://academic.oup.com/rfs/article-abstract/35/7/3272/6412574)

### Inferences
Classification of conditioning variables for this book:

| Variable | Type | Usable as static interaction without fitting? | Comment |
|---|---|---|---|
| Earnings-announcement window (next expected date) | Calendar, cross-sectional | Partly: needs expected dates (last year's date plus about 252 sessions is a zero-parameter proxy) | Large-cap effect is about one quarter of small-cap effect; announcement days also carry more risk |
| Earnings window in the lookback (exclude announcement days from reversal; isolate them for momentum) | Calendar, cross-sectional | Yes, 0-1 parameters | Best supported: Dai et al. and Gerard-Jehl point the same way |
| Share turnover (reversal vs short-term momentum; reversal persistence) | Cross-sectional | Yes as a tercile interaction; 1 parameter (bucket count) | Already proposed in v6 3.7; Dai et al. add the horizon dimension |
| Volatility (reversal speed) | Cross-sectional | Yes; but the choice of formation length by vol bucket adds 1-2 parameters | Medium overfit risk |
| Information discreteness | Cross-sectional | Yes, 0 parameters | Single-paper evidence |
| Size | Cross-sectional | Yes, but it tilts to low capacity | Use for within-bucket ranking, not for up-weighting small names |
| Institutional ownership | Cross-sectional, quarterly | Yes, but identifies hard-to-short names | Alpha there is mostly unharvestable net of borrow (v6 2d) |
| Idiosyncratic volatility | Cross-sectional | Yes, but conflicts with the vol projection | The composite is projected on vol63, which removes a level tilt but not an interaction |
| Sentiment, market volatility regime, comomentum | Time series | No: needs a threshold or regression fitted on few regimes | 3-13 years contain 1-3 regimes; High overfit risk |

| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| C1 | Reversal member: industry-excess return over 5 or 21 sessions with sessions within +/-1 (or +/-2) of an earnings flag set to zero; sign negative | Dai et al.: t 1.68 to 9.35 value-weighted gross, 1973-2021 | Medium: gross result; turnover of reversal remains the binding cost | 1 (window) |
| C2 | Do not scale whole-book exposure into announcements; instead keep slow-signal positions unchanged through announcements (no de-risking rule that sells before the event) | EMP: 6x anomaly return on announcement days, positive for longs and negative for shorts | Low | 0 |
| C3 | Optional tilt: within each theme, up-weight names with an expected announcement in the next 21 sessions | EMP; large-cap interaction is 0.715 vs 3.067 small-cap | Medium: adds turnover on a quarterly cycle; large-cap effect small | 1-2 (window, tilt size) |

- C2 is a "do no harm" rule: a quarter of large-cap anomaly return difference arrives on a few days per quarter, so any risk rule that trims positions before announcements gives up a disproportionate share of alpha `[est]`.
- Interactions with IVOL, size or institutional ownership would raise gross IC and lower capacity and net return at the same time; they fail the book's high-capacity objective unless used only to choose where not to short.

### Gaps
- EMP's sample ends before the most recent decade in the version read (July 2017 draft); no post-2015 replication of the announcement-day concentration was found.
- No study tested announcement-window tilts net of costs in large caps.
- No cross-sectional, fit-free proxy for sentiment was found.

---

## 9. Factor timing and factor momentum: realistic net benefit on a 3-13 year sample

### Takeaway
Factor momentum is real in long samples but its timing component is weak, concentrated in a few factors and eroded by costs; valuation-spread timing adds little to a book that already holds value. On 3 to 13 years neither can be validated, and the sizing arithmetic says even a genuinely skilled timing signal earns only a small tilt.

### Cited Findings
- Ehsani and Linnainmaa (2022, JF): the average factor earns 1 bp/month after a losing year and 53 bps/month after a positive year; time-series factor momentum earns 3.9% per year (t 7.01) and cross-sectional 2.4% (t 5.04); it explains all forms of individual stock momentum — [NBER w25551 PDF](https://www.nber.org/system/files/working_papers/w25551/w25551.pdf)
- Arnott, Kalesnik and Linnainmaa (2023, RFS): cross-sectional factor momentum concentrates in the first few highest-eigenvalue factors and is distinct from time-series factor momentum — [RFS](https://academic.oup.com/rfs/article-abstract/36/8/3034/6988043)
- Leippold and Yang (working paper): factor momentum returns "do not stem from momentum in factor returns"; the unconditional factor premium accounts for the dominant share; return autocorrelation is too weak, so timing "only adds noise and incurs momentum crashes" — [SSRN 3517888](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3517888)
- Fan, Li, Liao and Liu (working paper): factor momentum is mostly driven by six factor strategies; return continuation in the remaining factors is weak — [SSRN 3844484](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3844484)
- Falck, Rej and Thesmar (CFM, 2020) question whether factor momentum is more than stock momentum — [CFM PDF](https://www.cfm.com/wp-content/uploads/2022/12/180-2020-09-Is-Factor-Momentum-More-than-Stock-Momentum.pdf); [arXiv 2009.04824](https://arxiv.org/pdf/2009.04824)
- Gerard and Jehl (2025): a principal-component time-series factor momentum strategy built as in Ehsani-Linnainmaa (10 PCs from 37 US factors) performs strongly in the US and subsumes stock momentum, but there is "scant global evidence" that stock returns predict through timely factor exposure — [paper PDF](https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf)
- Haddad, Kozak and Santosh (2020, RFS): for the two most predictable principal components of anomaly portfolios, their own book-to-market ratios predict monthly returns with out-of-sample R-squared around 4%, about four times that of the aggregate market — [NBER w26708 PDF](https://www.nber.org/system/files/working_papers/w26708/w26708.pdf)
- Asness, Chandra, Ilmanen and Israel (2017): no robust evidence that value-spread timing delivers meaningful outperformance in a multi-style portfolio — [AQR](https://www.aqr.com/Insights/Research/Journal-Article/Contrarian-Factor-Timing-is-Deceptively-Difficult)
- Israel, Jiang and Ross (2017), sizing exercise with a strategic portfolio of Sharpe 1.0: a timing strategy needs a standalone Sharpe ratio above 0.1 to merit any weight; with a standalone Sharpe of 0.5 the optimal tactical tilt is still only about +/-12% — [AQR PDF](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)
- Cederburg et al. (2020) and Barroso and Detzel (2021): volatility timing of factors fails in real time and after costs (section 2) — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X2030132X); [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X21000775)

### Inferences
- Power arithmetic `[est]`: a timing overlay with true standalone SR 0.3 has a t-statistic of 0.3 x sqrt(3) = 0.52 on 3 years and 0.3 x sqrt(13) = 1.08 on 13 years. Neither sample can distinguish it from zero, and the book's TRAIN window holds roughly one full value/momentum cycle.
- Applying the AQR sizing result to this book (strategic SR above 1): a timing signal of SR 0.3 would earn a tilt of a few percent of theme weight, with a book-level gain of at most about 0.02-0.04 SR before the added turnover `[est]`. v6 lever 7 (0 to +0.05) is at the upper edge of what these sources support.
- Haddad-Kozak-Santosh's 4% monthly R-squared applies to principal components of 50 anomaly portfolios estimated on about 45 years; the PCs themselves cannot be estimated stably from 10 themes on 3 years `[est]`.
- A zero-parameter use of factor momentum exists: keep industry momentum and raw momentum members, which already carry factor-momentum exposure (Arnott-Kalesnik-Linnainmaa), and do not add a theme-timing layer.
- Risk of over-fitting for any explicit timing rule: High. Parameters: lookback, threshold or slope, tilt cap (at least 2-3).

### Gaps
- I did not find a published net-of-cost factor-momentum result for US large caps after 2015.
- A 2026 paper titled "Factor timing with transaction costs" surfaced in search but was not read; its findings are not used.
- No evidence on factor momentum at the theme level (10 equally weighted themes of ranked signals) was found.

---

## 10. Large-cap efficacy: which signals keep power in the top 1000 and which designs preserve capacity

### Takeaway
Factor themes replicate in mega and large caps at nearly the same rate as overall, but magnitudes are smaller and the after-cost, value-weighted average published anomaly is about zero. The signals with documented large-cap power are profitability, issuance, accruals and momentum historically, plus earnings-announcement-based momentum and short-term momentum in high-turnover names in recent studies.

### Cited Findings
- Jensen, Kelly and Pedersen (2023): replication rates are 77.3% in mega caps (top 20% by NYSE breakpoints) and 81.5% in large caps (NYSE 50th-80th percentile), against 84.0% overall, 81.5% in micro and 71.4% in nano caps; the ordering of cluster alphas is similar across size groups (Spearman rank correlation 73% between mega and micro caps, 35% between mega and nano) — [NBER w28432 PDF](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- Hou, Xue and Zhang (2020, RFS): with NYSE breakpoints and value weights, 64% of 452 anomalies are insignificant at the 5% level (85% at t > 3), including 95 of 102 liquidity variables; microcaps are 3.28% of market value and big stocks about 90% — [NBER w23394 PDF](https://www.nber.org/system/files/working_papers/w23394/w23394.pdf)
- Fama and French (2008, JF): net stock issues, accruals and momentum are pervasive across micro, small and big stocks; the asset-growth anomaly is absent in big stocks; profitability is less robust — [JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2008.01371.x)
- Chen and Velikov (2023, JFQA): restricting cost-mitigated implementations to value weighting gives an average expected net return of -2 bps/month post-publication and post-2005, against +4 bps when equal weighting is allowed; anomaly portfolios paid a mean spread of 67 bps in 2014 against 16 bps for the average NYSE stock, because they load on the illiquid tail — [JFQA PDF](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/945133D5A3ECEEAF466AEE91551FD225/S0022109022000874a.pdf/zeroing-in-on-the-expected-returns-of-anomalies.pdf)
- An earlier version of the same paper, as summarised in search: using only non-micro top-3000 stocks in the top 90% of market cap reduces the average to 26 bps/month and post-2005 to 7 bps (not verified against the published version) — [SSRN 3073681](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3073681)
- Engelberg, McLean and Pontiff (2018): the announcement-day anomaly interaction in large stocks is about one quarter of that in small stocks — [paper PDF](https://rady.ucsd.edu/faculty/directory/engelberg/pub/portfolios/ANOMALIES_NEWS.pdf)
- Novy-Marx (2015): the result that earnings momentum subsumes price momentum "hold[s] among large cap stocks" (above NYSE median) — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/FMFM.pdf)
- Gerard and Jehl (2025): all results are for the largest 1,000 US, 400 European and 200 Japanese stocks — [paper PDF](https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf)
- Medhat and Schmeling (2022): short-term momentum is strongest among the largest, most liquid and most covered stocks — [RFS](https://academic.oup.com/rfs/article-abstract/35/3/1480/6286969)
- Blitz, Hanauer, Honarvar, Huisman and van Vliet (2023, FAJ): a composite of short-term signals (reversal, short-term momentum, analyst revisions, short-term risk, monthly seasonality) applied to the MSCI World standard universe (about 1,750 large caps, 1985-2021) with buy/sell rules gives significant net alpha, robust to implementation lags of several days and uncorrelated with Fama-French factors — [FAJ](https://www.tandfonline.com/doi/abs/10.1080/0015198X.2023.2173492); [SSRN 4115411](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4115411)
- Hong et al. (2015): days-to-cover value-weighted 0.67%/month (t 2.24); advantage over the short ratio shrinks as small stocks are removed — [NBER PDF](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)
- Novy-Marx and Velikov (2022): BAB's performance depends on a $1.05 position in the bottom 1% of market cap per dollar invested — [paper PDF](https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf)
- Ehsani and Linnainmaa (2022 WP): beta-neutralising residual momentum raises the Sharpe ratio most among large stocks (+149%) — [AEA conference PDF](https://www.aeaweb.org/conference/2023/program/paper/8Ah8THYY)
- Israel and Moskowitz (2013): momentum returns are largely unaffected by size over 86 years; value is weaker in large caps, and shorting matters more for large-cap momentum — [JFE](https://www.sciencedirect.com/science/article/pii/S0304405X12002401)

### Inferences
Large-cap prior by theme `[est]`, combining the sources above with v6/v7 priors:

| Theme | Large-cap evidence | Design that preserves capacity |
|---|---|---|
| Profitability | Strong, and stronger post-publication | Within-industry rank; R&D add-back |
| Issuance / investment | Issuance pervasive; asset growth absent in big stocks | Prefer issuance and composite financing members over asset growth |
| Value | Weaker in large caps; within-industry part carries the signal | Within-industry composite, current price |
| Price momentum | Present, short leg matters more, crash-prone | Add announcement-based momentum; keep industry momentum |
| Earnings momentum | Latest-surprise drift absent since about 2006-2010 | 12-month announcement-return sum |
| Low risk | Value-weighted alpha insignificant vs FF5 | Only members orthogonal to beta and vol |
| Short interest | DTC value-weighted t 2.24 to 2012; fee-exposed | Slow denominator, within size bucket |
| Reversal | Exists in large caps gross; cost-bound | Cleaned signal, used for trade timing |
| Seasonality, insider/ownership | Not re-examined here | n/a |

| id | What exactly to change | Evidence and effect size | Overfit risk | Free params |
|---|---|---|---|---|
| X1 | Within-size-tercile ranking (terciles by market cap or dollar volume inside the top 3000), then pool ranks | JKP non-micro breakpoints; removes the implicit small-cap tilt of pooled ranks | Low | 1 (bucket count) |
| X2 | Report every member's IC in the top 1000 and require same sign and at least half the full-universe IC (already v7 R5.5 iv) | EMP, Chen-Velikov, Subrahmanyam: effects shrink or vanish out of small caps | Low | 0 |
| X3 | Cap the composite weight per name by a liquidity-scaled bound (capped-weight idea applied to a rank book) | JKP capped value weights: +8.5 points replication rate | Low-Medium | 1 (cap percentile; JKP use NYSE 80th) |

- The short-term composite result (Blitz et al. 2023) partly **contradicts v6 lever 1** (shrink fast sleeves): in a large-cap universe with trading rules, a diversified set of fast signals has net alpha. One of their five signals (analyst revisions) is unavailable here, and their evidence is for a dedicated fast sleeve with its own trading rules, not for fast signals passed through a 21-day decay and a slow book.
- JKP's replication rates are about existence (sign and significance of alpha), not about net profitability; Chen-Velikov's value-weighted -2 bps is the after-cost counterpart. Both can be true.

### Gaps
- JKP's alphas by size group were only available as a figure; no numeric table was extracted.
- No study reported performance for the rank 1000-3000 segment separately from microcaps.
- No evidence on size-bucketed ranking versus pooled ranking in a rank-weighted daily book was found.

---

## 11. Post-publication decay and crowding: current estimates and haircuts for priors

### Takeaway
For US anomalies the central estimates are a 50-66% fall in gross return after publication, about 72% when only post-2005 data are used, and about 93% for cost-mitigated net returns; the decay is specific to the US and absent in pre-1926 data, which points to arbitrage capital rather than data mining. Profitability is the documented exception.

### Cited Findings
- Jacobs and Mueller (2020, JFE), 241 anomalies in 39 markets: the US is the only country with a reliable post-publication decline; for equal-weighted (value-weighted) returns the US decline is 38% (37%) post-sample and 62% (66%) post-publication — [JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X19301618); [SSRN 2816490](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2816490)
- Chen and Velikov (2023, JFQA), 204 anomalies: in-sample gross return 68 bps/month; with cost mitigation the in-sample net return is 44 bps; post-publication net 9 bps; post-publication and post-2005 net 4 bps (standard error 2 bps) — [JFQA PDF](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/945133D5A3ECEEAF466AEE91551FD225/S0022109022000874a.pdf/zeroing-in-on-the-expected-returns-of-anomalies.pdf)
- Same paper: the regression slope of post-publication on in-sample gross returns is 44% (about 50% decay); restricted to post-2005 the decay is 72%; for cost-mitigated net returns the slope is 7% (93% decay); the 90th-percentile anomaly has 127 bps/month gross in sample, about 56 bps post-publication gross, and about 6 bps net post-2005 — [JFQA PDF](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/945133D5A3ECEEAF466AEE91551FD225/S0022109022000874a.pdf/zeroing-in-on-the-expected-returns-of-anomalies.pdf)
- Same paper: post-publication gross return averages 28 bps/month; typical two-sided turnover is about 40% per month and the average spread paid post-publication is about 85 bps, which removes about 30 bps; the t-statistics of post-2005 net returns look standard normal (7% exceed 2 in absolute value); the standard deviation of true net Sharpe ratios is estimated at 0.15 per year against a mean standard error of 0.30 — [JFQA PDF](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/945133D5A3ECEEAF466AEE91551FD225/S0022109022000874a.pdf/zeroing-in-on-the-expected-returns-of-anomalies.pdf)
- Same paper: combination strategies (Fama-MacBeth, average rank, IPCA, LASSO on 58 predictors, stocks below the 20th size percentile dropped) earn about 374 bps/month gross and 188 bps net in 1985-2005 and about 21 bps/month net, or around zero, after 2005; low-frequency spread estimators overstate post-2005 spreads by 25-50 bps, so earlier cost studies overestimate future costs — [JFQA PDF](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/945133D5A3ECEEAF466AEE91551FD225/S0022109022000874a.pdf/zeroing-in-on-the-expected-returns-of-anomalies.pdf)
- Same paper: short-sale costs average roughly 10-20 bps/month (citing Cohen-Diether-Malloy 2007 and Drechsler-Drechsler 2016) and are omitted from the 4 bps figure — [JFQA PDF](https://www.cambridge.org/core/services/aop-cambridge-core/content/view/945133D5A3ECEEAF466AEE91551FD225/S0022109022000874a.pdf/zeroing-in-on-the-expected-returns-of-anomalies.pdf)
- Baltussen, van Vliet and van Vliet (working paper, pre-CRSP US data): value, momentum, low-risk and seasonality premia are sizeable and significant before 1926, and "do not materially decay out-of-sample when unaffected by post-publication arbitrage" — [SSRN 3969743](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3969743)
- Novy-Marx and Medhat (2025): the profitability factor's return roughly doubled (31 to 60 bps/month) in the 17 years that largely coincide with the period after publication of the 2013 paper — [NBER w33601 PDF](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)
- Ehsani and Linnainmaa (2022 WP): firm-specific momentum fell from 10.10%/yr (t 10.48) before 2000 to 3.45%/yr (t 1.53) after — [AEA conference PDF](https://www.aeaweb.org/conference/2023/program/paper/8Ah8THYY)
- Swade, Hanauer, Lohre and Blitz (2024, JPM): about 15 factors (10 to 20 depending on the significance level) span 153 US factors, covering 8 of 13 style clusters — [SSRN 4605976](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4605976); [JPM](https://www.pm-research.com/content/iijpormgmt/50/3/11)
- Insider signals, low-reliability sources (a vendor note and non-peer-reviewed preprints): abnormal returns to opportunistic insider trades in 2008-2024 Form 4 data are reported as 60-70% lower than in the original study, and the routine/opportunistic split is reported not to reproduce on recent data — [arXiv 2602.06198](https://arxiv.org/html/2602.06198v1); [Equibles research note](https://equibles.com/research/what-happens-after-an-insider-buys-evidence-from-47-458-open-market-purchases)

### Inferences
Suggested haircuts to in-sample gross Sharpe or return when setting priors `[est]`; these extend the flat 50% used in v6/v7:

| Signal class | Haircut to published gross | Basis |
|---|---|---|
| Generic published anomaly, liquid US names, post-2005 | 65-75% | Jacobs-Mueller 62-66%; Chen-Velikov 72% |
| Profitability family | 0-30% | Post-publication performance above in-sample |
| Issuance, investment, value composites (slow, low turnover) | 50-60% | McLean-Pontiff / Jacobs-Mueller central values |
| Price momentum, residual momentum | 60-70% on the firm-specific part | Ehsani-Linnainmaa post-2000 |
| Latest-surprise earnings drift | 80-100% in top 1000 | Martineau; Gerard-Jehl; Subrahmanyam |
| Short interest and other fee-exposed short-leg signals | 60-70%, then subtract 10-20 bps/month borrow | Chen-Velikov note on short-sale costs; v6 2d |
| Recently published signals with under 10 years of out-of-sample data (announcement-based momentum, BAC, days-to-cover) | In-sample to post-sample step only (about 35-40%), plus an allowance for decay still to come | Jacobs-Mueller post-sample 37-38% |

- Chen-Velikov's near-zero net figure is for single anomalies traded as stand-alone decile portfolios paying effective spreads of 67-85 bps; a rank-weighted, netted, top-3000 book with 13 bps/$ modelled cost is a different object. The relevant lesson is the slope (7% for net, 44% for gross), which says in-sample net performance carries almost no information about which anomaly will be best later. That supports equal theme weights with prior-based, not TRAIN-based, selection.
- With the standard deviation of true Sharpe ratios across anomalies at about 0.15 and the standard error of each estimate at about 0.30 (on samples far longer than 3 years), choosing between two definitions of the same signal on a 3-year paired test is dominated by noise unless the paired standard error is below roughly 0.05 `[est]`.
- Decay being US-specific and absent pre-1926 implies crowding, not false discovery, is the main driver; signals that are cheap to arbitrage and well known (large-cap momentum, latest-surprise drift) should carry the largest haircuts, slow signals with structural holders on the other side (profitability, issuance) the smallest `[est]`.
- The factor-zoo compression result (about 15 factors span 153) bounds the number of independent bets available from public price and accounting data; with 48 signals in 10 themes the book is near that bound, so definitional upgrades are more likely to lower noise and turnover than to add independent return `[est]`.

### Gaps
- McLean-Pontiff and Falck-Rej-Thesmar were not re-read; their numbers are as reported in v6/v7.
- No peer-reviewed estimate of insider-signal decay after 2015 was found; the cited sources are low reliability and should not set a prior by themselves.
- No crowding measure for 2023-2026 (hedge-fund factor exposure, comomentum, short-interest concentration) was found in accessible sources.
- No study gives decay estimates specifically for composite, rank-weighted, multi-signal books.

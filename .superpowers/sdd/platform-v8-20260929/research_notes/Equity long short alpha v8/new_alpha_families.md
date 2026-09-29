# New, orthogonal alpha families for the v8 library (US equity L/S, low turnover, $1-8bn), state of knowledge 2026-09

Conventions used throughout.

- **CZ-OP** = the original paper's own statistic as recorded in the Chen-Zimmermann signal documentation (`SignalDoc.csv`): monthly long-short return in %, t-stat, sample years, weighting. Source: [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv).
- **CZ-own** = my own computation on the Chen-Zimmermann public portfolio return files (October 2025 release, monthly returns to 2024-12), NOT a published number. Three implementations: **OP** (original paper's weighting and quantiles), **XM** (ex-microcap: market equity above the NYSE 20th percentile, original weighting), **VW** (value weights forced). All are gross of costs, monthly rebalanced, CRSP universe, so they overstate what a top-3000, cost-aware, slow book earns. Source for all CZ-own numbers: [openassetpricing.com data](https://www.openassetpricing.com/data/) (files `PredictorLSretWide.csv`, `PredictorAltPorts_LiqScreen_ME_gt_NYSE20pct.zip`, `PredictorAltPorts_LiqScreen_VWforce.zip`).
- **Spanning alpha** = CZ-own intercept from regressing the candidate's XM long-short return, 2005-01..2024-12 (240 months), on nine theme-proxy composites built from CZ portfolios: value (BMdec, CF, EP, cfp, EntMult, SP, NetPayoutYield), profitability (GP, CBOperProf, OperProf, roaq, RoE, PS), investment (AssetGrowth, NOA, ShareIss1Y, dNoa), earnings momentum (EarningsSurprise, AnnouncementReturn, ChTax, NumEarnIncrease), momentum (Mom12m, IndMom, High52, ResidualMomentum), low risk (BetaFP, IdioVol3F, MaxRet), short interest (ShortInterest), reversal/seasonality (STreversal, MomSeasonShort), accruals (Accruals). These are proxies for the library's themes, not the library itself. There is no options_implied or ownership_flow proxy.
- **JKP cluster** = the theme assigned in Jensen-Kelly-Pedersen's cluster file. Source: [Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv).
- Data availability statements come from the local atx-db docs (`C:/atx/atx-db/docs/ALPHA_PANEL*.md`), read 2026-09-29.
- Grades (A, A-, B+, B, B-, C+) and turnover classes are my judgement and appear only under Inferences.
- **Process note.** The session's web-search budget ran out part-way through. Families that could not be verified online are listed under Gaps, not reported as findings.

## Key question 1: Which signal families with grade A/B evidence are missing from the library?

### Takeaway
No missing family has grade A evidence once the test is "post-2004, ex-microcap or value-weighted, holding period of months". The best buildable additions are B/B- level: a tax-to-book-income ratio (Lev-Nissim), 5-year composite equity issuance, coskewness, percent accruals, Mohanram's G-score, and three event or structure families with no counterpart in the book (8-K/NT filing events, overnight-intraday "tug of war", frog-in-the-pan conditioning of momentum). Several famous families fail the post-2004 test outright (Amihud illiquidity, turnover, long-term reversal, Soliman DuPont changes, short-lag seasonality, dividend initiations, debt issuance), and the strongest cross-firm momentum results need data the platform does not have (segments, patents, analyst coverage).

### Cited Findings

**What the two replication projects say about the candidate categories**
- With NYSE breakpoints and value weights, 286 of 447 anomalies (64%) are insignificant at the 5% level and 380 (85%) fail t >= 3. — [Hou-Xue-Zhang, Replicating Anomalies, NBER w23394](https://www.nber.org/system/files/working_papers/w23394/w23394.pdf)
- The worst category is trading frictions: 95 of 102 variables (93%) are insignificant. Named failures include Jegadeesh short-term reversal, Datar-Naik-Radcliffe share turnover, Chordia-Subrahmanyam-Anshuman volume variability, Amihud illiquidity, Acharya-Pedersen liquidity betas and Ang et al. idiosyncratic volatility. — [Hou-Xue-Zhang, NBER w23394](https://www.nber.org/system/files/working_papers/w23394/w23394.pdf)
- Failures by category: 20 momentum, 37 value-versus-growth, 11 investment, 46 profitability, 77 intangibles, 95 trading frictions. — [Hou-Xue-Zhang, NBER w23394](https://www.nber.org/system/files/working_papers/w23394/w23394.pdf)
- Jensen-Kelly-Pedersen use 153 factors in 93 countries, classify them into 13 themes, and report a baseline US replication rate of 56.9% on raw returns against Hou-Xue-Zhang's 35%. — [JKP, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- JKP estimate a replication rate above 75% in 10 of 13 themes; the exceptions are seasonality, leverage and size. In the ex-post tangency portfolio of 13 theme portfolios, 10 themes have significantly positive weights; the three displaced themes are profitability, investment and size. — [JKP, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- The 13 JKP clusters are: Accruals, Debt Issuance, Investment, Low Leverage, Low Risk, Momentum, Profit Growth, Profitability, Quality, Seasonality, Short-Term Reversal, Size, Value. — [JKP Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv)
- Mining 29,000 accounting ratios for t > 2.0 gives post-sample predictability similar to peer-reviewed predictors; about 50% of predictability remains after the original sample for both. — [Chen-Lopez-Lira-Zimmermann, arXiv 2212.10317](https://arxiv.org/abs/2212.10317)

**Families that pass a post-2004 ex-microcap or value-weighted test (CZ-own, 2005-2024, % per month, t in brackets)**

| signal (CZ acronym) | paper | OP | XM | VW | VW 2012-24 | spanning alpha (XM) | R2 vs themes |
|---|---|---|---|---|---|---|---|
| Tax | Lev-Nissim 2004 | .37 (3.19) | .34 (2.80) | .40 (3.00) | .57 (3.54) | .18 (1.69) | .25 |
| CompEquIss | Daniel-Titman 2006 | .35 (2.53) | .38 (2.24) | .56 (2.56) | .52 (1.95) | .38 (2.68) | .36 |
| ShareIss5Y | Daniel-Titman 2006 | .84 (3.42) | .41 (2.69) | .45 (3.03) | .52 (2.80) | .10 (1.01) | .63 |
| Coskewness | Harvey-Siddique 2000 | .34 (2.18) | .39 (2.86) | .34 (2.18) | .28 (1.43) | .39 (3.07) | .20 |
| MomSeason16YrPlus | Heston-Sadka 2008 | .52 (3.06) | .45 (3.28) | .55 (2.49) | .68 (2.54) | .45 (3.40) | .15 |
| MS (G-score) | Mohanram 2005 | .51 (2.58) | .43 (2.52) | .39 (1.85) | .59 (2.21) | .32 (2.11) | .27 |
| PctAcc | Hafzalla et al. 2011 | .24 (2.50) | .25 (2.30) | .21 (1.07) | .17 (.78) | .16 (1.94) | .48 |
| EarningsConsistency | Alwathainani 2009 | .43 (3.00) | .23 (1.83) | .46 (1.91) | .41 (1.26) | .22 (2.38) | .46 |
| IndRetBig | Hou 2007 | .64 (2.87) | .55 (2.43) | .36 (1.48) | .06 (.24) | .52 (2.67) | .33 |
| retConglomerate | Cohen-Lou 2012 | .52 (2.35) | .49 (1.97) | .28 (1.07) | .22 (.68) | .48 (2.29) | .37 |
| AgeIPO | Ritter 1991 | .89 (2.79) | .76 (2.43) | .97 (2.92) | .90 (1.99) | .75 (2.65) | .26 |
| ExchSwitch | Dharan-Ikenberry 1995 | .76 (2.82) | .71 (2.48) | .61 (2.06) | .76 (2.09) | .61 (2.18) | .11 |
| IO_ShortInterest | Asquith-Pathak-Ritter 2005 | 5.82 (3.31) | 2.60 (3.24) | 2.89 (2.56) | 2.97 (1.87) | 2.45 (3.24) | .18 |
| FirmAgeMom | Zhang 2006 | .80 (2.63) | .75 (2.19) | .84 (2.08) | 1.33 (2.73) | .68 (2.79) | .54 |
| OPLeverage | Novy-Marx 2011 | .28 (1.42) | .47 (3.05) | .45 (2.18) | .28 (1.21) | .51 (3.96) | .36 |

— all rows: [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/); paper attributions from [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)

- OPLeverage is (COGS + SG&A) / total assets. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv). The library member `opex_at` uses the proxy (sale - oi) / assets and is described as correlated with GP/A through asset turnover. — local file `C:/atx/.superpowers/sdd/mega-alpha-20260926/v6-literature.md` section 3.6. So this row confirms an existing member; it is not a new family.

**Families that FAIL the same test (CZ-own, 2005-2024)**

| family | signal | OP | XM | VW | verdict |
|---|---|---|---|---|---|
| Illiquidity premium | Illiquidity (Amihud 2002) | -.01 (-.11) | -.09 (-.65) | -.07 (-.48) | dead |
| Dollar volume | DolVol (Brennan et al. 1998) | -.03 (-.18) | -.15 (-1.35) | -.41 (-2.14) | wrong sign |
| Share turnover | ShareVol (Datar et al. 1998) | .19 (.91) | -.04 (-.27) | .04 (.19) | dead |
| Zero-trade days | zerotrade6M (Liu 2006) | .22 (.56) | .22 (.86) | .01 (.04) | dead |
| Long-term reversal | LRreversal (De Bondt-Thaler) | -.20 (-.66) | -.03 (-.11) | -.10 (-.28) | dead |
| Same-month seasonality, lag 1 | MomSeasonShort | .04 (.17) | -.23 (-1.13) | -.22 (-.63) | dead |
| Same-month seasonality, lags 2-5 | MomSeason | .08 (.30) | -.03 (-.15) | -.65 (-2.06) | dead / wrong sign VW |
| Same-month, lags 6-10 | MomSeason06YrPlus | .16 (.71) | .24 (1.66) | .19 (.79) | weak |
| Same-month, lags 11-15 | MomSeason11YrPlus | .31 (1.80) | .11 (.80) | .17 (.72) | weak |
| Trend factor | TrendFactor (Han-Zhou-Zhu 2016) | .41 (1.70) | .41 (1.69) | .17 (.58) | weak; VW 2012-24 -.29 (-.93) |
| DuPont: asset turnover change | ChAssetTurnover (Soliman 2008) | .07 (.87) | .04 (.51) | -.08 (-.60) | dead |
| DuPont: NNCOA, NWC changes | ChNNCOA, ChNWC | -.02, .01 | .11 (1.45), -.05 | .04, -.02 | dead |
| Sales growth minus inventory growth | GrSaleToGrInv (Abarbanell-Bushee) | -.14 (-1.46) | -.17 (-2.06) | -.19 (-1.21) | wrong sign |
| Inventory change | ChInv (Thomas-Zhang 2002) | .17 (1.42) | .02 (.20) | .19 (.96) | dead |
| Hiring | hire (Bazdresch-Belo-Lin 2014) | .22 (1.53) | .21 (1.39) | -.24 (-1.19) | dead |
| Net debt financing | NetDebtFinance (BRS 2006) | .41 (3.30) | .16 (1.34) | -.33 (-1.85) | EW only; VW 2012-24 -.50 (-2.00) |
| Composite debt issuance | CompositeDebtIssuance | .24 (2.76) | .10 (1.08) | -.08 (-.52) | EW only |
| Net external financing | XFIN (BRS 2006) | 1.28 (4.05) | .61 (2.43) | .50 (1.51) | works but spanning alpha -.03 (-.24), R2 .81 |
| Dividend initiation | DivInit | .23 (1.21) | .12 (.75) | -.18 (-.64) | dead |
| Dividend omission | DivOmit | 1.08 (3.00) | .48 (1.26) | .13 (.37) | microcap only |
| Repurchase indicator | ShareRepurchase | .34 (2.39) | .22 (2.38) | .08 (.67) | spanning alpha .01 (.27), R2 .71 |
| Return skewness | ReturnSkew (Bali-Engle-Murray) | .18 (1.42) | .00 (.01) | .17 (1.27) | dead |
| Liquidity beta | BetaLiquidityPS | -.05 (-.22) | .01 (.02) | -.05 (-.22) | dead |
| R&D capital, R&D ability, advertising | RDcap, RDAbility, AdExp | -.05, .13, .12 | n/a, -.16, .18 | -.05, -.01, -.01 | dead |
| Intangible return (Daniel-Titman) | IntanBM / IntanCFP / IntanEP / IntanSP | .10 to .32 | .11 to .32 | -.20 to -.40 | dead VW |
| Abnormal accruals, total accruals | AbnormalAccruals, TotalAccruals | -.12, -.04 | .08, .13 | .16, -.00 | dead |
| Price delay | PriceDelayRsq (Hou-Moskowitz 2005) | -.24 (-1.03) | -.52 (-2.93) | -.76 (-2.95) | wrong sign |
| Customer momentum | CustomerMomentum (Cohen-Frazzini) | .29 (1.27) | .33 (1.39) | .29 (1.27) | weak; also data-gated |
| Industry customer / supplier momentum | iomom_cust, iomom_supp (Menzly-Ozbas) | .15 (.76), .08 (.39) | .17, .09 | .28 (1.51), .27 (1.20) | weak |
| Breadth of ownership | DelBreadth (Chen-Hong-Stein) | .31 (1.38) | .31 (1.38) | .14 (.65) | weak |
| Organisation capital | OrgCap (Eisfeldt-Papanikolaou) | .31 (1.95) | .22 (1.57) | .31 (1.95) | borderline; VW 2012-24 .13 (.68) |
| Dividend month | DivSeason (Hartzmark-Solomon) | .20 (5.69) | .07 (1.98) | .11 (1.73) | small; VW 2012-24 .04 (.43) |

— all rows: [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)

**Families whose published evidence is strong but which the platform cannot build today**
- Shared analyst coverage: connected-stock momentum factor alpha 1.68%/month (t 9.67), Sharpe 1.71 over 1984-2015; it subsumes industry, geographic, customer, customer/supplier-industry, single-to-multi-segment and technology momentum in spanning tests. It needs analyst coverage data. — [Ali-Hirshleifer 2020 JFE, AQR copy](https://images.aqr.com/-/media/AQR/Documents/AQR-Insight-Award/2019/Shared-Analyst-Coverage_Ali-Hirshleifer.pdf); [SSRN](https://www.ssrn.com/abstract=3015582)
- Customer-supplier links need Compustat segment principal-customer data; conglomerate returns need OPSEG/BUSSEG segment data. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv). Company Facts, the platform's XBRL source, drops dimensional facts, so segment-only revenue is invisible. — local file `C:/atx/atx-db/docs/ALPHA_PANEL.md`, Known limitations
- Technological links use patent data to measure technological closeness: value-weighted long-short 69 bps/month (t 3.19), equal-weighted 117 bps/month (t 5.47). — [Alpha Architect summary of Lee-Sun-Wang-Zhang 2019 JFE](https://alphaarchitect.com/technological-links-and-predictable-returns/); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3036241)
- Text: "Lazy Prices" long non-changers / short changers of 10-K and 10-Q language earns 34-58 bps/month value-weighted (t up to 3.59), 1995-2014, accruing for up to 18 months without reversal; average changer has $3.5bn market cap and modest shorting fees; turnover is modest. Needs EDGAR text ingestion. — [Cohen-Malloy-Nguyen, NBER w25084](https://www.nber.org/system/files/working_papers/w25084/w25084.pdf)
- Not in any atx-db source: GICS, NAICS, consensus, borrow fee and utilisation, options skew / volume / OI, VWAP, trade count, index add/drop; 13F filer type is not classified; buyback announcements sit in 8-K 8.01/7.01 text and are not parsed; no going-concern flag; no employee count among the delivered items. — local files `C:/atx/atx-db/docs/ALPHA_PANEL_STATUS.md` and `C:/atx/atx-db/docs/ALPHA_PANEL_REQUEST_V7_RESPONSE.md`

### Inferences
- Grade A is empty. The highest honest grade for a new, buildable, slow signal is B (Tax, CompEquIss, Coskewness), because each rests on one paper plus a CZ-own replication that I ran, not on an independent published post-2004 test.
- The v6 proposal to extend `seasonality_same_month` to annual lags 1..5 is contradicted by the CZ-own numbers: lags 1 and 2-5 earned nothing ex-microcap after 2005 and -0.65%/month value-weighted for lags 2-5. Only lags 16-20 kept a premium, and those lags cannot be built from prices that start 2012-03. The existing single-lag member deserves a review, not an extension.
- The v6 entry "Composite issuance (5y) / XFIN, A-" should be split. XFIN is fully spanned by the value and profitability proxies (R2 .81, alpha t -0.24). CompEquIss keeps an alpha (t 2.68, R2 .36). ShareIss5Y (pure share count growth) sits between (alpha t 1.01, R2 .63).
- Every "volume / liquidity level" family should be dropped from the candidate list. This agrees with Hou-Xue-Zhang's 93% failure rate for trading frictions and with the $1-8bn capacity constraint, which rules out an illiquidity tilt anyway.
- Debt issuance and dividend events earn returns only in equal-weighted, microcap-heavy portfolios. They do not suit a top-3000 book.

### Gaps
- No independent, published post-2004 large-cap test was found for Tax, Coskewness, Mohanram G-score, EarningsConsistency or ExchSwitch. The CZ-own numbers are the only post-2004 evidence here and come from one data source.
- CZ portfolios are rebalanced monthly on the CRSP universe. None of the CZ-own numbers reflects the platform's universe (top 3000 by dollar volume, price > $5), its acceptance-clock timing, or costs.
- Not verified online in this session (search budget exhausted): Gervais-Kaniel-Mingelgrin high-volume return premium; Beneish-Lee-Nichols M-score returns; Peters-Taylor knowledge capital; Lou (2012) flow-induced trading; Jiao-Massa-Zhang hedge-fund-versus-short-interest; Hoberg-Phillips text-based industry momentum; Fu-Huang on the disappearance of buyback and SEO drift; lockup-expiry and SEO event returns; 8-K Item 5.02 executive departures; insider-silence and insider-horizon variants of Form 4 signals; Swade-Hanauer-Lohre-Blitz "Factor Zoo (.zip)". These should be treated as unresearched, not as rejected.

## Key question 2: For each family, the exact definition, evidence, robustness, turnover, correlation, data need, grade and known failures

### Takeaway
The cards below give the paper's definition and the numbers. The pattern is consistent: original t-stats of 3-10 shrink to 1.5-3 after 2004 outside microcaps, and the signals that keep an alpha against the existing themes are those built from information the book does not already use (tax accounts, co-moments, five-year issuance, event filings, the open price).

### Cited Findings

**F1. Tax-to-book income (Lev-Nissim 2004, The Accounting Review)**
- Definition: (federal + foreign income taxes; if missing, total taxes minus deferred taxes) / (statutory tax rate x net income before extraordinary items). Rate is .35 from 1993 on. If net income is negative and the numerator positive, set to 1. Exclude price < $5. Annual rebalance (12-month portfolio period), long high ratio. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original: t = 3.85 in regression, 1973-2000; the documentation notes it "only works in the subsample 1973-1992" in Table 5 and in 1993-2000 only if 1998 is dropped. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own: in-sample replication .44%/month (t 3.45); 2005-24 OP .37 (3.19), XM .34 (2.80), VW .40 (3.00); VW 2012-24 .57 (3.54). Spanning alpha .18 (t 1.69), R2 .25; largest correlations: investment +.37, value +.32. — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)
- JKP place tax-related factors apart from each other: `tax_gr1a` (change in taxes) in Profit Growth and `pi_nix` (pretax income to net income) in Seasonality. — [JKP Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv)
- Data: the platform carries `txt_q` (total tax) and `ni_q`/`ni_ttm`, but no current-versus-deferred tax split in its item list. — local file `C:/atx/atx-db/docs/ALPHA_PANEL.md`, stage F

**F2. Composite equity issuance, 5 years (Daniel-Titman 2006, JF)**
- Definition (CompEquIss): 5-year growth rate of market value of equity minus 5-year stock return; low is good. ShareIss5Y: 5-year growth in split-adjusted shares. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original: t = 4.39 (multivariate regression), 1968-2003. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own: CompEquIss 2005-24 OP .35 (2.53), XM .38 (2.24), VW .56 (2.56), VW 2012-24 .52 (1.95); spanning alpha .38 (2.68), R2 .36, largest correlations low risk -.34 and profitability +.34. ShareIss5Y: XM .41 (2.69), VW .45 (3.03), VW 2012-24 .52 (2.80); spanning alpha .10 (1.01), R2 .63. — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)
- JKP put equity net issuance and net payout (`eqnetis_at`, `netis_at`, `chcsho_12m`, `eqnpo_12m`) in the Value cluster. — [JKP Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv)
- Data: prices start 2012-03-26 and shares are in fundamentals from 2010. — local file `C:/atx/atx-db/docs/ALPHA_PANEL_REQUEST_V7_RESPONSE.md`, D7

**F3. Coskewness (Harvey-Siddique 2000 JF; Ang-Chen-Xing 2006 RFS)**
- Definition: sample counterpart of E[r_i r_m^2] / (SD[r_i] SD[r_m]^2) with de-meaned returns; Harvey-Siddique use 60 months of monthly excess returns; Ang-Chen-Xing use one year of daily returns, continuously compounded. Long low coskewness. Monthly rebalance. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original: Harvey-Siddique .30%/month, t 1.96, VW, 1964-1993 ("3.60 percent annual hedge return and p value < 0.05 ... but no table"); Ang-Chen-Xing .28%/month, t 2.76, EW, NYSE only, 1963-2001. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own: Harvey-Siddique version 2005-24 XM .39 (2.86), VW .34 (2.18), VW 2012-24 .28 (1.43); spanning alpha .39 (3.07), R2 .20; largest correlations momentum -.31, value +.30. Daily version: XM .40 (2.24), VW .06 (.25); spanning alpha .38 (2.34). — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)
- JKP assign `coskew_21d` to the Seasonality cluster and idiosyncratic skewness (`iskew_*`, `rskew_21d`) to Short-Term Reversal. — [JKP Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv)
- Tail-risk beta (Kelly-Jiang 2014) needs a 120-month rolling regression; CZ-own 2005-24 OP .19 (.78), VW .41 (1.31). — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)

**F4. Percent accruals (Hafzalla-Lundholm-Van Winkle 2011)**
- Definition: (income before extraordinary items - operating cash flow) / |income before extraordinary items|; divide by .01 if income is 0; exclude price < $5; annual rebalance; long low. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original: .97%/month, t about 3.29 (converted from p < .001), EW deciles, 1989-2008. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own: 2005-24 OP .24 (2.50), XM .25 (2.30), VW .21 (1.07), VW 2012-24 .17 (.78); spanning alpha .16 (1.94), R2 .48, correlation with Sloan accruals +.60. Percent total accruals: XM .10 (.99), VW .03 (.22). — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)
- JKP cluster: `oaccruals_ni` and `taccruals_ni` are in Accruals. — [JKP Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv)

**F5. Mohanram G-score (2005, RAS)**
- Definition: evaluated only among low book-to-market firms; combines three signals on profitability and cash flow, two on income volatility, three on investment (R&D, capex, advertising). Monthly portfolio period in the replication. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original: 1.58%/month, t 9.14, EW, 1978-2001, with a non-standard data lag; the replicators "get a t-stat near 6" and call the signal "really complicated". — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own: 2005-24 OP .51 (2.58), XM .43 (2.52), VW .39 (1.85), VW 2012-24 .59 (2.21); spanning alpha .32 (2.11), R2 .27. — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)

**F6. Earnings consistency (Alwathainani 2009)**
- Definition: average earnings growth over the previous 48 months, where growth = (EPS - EPS 12 months ago) / average of EPS 12 and 24 months ago; exclude |growth| > 600% and cases where growth and its 12-month lag differ in sign. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original: .36%/month, t 2.67, 1971-2002; the replicators could not access the original paper and used the dissertation. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own: 2005-24 OP .43 (3.00), XM .23 (1.83), VW .46 (1.91), VW 2012-24 .41 (1.26); spanning alpha .22 (2.38), R2 .46; correlation with CompEquIss +.46. — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)
- The CZ "EarningsStreak" signal (Loh-Warachka 2012; 2005-24 XM .42, t 3.40) is defined on analyst forecast surprises and is therefore not buildable. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)

**F7. Cross-firm momentum / economic links**
- Hou (2007) definition: average monthly return of the 30% largest firms in the same Fama-French 48 industry; the largest 30% are excluded from the traded universe. Original t = 11 (regression), 1972-2001, 1-month holding. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own IndRetBig: in-sample 2.33 (9.54); 2005-24 OP .64 (2.87), XM .55 (2.43), VW .36 (1.48), VW 2012-24 .06 (.24). Spanning alpha .52 (2.67), R2 .33, correlation with the reversal proxy -.54. — [own computation](https://www.openassetpricing.com/data/)
- Cohen-Lou (2012) definition: sales-weighted return of stand-alone firms in each of a conglomerate's 2-digit SIC segments; original 1.18%/month, t 5.51 EW, "also works VW (t=3.2)", 1977-2009. CZ-own 2005-24 OP .52 (2.35), XM .49 (1.97), VW .28 (1.07). Its 2005-24 correlation with IndRetBig is .67. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- Cohen-Frazzini (2008): original 1.58%/month, t 3.79, VW quintiles, 1980-2004; CZ-own 2005-24 .29 (1.27). — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- Shared-analyst momentum: value-weighted quintile long-short five-factor alpha 1.19%/month (t 6.71); cumulative return rises to 3.21% one year after formation, so most of the return arrives in month 1. In the more recent half-sample the alpha is 1.13%/month (t 4.48), while only two of seven earlier cross-asset momentum strategies keep significant (and small) alphas. Among large stocks (above NYSE median) longer-term lags of connected, industry and geographic returns are all insignificant. — [Ali-Hirshleifer, AQR copy](https://images.aqr.com/-/media/AQR/Documents/AQR-Insight-Award/2019/Shared-Analyst-Coverage_Ali-Hirshleifer.pdf)
- Geographic lead-lag between co-headquartered firms in different sectors: risk-adjusted 5-6% per year, about half the industry lead-lag effect, and unrelated to size, liquidity or analyst coverage. — [Parsons-Sabbatucci-Titman 2020 RFS](https://academic.oup.com/rfs/article-abstract/33/10/4721/5682420); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2780139)
- Critique: cross-firm predictability arises when both firms have own momentum and correlated returns; momentum and news contribute about equally at 1 month, and commonality in momentum alone explains longer horizons. — [Burt-Hrdlicka 2021 JFQA](https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/abs/where-does-the-predictability-from-sorting-on-returns-of-economically-linked-firms-come-from/6B9F2560E0A27615572309C6F295AFBF)
- Data: the issuer profile holds `business_state_or_country`, but only as a 2026-09-19 snapshot flagged `snapshot_non_pit`; SIC codes are dated. — local file `C:/atx/atx-db/docs/ALPHA_PANEL_SEC.md`

**F8. Shared 13F ownership ("connected stocks", Anton-Polk 2014 JF)**
- Definition: common ownership FCAP_ij = total value of the two stocks held by their common active funds / total market cap of the two stocks, measured each quarter-end. A stock's connected return is the return on the portfolio of stocks abnormally connected to it. The strategy buys stocks whose own and connected returns are both low and sells those with both high. — [Anton-Polk, JF 2014](https://personal.lse.ac.uk/polk/research/ConnectedStocks.pdf)
- Evidence: five-factor alpha 76 bps/month (t 4.96), more than 9% per year after controlling for market, size, value, momentum and own short-term reversal; over 71% of alpha from the long (low) side; a version ignoring own return earns 36 bps/month (t 4.13). Sample 1980-2008, stocks above the NYSE median capitalisation, active mutual funds. — [Anton-Polk, JF 2014](https://personal.lse.ac.uk/polk/research/ConnectedStocks.pdf)
- The long-short hedge fund index loads negatively on the strategy, more so when VIX rises. — [Anton-Polk, JF 2014](https://personal.lse.ac.uk/polk/research/ConnectedStocks.pdf)

**F9. Other 13F signals**
- Breadth change (Chen-Hong-Stein 2002): quarterly change in the number of 13F institutional owners, excluding the lowest NYSE size quintile; original .67%/month, t 3.96, EW deciles, 1979-1998, 3-month holding. CZ-own 2005-24 OP and XM .31 (1.38), VW .14 (.65); spanning alpha .36 (2.57) with R2 .65 and correlation +.74 with the momentum proxy. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- Short interest x institutional ownership (Asquith-Pathak-Ritter 2005): among NYSE stocks with short interest above the 99th percentile, sort on institutional ownership. CZ-own 2005-24 XM 2.60 (3.24), VW 2.89 (2.56); spanning alpha 2.45 (3.24). The portfolio is by construction the top 1% of short interest. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- Residual institutional ownership interactions (Nagel 2005: RIO_MB, RIO_Turnover, RIO_Volatility) turn negative value-weighted after 2005: -.47 (-1.92), -.74 (-2.23), -.54 (-1.26). — [own computation](https://www.openassetpricing.com/data/)
- Hedge fund crowding: days-ADV = hedge fund holdings / average daily volume rose from about 18 days in 2004 to 26 in 2016; the days-ADV factor is significant for about 26% of hedge funds; higher exposure means larger drawdowns in industry distress. Sample: hedge fund long US equity positions from 13F, 2004-2016. — [Brown-Howard-Lundblad, working paper copy](https://uncipc.com/wp-content/uploads/2019/02/CTTR.pdf); [RFS 2022](https://academic.oup.com/rfs/article-abstract/35/7/3231/6371884)
- Data: 13F holdings 2013q2-2026q2 with aggregates `inst_shares, n_holders, top10_share` and quarter-on-quarter changes, as-of-45-day PIT version; filer type is not classified. — local file `C:/atx/atx-db/docs/ALPHA_PANEL_REQUEST_V7_RESPONSE.md`, D1

**F10. Overnight versus intraday returns**
- Lou-Polk-Skouras (2019 JFE): sample 1993-2013; stocks with price < $5 or in the bottom NYSE size quintile are excluded; value-weighted deciles with NYSE breakpoints. Sorting on last month's overnight return: the winner-minus-loser portfolio has a three-factor overnight alpha of 3.47%/month (t 16.83) and an intraday alpha of -3.02%/month (t -9.74). Sorting on last month's intraday return: intraday alpha 2.41%/month (t 7.70), overnight alpha -1.77%/month (t -7.89). Effects remain statistically significant up to five years later. — [Lou-Polk-Skouras, JFE 2019](https://personal.lse.ac.uk/polk/research/TugOfWar.pdf)
- For 14 strategies, profits are earned either entirely overnight (momentum variants, short-term reversal) or entirely intraday (size, value, profitability, investment, beta, idiosyncratic volatility, issuance, accruals, turnover). A one-standard-deviation rise in a strategy's smoothed overnight-minus-intraday spread forecasts a 1.01% higher close-to-close strategy return; the sign is as predicted for all but one of 11 anomalies and significant for six. — [Lou-Polk-Skouras, JFE 2019](https://personal.lse.ac.uk/polk/research/TugOfWar.pdf)
- Akbas-Boehmer-Jiang-Koch (2022 JFE): a higher monthly frequency of positive overnight returns followed by negative daytime reversals predicts higher future returns in the cross-section. — [ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0304405X21004116)
- Barardehi-Bogousslavsky-Muravyev (RFS, 2026): over 1926-2019, portfolios formed on past intraday returns show momentum without long-term reversal, and portfolios formed on past overnight returns show no momentum. — [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4069509); [RFS](https://academic.oup.com/rfs/advance-article-abstract/doi/10.1093/rfs/hhag036/8626980)
- Data: the panel carries `open` and `ret_intraday`, `ret_overnight`; no VWAP. — local file `C:/atx/atx-db/docs/ALPHA_PANEL_REQUEST_V7_RESPONSE.md`, D9

**F11. Information discreteness ("frog in the pan", Da-Gurun-Warachka 2014 RFS)**
- Definition: ID = sgn(PRET) x [%neg - %pos], where PRET is the cumulative return over the past 12 months skipping the most recent month, and %pos, %neg are the shares of days in the formation period with positive and negative returns. A variant scales by (%neg + %pos) to handle zero-return days. Low ID = continuous information. — [Da-Gurun-Warachka, RFS 2014](https://academicweb.nd.edu/~zda/Frog.pdf)
- Evidence: sequential double sorts (PRET quintiles, then ID) after a $5 price filter, 1927-2007; six-month momentum rises monotonically from -2.07% for discrete to 5.94% for continuous information. — [Da-Gurun-Warachka, RFS 2014](https://academicweb.nd.edu/~zda/Frog.pdf)
- An extension applies information discreteness to the lead-lag literature. — [Huang-Lee-Song-Xiang 2022 JFE](https://ideas.repec.org/a/eee/jfinec/v145y2022i2p83-102.html)

**F12. 8-K and filing events**
- Late filings: in the five days around a Form NT, mean returns are -2.93% (NT 10-Q) and -1.96% (NT 10-K); abnormal returns keep drifting down in the post-filing months; drift is less pronounced when the stated reason is an accounting one; sample of 2,115 first-time late filers. — [Columbia Law blog on Bartov-Konchitchki 2017, Accounting Horizons](http://clsbluesky.law.columbia.edu/2017/11/27/how-missing-sec-filing-deadlines-affects-a-companys-stock-value/)
- Item 4.02 (non-reliance): over 8,000 disclosures 2004-2023; average cumulative abnormal return -1.1% one day after and -2% over a 20-day window (2007-2023, excluding 2021); 97% lead to a restatement. This is a data vendor's study, not a peer-reviewed paper. — [sec-api.io](https://sec-api.io/resources/stock-price-reactions-to-item-4-02-disclosures-in-sec-form-8-k-filings)
- Data: every 8-K item code (2,846,172 item rows) and 1.08M classified events: 2.02, 2.01, 5.01, 1.01, 1.03, 3.01, 4.02, 4.01, Forms 25 / 25-NSE / 15, and NT 10-K, NT 10-Q, NT 20-F, each with an acceptance clock. — local files `C:/atx/atx-db/docs/ALPHA_PANEL_SEC.md` and `ALPHA_PANEL_REQUEST_V7_RESPONSE.md`, D12

**F13. Return seasonality beyond the library's single lag**
- Definitions: MomSeason = average same-calendar-month return over years 2-5; 06YrPlus = years 6-10; 11YrPlus = years 11-15; 16YrPlus = years 16-20; MomOffSeason06YrPlus = average return in the other months over years 6-10. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original (Heston-Sadka 2008, EW deciles, NYSE/AMEX, 1965-2002): years 2-5 .67 (t 5.35); 6-10 .68 (6.15); 11-15 .66 (6.43); 16-20 .52 (4.58); lag 1 1.15 (7.60). — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- CZ-own 2005-24 is in the two tables under Key question 1. Off-season years 6-10: OP .45 (2.15), XM .27 (1.77), VW .60 (2.30), VW 2012-24 .60 (1.80); spanning alpha .22 (1.49). — [own computation](https://www.openassetpricing.com/data/)
- Keloharju-Linnainmaa-Nyberg report 13% per year for a same-calendar-month strategy and seasonalities in anomalies, indices and commodities. — [JF 2016](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12398)
- JKP: seasonality is one of three themes with a replication rate below 75%. — [JKP, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- Dividend month (Hartzmark-Solomon 2013): a dividend is predicted if a quarterly dividend was paid 3, 6, 9 or 12 months ago, a semi-annual one 6 or 12 months ago, or an annual one 12 months ago; buying predicted payers earns 41 bps abnormal return; original long-short .36%/month, t 16.2, 1927-2011. — [JFE 2013](https://www.sciencedirect.com/science/article/abs/pii/S0304405X13000585); [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)

**F14. Price-path variants**
- Intermediate momentum (Novy-Marx 2012; return from t-12 to t-6): CZ-own 2005-24 OP(VW) 1.06 (2.38), XM .44 (1.07); spanning alpha about zero in the first specification (.27, t .79). JKP put `ret_12_7` in Profit Growth. — [own computation](https://www.openassetpricing.com/data/); [JKP Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv)
- Momentum among young firms (Zhang 2006): 6-month return within the bottom quintile of firm age, price >= $5, age >= 12 months. CZ-own 2005-24 XM .75 (2.19), VW .84 (2.08), VW 2012-24 1.33 (2.73); correlation +.68 with the momentum proxy; spanning alpha .68 (2.79). — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- Momentum x volume (Lee-Swaminathan 2000): 2005-24 OP 1.56 (2.45), XM .73 (1.41), correlation +.86 with momentum. — [own computation](https://www.openassetpricing.com/data/)
- Context for existing members: Mom12m XM .31 (.76), High52 XM -.01 (-.02), ResidualMomentum XM .07 (.29) over 2005-24. — [own computation](https://www.openassetpricing.com/data/)

**F15. New-listing and listing-change events**
- AgeIPO (Ritter 1991): firm age = current year minus founding year from Jay Ritter's data set, restricted to recent IPOs. Original is an event study without a t-stat. CZ-own 2005-24 XM .76 (2.43), VW .97 (2.92), VW 2012-24 .90 (1.99); spanning alpha .75 (2.65), R2 .26. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- IndIPO (recent IPO indicator): 2005-24 XM .32 (1.73); spanning alpha about zero; correlation +.72 with value. RDIPO (recent IPO with zero R&D): XM .52 (2.26), VW .24 (.81). — [own computation](https://www.openassetpricing.com/data/)
- ExchSwitch (Dharan-Ikenberry 1995): indicator for a move from AMEX or Nasdaq to NYSE, or Nasdaq to AMEX, within the past year; short. Original .46%/month, t 3.61, about 3,000 events 1962-1990. CZ-own 2005-24 XM .71 (2.48), VW .61 (2.06); spanning alpha .61 (2.18), R2 .11. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- Data: `exchange`, `listing_date` and `delisting_date` are on the panel; the security master combines point-in-time FINRA names with a non-PIT Nasdaq directory. — local files `C:/atx/atx-db/docs/ALPHA_PANEL_REQUEST_V7_RESPONSE.md` (U3) and `ALPHA_PANEL_STATUS.md`

**F16. Organisation capital (Eisfeldt-Papanikolaou 2013 JF)**
- Definition: OrgCap_0 = 4 x SG&A in the first year; OrgCap_t = .85 x OrgCap_(t-1) + SG&A_t / GNP deflator; scale by total assets; non-financials, December fiscal year ends; winsorise at 1%; standardise within Fama-French 17 industries. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv)
- Original .39%/month, t 2.85, VW quintiles, 1970-2008. CZ-own 2005-24 OP(VW) .31 (1.95), XM .22 (1.57), VW 2012-24 .13 (.68); spanning alpha .21 (1.55), R2 .11. — [SignalDoc.csv](https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv); [own computation](https://www.openassetpricing.com/data/)
- Prior local finding: profitability explains all abnormal performance of "alternative value", including intangibles-adjusted value (Novy-Marx-Medhat 2025). — [NBER w33601](https://www.nber.org/papers/w33601), as summarised in local file `v6-literature.md` section 3.6

**F17. Factor momentum (a sleeve-timing signal, not a stock signal)**
- The average factor earns 1 bp/month after a year of losses and 53 bp/month after a positive year. A time-series factor momentum strategy across 20 factors earns 4.2% per year (t 7.04); the cross-sectional version earns 2.8% (t 5.74). Factor momentum explains all forms of individual stock momentum. — [Ehsani-Linnainmaa, NBER w25551](https://www.nber.org/system/files/working_papers/w25551/w25551.pdf); published [JF 2022](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13131)
- Momentum in industry-neutral factors explains industry momentum, but industry momentum explains none of factor momentum. — [Arnott-Kalesnik-Linnainmaa 2023 RFS](https://academic.oup.com/rfs/article-abstract/36/8/3034/6988043)

**Cost benchmark used for the turnover judgements**
- Anomalies with one-sided monthly turnover above 50% rarely survive costs. — [Novy-Marx-Velikov 2016 RFS](https://academic.oup.com/rfs/article-abstract/29/1/104/1844518), as summarised in local file `v6-literature.md` section 3.7
- Average net post-publication returns are about 4 bps/month per anomaly and about 20 bps/month for combinations. — [Chen-Velikov 2023 JFQA](https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/zeroing-in-on-the-expected-returns-of-anomalies/945133D5A3ECEEAF466AEE91551FD225), as summarised in local file `literature-v7.md` section S6

### Inferences

Candidate list for pre-registration. Grade, turnover class, expected correlation and gross Sharpe are my estimates [est]. "Turnover" is one-sided per day for a decay-smoothed rank signal: very low < 0.3%, low 0.3-0.7%, mid 0.7-2%, high > 2%.

| # | candidate | definition to pre-register | data | turnover [est] | expected overlap | grade | main risk |
|---|---|---|---|---|---|---|---|
| 1 | `tax_book` | current tax expense / (statutory rate x net income), TTM; = 1 if NI < 0 and tax > 0; long high | NEW XBRL item: current (or deferred) income tax expense; `ni_ttm` | very low | investment, value (.3-.4) | B | original paper's own subsample instability; statutory rate changed to .21 in 2018 |
| 2 | `comp_eq_iss_5y` | log(ME_t / ME_t-5y) - log(1 + 5-year total return); long low | bars, shares; 5 years of history (usable from 2017) | very low | low risk (-), profitability (+), `issuance_*`, `net_payout` | B | near-duplicate of existing issuance members under the redundancy screen |
| 3 | `coskew_60m` | Harvey-Siddique coskewness on 60 monthly returns (or 252 daily); long low | bars (usable from 2017 for 60m) | low | momentum (-), value (+), low risk (+) | B | original evidence thin (t 1.96); daily version fails VW |
| 4 | `pct_accruals` | (NI - CFO) / abs(NI), TTM; long low | `ni_ttm`, `cfo_ttm` | very low | `accruals` (.6), `cbop` | B- | VW t 1.07; largely an accruals variant |
| 5 | `gscore_lowbm` | Mohanram G among the bottom B/M tercile, 7 of 8 components (no advertising item) | fundamentals, `xrd_ttm`, `capx_ttm`, quarterly history for variability | low | profitability, short interest (.3) | B- | complex; applies to a sub-universe only |
| 6 | `earn_consistency` | 4-year average YoY EPS growth, same-sign filter | `ni_q`, `shrs_q` history (2010+) | very low | `nincr`, profitability, issuance | C+ | one obscure paper |
| 7 | `fip_id` | ID = sgn(PRET) x (%neg - %pos) over t-252..t-21, used as an interaction with `mom_12_1` / `res_mom_12_1` | bars | low-mid | price momentum (conditioning, not additive) | B | raises momentum's weight in the book; momentum itself was flat 2005-24 ex-microcap |
| 8 | `night_day` | (a) 21-63 day sum of overnight return minus intraday return; (b) monthly count of "positive overnight, negative intraday" days | `ret_overnight`, `ret_intraday` | mid-high | reversal; otherwise none | B- | returns accrue by session; close-to-close capture is a fraction of the headline |
| 9 | `nt_late` | short for 3-6 months after a first NT 10-K / NT 10-Q | 8-K/filings events | event, low | quality, short interest | B- | small and hard-to-borrow names; few events in the top 3000 |
| 10 | `nonreliance_402` | short for 1-3 months after Item 4.02 | events | event, low | quality, accruals | C+ | vendor evidence only; short window |
| 11 | `auditor_401` | short after Item 4.01 auditor change | events | event, low | quality | C+ | no evidence verified in this session |
| 12 | `ind_lead_lag` | prior 21-day return of the largest 30% of the FF49 industry, applied to the rest | bars, FF49 | high | reversal (-.5), `ind_mom_12_1` | B- | 1-month signal; VW 2012-24 return .06 |
| 13 | `connected_13f` | connected-portfolio return through common 13F holders, combined with own return | 13F holdings (124M rows) | mid | reversal | C+ | pre-2009 sample, active mutual funds only; 13F has no fund type |
| 14 | `breadth_chg` | quarterly change in number of 13F holders / lagged holders | `inst_d_holders` | very low | momentum (.74) | C+ | post-2005 t 1.4 |
| 15 | `si_low_io` | short interest / institutional ownership in the top SI tail | SI, `inst_shares` | low | short interest | B- | borrow cost unobserved; tiny capacity |
| 16 | `new_listing_age` | years since `listing_date` for lines listed < 5 years; short the youngest | security master | very low | value (.3), momentum (-.3) | C+ | founding year unavailable; listing date is a proxy |
| 17 | `exch_switch` | indicator: moved to NYSE, or Nasdaq to NYSE American, in the past year; short | `exchange` history | event, very low | none (R2 .11) | C+ | few events; exchange history partly non-PIT |
| 18 | `seas_offseason_6_10` | average other-month return, years 6-10 | bars (full window only from 2022) | mid | value (.3) | C+ | price history too short for TRAIN 2020-2022 |
| 19 | `div_month` | predicted dividend month from the distribution calendar | corporate actions | high (monthly) | none (R2 .06) | C+ | .07%/month ex-microcap; a timing tilt at best |
| 20 | `factor_mom_overlay` | tilt theme sleeve weights by the sign of each sleeve's trailing 12-month return | sleeve returns | n/a | price momentum | B+ evidence, C+ fit | conflicts with the v7 rule against fitted weights on short samples |

- Families to leave out of the pre-registration entirely: Amihud illiquidity, turnover and zero-trade levels, long-term reversal, DuPont changes, inventory and hiring growth, debt issuance, dividend initiation/omission, repurchase indicator, XFIN, short-lag multi-year seasonality, price delay, R&D capital variants.
- Arithmetic from the Lou-Polk-Skouras numbers: sorting on overnight return gives 3.47 - 3.02 = about +.45%/month close-to-close, and sorting on intraday return gives 2.41 - 1.77 = about +.64%/month. A close-to-close book therefore captures roughly one-sixth to one-quarter of the headline component effect, before costs. The family is more useful as a trade-timing rule (which session to trade in) than as a position signal, and that use needs intraday execution the platform does not have.
- Event sleeves (9-11, 17) are one-sided shorts. They add no capacity-scale alpha but are cheap exclusion screens for the long side.
- Hou-type industry lead-lag and the Cohen-Lou signal have a .67 correlation after 2005, so segment data would buy little beyond the buildable industry signal.

### Gaps
- Turnover numbers were not found in any source for these signals; every turnover entry is an estimate.
- Post-2004 large-cap evidence for frog-in-the-pan was not retrieved; the paper's sample ends 2007.
- The Akbas et al. effect size (return spread, t-stat, value-weighted result) was not retrieved; only the direction is confirmed.
- No peer-reviewed estimate of post-filing drift after Items 4.01 or 5.02 was found. The 4.02 numbers come from a vendor page.
- 13D activist-filing drift: a search summary reported a 7-8% announcement-window abnormal return (Brav-Jiang-Partnoy-Thomas 2008) and no reversal over five years (Bebchuk-Brav-Jiang), but I could not attribute the figures to a specific page, so they are not reported as findings. Relevant source for follow-up: [NBER w21227](https://www.nber.org/system/files/working_papers/w21227/w21227.pdf).
- Whether the filings table carries SC 13D / 13G, S-3 and 424B forms mapped to the subject company was not checked in the data; the doc states only that a filing appears once per CIK it is filed under.

## Key question 3: Which families are most likely to be genuinely orthogonal to a book that already holds value, quality, investment, earnings momentum, price momentum, low risk and short interest?

### Takeaway
Orthogonality comes from information type, not from the label. Signals built from the same income statement and balance sheet items (XFIN, repurchases, 5-year share growth, hiring, percent accruals) are 50-80% explained by the existing themes. Signals built from co-moments, tax accounts, listing events, filings events and the open price are 10-35% explained. The cross-firm momentum family is orthogonal to the slow themes but loads -.5 on short-term reversal and is a one-month signal.

### Cited Findings
- R2 of each candidate's 2005-2024 ex-microcap long-short return on the nine theme proxies, lowest first: DivSeason .06; ExchSwitch .11; OrgCap .11; MomSeason06YrPlus .11; MomOffSeason06YrPlus .13; RDIPO .13; MomSeason16YrPlus .15; IO_ShortInterest .18; CustomerMomentum .19; Coskewness .20; CoskewACX .21; Tax .25; AgeIPO .26; MS .27; IndRetBig .33; OPLeverage .36; CompEquIss .36; retConglomerate .37; VolumeTrend .40; EarningsConsistency .46; PctAcc .48; BetaTailRisk .51; FirmAgeMom .54; ShareIss5Y .63; DelBreadth .65; ShareRepurchase .71; XFIN .81. — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)
- Pairwise correlations among the surviving candidates, 2005-2024 ex-microcap, are near zero except IndRetBig-retConglomerate .67, CompEquIss-EarningsConsistency .46, DelBreadth-Coskewness -.32, DelBreadth-AgeIPO -.32, MomSeason16YrPlus-Coskewness .26, Tax-OPLeverage .24. — [own computation](https://www.openassetpricing.com/data/)
- Correlations among the nine theme proxies, 2005-2024 ex-microcap: earnings momentum-momentum .73; profitability-short interest .60; momentum-low risk -.49; value-investment .46; value-low risk .43; value-momentum -.31. Average pairwise correlation .113. — [own computation](https://www.openassetpricing.com/data/)
- JKP's algorithm places the candidates outside the library's main clusters as follows: debt issuance and financing items form their own Debt Issuance cluster (`debt_gr3`, `fnl_gr1a`, `ncol_gr1a`, `nfna_gr1a`, `noa_at`, `capex_abn`); age, cash, R&D intensity, tangibility, Altman Z and bid-ask spread form Low Leverage; Amihud and dollar volume sit in Size; turnover and zero-trade days sit in Low Risk; coskewness sits in Seasonality; idiosyncratic skewness sits in Short-Term Reversal. — [JKP Cluster Labels.csv](https://github.com/bkelly-lab/ReplicationCrisis/blob/master/GlobalFactors/Cluster%20Labels.csv)
- JKP find low across-theme correlation and within-theme correlations "well in excess of 50% on average". — [JKP, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- Anomalies split cleanly by session: momentum variants and short-term reversal earn their premia overnight; size, value, profitability, investment, beta, idiosyncratic volatility, issuance, accruals and turnover earn theirs intraday. — [Lou-Polk-Skouras, JFE 2019](https://personal.lse.ac.uk/polk/research/TugOfWar.pdf)
- Shared-analyst momentum is not explained by a factor combining all seven earlier linkage strategies, and the predictive power of longer lags is limited to smaller stocks. — [Ali-Hirshleifer, AQR copy](https://images.aqr.com/-/media/AQR/Documents/AQR-Insight-Award/2019/Shared-Analyst-Coverage_Ali-Hirshleifer.pdf)
- Geographic lead-lag is unrelated to size, liquidity and analyst coverage, unlike industry lead-lag, which is strongest in small, thinly traded stocks. — [Parsons-Sabbatucci-Titman 2020 RFS](https://academic.oup.com/rfs/article-abstract/33/10/4721/5682420)

### Inferences
- Ranking by likely orthogonality to the actual library, combining R2 and information source:
  1. Filing events (NT, 4.02, 4.01, listing changes): no price or statement input is shared with the book. Expected correlation with every theme below .1 [est]. Small capacity.
  2. Co-moment signals (coskewness): R2 .20, and the input (covariance with squared market returns) is not used by `bac`, `smax` or `qmj_safety`.
  3. Tax-to-book income: R2 .25; it uses tax accounts that only `chtax` touches, and `chtax` is a change, not a level.
  4. Overnight-intraday decomposition: uses the open price, which no library member uses. Expected overlap only with `ind_adj_rev_5`.
  5. Long-lag and off-season seasonality: R2 .11-.15, but not buildable on 2012+ prices for the TRAIN window.
  6. 13F structure (connected stocks, SI over IO): orthogonal to fundamentals; overlaps reversal and short interest.
  7. Cross-firm momentum: orthogonal to slow themes, -.5 to reversal, and too fast for a 1-6 month holding period.
- Families that look new but are not: XFIN, ShareIss5Y, repurchase indicators and hiring are 60-80% explained by value, profitability and investment. Breadth change and young-firm momentum are 55-65% explained by momentum. Percent accruals is half explained by accruals.
- The library has no member in JKP's Debt Issuance, Low Leverage or Size clusters. These are also the clusters with the weakest post-2004 evidence in the CZ-own tables, and JKP name leverage and size among the three themes with low replication rates. Filling them would add themes on paper and little return.
- The theme proxies show that the existing book is less diversified than nine labels suggest: earnings momentum and price momentum correlate .73, and profitability and short interest .60. A new family that is unrelated to both pairs is worth more than its standalone Sharpe suggests.

### Gaps
- All correlations are between CZ long-short portfolios, not between the library's own ranked, decay-smoothed, FF12-neutral signals. Overlap with `iv_rv_spread`, `ins_opp`, `inst_best_ideas`, `sv_flow` and `ftd_fail` could not be measured because CZ has no equivalent portfolios.
- No correlation estimate exists for the event sleeves or the overnight-intraday signals; the entries above are judgement.

## Key question 4: What does the literature say about the marginal Sharpe of the N-th weakly correlated signal, and about how many distinct themes exist?

### Takeaway
The literature agrees on "many signals, few themes": 13 clusters (JKP), a small number of principal components (Kozak-Nagel-Santosh), yet a dense SDF in which no short list of factors suffices (Bryzgalova-Huang-Julliard), and more useful characteristics once trading costs are counted, because trades net (6 rises to 15 in DeMiguel et al.). In the CZ data an unselected pool of 45 buildable candidates had an average pairwise correlation of .02 and lifted a proxy book's gross Sharpe from about .6 to about .9-1.1 over 2005-2024, which is an upper bound before costs, turnover limits and further decay.

### Cited Findings
- 13 themes; 10 of 13 enter the ex-post tangency portfolio with significantly positive weights; within-theme correlations exceed 50% on average. — [JKP, NBER w28432](https://www.nber.org/system/files/working_papers/w28432/w28432.pdf)
- A characteristics-sparse SDF (four or five factors) cannot adequately summarise the cross-section, but a relatively small number of principal components of the characteristics-based factors approximates the SDF well. The test estimates on data to the end of 2004 and evaluates out of sample over 2005-2016. — [Kozak-Nagel-Santosh, NBER w24070](https://www.nber.org/system/files/working_papers/w24070/w24070.pdf); published [JFE 2020](https://www.sciencedirect.com/science/article/abs/pii/S0304405X19301655)
- Across 2.25 quadrillion models from 51 candidate factors, the SDF is "dense in the space of observable factors", i.e. a large subset of variables is needed; yet the implied maximum Sharpe ratio is "not unrealistically high", indicating substantial commonality among the risks the factors span. A model-averaged SDF beats existing models in and out of sample. — [Bryzgalova-Huang-Julliard, JF 2023](https://lbsresearch.london.edu/id/eprint/2743/1/BRYZGALOVA%20-%20Bayesian%20Solutions%20for%20the%20Factor%20Zoo.pdf)
- Transaction costs raise the number of jointly significant characteristics from 6 to 15, because the trades needed to rebalance different characteristics often cancel. — [DeMiguel-Martin-Utrera-Nogales-Uppal, RFS 2020](https://lbsresearch.london.edu/id/eprint/1124/1/DeMiguel_TransactionCostPerspective.pdf)
- After 2003, multivariate regressions in non-microcaps leave only two characteristics independent (nincr and chempia). — [Green-Hand-Zhang 2017 RFS](https://academic.oup.com/rfs/article-abstract/30/12/4389/3091648), as summarised in local file `literature-v7.md` section S6
- US post-publication decline: -58% (McLean-Pontiff) and -62% equal-weighted / -66% value-weighted (Jacobs-Mueller). — [McLean-Pontiff 2016 JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12365); [Jacobs-Mueller 2020 JFE](https://www.sciencedirect.com/science/article/abs/pii/S0304405X19301618), both as summarised in local file `literature-v7.md` section S6
- Peer review adds little: data-mined accounting ratios matched on in-sample statistics do as well post-sample as published predictors; risk-based predictors decay 65% post-publication against 50% for the rest. — [Chen-Lopez-Lira-Zimmermann, arXiv 2212.10317](https://arxiv.org/abs/2212.10317)
- CZ-own combination evidence, gross, monthly:

| implementation, window | proxy book (9 themes, equal risk) SR | pool of 45 candidates, equal risk, SR | correlation book-pool | blended SR at pool weight 0 / .2 / .33 / .5 | share of pool with t > 2 |
|---|---|---|---|---|---|
| ex-microcap, 2005-24 | .58 | 1.49 | +.66 | .58 / .81 / .95 / 1.14 | 38% |
| ex-microcap, 2012-24 | .60 | 1.32 | +.72 | .60 / .78 / .90 / 1.04 | 24% |
| value-weighted, 2005-24 | .46 | 1.29 | +.49 | .46 / .68 / .83 / 1.01 | 27% |
| value-weighted, 2012-24 | .42 | 1.42 | +.46 | .42 / .68 / .86 / 1.08 | 13% |

  The pool is every candidate in this note that the platform could build in principle, chosen before looking at post-2004 returns: IndRetBig, DelBreadth, IO_ShortInterest, MomSeason06/11/16YrPlus, MomOffSeason06YrPlus, DivSeason, Coskewness, CoskewACX, ReturnSkew, BetaTailRisk, OrgCap, PctAcc, PctTotAcc, Tax, EarningsConsistency, OPLeverage, CompEquIss, ShareIss5Y, XFIN, NetDebtFinance, AgeIPO, ExchSwitch, MS, IntMom, MomVol, FirmAgeMom, VolumeTrend, Illiquidity, ShareVol, std_turn, zerotrade6M, ChAssetTurnover, GrSaleToGrInv, ChInv, DivInit, DivOmit, ShareRepurchase, LRreversal, RevenueSurprise, TrendFactor, Herf, Cash, tang. Average pairwise correlation within the pool: .02 (ex-microcap), .01 (value-weighted). 82-84% of pool members had a positive mean return. — [own computation from Chen-Zimmermann portfolio files](https://www.openassetpricing.com/data/)
- Theme-proxy Sharpe ratios, 2005-24 ex-microcap, gross: value .38, profitability .52, investment .53, earnings momentum .27, momentum .07, low risk .32, short interest .33, reversal/seasonality -.12, accruals .09. — [own computation](https://www.openassetpricing.com/data/)
- Net of costs, combinations earn about 20 bps/month post-publication and single anomalies about 4 bps/month. — [Chen-Velikov 2023 JFQA](https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/zeroing-in-on-the-expected-returns-of-anomalies/945133D5A3ECEEAF466AEE91551FD225), as summarised in local file `literature-v7.md` section S6

### Inferences
- For N signals with equal Sharpe s and equal pairwise correlation rho, the equal-risk combination has Sharpe s x sqrt(N / (1 + (N-1) rho)), with limit s / sqrt(rho). At the measured theme correlation of .113 and s = .4 the ceiling is 1.19; nine themes already reach .87 (73% of the ceiling). Adding themes 10-12 at the same correlation adds about .05. The N-th signal only matters if its correlation with the book is well below the book's internal average.
- The pool result fits that arithmetic: 45 signals with average individual Sharpe near .25 and rho .02 give .25 x sqrt(45 / 1.88) = 1.2. The benefit comes from the low correlation, not from any single strong signal; only 13-38% of members are individually significant.
- The pool's .46-.72 correlation with the proxy book shows that a large part of the pool is the book in other clothes. The part that is new is what Key question 3 ranks first.
- Honest expectation for the platform: apply the v7 haircut (about 50% post-publication) and costs to a gross blended gain of +.2 to +.4, restrict to slow signals in the top 3000, and the plausible net gain from 10-20 new members is +.05 to +.15 Sharpe [est]. This is in line with the v6 estimate of +8% gross from three new independent themes.
- A pre-registration of 10-20 candidates at individual post-2004 t-stats of 1.5-3 implies that perhaps a third will pass an admission test on 2-3 years of TRAIN data by skill and several by luck. The effective number of trials for the deflated Sharpe calculation should count all pre-registered candidates, including those rejected.
- The two readings of the literature are compatible. "Few principal components" describes variance; "dense SDF" and "6 to 15 characteristics with costs" describe expected return and netting. For a cost-constrained book, breadth across weak, uncorrelated, slow signals is the supported design.

### Gaps
- No source gives the marginal Sharpe of the N-th signal for a US large-cap, cost-aware, daily-rebalanced book; the numbers above are arithmetic and a gross proxy exercise.
- The pool is free of post-2004 selection but not of publication selection: every member was published because it worked in its original sample.
- The proxy book is equal-risk across nine CZ composites and excludes options-implied and ownership-flow themes; the real library's correlation structure may differ.
- The Swade-Hanauer-Lohre-Blitz result on how few factors span the JKP set could not be retrieved (SSRN returned HTTP 403 and the search budget was exhausted).

# atx-db: which characteristics are most promising for tradeable alpha (an evidence-weighted PRIOR)

> **PRIOR, NOT MEASURED IN atx-db.** No characteristic has been evaluated on real data in this project. The research
> lane is proven on fixtures only (`docs/research/QUALIFIED_SIGNALS.md`: "No qualification ledger yet"). The first real
> numbers come from node 1.13 (price wave, selection sample, provisional labels). This file ranks characteristics by
> combining three things: (a) published out-of-sample and post-publication evidence, (b) turnover and cost, and
> (c) whether atx-db can build them and how strong their PIT (point-in-time) basis is. Every ranking and number below
> is literature or arithmetic, never an atx-db result.

Date 2026-09-26. Synthesized for the tier-1 v2 status file (`2026-09-26-tier1-v2-status-1.md`) from:
- the plan web research and plan review research (scratchpad of session b4bd2aa4: `plan-web-research.md`, `plan-review-research.md`);
- `atx-db/src/atx_db/seeds/research_anomaly_catalog.csv` (186 rows: 175 `w0_existing`, 6 `w4_events`, 5 `w5_ownership`; 141 replication, 45 discovery);
- `docs/research/ANOMALY_CATALOG.md`;
- the v2 DAG index (§1, §2, §8, rulings R-6, R-7, R-12, R-13) and sprint files S1 (Tasks 10–13), S2 (9–11), S3 (8–10), S4 (1–5) and SX.

**Readiness codes**
- **NOW**: buildable from the retained `TickerHistory3.parquet` (2012-03-26 → 2026-09-18). Wave `w1_price`, first graded at 1.13 on provisional labels, holdout opened at 3.8.
- **B1**: needs the fundamentals B1 run (2.7). Catalog rows arrive through 3.11 (`w0`) or 2.10/2.11 (`w2_fund_a`) and are graded at 4.3.
- **B1+ME**: also needs company ME from the 3.8 spine. Built in 3.10 (`w3_compositions`), graded at 4.3.
- **EV**: needs the B0 submissions (1.8) plus P3/E1 (`w4_events`, 3.9), graded at 4.3.
- **U2**: 13F / FINRA. The index §8 lists U2 as granted on 2026-09-26, but the session-1 `/goal` said no gate is granted unless the user says so, and the controller followed the `/goal` (ruling C-1). Either way no 13F or FINRA file is on disk yet (X.1/X.2). Graded at 4.5.
- **U9 and U4** are not granted: 8-K EX-99 SUE at the announcement date, and prices before 2012.
- A dagger (†) marks a row planned in the v2 sprint files but not yet in the 186-row CSV.

## 1. Ranked shortlist (prior tiers: A = strongest prior, B = worth grading, C = marginal)

Tiers and ranks weight the chance of being **tradeable** in a post-2012 US sample, after costs and after decay. How soon
atx-db can grade a row is a tiebreaker, not the main criterion.

| # | Family (catalog ids; † planned) | JKP theme | Why promising: prior evidence | Turnover and cost | atx-db readiness and PIT strength | First graded |
|---|---|---|---|---|---|---|
| 1 A | **Equity issuance and net payout**: `share_issuance_1y`, `share_issuance_3y`, `shares_growth_yoy`, `net_equity_issuance`, `net_payout_yield`, `buyback_yield`; † `chcsho_12m`, `compeqiss` | value (JKP places chcsho and netis in Value) | Pontiff–Woodgate 2008 and Daniel–Titman 2006 (OSAP ShareIss1Y t=7.08, CompEquIss t=4.39). Fama–French 2008 ("Dissecting Anomalies") find net issues is one of the few anomalies present in **big** stocks too. Both legs have an economic reason: dilution and stock compensation on the short side, buybacks on the long side (Boudoukh et al. 2007). JKP replicates it. | Low: ranks change slowly, far below the Novy-Marx–Velikov 2016 threshold of 50%/month one-sided | B1: XBRL weighted shares plus cash-flow items. `split_basis` needs the R1d split epochs. Issuance and buybacks are read only when tagged every quarter (`coverage_bias`). `chcsho_12m` could be proxied NOW from the spine's vendor `shares_lagged` (not planned). `compeqiss` needs 60 months, so starts 2017+ | w0 → 4.3; † w3 (3.10) → 4.3 |
| 2 A | **Cash-based operating profitability and gross profitability**: `cash_profitability`, `operating_profitability`, `gross_profitability`, `cfo_to_assets`, `fcf_to_assets`; † `cop_at`, `op_at`, `ope_be` | quality / profitability | Novy-Marx 2013: gross profitability works in large caps and hedges value. Ball et al. 2015/2016: cash-based operating profitability **subsumes accruals** and has a higher Sharpe than GP or OP. Also FF 2015 RMW and Asness–Frazzini–Pedersen 2019 QMJ. The JKP Quality theme keeps tangency weight. | Very low: TTM, persistent ranks | B1. The FC1 filing clock (R-13) costs little on a slow signal. The catalog's `cash_profitability` has a `construct_deviation` (no R&D add-back), so prefer † `cop_at` as primary | w0 → 4.3; † w2 (2.10) → 4.3 |
| 3 A | **Momentum, crash-resistant variants**: `pct_from_high_252d`, `momentum_12_1`; † `prc_highprc_252d`, `ret_12_1`, `frog_in_pan`, `ret_6_1`. **Gap:** residual momentum is in no v2 wave | momentum | The JKP Momentum theme replicates and carries tangency weight. George–Hwang 2004: the 52-week high is less crash-prone. Da–Gurun–Warachka 2014: continuous-information momentum is stronger. Blitz–Huij–Martens 2011: residual momentum has about half the volatility and crash exposure. Caveat: plain US UMD was crash-prone and weak after 2009 (Daniel–Moskowitz 2016). | Medium: about 30–50%/month one-sided for deciles. It survives with a buy/hold spread (NMV 2016) and scales at institutional costs (Frazzini–Israel–Moskowitz) | **NOW**, PIT-clean (bar clock). Provisional labels set terminal returns to NULL, which drops the worst losers, biases the short leg up and understates the L/S until 3.8 | **w1 → 1.13** (selection); holdout 3.8 |
| 4 A− | **Quarterly profitability and profit growth**: `roe_q`, `roa_q`, `roe_q_change_yoy`, `roa_q_change_yoy`, `tax_expense_change_yoy`, `operating_profitability_change_yoy`; † `tax_gr1a`, `nincr`, `ocf_at_chg1` | profitability / profit_growth | HXZ 2015 q-factor ROE. Balakrishnan–Bartov–Faurel 2010. Thomas–Zhang 2011 ChTax (OSAP t=11.26). Novy-Marx 2015: fundamental momentum subsumes price momentum. The JKP Profit Growth theme is significant. | Medium: quarterly refresh, about 20–40%/month | B1. The 10-Q + 46 h clock arrives weeks after the earnings release (`filing_clock_lag`), so the change signals lose the early drift. Single-quarter levels carry `fiscal_seasonality` | w0 → 4.3 |
| 5 B+ | **Cash-flow and enterprise-value value**: `ebitda_to_ev`, `ebit_to_ev`, `cfo_to_price`, `fcf_yield`, `gross_profit_to_ev`, `sales_to_price` | value | Loughran–Wellman 2011: the enterprise multiple works in large caps (OSAP EntMult t=6.54). Multi-metric value lost less than B/M in 2017–20, and value spreads are wide (Israel–Laursen–Richardson 2021; Arnott et al. 2021). Its main job is to hedge momentum and quality (Asness–Moskowitz–Pedersen 2013). | Low: about 10–20%/month; ranks persist even though the ratios move with price | B1 plus current ME from verified DEI caps. The EV rows carry `sign_flip` and `unguarded_zero` (net-cash firms), so the domain rules are load-bearing | w0 → 4.3 |
| 6 B+ | **Earnings-announcement return**: `ear_m1p1` | profit_growth | Chan–Jegadeesh–Lakonishok 1996 and Brandt et al. 2008 (OSAP AnnouncementReturn t=9.25). It is the **only earnings-news signal dated at the announcement** (8-K 2.02 acceptance), so it avoids the SUE filing lag. Engelberg–McLean–Pontiff 2018: anomaly returns concentrate on news days. Caveat: Martineau 2022 finds drift largely gone in large caps since the mid-2000s. | Medium: quarterly event, 1–3 month hold | EV. Keyed to 10-K/10-Q periods, so releases without a periodic filing are missing. PIT-strong | w4 → 4.3 |
| 7 B+ | **Short interest and days to cover**: `days_to_cover_si`, `short_interest_ratio`; † IO-constrained SI | none (outside JKP) | Boehmer–Huszar–Jordan 2010: **lightly** shorted stocks earn positive alpha, so there is a long leg. Hong et al. 2015 (DTC). Boehmer–Jones–Zhang 2008: short sellers are informed. | Medium: twice-monthly data. The heavy-short leg is often hard to borrow, and fees absorb much of it (Drechsler–Drechsler 2014). atx-db has **no borrow data** | U2 (see readiness codes); nothing fetched (X.2). How far back free FINRA history goes is unknown: record the first settlement date at X.2. The publication clock is modeled | w5 → 4.5 (last S4 node) |
| 8 B | **Intangible-adjusted and enterprise-book value**: † `intang_bm`, `bev_mev`, `netdebt_me`, `tangible_bm` | value / low_leverage | Eisfeldt–Kim–Papanikolaou 2022: intangible-adjusted HML beats HML after 2000. Peters–Taylor 2017. Penman–Richardson–Tuna 2007 (NetDebtPrice). | Low | B1+ME. R&D and SG&A capital start in 2014 | † w3 (3.10) → 4.3 |
| 9 B | **Distress / failure probability**: † CHS score; `ohlson_o`, `altman_z` | low_leverage / profitability | Campbell–Hilscher–Szilagyi 2008 (the distress puzzle); OSAP OScore t=3.36. Robust across samples, but mostly a short-leg effect concentrated in illiquid names. | Low–medium | CHS needs ME and price σ, so it comes in 3.10. O- and Z-scores are B1 rows in w0 | 3.10 / w0 → 4.3 |
| 10 B | **NOA and balance-sheet investment**: `noa_to_assets`, `delta_noa`, `investment_to_assets`; † `nncoa_gr1a`, `ppeinv_gr1a`, `coa_gr1a`, `taccruals_at` | debt_issuance / investment | Hirshleifer et al. 2004 NOA (OSAP t=8.45); Richardson et al. 2005; Lyandres–Sun–Zhang 2008. These are legs of the Stambaugh–Yuan 2017 MGMT cluster. Decay risk: accrual-type effects weakened after about 2000 (Green–Hand–Soliman 2011). | Low | B1. `presence_rule` and `coverage_bias` apply to debt aliases; COA/NCOA exist only for classified balance sheets | w0 → 4.3; † 2.10 |
| 11 B− | **Low risk and lottery as a screen**: `max_daily_return_21d`, `realized_vol_252d`, `ivol_252d`, `beta_mkt_252d`; † `rmax5_21d`, `ivol_ew_252d`, `beta_bab_1260d` | low_risk | The JKP Low Risk theme carries tangency weight. Bali–Cakici–Whitelaw 2011 (MAX); Ang–Hodrick–Xing–Zhang 2006. But the alpha sits in overpriced, hard-to-short names (Stambaugh–Yu–Yuan 2015), and BAB's standard-construction returns are weak (Novy-Marx–Velikov 2022). Use it as an exclusion screen and a composite diversifier, not a standalone L/S. | 21-day versions high (> 50%/month); 252- and 1,260-day versions low | NOW (w1 uses the bar-only EW market); the 1,260-day window starts 2017. Ruling C-24: the value-weight beta / FF-residual ivol are the tested hypotheses, EW twins reported only | **w1 → 1.13**; P4 rows w4 → 4.3 |
| 12 C+ | **Return seasonality**: † `seas_1_1an`, `seas_2_5an` | seasonality | Heston–Sadka 2008; Keloharju–Linnainmaa–Nyberg 2016. The JKP Seasonality theme carries tangency weight, and its low correlation with other themes makes it useful in composites. | High: monthly, 1-month hold, weak net of costs on its own | NOW. `seas_2_5an` starts 2017+ (about 7 selection years), so it is close to untestable | w1 → 1.13 |
| 13 C+ | **Quality composites**: `piotroski_f`; † `mohanram_g`, QMJ, Stambaugh–Yuan MGMT and PERF | profitability / quality | Piotroski 2000 (OSAP PS t=5.59, strongest in small value); Mohanram 2005 (growth stocks); AFP 2019; Stambaugh–Yuan 2017. | Low | B1. QMJ and SY need ME and beta, so they come in 3.10 | w0 / † 2.11 / 3.10 → 4.3 |
| 14 C | **Institutional breadth and residual IO**: `breadth_change_13f`, `io_ratio_13f`, `io_change_13f`; † RIO | none | Chen–Hong–Stein 2002 (OSAP DelBreadth t=3.96). Nagel 2005: anomalies are strongest where residual IO is low. Post-2012 evidence is thin. | Low–medium: quarterly | U2; X.1 not fetched. Values are visible only 45 days after quarter end, and CUSIP identity is reconstructed (`cusip_survivor_conditioned`) | w5 → 4.5 |
| 15 C | **EDGAR filing red flags**: † `non_reliance_24m`, `late_filer_12m`, `auditor_change_12m`, `shelf_seo_12m`, `filing_delay_chg` | none | Hennes–Leone–Miller 2008; Alford–Jones–Zmijewski 1994; Loughran–Ritter 1995 (SEO). Short-leg effects. | Very low | EV. Acceptance-stamped, so this is the **strongest PIT data atx-db has**. The events are rare, so power is low | † w4 (3.9) → 4.3 |

**Out of plan, but the prior is high (a scoping question, not a ranking):**
- **Residual momentum** is buildable NOW from bars plus the P4 factors, from about 2016. It is in no v2 wave, even though RS-04 listed it.
- **Insider buying** (Form 4; Cohen–Malloy–Pomorski 2012 "opportunistic" insiders). The warehouse module `insider_metrics.py` exists but is not in the DAG.
- **q5 expected growth** (Hou–Mo–Xue–Zhang 2021; `expected_growth.py`).
- **Options signals**: IV spread and skew (Cremers–Weinbaum 2010; Xing–Zhang–Zhao 2010) via the atx-vol SpiderRock surfaces. The literature's strongest short-horizon predictors, but untested here.
- **SUE at the announcement date** (U9).

**Honest ordering note.** Several top rows were published **after** the atx-db sample starts: Novy-Marx 2013, HXZ 2015, Ball et al. 2016, EKP 2022. Their 2013–2023 selection window partly overlaps the original papers' in-sample periods. That makes an atx-db pass *less* independent and *more* flattering than a true post-publication test.

## 2. Likely to disappoint (widely known; weak post-2012 or post-cost evidence)

All of these are priors, and each will still be graded under R-7. "Disappoint" means the characteristic is unlikely to be tradeable alpha, not that the code is useless: several remain FM controls or cost inputs.

- **Book-to-market alone** (`book_to_market`, † `be_me_jkp`): the 2017–2020 value drawdown was the deepest on record, and intangibles distort book equity (EKP 2022). Keep it as the value control and the external-validation anchor (HML ρ ≥ 0.85 at 4.2), not as an alpha.
- **Size** (`market_cap`, † `me_line_log`, `dolvol_126d`): no reliable premium since about 1980 outside microcaps; it needs a junk control (Asness et al. 2018). The v4 investable co-primary cell (≥ $5, above the NYSE p20) removes what is left.
- **Short-term reversal** (`total_return_1m`, † `ret_1_0`): large gross return, but turnover near 100%/month (NMV 2016) and it does not scale at institutional cost (Frazzini–Israel–Moskowitz). The industry-adjusted version (Da–Liu–Schaumburg 2014) is better but still cost-bound.
- **Illiquidity level** (`amihud_illiquidity_21d`, † `ami_126d`, `zero_trade_*`, `prc_log`): Ben-Rephael–Kadan–Wohl 2015 — the liquidity premium has shrunk to insignificance outside the smallest stocks. Use these as cost and capacity inputs (`bidask_ar_21d`).
- **Sloan-style accruals** (`total_accruals`, `working_capital_accruals`, `percent_accruals`, `rsst_accruals`): the "apparent demise" after about 2002 (Green–Hand–Soliman 2011), and cash-based OP subsumes them (Ball et al. 2016).
- **SUE and earnings growth on the filing clock** (`sue_ni`, `sue_revenue`, `earnings_surprise_to_market`, the `*_q_growth_yoy` rows): drift has largely disappeared in large caps (Martineau 2022), and the 10-Q + 46 h clock misses the announcement window (`filing_clock_lag`). Revisit only if U9 is granted.
- **Asset growth and capex growth alone** (`asset_growth`, `capex_growth_*`, `total_assets_cagr_3y`): FF 2008 — absent in big stocks; JKP — the Investment theme is displaced in the tangency portfolio. Keep them inside composites only.
- **The growth-variant zoo** (52 growth rows, 44 tagged `profit_growth`, most of the 45 discovery rows: qoq, CAGR, accel): LSV-style growth effects are weak after publication, and each row adds to the BH family and to the DSR `n_trials` (R-6 trial registry) for little independent information. Consider collapsing them to their `hypothesis_family` before w0 is frozen.
- **IVOL, MAX and BAB as standalone L/S net of costs**: see row 11.
- **Long-term reversal** († `ret_36_13`, `ret_60_13`): weak after publication and overlaps value; the 5-year window only starts in 2017.
- **Dividend yield and payout ratio** (`dividend_yield`, `payout_ratio`): non-monotone, and weak since the 1990s.
- **Days since announcement** (`days_since_announcement`): a construct deviation from Frazzini–Lamont's predicted-month premium, with a 1-month horizon and high turnover.

## 3. Composite and theme view (what to combine, and why)

**Priority themes for combination** (prior): 1. Quality + Profitability (cash-OP, GP, ROE). 2. Value including issuance
(cash-flow and EV yields, net payout, issuance, intangible-adjusted B/M). 3. Momentum + Profit Growth (52-week high,
12-1, residual momentum, dROE, ChTax). 4. Low Risk as a screen or diversifier. 5. Seasonality as a low-correlation
diversifier. Second rank: Debt Issuance / NOA, which together with issuance forms the Stambaugh–Yuan MGMT cluster.

**Why**
- Value and momentum are negatively correlated (Asness–Moskowitz–Pedersen 2013), and profitability is negatively correlated with value (Novy-Marx 2013), so the combined Sharpe ratio is well above any leg's. Rough rule: SR_combo ≈ SR̄·√(N/(1+(N−1)ρ̄)); N=5 at ρ̄=0.2 gives about 1.7× the average leg.
- JKP: 10 of 13 themes carry significant tangency weight, so breadth across themes beats depth within one.
- Chen–Velikov: net of costs and decay, combinations earn about 20 bp/month against about 4 bp for the average single anomaly.
- DeMiguel et al. 2020 (RFS): combined characteristics net their trades, so more characteristics matter after costs.
- Stambaugh–Yuan 2017: the MGMT and PERF clusters are the classic pre-registered two-factor combination.

**Concrete suggestions for 4.4** (recommendations only; each must respect R-6 — freeze before returns are read):
- (a) Weight theme composites by `hypothesis_family`, not by row; today's equal weight over every catalog row lets the 44-row `profit_growth` and 29-row `investment` themes be dominated by one construct's qoq/yoy/CAGR variants.
- (b) Pre-register one "core" composite before any w0/w2 return is read, with equal weights on signed ranks of issuance (#1), cash-OP (#2), EV and CF value (#5), 52-week high + 12-1 (#3), ROE_q + ChTax (#4). It is one discovery trial whose members were not chosen by atx-db performance, and the only object with a realistic chance of clearing the HLZ path (§4).
- (c) Judge tradeability at the composite level, with buy/hold bands (NMV 2016) and the 4.1 break-even cost; treat single-row cells as diagnostics.

## 4. Power caveat: what ~14.5 years (R-7) can and cannot confirm

Rule of thumb: t ≈ SR_annual·√years. These are arithmetic priors, not atx-db results.

| Sample | Years | SR for one-sided p<0.05 (t≈1.65) | SR for t≈1.96 | SR for HLZ t≈3 |
|---|---:|---:|---:|---:|
| Full price window (R-7's basis) | 14.5 | 0.43 | 0.51 | **0.79** |
| **Selection sample actually graded** (2013-01 → 2023-12, about 130 formations) | ≈10.8 | **0.50** | 0.60 | **0.91** |
| Holdout (2024-01 → about 2026-06, about 30 formations) | ≈2.5 | 1.04 | 1.24 | 1.90 |
| Selection with U4 prices (≈2006 → 2023, price signals only) | ≈18 | 0.39 | 0.46 | 0.71 |

- R-7's 0.79 uses the whole 14.5 years; the selection sample, which is what gates, needs about 0.91 for t > 3. On the rank-IC path the review's approximation gives ICIR₃ₘ ≈ 0.60 for HLZ and ≈ 0.35 for BH + |z| ≥ 2; v4's one-sided replication gate needs about 0.28 (extrapolation).
- The typical published anomaly will not individually pass: in-sample SR ≈ 0.5–0.7 with ~58% post-publication decay (McLean–Pontiff) gives post-2012 SR ≈ 0.2–0.3 and expected t over 10.8 years ≈ 0.7–1.0. Chance of passing a one-sided 5% test on the VW L/S: ~20% at SR 0.25, ~50% at 0.5, ~84% at 0.8 — lower after BH q=0.10 and the investable co-primary cell. Most true signals will end `candidate` or `not_significant`: a power outcome, not evidence of absence.
- The holdout cannot discriminate: with ~2.5 years, P(correct sign | true SR 0.4) = Φ(0.4·√2.5) ≈ 0.74 while a null passes 50% of the time. v4's "sign-consistent, no significant shrink" rule confirms consistency, not significance. 12-month claims need Jegadeesh–Titman calendar-time portfolios (T≈115, B=9).

**What atx-db can confirm:** data trust at 4.2 (atx Mkt/SMB/HML/RMW/CMA/UMD vs Ken French; ~40 JKP-overlap characteristics at ρ ≥ 0.6 — the most valuable real deliverable, because it licenses borrowing JKP/OSAP long-sample evidence as the prior); sign and rough magnitude for strong effects (SR ≳ 0.6) and composites; coverage, turnover and break-even cost; coarse relative ranking inside a theme.

**What it cannot confirm:** tradeability after costs (costs reported only; Abdi–Ranaldo spreads from daily OHLC are noisy; no borrow fees — matters for rows 7, 9, 11, 14; no impact model beyond 1%-of-ADV capacity); decay rates (the window is post-publication for > 90% of the canon); regime vs decay (2013–2023 is one regime); small variant differences (ΔSR tests have almost no power); survivorship-clean short legs until 3.8 (provisional labels leave terminal returns NULL; fundamentals rank linked owners only; release capped at `candidate` without U5).

**What would raise power, roughly in order of leverage:** (1) empirical-Bayes shrinkage toward the JKP/OSAP priors (JKP 2023), valid only if 4.2 passes; (2) composites — family-weighted theme composites plus the one pre-registered core composite (≈ √N_effective); (3) U4 pre-2012 prices back to 2005 (price signals: SR needed for t > 3 drops from 0.91 to ~0.71); (4) Jegadeesh–Titman calendar-time portfolios for 6- and 12-month claims; (5) fewer trials — collapse discovery growth variants to families before the w0 freeze; (6) annual re-grading (holdout grows ~12 formations a year); (7) U9 SUE at the announcement date (signal quality, not sample length).

---
*Everything above is a prior assembled from the literature and the plan documents. Revise the rank order after 1.13
(rows 3, 11, 12), after 4.2 external validation (all rows), and after 4.3 and 4.5. No row may be described as
"qualified" or "alpha" until the v4 ledger says so.*

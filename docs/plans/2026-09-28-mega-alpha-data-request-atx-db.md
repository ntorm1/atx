# Data request to the atx-db team: mega-alpha v6.1 -> v7

**From:** mega-alpha (atx-engine / atx-impl) · **Date:** 2026-09-28 · **Consumer status:** library v6.1, 39 candidates,
S2 net Sharpe +1.239 on TRAIN 2020-2022 (`docs/plans/2026-09-28-mega-alpha-v6-pitch.html`).

**Goal.** Raise the book's net Sharpe by (a) widening the linked operating-company universe, (b) fixing the fields whose
coverage caps existing alphas, (c) adding the raw data that the literature-ranked new alpha families need, and (d) keeping
out-of-sample years untouched. Ranked by expected Sharpe impact per unit of your effort. Section 8 is the acceptance
contract; sections 1-7 are the content. Appendix A lists what we consume today with measured coverage, so you can see what
is already fine.

---

## 0. Ground rules every deliverable must satisfy (unchanged from today)

1. **Point in time.** Every value carries `available_at` (dissemination timestamp, UTC). Visibility rule: a value is usable at
   decision session d if `available_at < 22:00 UTC of session d-1`. Restated values keep their original vintage; never
   overwrite history in place. Where a vintage cannot be proven (FINRA short interest republished after 2021-06), flag rows
   with a `vintage_risk` marker as the current builder does.
2. **Identity.** Keyed on our instrument namespace (`securityID` from TickerHistory3 with the `sid0-bracket-v1` repair) plus
   `CIK` for issuer items. One row per (instrument or issuer, date). `link_tier` column on every issuer join (`strict`,
   `backfill`, `name`) so TRAIN can exclude survivorship-biased tiers.
3. **Coverage reporting.** Every field ships with a per-year table of finite share over **operating-company members**
   (`member_equity`) in the score window, the basis used in Appendix A. Shortfalls are reported, not silently NaN.
4. **Staleness.** Each field declares its staleness rule (days after which a value becomes NaN). Today: fundamentals 400 d
   (`sue` 200 d), SIC 550 d, short interest 45 d.
5. **Windows.** Deliver 2018-01-01 to the latest available. We read only 2020-2022 for TRAIN; 2019 is a reserved pre-sample
   backcast; 2023-2024 is validation; 2025+ is holdout. Never summarise or screen anything against returns for us.
6. **Format.** Parquet, one directory per stage, manifest with SHA-256 of every file and of the producing code, as
   `atx_db.alpha_panel` does. We bind manifests by SHA in every run.

---

## 1. Universe and identity (largest measured lever: +0.22 net Sharpe came from this)

The single biggest v6 gain came from restricting the book to **linked operating companies** (`linked-operating-v1`): the same
signals on a cleaner universe went from +0.975 to +1.192. We still lose 40% of member cells to `unlinked` (899k of 2.2M
member cells in the score window) with strict links; with your CIK link tiers the loss should fall to about 7%.

| id | what exactly | why | target |
|---|---|---|---|
| U1 Full CIK link table | (securityID, CIK, valid_from, valid_to, link_tier, is_issuer_primary, share_class) for every line that was ever a top-3000 ADV member 2018+ | breadth inside the operating universe; every issuer field inherits this coverage | >= 93% of operating-company member cells linked per year (your measured 92.6-94.5% for 2020-2022 with all tiers) |
| U2 Dead-line links | the `name` tier extended to lines that died before the 2026 snapshot (your note: 126k-322k line-days/yr) | survivorship: TRAIN must not favour survivors; `link_tier` lets us run a strict-only sensitivity | measured share of dead lines linked per year |
| U3 Security classification | `is_common`, `is_adr`, `is_etf`, `is_index_line`, `is_reit`, `is_royalty_trust`, `is_spac`, `is_preferred`, share-class group id, primary-line flag, exchange, listing date, delisting date | today inferred from SIC and earnings-reaction days; explicit flags remove ambiguity and let us include ADRs deliberately | 100% of member lines |
| U4 Delisting returns | last trade to delisting proceeds (Shumway-style) | short-side returns of delisted names are the vendor's last close today: biased | all delistings 2018+ |
| U5 Sector / industry | SIC (have), NAICS, GICS if licensable, FF12/FF49 (have), with historical changes and dates | industry neutralisation and within-industry members (`ind_adj_rev_5`, `within_ind_mom`, `sv_flow`); GICS is the standard for neutralisation | 100% of linked issuers |

## 2. Fields whose coverage caps existing alphas (cheap, direct Sharpe)

Measured on operating-company member cells, score window 2020-2022 (Appendix A). Each alpha using one of these runs on a
fraction of the universe, and the fraction is not random (large, old, US-GAAP filers are over-represented).

| field | coverage now | used by | ask | target |
|---|---|---|---|---|
| `gp_ttm` (gross profit) | **0.61** | `gpa`, `value_composite` | derive `sale_ttm - cogs_ttm` when GrossProfit is absent; map IFRS `CostOfSales`; industry templates for banks and insurers (no COGS: mark structurally NaN, not missing) | >= 0.90 |
| `oi_ttm` (operating income) | 0.77 | `ebit_ev`, `opbe`, `cbop` | fallback chain `OperatingIncomeLoss` -> `IncomeLossFromContinuingOperationsBeforeIncomeTaxes + InterestExpense`; IFRS tags | >= 0.92 |
| `xrd_ttm` (R&D) | **0.38** | `rd_me`, intangible-adjusted value (new) | R&D is legitimately zero for most firms: deliver **0 with a `reported_zero` flag** when the filer has no R&D line, NaN only when the statement is missing | >= 0.95 (zero-filled) |
| `fscore` components | 0.54 | `fscore` | the 9 Piotroski inputs individually (ROA, CFO, dROA, accrual, dLever, dLiquid, eq_offer, dMargin, dTurn) so we compute a partial score with >= 6 inputs | >= 0.85 |
| `capx_ttm` | 0.86 | `cbop`, `fcfp` | fallback `PaymentsToAcquirePropertyPlantAndEquipment`; IFRS | >= 0.95 |
| `shrs_q` (shares from filings) | 0.85-0.94 | `issuance_xbrl` | dimensional facts: multi-class issuers (GOOGL, META, BRK) summed across classes; `dei:EntityCommonStockSharesOutstanding` cover-page fallback | >= 0.97 |
| `txt_q` (tax expense) | 0.89 | `chtax` | IFRS `IncomeTaxExpenseContinuingOperations` | >= 0.95 |
| `sale_ttm` | 0.91 | `sp`, `gp_ttm` | `Revenues` / `RevenueFromContractWithCustomer...` / `SalesRevenueNet` chain; IFRS `Revenue` | >= 0.96 |
| IFRS filers (20-F / 40-F) | 0 issuer items | all fundamental alphas | Company Facts `ifrs-full` taxonomy mapped to the same items | ~3% of operating members recovered |

## 3. New raw data for the literature-ranked alpha families

Ranked by the literature review's expected standalone gross Sharpe and by low overlap with the 9 themes we hold
(`v6-literature.md` section 4). **[have partial]** marks data that exists somewhere in atx-db already.

### 3.1 High priority (new independent themes; each expected +0.02 to +0.05 net Sharpe at book level)

| id | raw data | derived features we build (you deliver raw + PIT only) | family / cite |
|---|---|---|---|
| D1 **13F holdings** [have partial: `thirteenf_archive.py`] | per (filer CIK, CUSIP/securityID, quarter-end): shares, value; `available_at` = filing date (45-day lag); filer type (hedge fund / mutual fund / other via Form ADV or a curated list) | institutional ownership level and 4-quarter change; breadth change (# holders); SI x IO interaction; hedge-fund consensus and conviction; connected-stock reversal | Chen-Hong-Stein 2002; Nagel 2005; Edelen-Ince-Kadlec 2016; Anton-Polk 2014 |
| D2 **Earnings calendar with expected dates** | historical announcement dates (have as `earn_recent`), plus the **expected** next date as known in advance (vendor earnFlag history or last-year-same-quarter rule), plus time of day (pre / post market) | earnings seasonality; announcement-premium timing tilt; exact `ear` window (3 days around the actual date instead of the `earn_recent` proxy); pre-announcement drift | Chang et al. 2017; Frazzini-Lamont 2007 |
| D3 **Daily short volume** [have: FINRA CNMS 2018-08+] | keep landing daily; add the exempt and per-venue splits already in the file; add the FINRA **Reg SHO threshold list** and SEC **fails-to-deliver** (twice monthly) | `sv_flow` is live (+0.058); FTD and threshold flags add a short-constraint dimension | Wang-Yan-Zheng 2020; Boehmer et al. |
| D4 **Borrow cost and utilization** (IHS Markit / S3 / Ortex if licensable; else a FINRA-derived proxy) | per (securityID, date): indicative fee, utilization, lendable quantity, `available_at` | the only way to make short financing realistic: special-tier shorts are a flat 500 bps guess today; also needed to screen out hard-to-borrow shorts, which the book cannot see now | Drechsler-Drechsler 2016; Engelberg et al. 2018 |
| D5 **Analyst consensus** (I/B/E/S-class if licensable) | EPS and revenue consensus mean / median / n / std by fiscal period with snapshot dates; recommendation counts; target price | forecast revisions, dispersion, SUE against consensus instead of a seasonal random walk, expected-growth proxy | Diether-Malloy-Scherbina 2002; Hou-Mo-Xue-Zhang 2021 |

### 3.2 Medium priority (upgrade existing themes)

| id | raw data | derived / why |
|---|---|---|
| D6 Quarterly statement items we lack | `cogs`, `sga`, `interest_expense`, `depreciation`, `inventory`, `receivables`, `payables`, `deferred_revenue`, `ppe_gross`, `goodwill`, `intangibles`, `minority_interest`, `preferred_equity`, `dividends_declared`, `ebitda` | exact cash-based operating profitability (Ball et al. 2016), intangible-adjusted book (Eisfeldt-Kim-Papanikolaou 2022), cash-flow duration inputs, distress logit inputs (Campbell-Hilscher-Szilagyi 2008), profitability trend (8-quarter ROA slope) |
| D7 Full quarterly history 2010+ for the above | 8-20 quarter lookbacks | 5-year composite issuance (Daniel-Titman 2006), expected-growth regressions, earnings seasonality, ROA trend |
| D8 Options surface (have ATM IV 21/63/126) | 25-delta put and call IV at 30 d (skew), term slope, option volume and open interest by call / put, implied dividend | skew (Xing-Zhang-Zhao 2010), option-to-stock volume ratio (Johnson-So 2012), put-call OI; realized vol we compute ourselves |
| D9 Daily bar stats | open, high, low, close, VWAP, volume, **dollar volume**, trade count, overnight vs intraday return split | Amihud illiquidity, overnight-vs-intraday momentum (Lou-Polk-Skouras 2019), spread proxy (Corwin-Schultz) for cost-model calibration |
| D10 Corporate actions | splits and dividends (have via factor), spin-offs, mergers with terms and dates, buyback announcements (8-K 8.01), index add / drop dates | event exclusions around M&A; buyback-announcement alpha; index-inclusion flows for the cost model |
| D11 Insider transactions (Form 4) | per (CIK, insider, date, buy / sell, shares, price), `available_at` = filing date | net insider buying (Lakonishok-Lee 2001; Cohen-Malloy-Pomorski 2012 opportunistic filter) |
| D12 8-K / 10-K filing metadata (later) | filing dates and item codes first; text sentiment only if cheap | going-concern, restatement flags, risk-factor change (Cohen-Malloy-Nguyen 2020) |

### 3.3 Low priority / research only

Daily FF5 + momentum factor returns (attribution only); issuer credit spreads if any CDS data; ETF holdings for flow
pressure. Not needed for the next library.

## 4. History windows

| item | window | reason |
|---|---|---|
| All fields in Appendix A and section 2 | 2018-01-01 to today, daily role | 2019 backcast (reserved, unread) + TRAIN + VAL + holdout |
| Prices, volume, adjusted returns | 2010+ if cheap | 5-year issuance and expected growth need 20 quarters before 2020 |
| 13F, insider, short volume | 2018+ (13F 2015+ so 4-quarter changes exist at the 2020 start) | lookback |
| Options | 2018+ | already there |

## 5. Panel-level metrics we would like **you** to compute

We build every alpha in the DSL from raw fields; we do not need derived alphas. These diagnostics only the producer can
compute correctly, and we read them as metadata (never joined to returns):

1. Per-field, per-year coverage over `member_equity` and over CIK-linked members (the Appendix A basis).
2. Per-field distribution per year: p0.1, p1, p50, p99, p99.9, share of exact zeros, share of negatives, so we can set clip
   and winsor rules without reading the data.
3. Vintage audit: share of cells whose `available_at` is a republication, per source per year.
4. Identity audit: per year, share of member cells by `link_tier`; ambiguous-link counts; multi-class issuer list.
5. Field change log between panel versions (added / dropped / redefined fields with SHAs) so a rebuilt library can cite it.

## 6. Out-of-sample data: hold it, do not report it

Keep 2023-2024 and 2025+ in the same panel, byte-for-byte identical construction, but **report no statistic that conditions
on returns** for those years. Our protocol treats every read of 2023+ as a validation trial with an owner gate; we have
spent 2 of 3. The atx-engine seal (`strategy_data.cpp` refuses sessions >= 2025-01-01) stays; lifting it is an owner
decision, not a data request.

## 7. What this buys, in order of confidence

| deliverable | mechanism | expected effect on S2 net Sharpe (TRAIN-calibrated, sign-only confidence) |
|---|---|---|
| U1-U3 wider linked universe | breadth: the v6 restriction gave +0.22 with 60% of member cells; recovering most of the remaining 40% adds names, not noise | +0.05 to +0.15 |
| Section 2 fundamental coverage | `gpa`, `ebit_ev`, `rd_me`, `fscore` are admitted or rejected on 40-60% coverage; 7 candidates had no defined IC for that reason | +0.03 to +0.08; rescues 3-5 rejected members |
| D4 borrow data | removes hard-to-borrow shorts (they cost, not earn); real financing replaces the 500 bps special-tier guess | +0.02 to +0.06 net; removes the largest unmodelled risk in the S2 estimate |
| D1 13F, D2 calendar, D5 consensus | 2-3 new low-turnover themes; `v6-literature.md` section 4 arithmetic: 9 -> 12 independent themes is +8% gross | +0.05 to +0.10 |
| D6-D7 statement items and history | upgrades inside profitability, value, investment (CbOP, intangibles, 5y issuance) | +0.02 to +0.05 |
| D8-D9 options and daily bars | refinements; cost-model calibration | +0.01 to +0.03; better cost realism |

All effects are TRAIN expectations subject to pre-registration and the sign-only acceptance rule; nothing is promised.

## 8. Acceptance contract per deliverable

Accepted when: (i) manifest with per-file SHA-256 and code SHA; (ii) `available_at` on every row; (iii) coverage table on
the `member_equity` basis per year, target met or shortfall explained (structural zero vs missing); (iv) staleness rule
stated; (v) `link_tier` on issuer joins; (vi) one atx-impl load (`prepare_research_fields.py --fields <new>`) on the role
`recent-fast-train-2020-2022-v2-lo1` succeeds and its manifest binds yours; (vii) no returns-conditioned statistic for 2023+.

Owner of the request: mega-alpha controller. Questions in this file's PR thread or as `docs/plans/` follow-ups.

---

## Appendix A. What we consume today: fields-v7 on role `recent-fast-train-2020-2022-v2-lo1`

Finite share over operating-company member cells (score window 2020-2022, then per year), all PIT:

| field | score win. | 2020 | 2021 | 2022 | used by |
|---|---|---|---|---|---|
| si_shares, si_dtc | 1.000 | 1.000 | 1.000 | 1.000 | si_ratio, dtc, si_change |
| sv_ratio126 (FINRA short volume, new) | 1.000 | 1.000 | 1.000 | 1.000 | sv_flow |
| iv_atm_21d / 63d / 126d | 0.983 | 0.974 | 0.983 | 0.992 | iv_rv_spread |
| earn_recent, shares_out, mkt_ret, me_company | 1.000 | 1.000 | 1.000 | 1.000 | ear, issuance, bac, size |
| be | 0.975 | 0.976 | 0.975 | 0.973 | bm, roe_q, value_composite |
| at / at_lag4 | 0.984 / 0.969 | | | | asset_growth, roa, gpa, noa |
| lt, che, debt | 0.980-0.984 | | | | ebit_ev, cbop, noa |
| sale_ttm | 0.907 | 0.905 | 0.906 | 0.911 | sp |
| **gp_ttm** | **0.606** | 0.604 | 0.610 | 0.603 | gpa |
| **oi_ttm** | **0.767** | 0.766 | 0.769 | 0.767 | ebit_ev, opbe, cbop |
| ni_ttm, ni_q, ni_q_lag4 | 0.960-0.964 | | | | ep, roe_q, droe, sue |
| be_lag1q, be_lag1q_lag4 | 0.961 | | | | roe_q, droe |
| cfo_ttm | 0.964 | 0.965 | 0.960 | 0.967 | cfp, accruals, cbop |
| capx_ttm | 0.855 | 0.861 | 0.851 | 0.851 | fcfp, cbop |
| **xrd_ttm** | **0.375** | 0.374 | 0.388 | 0.363 | rd_me |
| dvc_ttm, prstkc_ttm, sstk_ttm | 0.969-0.971 | | | | net_payout, issuance_xbrl |
| txt_q / txt_q_lag4 | 0.894 / 0.897 | | | | chtax |
| shrs_q / shrs_q_lag4 | 0.882 / 0.880 | 0.849 | 0.859 | 0.938 | issuance_xbrl |
| noa / noa_lag4 | 0.976 / 0.962 | | | | noa |
| sue | 0.940 | 0.945 | 0.937 | 0.939 | sue |
| **fscore** | **0.535** | 0.530 | 0.541 | 0.533 | fscore |
| grp_sic2, grp_ff12, grp_ff49 | 1.000 / 1.000 / 0.999 | | | | industry ops |

Admission on this role (library v6.1, screen v4-prior-v1): 32 admitted, 6 rejected as redundant (ep, cfp, fcfp, sp,
issuance_vendor, mom_12_1: |rho| > .90 with a sibling), 1 vetoed (si_change: sign against prior). Seven candidates (ep,
ebit_ev, rd_me, gpa, opbe, fscore, opex_at) had no defined IC at one or more horizons because of field coverage: that is
the section 2 list.

Sources: `build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7/manifest.json`, `build-equity/mega-weights-v61-ew/admission.json`,
`.superpowers/sdd/mega-alpha-20260926/v6-literature.md` sections 3-4, `C:/atx/atx-db/docs/ALPHA_PANEL_STATUS.md`.

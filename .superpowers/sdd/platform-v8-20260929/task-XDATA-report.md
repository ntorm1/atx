# Task XDATA report: datasets (expansion X, lane XDATA)

Lane XDATA, pool 14, branch `feat/platform-v8-xdata-20261002`, base `3c6ae225`. Blind: no return, IC, Sharpe, turnover
or NAV output of 2020-2023 was opened; nothing dated 2024-01-01 or later was opened as data. Warehouse documents,
schemas and source were read through git objects of local `main` (`179af41c`) only; no atx-db data file and no atx-db
working tree was opened or written. Python only, synthetic tests only, nothing built, nothing run on data.

| item | status | commit |
|---|---|---|
| 1. dataset map | DONE | `1df3da62` |
| 2. data asks against the warehouse | DONE | `1df3da62` |
| 3. eight field families, ranked | DONE | `e035a773` |
| 4. builders and synthetic tests, top three in house | DONE (`div_month_pred`, `beta_dvol_21`, `season_y2_5`) | `71238bb4` |
| 5. external datasets | DONE (5) | this commit |

Tests: `test_research_fields_xdata.py` 7 passed; `atx-engine/tools` 260 passed, 6 subtests passed.

Hygiene and disclosures (full list in section 6): two atx-db documents printed per-year data-presence counts that run
into 2024-2026 (LICENSED_ADAPTERS.md options coverage table; ALPHA_PANEL_SEC.md delisting-cause counts). They are
availability and filing counts, not return or signal statistics; nothing below uses them. The fields v10 lo3 manifest
was read for field names, sources, clocks and member-cell coverage fractions (data presence, 2018-2023 role).

## 1. Dataset map

"v8 fields" = the fields v10 lo3 list (70 fields, manifest `a4a060ae`) plus the opt-in v11 / v12 / v13-draft fields.
Stamping is the session at which a value becomes known (the decision at 23:00 UTC of session d uses what is known
by the 22:00 UTC mark of d; issuer, SEC and holdings data use `available_at < 22:00 UTC of d-1`).

### 1a. Sources bound by v8 builds today

| source (pin) | holds | stamping / lag | coverage | v8 fields that read it | in house, unused |
|---|---|---|---|---|---|
| SpiderRock TickerHistory3 parquet (`0ed96b26`, 3.6 GB, 32.3 M rows, 71 columns; audit `atx-engine/reviews/2026-09-19-tbltickerhistory-input-audit.md`) | OHLC, volume, vendor shares, `cumulReturnFactor` / `returnFactor` / `totalReturn`, prior-day closes, `earnFlag`, ATM IV at 15 tenors (`atmCenI_5d..504d`, `_decay/_st/_lt`), `atmCenH_*`, `nEarnCnt_*`, `ccVar/hlVar/rvVar`, `expiryCount`, `iEMove/hEMove`, `wkD1/shD1/qtrD1/lnD1`, tickers; every vendor line incl. ETFs (SPY, QQQ) and index lines | EOD row known at the session's 22:00 UTC mark (`TH_CLOCK`, same date); F-1 fields read t-1 only. Exception: the vendor delivers IV at 22:00 America/Chicago (03:00-04:00 UTC next day), so the same-date `iv_atm_*` fields are one session early (atx-db TIER1_V3_STATUS "IV clock", open). No vintage proof | 2012-03-26 on; research reads stop at the seal | role projection (`close`, `raw_close`, `volume`, `present`, `member`), `shares_out` (A8 90-day lag), `iv_atm_21d/63d/126d`, `earn_recent`, `mkt_ret`, `ret_overnight`, `ret_intraday` (open: OD-6 served), `ceq_iss_5y`, `coskew_60m`, `vol_126`; `sv_ratio126` maps tickers through it | `high`, `low`; the distribution history (`returnFactor`) as an ex-date ledger; IV tenors 5d, 42d, 84d, 105d, 189d, 252d-504d, `_decay/_st/_lt`; `ccVar/hlVar/rvVar`; `expiryCount`; the ETF lines (SPY's IV and return). Excluded by design: `atmCenH_*`, `nEarnCnt_*` (forward calendar), `iEMove/hEMove/wkD1/shD1/qtrD1/lnD1` (excluded as forward-horizon; LICENSED_ADAPTERS calls `wkD1..` ATM slopes: an open question, section 2) |
| FINRA short interest as-of CSVs (`C:/atx/data/finra_short_interest/asof`, dissemination schedule) | short interest, days to cover | latest row with official dissemination date < session date; stale 45 days; pre-2021-06 is FINRA's republication | 2018 on | `si_shares`, `si_dtc` | atx-db's newer `short_interest/si.parquet` (rule si-ticker-asof-settlement-v3) is not bound |
| FINRA daily short volume CNMS files (`raw/finra_short_volume`) | short and total volume per symbol-day | trade date T visible from T+1 (`finra-cnms-ratio126-lag1-v1`) | 2018 on | `sv_ratio126` | - |
| atx-db `short_volume_ext/` | short, exempt and total volume by venue | `available_at`, T+2 rule (`finra-svx-126-t2-v1`) | 2018 on | `sv_offexchange_share126` | exempt volume, per-venue split |
| atx-db `ftd/` | SEC fails-to-deliver | publication clock (`sec-ftd-latest-visible-21-v1`) | 2018 on | `ftd_shares_ratio21` | - |
| atx-db `regsho_threshold/` | threshold lists, 5 markets | `available_at` (derived clock outside Nasdaq: `vintage_risk`) | 2018 on | `regsho_threshold_days63` | - |
| atx-db `thirteenf/` (holdings parts, `cusip_map_pit`, `agg_asof45`) | 124.4 M INFOTABLE rows: filer CIK, CUSIP, shares, value, put/call, discretion | quarter visible after its 45-day deadline (`13f-filed-plus-46h-v1`, `13f-quarter-asof45-v1`); stale 150 days | filing quarters 2013q2 on | `inst_own_share`, `inst_breadth_chg`, `inst_own_chg_q`, `inst_best_ideas`, `inst_n_holders` | manager-level structure (who holds what: common ownership, manager turnover / horizon, concentration), put/call holdings, `top10_share`, values |
| `fundamental-events-v3` (engine export of atx-db `fundamentals/` v5 events) + identity bridge v2-pit | 30 issuer items (`be` .. `fscore`, TTM and quarterly, `_lag4`) | `fund-events-lagged-v1`: latest row with `accepted_utc` < mark(t-L), L = 1; stale 200 / 400 days | events from 2014-06 | 30 issuer fields; v11 `gscore7_lowbm`, `eps_consist_4y`; v13-draft `earn_season_rank` | the rest of atx-db's item catalogue (not exported) |
| atx-db `fundamentals/sic_events.parquet` | dated SIC | as above, 550-day staleness | 2014 on | `grp_sic2`, `grp_ff12`, `grp_ff49`, `grp_ff12f49` | - |
| atx-db `sec_filings/` (`filings`, `eight_k_items`, `events`, `filer_regime`, `issuer_profile`, `delisting_causes`) | every EDGAR filing per CIK (form, acceptance, report date, items, size, XBRL flags), 8-K items, NT / Form 25 / Form 15 events | EDGAR acceptance (`acceptance-per-file-clock-v1`), usable from the session after the first 22:00 UTC mark that follows it (`sec-acceptance-lag1-v1`); `issuer_profile` and `filer_regime` segment ends are not PIT | filing dates 2009-01-01 on | `k8_count_63`, `k8_item_material_21`, `k8_days_since_any`; v11 `k8_item402_63`; v13-draft `nt_first_126` | almost every form type (S-1 / S-3 / 424B offerings, 10-K/A, DEF 14A / DEFC14A, SC 13D/G subject-side metadata, CORRESP / UPLOAD comment letters, Form D, SC TO), 8-K items 5.02, 4.01, 3.01, 1.03, 2.05, 2.06, `report_date` (filing lag), `size` |
| atx-db `earnings_calendar/` | 8-K 2.02 announcements, timing, expected dates (`yoy_364`) | announcement acceptance; expected date known at the year-ago announcement | 2009 on | `ea_days_to_expected`, `ea_days_since`, `ea_window_pre5`, `ea_window_post3`, `ea_delay_days`, `ea_time_of_day` | (the roster reads only two of the six; XSIG's ground) |
| atx-db `insider/` (SEC insider transactions data sets) | Form 3/4/5 transactions | acceptance from `sec_filings`, else filing date + 1 day | 2015q1 on | `ins_net_buy_ratio`, `ins_n_buyers`, `ins_n_sellers`, `ins_opportunistic_net`, `ins_cluster_buy` | (the roster reads one of five) |
| atx-db `delisting/` (U4) | terminations, delisting returns | cause known up to 30 days after T: labels and NAV only | 2018 on | the `-dlret` role (labels) | - |

### 1b. Published atx-db stages no v8 field reads (in house per the warehouse docs; a consumer binding is all they need)

| stage | holds | stamping | note |
|---|---|---|---|
| `market/shares_daily` (S3.4 PASS) | PIT `shrout` (SEC dei 76%, vendor 24%) | latest filing at 22:00 UTC of d-1; vendor change visible at the matching filing's clock | owner decision open: replace the A8 vendor `shares_out` (TIER1_V3_STATUS 3c) |
| `options/` | `iv_atm_{21,63,126,252}d`, term slopes | `options-clock-v1`: usable from the second following session | a copy of TH3 columns; licensed skew / volume / OI columns are NULL |
| `reference/` | FRED rates, `VIXCLS`, H.10 FX, French factors and SIC maps | release calendars (VIX 16:30 ET same day) | VIX is an alternative to the SPY IV of section 4 |
| `classification/` | FF 5..49, approximate NAICS 2002 / 2022 | SIC event clock | - |
| `identity/link_table_v3`, `export/identity-bridge-v3-*` | dated CIK links, seal-honouring exports | link clocks | v8 still binds bridge v2-pit |
| `security_master` v3 | listing events, line listing, name and CUSIP history | event clocks | `exch_up_365d` (R-12) reads listing events through the holdings module |
| `insider_ext/` | CMP-labelled open-market trades, monthly net buying; Form 144 notices (not landed) | acceptance | value sums need a price-sanity rule; Form 144 is mostly paper before 2023-04 |
| `events/{governance,capital}` | 8-K text events | acceptance | published, not hand-checked |
| `notes/` | raw FSDS notes landing 2019q1 on (818 MB of parts) | SUB acceptance | consumer stage `fundamentals_notes/` not built |
| `borrow_proxy/`, `panel/` v2 | public short-side inputs lined up; daily panel | per input | the panel is a copy of inputs v8 already binds |

### 1c. Not built or partial (a warehouse change is needed)

`fundamentals_v10` and export `fundamental-events-v2` (code on main, a real-data validate/export commit `8a1b4159`
with a 2024-01-01 export seal; no pinned manifest a consumer can bind), `fundamentals_notes/` and `identity_cover/`
(code and fixtures only), `nport/` (16 of 27 quarters parsed, 2019q4-2023q3; build not run), `stakes/` (stale parser),
`thirteenf_filer_type/` (inputs landed, not built), `filing_text` / `text/` / `classification_tnic/` (7% landed: 2,286
of 32,411 filings; test builds only, "do not bind"), `market/liquidity`, `market/index_returns`, `indexes/` (not run),
events guidance / buyback / M&A (not done). atx-db's forward-return `labels/`, `ic_eval` and characteristics v2 hold
return and IC artifacts: closed to X lanes under the blind rule.

## 2. The data asks against the new warehouse

The four asks of status 5 (OD-6 form) and the three OD-6 items of the sprint plan:

| ask | serves | warehouse state on `main` | status | what is left (owner / atx-db) |
|---|---|---|---|---|
| open, high, low in the price export (plan OD-6) | `night_day`, `day_rev_freq` | TH3 carries `open`, `high`, `low`; F-1 reads the open from the pinned source | SERVED (fields v10 built `ret_overnight`, `ret_intraday`) | nothing; high / low unused |
| NT 10-K / NT 10-Q rows (plan OD-6) | `nt_late` | `sec_filings/events.parquet` classifies NT 10-K / 10-Q / 20-F with acceptance clocks | SERVED (`nt_first_126` FIELD-BUILT on `834d5a05`) | integration 8 |
| current and deferred income tax (plan OD-6; status-5 "fundamentals notes") | `tax_book` | two routes on main, neither published: (a) fundamentals v10 catalogue items `txc_ttm` (TXC, `CurrentIncomeTaxExpenseBenefit`), `txdi_ttm`, `txdc_ttm`, joined into export `fundamental-events-v2`; (b) `fundamentals_notes/` `tax_current` / `tax_deferred` (and pension, SBC, impairment, leases) | PARTIAL: code on main, no pinned stage | atx-db: publish `fundamentals_v10` + `fundamental-events-v2` with a manifest (route a, nearest). Engine side (ours, after that): `build_fundamental_events.py` / issuer fields carry `txc_ttm`. Route b also needs the notes build (minutes per the doc) |
| filing text | `lazy_prices`, `tnic_mom` | TXT lane code on main (`filing_text`, `tnic`, `text_features`): Lazy Prices similarities per section, TNIC-3 style peers | OPEN: 7% landed, stages are test builds | atx-db: finish the landing (about 30k filings within a 60k-request budget), then `tnic` and `text_features`, then a consumer export. Sentiment needs a commercial Loughran-McDonald licence (owner) |
| N-PORT | `fund_fit`, `conn_rev`, flow-induced trading | `nport.py` on main with `fund_ownership.parquet` (incl. `flow_induced_shares`, Lou 2012) and `fund_flows.parquet` | OPEN, near: 16 of 27 quarters parsed (2019q4-2023q3), build not run | atx-db: parse the remaining quarters, build, publish a manifest |
| put-wing implied volatility | `iv_skew` (and option volume / OI families) | no source; the `options/` stage has NULL licensed columns and a licensed adapter with mocks | OPEN: a purchase | owner: buy history (section 5, item 1) |

The warehouse items the brief names:

| item | on `main`? | relevance to v8 / X |
|---|---|---|
| gold alpha panels | NO. Only in another session's uncommitted tree (`C:/atx`, branch `feat/tier1-v3-warehouse`: untracked `alpha_panel/gold.py`, `gold_freeze.py`, `holdout_eval.py`, `labels_holdout.py`, `silver_refresh.py`, `source_audit.py`, `databento_tail.py`, docs `ALPHA_PANEL_GOLD.md` / `ALPHA_PANEL_VALIDATION.md`; names from that tree's git status, files not opened). On main: their precursors, the forward-return labels stage and IC evaluator v2 (`d2417b23`, "gold alpha panels task 3") and characteristics v2 | Not usable now. Risk to rule on before any X lane binds a gold panel: file names say holdout evaluation and holdout labels; a gold panel must be shown to carry no label or IC column and no row on or after 2024-01-01 before an X consumer reads it |
| 13F archive (`thirteenf_archive.py`) | yes, warehouse-v2 DuckDB module (2026-08-09); uncommitted edits elsewhere | v8 reads the alpha-panel `thirteenf/` stage instead (filing quarters 2013q2 on), already in house; no ask |
| FINRA short volume (`finra.py`, `short_volume.py`) | yes (v2 modules) plus alpha-panel `short_volume/`, `short_volume_ext/` | served |
| shares outstanding (`shares_outstanding.py`; alpha-panel `market/shares_daily`) | yes; `market/shares_daily` published, S3.4 PASS | a data-quality ask, not a family: bind PIT `shrout` for issuance, `si_ratio`, `me_company` denominators (owner decision 3c) |
| fundamental statements, XBRL catalog (`fundamental_statements.py`, `xbrl_catalog.py`) | yes, warehouse-v2 companyfacts modules; uncommitted edits elsewhere | v8's path is the alpha-panel fundamentals stage; its successor is the v10 catalogue (81 Compustat-analog items). Ask: publish v10 + export v2 (row above) |

Open question for the owner (no data opened): TH3's `wkD1`, `shD1`, `qtrD1`, `lnD1` are "ATM slopes" in atx-db's
LICENSED_ADAPTERS.md and "forward-horizon moves" in the v8 builder's exclusion list. If the SpiderRock dictionary
says they are skew or term slopes observed at the session, an in-house skew proxy exists; until then they stay
excluded.

## 3. Eight field families, ranked by prior of alpha orthogonal to the 10 v8.0 themes

The 10 themes (52 members, `fund_industry_ic_v80.json`): value, profitability_quality, investment_issuance,
earnings_momentum, price_momentum, low_risk, short_interest, reversal_seasonality, options_implied, ownership_flow.
Excluded from consideration: everything allocated to R-7 (v8.1) and R-12 (v8.2), the v9 draft's built fields, and
signals on fields already in the store (XSIG's ground). The rank weighs the published effect after the house haircut
class, orthogonality (a data source or a horizon no member reads) and how clean the stamping is. Effect sizes are as
recalled from the papers, not re-derived here; root verifies the citations before any registration.

| rank | family | source | in house? | candidate signal it serves | builder |
|---|---|---|---|---|---|
| 1 | filing-text change | atx-db `text/` (EDGAR 10-K / 10-Q sections) | NO: landing 7% | `lazy_prices` (Cohen-Malloy-Nguyen 2020), long similar filings | design (data ask) |
| 2 | fund flows and fund holdings | atx-db `nport/` | NO: 16 / 27 quarters, no build | `fit_q` flow-induced trading (Lou 2012); `conn_rev` (Anton-Polk 2014) | design (data ask) |
| 3 | dividend calendar | TH3 vendor factor (ex-date ledger) | YES | `div_season` (Hartzmark-Solomon 2013) | BUILT `div_month_pred` |
| 4 | aggregate-volatility risk | TH3 SPY ATM IV and SPY return | YES | `vol_beta` (Ang-Hodrick-Xing-Zhang 2006), long low beta | BUILT `beta_dvol_21` |
| 5 | long-history return seasonality | TH3 closes from 2012 | YES | `season_y2_5` (Heston-Sadka 2008; KLN 2016) | BUILT `season_y2_5` |
| 6 | tax and pension notes items | fundamentals v10 catalogue / `fundamentals_notes/` | NO: not published | `tax_book` (Lev-Nissim 2004); `pension_fund` (Franzoni-Marin 2006) | design (data ask) |
| 7 | 13F manager network | atx-db `thirteenf/` holdings | PARTLY: holdings yes; manager types no | 13F `conn_rev`; short-term institutional ownership (Yan-Zhang 2009) | design (data ask for the canonical form) |
| 8 | text-based industry peers | `classification_tnic/` or the Hoberg-Phillips library | NO (or external) | `tnic_mom` (Hoberg-Phillips 2018) | design |

Common builder contract (every row below): a new opt-in field module (`research_fields_*.py`) registered through a
draft entry, never through the plain builder; `point_in_time` true; the seal read from `research_window` on the reader
side (rows with `available_at` / `accepted_utc` / trading date on or after `SEAL_NS` dropped and counted; a year or
quarter partition that begins on or after the seal never opened, `rw.partition_is_sealed`); reuse fingerprint =
`PRODUCERS` group AST closure with the builder closure read through `h`, plus each input stage's manifest SHA-256 in
`reuse_inputs` / `entry_inputs`, plus `imported_code` for any code imported from another field module (review B-1).

### Rank 1. Filing-text change (Lazy Prices)

- **Signal and prior.** Cohen, Malloy and Nguyen (2020, JF) "Lazy Prices": firms whose 10-K / 10-Q language changes
  from the prior year's ("changers") subsequently underperform non-changers, with no announcement reaction; the
  abstract reports up to 188 bp a month of alpha for the long-short (as recalled). Sign: long high similarity.
- **Orthogonality.** No theme reads filing text. Nearest roster members: the earnings_momentum surprise composite
  (`earn_surprise_comp`), which reacts to reported numbers; the text change anticipates later news instead. Slow
  (annual / quarterly), so low turnover and good capacity.
- **Builder design.** `text_sim_10k` = the latest original 10-K's section cosine similarity to its prior-year 10-K
  (`text/features.parquet`, `*_sim_cosine`, mean of Items 1A and 7 present). Stamping: the newer filing's EDGAR
  acceptance (`available_at < 22:00 UTC of t-1`), stale 400 days (the stage's consumer rule). Seal: rows with
  `available_at >= SEAL_NS` dropped. Reuse: stage manifest SHA-256 and the SEC identity bridge SHA (as the SEC
  module). Data ask: the TXT landing, `text_features`, and a consumer export.

### Rank 2. Fund flows and fund holdings (N-PORT)

- **Signal and prior.** Lou (2012, RFS) "A flow-based explanation for return predictability": flow-induced trading
  (FIT) predicts returns positively over the following quarter and reverses over the next years. Coval and Stafford
  (2007, JFE): stocks sold by funds in large outflows underperform, then revert. Anton and Polk (2014, JF) "Connected
  stocks": common active-fund ownership forecasts excess comovement; a cross-stock-reversal strategy earns about 9% a
  year of four-factor alpha (as recalled). Signs per horizon to be registered: FIT +1 at one quarter (Lou).
- **Orthogonality.** Price pressure from flows is not firm information. Nearest theme: ownership_flow (`ins_opp`
  insider trades; `inst_best_ideas` 13F conviction weights); neither reads fund flows or fund-level holdings.
- **Builder design.** `fit_q` = `fund_ownership.flow_induced_shares` / shares of the latest visible (security,
  quarter) row. Stamping: the row's `available_at` = the latest included N-PORT acceptance (filed by report date + 60
  days), usable from the session after the first 22:00 UTC mark that follows it; stale 180 days after the quarter end
  (stage rule). Seal: rows on or after `SEAL_NS` dropped; `quarter=YYYYqN` parts that begin on or after the seal never
  opened. Reuse: the `nport` manifest SHA and the 13F CUSIP map SHA. Coverage: filings from 2019q4, so FIT (it needs
  the prior net assets) from 2020q1: all of TRAIN. Data ask: the remaining 11 quarters, the build, a manifest.

### Rank 3. Dividend calendar (BUILT, section 4)

- **Signal and prior.** Hartzmark and Solomon (2013, JFE) "The dividend month premium": dividend payers earn about
  41 bp a month of abnormal return in months in which a dividend is predicted (as recalled), predicted from the
  payment three, six, nine or twelve months earlier. Sign +1.
- **Orthogonality.** A calendar effect around ex-dates: no member reads dividend timing. Nearest: `net_payout`
  (value) reads the TTM dividend level, not its month. The field's cross-section is quarterly payers only, so the
  payer-versus-non-payer tilt (a quality / low-risk exposure) is not in it.
- **Cost.** The flag flips monthly: the trading cost per unit of signal is the question its cell must answer.

### Rank 4. Aggregate-volatility risk (BUILT, section 4)

- **Signal and prior.** Ang, Hodrick, Xing and Zhang (2006, JF) "The cross-section of volatility and expected
  returns": stocks with high past sensitivity to innovations in aggregate volatility (daily changes of VXO, the S&P
  100 30-day ATM implied volatility) earn low returns, about -1% a month for the extreme-quintile spread (as
  recalled), robust to size, value, momentum, liquidity and market beta. Sign: long low beta.
- **Orthogonality.** A macro-volatility exposure. Nearest low_risk members: `bac` (correlation with the market),
  `smax` / `smax5` (lottery), `qmj_safety` (market beta inside a composite); none reads an implied-volatility factor,
  and AHXZ separate the effect from market beta and from idiosyncratic volatility. A new theme (`macro_vol_risk`) or
  low_risk is the PM's registration choice.

### Rank 5. Long-history return seasonality (BUILT, section 4)

- **Signal and prior.** Heston and Sadka (2008, JFE) "Seasonality in the cross-section of stock returns": returns
  at annual lags (12, 24, ..., 240 months) predict the same calendar month; Keloharju, Linnainmaa and Nyberg (2016, JF)
  "Return seasonalities". Sign +1.
- **Orthogonality.** Same theme as `seasonality_same_month` (reversal_seasonality, year 1 only), different horizon:
  no member reads returns older than 252 sessions (the runner's 336-session bound forbids it in the DSL). Not a
  restatement: Heston-Sadka report each annual lag predicting on its own; years 2-5 exclude the year-1 window.

### Rank 6. Tax and pension notes items

- **Signal and prior.** Lev and Nissim (2004, TAR): taxable income relative to book income predicts earnings growth
  and returns (`tax_book`, withdrawn as R7-3 for data). Franzoni and Marin (2006, JF) "Pension plan funding and stock
  market efficiency": severely underfunded firms earn low future returns, not explained by size, value or momentum.
- **Orthogonality.** Pension funding: no member reads pension items. `tax_book` sits beside profitability_quality.
- **Builder design.** Issuer route (`fund-events-lagged-v1`: the latest events row with `accepted_utc` < mark(t-1),
  staleness 200 / 400 days, primary links), as `gscore7_lowbm`. Seal: the builder's `load_events` drops rows on or
  after `SEAL_NS`. Reuse: the events manifest SHA, the bridge SHA, the declared lag. Data ask: publish the v10 export
  (`txc_ttm`) or build `fundamentals_notes/` (`tax_current`, `pension_funded_status`; landing from 2019q1, so TTM from
  2020).

### Rank 7. 13F manager network

- **Signal and prior.** Anton-Polk connectedness built from 13F managers instead of mutual funds; or short-term
  institutional ownership (Yan and Zhang 2009, RFS: ownership by high-turnover institutions predicts returns
  positively; manager turnover after Gaspar, Massa and Matos 2005).
- **Orthogonality.** Nearest: `inst_best_ideas`, `si_low_io` (institutional ownership levels and weights), not the
  network or the manager horizon.
- **Builder design.** Stamping `13f-quarter-asof45-v1` (a quarter visible after its 45-day deadline + 46 h), stale
  150 days. Seal: `source=YYYYqN` parts that begin on or after the seal never opened; rows on or after `SEAL_NS`
  dropped (as `research_fields_holdings.py`). Reuse: thirteenf manifest and CUSIP map SHAs.
- **Why not built.** The canonical forms use active mutual funds (Anton-Polk) or a manager-type split; 13F alone has
  no type (S5.1 `thirteenf_filer_type` not built), and a 13F-only filter (say, dropping quasi-indexers by breadth)
  would be my construction, not the paper's definition. Data ask: the S5.1 build (inputs landed), or N-PORT (rank 2).

### Rank 8. Text-based industry peers

- **Signal and prior.** Hoberg and Phillips (2018, JFQA) "Text-based industry momentum" (v9 `tnic_mom`).
- **Orthogonality.** Lowest of the eight: a peer-momentum signal beside `ind_mom_12_1` and `res_mom_ind`.
- **Builder design.** Peers of year Y usable from the pair's `available_at` (the later of the two 10-K acceptances),
  at most 550 days (stage consumer rule). Data: the TXT landing then `classification_tnic/`, or the external library
  (section 5, item 5).

Considered and not ranked: TH3 high / low liquidity measures (Corwin-Schultz 2012, Amihud 2002: the premium has
shrunk in large caps and a long-illiquid tilt costs capacity); 8-K items 5.02, 4.01, 3.01 (no peer-reviewed
post-filing drift I could cite); S-3 / 424B offering events (inside investment_issuance); buyback announcements (8-K
text not parsed; the long-run drift weakened after 2003, Fu and Huang 2016, as recalled); 13D activism (Brav, Jiang,
Partnoy and Thomas 2008: the return is at the filing, with no later drift); TH3 `iEMove` (the vendor's forward
earnings calendar is not shown to be point in time).

## 4. Builders for the top three in-house families

New files only (`atx-engine/tools/`); no existing file changed:

| file | role |
|---|---|
| `research_fields_xdata.py` | the field module: `FIELDS` (the field spec entries), `PRODUCERS`, `HOST_HANDLES`, `bind`, `producer_group`, `field_spec`, `reuse_inputs`, `entry_inputs`, `imported_code`, `XdataFieldModule` (`check`, `compute`, no CLI option) |
| `prepare_research_fields_xdata.py` | the draft registration entry: `DRAFT_MODULES`, `FIELDS_XDATA_DRAFT = ("div_month_pred", "beta_dvol_21", "season_y2_5")`, `register(host_namespace)`, `main(argv)` = register then the builder's own `main` (the FIELDS-V9 pattern; at integration 8 it folds into `prepare_research_fields_draft.DRAFT_MODULES` of `834d5a05` as one tuple element) |
| `test_research_fields_xdata.py` | 7 synthetic tests (below) |

All three fields read the role's own vendor source through research_fields_price.py's `--price-source` (its SHA-256
must equal the role's `source_sha256`) and its panel machinery (`source_panel`: the observation contract, duplicate
quarantine, factor-break-v1 chaining, kept-gap spans; `extended_days`: NYSE rule sessions before the role). No new CLI
option.

### 4a. The fields as coded

| field | formula id | definition | stamping (when known) |
|---|---|---|---|
| `div_month_pred` | `hs-divseason-q3-6-9-12-v1` | Ex-date ledger: an observed session s of a line whose step from its previous observation p has y = 1 - F_p/F_s in [1 bp, 4%], no kept-gap step in (p, s], and is not a factor-break-v1 jump cell (a factor step with no matching raw drop). Row t, M = the month of session t: n_paid = months of M-12..M-1 holding an ex-date; a quarterly payer has 3 <= n_paid <= 6 and a first observation at or before month M-12's first session; value 1 if an ex-date is in M-3, M-6, M-9 or M-12, else 0; NaN for non-payers, annual, semiannual and monthly payers, short history | an ex-date is stamped at its own session (the vendor applies the factor on the ex-date; the dividend was declared before it); only months before the decision month are read, so every row used is at or before t-1. Constant within a month |
| `beta_dvol_21` | `ahxz-beta-dvol-spy21-v1` | OLS of the line's daily adjusted return on [1, r_SPY, dIV_SPY] over sessions t-21..t-1 (at least 17 usable days, regressors not collinear); value = the dIV slope. SPY = the unique securityID with `ticker_tk` 'SPY' (else refused); r_SPY its adjusted return (house guard, jump cells excluded); dIV the daily change of its `atmCenI_21d` inside `IV_DOMAIN`; line returns with the house guard and no kept-gap step | rows t-22..t-1 only: line and SPY closes at their 22:00 UTC marks; SPY's IV of session s is delivered by 04:00 UTC of s+1 (22:00 America/Chicago), before the mark of t |
| `season_y2_5` | `hs-season-y2-5-v1` | for k = 2..5: b = e - 252k, a = b + 21 (e = row t on the extended axis); r_k = P_a/P_b - 1 with both observations, no kept-gap step, the monthly divergence guard; value = mean of the finite r_k when at least 3 | rows t-1260..t-483 only |

Seal (reader side, research_window): `compute` refuses with `SealError` unless the builder's `SEAL` is
`research_window.SEAL`; the panel and the SPY reader skip and count every vendor row dated on or after it and never
read past the role's last session (`source_checks.xdata.source.rows_on_or_after_seal_skipped`,
`source_checks.xdata.vol_line.rows_on_or_after_seal_skipped`).

Reuse fingerprint: `PRODUCERS` = `x_div` (`div_events`, `div_month_rows`), `x_dvol` (`vol_line`, `line_returns`,
`dvol_beta_rows`), `x_season` (`season_rows`), each with the builder closure read through `h`; `reuse_inputs` =
`session_calendar` (the NYSE rule calendar of the extended axis, research_fields_price review B-1) and
`imported_code` (SHA-256 of the AST closure of the seven names imported from research_fields_price.py with the builder
definitions they read; an edit of `source_panel` changes it and recomputes the field). The price source needs no pin
beyond the role binding. Manifest entry extras: `producer`, `formula_id`, `formula_sha256`, `lag_sessions` 1,
`min_history`, `session_calendar`, `imported_code`, and per field: `ledger` stats and payer / predicted / non-payer /
infrequent / monthly / short-history member-cell counts (`div_month_pred`, domain [0, 1]); `short_window_member_cells`
and `vol_line_security_id` (`beta_dvol_21`); `short_history_member_cells` (`season_y2_5`).

### 4b. Registration (every constant, fixed blind)

| constant | value | basis |
|---|---|---|
| `LAG_SESSIONS` | 1 | the house t-1 clock (F-1) |
| `DIV_YIELD_MIN` | 1e-4 | mechanical: above the vendor factor's print precision (cumulative ratios agree with 1/returnFactor to 1.3e-7, TH3 audit) and below any regular payment |
| `DIV_YIELD_MAX` | 0.04 | mechanical: a regular quarterly payment above 4% of price is over 16% a year; specials, spin-offs and stock dividends of 5% or more (1 - 1/1.05 = 4.76%) fall outside |
| `DIV_PRED_MONTHS` | 3, 6, 9, 12 | Hartzmark-Solomon (2013), as recalled (the lag set of the CZ `DivSeason` replication); root verifies against the paper |
| `DIV_PAYER_MONTHS`, `DIV_MIN_PAID`, `DIV_MONTHLY_MAX` | 12, 3, 6 | mechanical: the lag set predicts a quarterly cycle; a quarterly payer shows 4 paid months a year, 3 when one is skipped or moved across a window edge, at most two extras; annual and semiannual payers (1-2) would be flagged in months they never pay, monthly payers (7+) have no off month |
| `VOL_LINE_TICKER`, `VOL_COLUMN` | SPY, `atmCenI_21d` | AHXZ use VXO (S&P 100, 30-day ATM implied volatility); SPY's 30-day (21 trading days) ATM IV is the in-house analogue; atx-db's `reference/vix_daily` (Cboe VIX, a variance-swap index) is the alternative |
| `DVOL_WINDOW` | 21 | AHXZ: daily returns within one month |
| `DVOL_MIN_DAYS` | 17 | declared: about 80% of the window, the house ratio of F-1's 48 of 60; AHXZ's own minimum not verified |
| `DVOL_DET_TOL` | 1e-10 | numerical guard only (collinear regressors on a line's days) |
| `SEASON_YEARS` | 2, 3, 4, 5 | Heston-Sadka (2008); the CZ `MomSeason` "years 2 to 5" form |
| `SEASON_YEAR_SESSIONS`, `SEASON_WINDOW` | 252, 21 | the alignment of the roster member `seasonality_same_month` (delay 252 / 231) |
| `SEASON_MIN_YEARS` | 3 | declared: a majority of the four windows (one halt or listing gap does not drop the name) |

Candidate signals the fields serve (suggested frozen strings for the PM's registration; not registered here):

| candidate | DSL | theme (suggested) | prior | budget |
|---|---|---|---|---|
| `div_season` | `rank(div_month_pred)` | reversal_seasonality (a calendar effect beside `seasonality_same_month`; avoids a one-member theme) | +1, Hartzmark-Solomon (2013, JFE) | 1 field, lookback 0 |
| `vol_beta` | `rank(decay_linear((-1 * beta_dvol_21), 21))` | low_risk (or a new `macro_vol_risk`) | +1 on the negated beta, Ang-Hodrick-Xing-Zhang (2006, JF) | 1 field, lookback 20 |
| `season_y2_5` | `rank(season_y2_5)` | reversal_seasonality | +1, Heston-Sadka (2008, JFE); Keloharju-Linnainmaa-Nyberg (2016, JF) | 1 field, lookback 0 |

Each needs a registry fields-table row (formula id, clock and basis copied from the built manifest) before `add-alpha`.

### 4c. Tests (`test_research_fields_xdata.py`, synthetic world 2012-06..2019-05, role 2018-07-02..2019-04-30)

- `test_values_match_definitions`: all three fields cell by cell (every third row, every line) against independent
  definitions written in the test (a numpy `lstsq` for the beta), at least 150 finite cells each; the payer rules
  (monthly 33, annual 44 and non-payer 55 all NaN; the initiation 66 NaN until its third payment, 0 the month after,
  1 three months later; quarterly payers always defined); the 6% special, the sub-1-bp step and the no-raw-drop jump
  are not ex-dates; SPY's duplicate session quarantined, two out-of-domain IV prints and one missing print counted;
  sealed-row counts; entry extras (`producer`, `formula_sha256`, `lag_sessions`, `imported_code`,
  `session_calendar`).
- `test_point_in_time_probe`: every vendor row (all lines, SPY and QQQ, close, factor, volume, IV) dated on or after
  role session 120 is moved at random; rows 0..120 of `div_month_pred` and `beta_dvol_21` stay bit-identical and later
  rows move. `season_y2_5` reads nothing after t-483, so its probe moves rows from the first session row 120 does not
  read: rows 0..120 identical, later rows move.
- `test_probe_fails_on_leaky_variants`: the probe has teeth. With `LAG_SESSIONS` 0 (`beta_dvol_21`: the same-session
  window), or a 21-session lead plus the current month (`div_month_pred`) or the current year's window
  (`season_y2_5`), rows at or before the cut move, so the probe fails; the module as written passes the same probe.
- `test_seal_and_refusals`: no `--price-source` (refused before any output); the builder's seal differing from
  research_window's (`SealError`, no manifest); a source without `atmCenI_21d` (refused before output for
  `beta_dvol_21`, the other two still build); `SPY` on two securityIDs (refused, no manifest); the CLI equals the API.
- `test_repository_window_fresh_interpreter`: a fresh interpreter (no test harness) under the repository window
  (seal 2024-01-01 from `research_window.json`) drops the synthetic 2024 rows as sealed (SPY 2 rows, panel 4) and
  writes byte-identical payloads; its manifest seal is the repository seal.
- `test_reuse`: self reuse copies all three byte for byte with entries verbatim and `reused_from.inputs` =
  {`session_calendar`, `imported_code`}; a mixed prior reuses one and computes two; a changed `imported_code`
  recomputes all three with identical bytes; an edit of `source_panel`'s signature moves `imported_code`.
- `test_registration_is_opt_in`: the plain builder lacks the three names and the module; registered, they follow
  every existing field; `register` is idempotent; unregistered again on exit.

Results: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_research_fields_xdata.py`
-> 7 passed (19 s). Whole `atx-engine/tools` directory -> 260 passed, 6 subtests passed (253 before + 7).

### 4d. How root verifies

1. The tests above, from `atx-engine/tools`.
2. Identity: `git diff --stat 3c6ae225 HEAD -- atx-engine/tools` lists only the three new files. The plain builder
   never imports the module (`test_registration_is_opt_in`), so every v8 field list, manifest byte, producer
   fingerprint and reuse count (v10 63 / 7, v11 70 / 3, v12 73 / 1) is unchanged by construction; flag absent =
   the module not registered.
3. Build (after V8-F and integration 8; not run by this lane), from whichever lo3 fields directory is current
   (`$FBASE`, its pin `$FB`), with that build's full argv (as the FIELDS-V9 report section 3 shows for v12) and the
   entry swapped:
   ```bash
   FX=$("$PY" -c "import json,sys;n=[f['name'] for f in json.load(open(sys.argv[1],encoding='utf-8'))['fields']];print(','.join(n+['div_month_pred','beta_dvol_21','season_y2_5']))" $FBASE/manifest.json)
   "$PY" scripts/run_bounded_research.py --seconds 900 --max-rss-mib 2560 --min-free-mib 512 \
     --output build-equity/train-2020-2023-lo3-fields-x-run \
     --bind atx-engine/tools/prepare_research_fields_xdata.py --bind atx-engine/tools/research_fields_xdata.py \
     --bind atx-engine/tools/prepare_research_fields.py --bind atx-engine/tools/research_fields_price.py \
     --bind atx-engine/tools/research_fields_sec.py --bind build-equity/train-2020-2023-lo3/manifest.json \
     --bind $FBASE/manifest.json -- \
     "$PY" atx-engine/tools/prepare_research_fields_xdata.py <the base build's argv with --output
       build-equity/train-2020-2023-lo3-fields-x --fields $FX --reuse $FBASE --reuse-sha256 $FB --reuse-hardlink
       --price-source C:/Users/natha/Downloads/TickerHistory3.parquet --max-rss-mib 2048 --max-seconds 880>
   ```
   Expected: `reuse.reused` = the base names; `reuse.computed` = the three new names; `seal.exclusive_end`
   2024-01-01; `source_checks.xdata.vol_line.security_id` is SPY's vendor line; sealed-row counts are counts only.
   Memory [est] for the 4-year role (about 1,400 sessions x 5,627 lines, 1,260 sessions of history): panel about
   210 MB, dividend ledger about 75 MB, beta returns about 65 MB, so about 0.4 GiB above the builder. Time [est]: the
   TickerHistory3 hash (about 20 s), the SPY scan of `ticker_tk` over 32 M rows (one to two minutes), the panel scan
   (about a minute), the three producers (seconds).

### 4e. Deviations, with reasons

1. Draft registration entry, not a builder hook (the FIELDS-V9 precedent): the plain builder and every v8 build stay
   byte-identical. The entry duplicates the FIELDS-V9 entry's 20 lines because that file is not on this base; fold
   at integration 8.
2. The field spec entries live in the module's `FIELDS` (definition, units, clock, staleness, caveats, formula id,
   min history), as every field module; there is no separate spec document.
3. Dividends come from the vendor's chained factor, not a dividend file (none in house): a cash distribution inside
   the band counts whatever its kind; the jump-cell rule removes factor steps with no raw drop of more than 1%.
4. The dividend field's cross-section is quarterly payers (a registered choice: the 3 / 6 / 9 / 12 lag set flags an
   annual payer in four months a year).
5. SPY's ATM IV stands in for VXO; SPY's adjusted return for the CRSP value-weighted market.

Cross-lane edits: none. Open risks: section 7.

## 5. External datasets not in house (owner decisions)

Prices are as published by the vendor at search time (2026-10-02) or marked unknown; none was negotiated. Commercial
terms for a proprietary strategy can differ from list prices.

| # | dataset | cost | history | point-in-time quality | signal class it opens |
|---|---|---|---|---|---|
| 1 | ORATS historical options (SMV strike and delta-bucket "monies" files, near end of day) | list: $399 one time for 2007-present plus $399 a year for daily updates (ORATS pages); confirm commercial-use terms | 2007 on, 5,000+ symbols | a dated near-close snapshot of each session, no revision; usable at t+1 under the house clock | put-wing skew (Xing, Zhang and Zhao 2010, JFQA), call-put IV spread (Cremers and Weinbaum 2010, JFQA), option-to-stock volume (Johnson and So 2012, JFE), open interest. Closes the put-wing IV ask (`iv_skew`). Alternative: SpiderRock OptionEODFeaturesHist + SurfaceFixedGridHist (the house vendor, atx-db D3; price unknown) |
| 2 | S&P Global Market Intelligence Securities Finance (ex IHS Markit) | enterprise licence, price not public (unknown) | 2002 on (daily) | daily vendor snapshots; ask for as-delivered files (backfilled history is a vintage risk) | borrow cost and utilization (Drechsler and Drechsler 2014, "The shorting premium"; Engelberg, Reed and Ringgenberg 2018, JF "Short-selling risk"); also a real borrow-fee input for the cost model and capacity (atx-db holds only a proxy) |
| 3 | Zacks earnings estimates (broker-level detail with estimate dates; daily consensus) | commercial feed or API, price not public (unknown); WRDS for academic use | quarterly EPS from 1982, annual from 1978 | estimate and observation dates allow an as-of view; use the detail file (the consensus is NRI-adjusted and drops estimates older than new guidance) | analyst revisions (Chan, Jegadeesh and Lakonishok 1996), surprise against consensus, dispersion (Diether, Malloy and Scherbina 2002), recommendation changes: the largest family the book lacks. Alternatives: LSEG I/B/E/S, FactSet |
| 4 | USPTO PatentsView bulk tables (granted patents, disambiguated assignees) plus the Kogan-Papanikolaou-Seru-Stoffman patent-CRSP match | free (CC BY 4.0; KPSS public) | grants from 1976 | a grant is public on its weekly issue date; assignee disambiguation is a later vintage; assignee -> CIK matching is ours to build (KPSS maps to CRSP permco) | innovative efficiency (Hirshleifer, Hsu and Li 2013, JFE), technological-link momentum (Lee, Sun, Wang and Zhang 2019, JFE), patent value. No theme reads innovation |
| 5 | Hoberg-Phillips Data Library, TNIC-3 | free | 1989-2023 (library page) | yearly peer sets from 10-K text; usable from the later 10-K's filing date; the library is rebuilt by its authors (vintage risk); keyed by gvkey (a CIK link is needed) | text-network peers for `tnic_mom` / peer momentum without finishing the TXT landing (Lazy Prices still needs it) |

Sources: [ORATS near-EOD data](https://orats.com/near-eod-data),
[ORATS historical quotes](https://orats.com/blog/historical-options-quotes-and-greeks),
[ORATS skew FAQ](https://orats.com/blog/answers-to-common-questions-calculating-skew),
[S&P Global Securities Finance dataset](https://www.marketplace.spglobal.com/en/datasets/securities-finance-short-interest-data-(1704372663)),
[Zacks historical data](https://www.zackspro.com/historical.asp),
[Zacks one sheet (WRDS)](https://wrds-www.wharton.upenn.edu/documents/1174/Zacks-One-Sheet.pdf),
[PatentsView download tables](https://patentsview.org/download/data-download-tables),
[KPSS patent-CRSP match](https://paper.dropbox.com/doc/Patent-CRSP-match-1926-2017%2D%2DAsy3186MyI9pMxoGjwjgzGkDAg-W3aHAj0Ce4CzKZayqCASj),
[Hoberg-Phillips Data Library](https://hobergphillips.tuck.dartmouth.edu/),
[Dimensional on lending fees](https://www.dimensional.com/us-en/insights/securities-lending-fees-as-a-short-term-driver-of-stock-returns).

## 6. Hygiene

- **Read in the worktree:** lane rules; task-X briefs (rules, XDATA); status 5 and 6; v8-prereg; progress.md (head,
  PM session 7, entries on OD-6, PM5-13, LIB2 / E-42); library-v8-draft sections 0, 1, 3-10; task-F-0..F-3 reports;
  the v8.0 library (DSL strings, themes); `research_fields_price.py`, `code_fingerprint.py`, `research_window.py`,
  `conftest.py`, the builder's run / Role / FieldWriter / factor_breaks / reuse code; the TH3 input audit.
- **Read through git objects:** `834d5a05` (FIELDS-V9 report, `research_fields_v9.py`, the draft entry); `main`
  `179af41c`: atx-db docs TIER1_V3_STATUS, ALPHA_PANEL_STATUS, ALPHA_PANEL (keys, panel, characteristics),
  ALPHA_PANEL_SEC (sources, clocks, stage schemas), ALPHA_PANEL_OWNERSHIP, ALPHA_PANEL_TEXT, ALPHA_PANEL_NOTES,
  ALPHA_PANEL_MARKET, ALPHA_PANEL_FUNDAMENTALS (progress, v10 rules), ALPHA_PANEL_SHORTFLOW, LICENSED_ADAPTERS (the
  options part); atx-db source heads (`ticker_history.py` column list, `fund_catalog.py` item names, the six
  warehouse-v2 modules the brief names); `git log main -- atx-db`.
- **Read in pool 2 (field lists and manifests only):** `train-2020-2023-lo3-fields-v10/manifest.json` (field names,
  sources, clocks, staleness, excluded columns, member-cell coverage fractions: data presence over the 2018-2023 role).
- **Not read:** any return, IC, Sharpe, turnover or NAV output; any data file; the atx-db working tree; atx-db docs
  that may hold signal statistics (ALPHA_PANEL_METRICS.md, `docs/research/*`, FUNDAMENTAL_SIGNAL_*); the untracked
  gold-panel files; `ORATS_SMV_Strikes_20240103.zip` (dated after the seal).
- **Disclosure:** LICENSED_ADAPTERS.md (options coverage per year 2018-2026) and ALPHA_PANEL_SEC.md (delisting causes
  per year 2018-2026) printed per-year counts that include 2024-2026: data-presence and filing counts, no return or
  signal statistic; nothing in this report uses them.
- **Web search:** item 5 only (ORATS, securities lending, Zacks, PatentsView / KPSS, Hoberg-Phillips).

## 7. Open risks

1. Two definitions rest on recall, not on a re-read of the paper: Hartzmark-Solomon's lag set (3, 6, 9, 12) and
   AHXZ's minimum days (17 is declared, not quoted). The quarterly-payer restriction is this lane's registration.
   Root (or XPRE) verifies before any registration; a mismatch is a blind fix before the build.
2. `div_month_pred` flips monthly: high turnover by construction; its cell must clear the cost model. The ledger is
   the vendor factor: specials, small spin-offs and returns of capital inside 1 bp-4% count, and a factor added to
   the history later would look known at its ex-date (no vintage proof).
3. `beta_dvol_21` is a 21-observation, 3-parameter estimate: noisy; the suggested form decays it over 21 sessions.
   SPY's ATM IV is not VXO.
4. `season_y2_5` shares the reversal_seasonality theme with `seasonality_same_month`; its orthogonality is by horizon
   only.
5. Coverage on real data is unknown (nothing run): the share of members that are quarterly payers, the SPY scan time
   (32 M ticker strings) and the memory are estimates.
6. The registration entry duplicates FIELDS-V9's (fold at integration 8). None of the three candidates is registered;
   their admission trials come out of the X budget XPRE proposes.
7. Gold alpha panels (another session, uncommitted): their file names point at holdout evaluation; a PM ruling should
   require proof of no label / IC column and no 2024+ row before any X consumer binds one.

## GOLD (task GOLD, Ruling PM7-19)

Read under the ruling, read only, in place: `C:/atx/atx-db/docs/ALPHA_PANEL_GOLD.md`, `ALPHA_PANEL_VALIDATION.md`;
the source of `src/atx_db/alpha_panel/gold.py`, `char_registry.py` (the 100-feature registry, loaded by `exec` of its
text with bytecode writing off), parts of `characteristics.py`, `common.py`, `export_impl.py` and
`scripts/build_gold_alpha_panels.py`; directory listings of the build root `C:/atx/atx-db/data/alpha_panel/v1`;
`validation/gold_pipeline/authorized_plan.json` and `implementation_handoff.json` (keys and status values only, with
IC-like keys filtered out of the print); `panel_goldready/refresh_progress.json` (keys and status); the parquet
schema of one silver file (`panel_goldready/year=2021/panel-01.parquet`: names, types, row and row-group counts). No
row of any atx-db file was read. Not opened: `labels/`, `labels_goldready/`, `labels_holdout/`,
`validation/ic_v2_smoke.json`, `validation/fundamentals.json`, any IC / OOS / scorecard file, `holdout_eval.py`,
`labels_holdout.py`, `ic_report.py`. Nothing was written under `atx-db/` or in `C:/atx` (the existing
`char_registry` bytecode cache is dated 2026-09-30 and was not rewritten).

### G1. What the panels hold

**State: the gold panels do not exist yet.** The build root holds no `gold/` and no `characteristics/` stage.
`validation/gold_pipeline/` holds only the dry-run plan (`dry_run: true`, `outcome_access_executed: false`) and the
implementation handoff (`status: tooling-tested-source-build-active-production-artifacts-pending`,
`real_oos_measurement_executed: false`; remaining: finish the silver refresh, build characteristics 2019-2026, then
labels, IC, gold, freeze). The silver input `panel_goldready/` is mid-build: `refresh_progress.json` says `status:
building` (rule `pit-silver-cache-refresh-v1`, 47 months done, 2018-01 .. 2021-11), year directories 2018-2021 only,
no `manifest.json`. Nothing can be bound today; everything below is the contract read from docs and source.

**Stages** (build root `atx-db/data/alpha_panel/v1`; all keyed `(session_date, security_id)`, `security_id` =
TickerHistory3 `securityID`; year partitions):

| stage | file | content | window |
|---|---|---|---|
| `panel_goldready/` (silver) | `year=YYYY/panel-MM.parquet` | 191 columns (schema read): prices (OHLC, ret, volume, dollar volume), ATM IV at 5/10/21/42/63/126/252 d, fundamentals v10 items (incl. `xsga_ttm`, `cogs_ttm`, `invt`, `rect`, `ppe`, `gdwl`, `intan`, `drev`, `ebitda_*`, `xint_*`, `dvt_*`, `act`, `lct`, `ap`, `buyback_authorized`, `buyback_remaining`, F-score parts), SIC / FF groups, short interest and short volume, FTD, Reg SHO, 13F (`inst_*` incl. `inst_top10_share`), earnings calendar (`earn_next_expected_date`, `earn_day_offset`), insider last trade, security-type flags, identity (link table v3 tiers), `member`, `member_equity`, `me_company`, vendor audit columns | 2018 on (2012-2017 in `prices_history/`) |
| `characteristics/` | `year=YYYY/characteristics.parquet` | the 100 registry features, raw and prior-signed (90 signals, 10 controls), with `member`, `member_equity`, groups and repair flags | 2019-2026 |
| `gold/` | `year=YYYY/gold.parquet` | `cik`, `grp_ff49`, `grp_ff12`; `g_<feature>`: the same-session member_equity score (1/99 winsorised, average-tie rank, inverse normal, centred, unit SD; FF49-demeaned in the industry variant) of each ADMITTED feature; `c_<family>` (>= 2 admitted, equal weight); `c_all` (>= 3 families); `ctl_<control>` raw controls | 2020-2026 |
| `labels*/`, `validation/gold_pipeline/*ic*`, `frozen_oos_*` | forward returns, IC and OOS evidence | closed to this lane |

**Universe.** `member` = the scorecard universe (top 3,000 by prior 63-session dollar volume, ADV > $5M, raw price
> $5, one-session lag); `member_equity` = member AND operating AND not an index line (operating from SEC periodic
filings and security type, not the vendor earnFlag). Gold keeps member_equity rows, drops rows whose identity was
repaired at the decision, and leaves scores null through a 1,260-session repair quarantine. The v8 store's universe
is the role's (`linked-operating-v3`); the sets overlap but differ.

**Admission (what decides which `g_*` exist; `gold.select`).** Coverage >= 50% on average and >= 30% in each of
2019-2021; for some horizon h in {5, 21, 63} and variant (raw / industry): prior-signed purged SELECTION IC > 0 with
Newey-West t >= 2.5 over 2020-2021 decisions whose labels end before 2022, and a positive 2019 discovery IC; then a
walk by decreasing t rejecting |pair correlation| >= .90. The frozen recipe records horizon, variant and weights. The
owner authorized a later frozen evaluation on 2024-2026 (not run).

**Stamping by column family** (`char_registry.py`, `characteristics.py`, `gold.py`):

| family | rule |
|---|---|
| price, return, volume, liquidity, risk, momentum, reversal, seasonality | rows of the same line with `session_date <= d` (closes known at d's 22:00 UTC mark); `lint_sql` refuses `lead(`, `FOLLOWING`, negative lags and unordered whole-history windows; `characteristics.pit_check` tests the property on data |
| options (`iv_*`) | the line's previous-session IV (lag 1; the vendor delivers at 22:00 Chicago) |
| fundamentals | panel fundamentals with clock < 22:00 UTC of d-1 |
| short side, ownership, insider, events | the panel's as-of rules, `available_at < 22:00 UTC of d-1` per source (short volume trade date d-1; 13F after its 45-day deadline) |
| event time (`ea_window_ahead_5`, `earn_days_since`) | expected date = +364 days of a visible SEC 8-K 2.02 primary announcement; reaction session known at its mark |
| gold `g_*`, `c_*` | the same-session member_equity cross section (normalisation and FF49 demeaning use session d only) |

**Point in time: provable or not.**
- Provable from source (cell values): every `characteristics` column and every gold `ctl_*` control, under the same
  source-vintage caveats the v8 store carries (vendor prices, IV and identity have no vintage proof; `shares_out` is
  the lag-90 vendor proxy; borrow inputs are proxies).
- Values provable, selection not admissible: gold `g_*` and `c_*`. Which features exist, and their horizon and
  variant, were chosen with 2019-2021 forward returns, inside this program's TRAIN window; for 2020-2021 rows the
  selection saw those rows' own outcomes. Binding one imports a mined screen (90 signals x 3 horizons x 2 variants)
  that no v8 trial count records. **Owner / PM question:** refuse them, or count the screen in deflation.
- Not point in time, never to bind: the silver panel's vendor audit columns (`earn_flag_vendor` and
  `earn_recent_vendor`, whose earnFlag presumes future dates; `is_operating_vendor`; `delisting_date`, a future event
  on earlier rows; `directory_type` / `directory_etf`, a snapshot; `gics`, vendor current) and the legacy `panel/`
  stage (snapshot identity backfills; the gold doc forbids it for the new release).
- Not bindable now: `panel_goldready/` (no manifest, writer active), and so `characteristics/` and `gold/` (not
  built). **Owner questions:** when they publish; whether the frozen 2024-2026 evaluation runs before X measures
  anything (it would make 2024+ non-pristine, to the owner, for those features).

**How they differ from the v8 store.** A re-computation from sources v8 already binds (TickerHistory3, FINRA, SEC
stages, 13F, fundamentals), not a new source. New relative to the v8 field list: ATM IV at 5, 10, 42 and 252 days
(`iv_term_slope`), `inst_top10_share`, the fundamentals v10 items in the silver panel (SG&A, COGS, inventory,
receivables, goodwill, deferred revenue, interest, buyback authorizations), the Corwin-Schultz spread, SPY-market
coskewness. Every other registry feature is a v8 roster member, an X candidate, or a DSL expression of v8 fields.

### G2. Sealed reader `research_fields_gold.py`

New files `atx-engine/tools/research_fields_gold.py` and `test_research_fields_gold.py`; the draft entry
`prepare_research_fields_xdata.py` registers it too (`DRAFT_MODULES` = (xdata, gold), `FIELDS_GOLD_DRAFT`).

- **Flag-gated twice:** the plain builder never registers it; a gold field also needs `--gold-panel-root` and its stage
  pin (`--gold-sha256` / `--gold-characteristics-sha256`), else refused before any output.
- **Stages:** `gold` (`atx.alpha-panel.gold/v1`) and `characteristics` (`atx.alpha-panel.characteristics/v2`) only;
  the manifest must match its pin, its schema and `status: complete`; each file opened must match the manifest's bytes
  and SHA-256 and stay unchanged while read.
- **Refusals by name** (`refused_reason`, `declare`): any other stage (labels, labels_holdout, validation, panel); any
  column with a token `label(s)`, `fwd`, `forward`, `future`, `ic`, `oos`, `holdout`, `target`, `outcome`, `lead(s)`
  (optionally with digits); gold `g_*` / `c_*` (`GoldSelectionColumn`; `ALLOW_ADMITTED_SCORES = False` until a PM
  ruling). Only the two keys and the declared columns are read (column projection).
- **Seal (research_window):** builder seal must equal `research_window.SEAL`; partitions beginning on or after the
  seal are never opened; `session_date < SEAL` and the role's range are pushed down into `pq.read_table(filters=)`
  (row groups wholly past the seal skipped by statistics and counted from metadata); a result row on or after the
  seal raises `SealError` (`assert_sealed`).
- **Clock:** `gold-panel-lag1-v1`: row t = the panel value of the previous session (the house t-1 clock on top of
  atx-db's own stamping).
- **Reuse fingerprint:** `PRODUCERS = {"gold_cols": (stage_manifest, stage_partitions, read_stage_columns,
  previous_sessions, gold_rows)}` with the builder closure through `h`; `reuse_inputs` = {stage, its manifest SHA-256,
  the NYSE rule calendar digest}.
- **Field spec entries:** `gp_hl_spread_21` (gold `ctl_hl_spread_21`: Corwin-Schultz spread, a cost and capacity
  input shipped regardless of IC; formula `gold-ctl-hl-spread-21-lag1-v1`) and `gp_iv_term_slope` (characteristics
  `iv_term_slope`: the 252-day tenor the v8 store lacks; `gold-char-iv-term-slope-lag1-v1`). Neither is a candidate
  below; they are the reader's declared surface, buildable the day the stages publish.

Tests (`test_research_fields_gold.py`, 7, a synthetic build root):
- values equal an independent previous-session lookup; duplicates quarantined; an off-role line ignored; the seal-year
  garbage partition never opened; the sealed row group pruned (3 rows, from statistics); pins recorded;
- the look-ahead probe: values dated on or after role session 100 moved, rows 0..100 bit-identical, later rows move;
  with `LAG_SESSIONS` 0 (the same-session clock) row 100 moves, so the probe fails on that leak;
- label columns refused by name (`fwd_ret_21`, `label_h21`, `ic_mean`, `holdout_ic`, `ret_forward_5`, `oos_score`,
  `target`, `lead_ret`), non-feature stages refused, `g_*` / `c_*` refused; a labels-schema manifest, an unfinished
  stage, a wrong pin, a missing root or pin refused with no output directory; a stage file differing from its
  manifest refused with nothing published;
- sealed rows: a read reaching past the seal returns no row on or after it; `assert_sealed` refuses a sealed row; the
  builder seal differing from research_window's raises `SealError`;
- a fresh interpreter under the repository window (seal 2024-01-01): the 2024 partition never opened, a 2024 row group
  pruned, payloads byte-identical;
- reuse (self copy; a republished gold manifest recomputes only its field); opt-in registration.

Results: `test_research_fields_gold.py` + `test_research_fields_xdata.py` 14 passed; `atx-engine/tools` 267 passed, 6
subtests passed.

Root verifies: the tests; `git diff --stat` shows the gold module, its test, the draft entry's extension and the
xdata registration test's expected tail. Build, once `gold/` and `characteristics/` publish:
`prepare_research_fields_xdata.py <base argv> --fields <base>,gp_hl_spread_21,gp_iv_term_slope --gold-panel-root
C:/atx/atx-db/data/alpha_panel/v1 --gold-sha256 <sha of gold/manifest.json> --gold-characteristics-sha256 <sha of
characteristics/manifest.json>`; expected `source_checks.gold.stages.*.sealed_partitions_never_opened` >= 3
(2024-2026) and `partitions_read` only years before 2024.

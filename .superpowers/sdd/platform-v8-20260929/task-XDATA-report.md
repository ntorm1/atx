# Task XDATA report: datasets (expansion X, lane XDATA)

Lane XDATA, pool 14, branch `feat/platform-v8-xdata-20261002`, base `3c6ae225`. Blind: no return, IC, Sharpe, turnover
or NAV output of 2020-2023 was opened; nothing dated 2024-01-01 or later was opened as data. Warehouse documents,
schemas and source were read through git objects of local `main` (`179af41c`) only; no atx-db data file and no atx-db
working tree was opened or written. Python only, synthetic tests only, nothing built, nothing run on data.

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

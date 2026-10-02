# Task YDATA report: vwap, three more field families, new return sources, the top in-house reader

Lane YDATA, pool 13, branch `feat/platform-v8-ydata-20261002`, base `798d3b23`. Blind: no return, IC, Sharpe, turnover
or NAV output of any window was opened; nothing dated 2024-01-01 or later was opened as data. Python only, synthetic
tests only, nothing built, nothing run on real data, no subagent. Owner directive of 2026-10-02 applied: new datasets,
Sharpe and gross return first, capacity second.

| item | status | commit |
|---|---|---|
| 1. vwap: finding and data ask (no in-house source; nothing built as `vwap`) | DONE | `60c918d1` |
| 2. three field families (builders, specs, synthetic tests, one frozen candidate each) | DONE (`iv_skew_21`, `stio_chg_q`, `div_init_omit`) | `33ef2bf1`, `e33e237e`, `5a79dc45` |
| 3. re-ranked list of new return sources (6) | DONE (section 3) | this commit |
| 4. reader for the top in-house item (schema fixed in a committed doc) | DONE (`deal_pending`) | `dc855b79` |

Tests: `test_research_fields_ivshape.py test_research_fields_mgr13f.py test_research_fields_divevent.py
test_research_fields_deals.py` -> 29 passed (from `atx-engine/tools`); whole `atx-engine/tools` directory: 329 passed,
6 subtests passed.

## 1. vwap

**Finding (two lines).** No point-in-time daily VWAP and no input for one (traded dollars, trade-level or minute bars)
exists in the engine store or the warehouse; the only "vwap" anywhere is a proxy (the engine's `--vwap-rule`: raw close
or (high + low + close) / 3) or a schema column every loader fills with NULL.

| place | evidence |
|---|---|
| TickerHistory3 (`C:/Users/natha/Downloads/TickerHistory3.parquet`, footer read only: 71 columns) | `open, high, low, close, closePr, volume, shares, ...`; no VWAP, average price, dollar volume or trade count. The SpiderRock TickerHistory3 dictionary agrees; `rvVar` is "reserved for future use" |
| engine store (pool 2, manifests only) | role `train-2020-2023-lo3`: `close, raw_close, volume, present, member`; fields v13 (75 names): none is a price-volume aggregate. `w0-vwap-closure/` is a build/test receipt directory of the VwapRule work, not data |
| engine code | atx-engine `alpha::VwapRule` / atx-impl `--vwap-rule raw-daily-close-v2 | adjusted-typical-v1`; atx-impl `dispatch.cpp`: "VWAP is a daily price proxy, not an intraday observation" |
| warehouse, `origin/main` (= `feat/tier1-v3-warehouse` + 50 commits; the branch has nothing main lacks) | `equity_daily_bars.vwap DOUBLE` in `schema.py` / `schema_contract.py` / `api/catalog.py` / DATA_DICTIONARY; written NULL by `ticker_history.py:434`, `pricing_bulk.py:268`, `ticker_history_incremental.py:815`. ALPHA_PANEL_IDENTITY_SECURITY.md D9: "Not available: VWAP and trade counts; the vendor file has neither"; ALPHA_PANEL_STATUS.md: "Not in any source: ... VWAP, trade count". Panel `dollar_volume` = raw close x volume |
| Databento | in house: atx-core `load_equs_summary_zip` and `python/scripts/extract_databento_equs_ohlcv_1d.py` (EQUS.SUMMARY `ohlcv-1d`: OHLCV only), `pull_equity_l1_1m_to_parquet` (`bbo-1m` quotes), atx-vol OPRA pulls. None carries traded value. `atx-db/.../alpha_panel/databento_tail.py` is untracked in another session's tree (`C:/atx`), in no commit: not opened; by name and the gold docs a price tail after TickerHistory3's end (2026-06-15), i.e. after the seal |

So: source none; column none; adjustment, coverage and session clock not applicable. A proxy would not be the paper's
input (Kakushadze 2016 uses the day's VWAP), so **nothing is built under the name `vwap`**; XWQ's 43 V formulas stay out.
**Data ask:** `docs/plans/2026-10-02-v8y-vwap-data-ask.md` (the exact stage schema: `daily_vwap/`,
`atx.alpha-panel.daily-vwap/v1`, per (session_date, security_id): `vwap_rth_raw`, `dollar_volume_rth_raw`, `volume_rth`,
`trade_count_rth`, optional all-hours columns, `available_at`, rule ids; regular hours 09:30-16:00 ET consolidated
volume-eligible trades; coverage from 2017-06-01 at the latest; consumer seal; quality checks), and the engine-side
`vwap_adj` design once it publishes (stamped as `open_adj`: x close / raw close of the same row, lag 0 only if
`available_at` < 22:00 UTC is proved for every row).

## 2. Three field families (no atx-db change)

**Which families.** Of XDATA's ranked 8, five were not built: rank 1 (filing text), 2 (N-PORT), 6 (tax / pension notes)
and 8 (TNIC) need an atx-db change (landing, build or publication) or an external library; only rank 7 (13F manager
structure) has its data in house. XDATA did not build it because Anton-Polk needs fund types; the short-term-institution
form of rank 7 (Yan and Zhang 2009, XDATA's own example) classifies managers by churn computed from their 13F books, so
it needs nothing else: built (`stio_chg_q`). The other two are the best in-house, unregistered families XDATA left open:
the TickerHistory3 smile slope (XDATA's open question on `wkD1 / shD1 / qtrD1 / lnD1`, now answered by the vendor
dictionary: "Interpolated 21 day atm vol slope", observed at the session) and dividend initiations / omissions on
XDATA's own ex-date ledger. Deviation from the brief (2 of 3 not from the ranked list), forced by it.

Withdrawn before any read: a vol-of-vol field (`iv_vov_21`, Baltussen-van Bekkum-van der Grient 2018) was written and
removed in `5a79dc45`: that hypothesis is the R-12 member `iv_vol_of_vol` (LIB2), allocated and excluded.

### 2a. `iv_skew_21` (research_fields_ivshape.py; the option smile slope)

- **Definition** (`xzz-yan-skew-shd1-spy-oriented-v1`): S = the vendor `shD1` of the line's session t-1 row (valid: a
  unique key, `atmCenI_21d` in IV_DOMAIN [0.02, 5], 0 < |S| <= 5); o_t = sign of the median of SPY's valid S over
  t-21..t-1 (at least 17); value = o_t x S. SpiderRock's slope is the volatility difference between the curve points at
  standardized moneyness -0.5 and +0.5 (ln(K/F) / (sigma sqrt T)); its sign convention is not documented, so the index's
  put-rich smile fixes it at run time: a positive value is a smile richer on the put side. Bit-identical under either
  vendor convention (tested).
- **Stamping / clock** `ivshape-lag1-v1`: the vendor delivers IV columns at 22:00 America/Chicago (by 04:00 UTC of the
  next day); row t reads sessions t-21..t-1 only. Seal from `research_window` (refusal; sealed rows skipped and counted).
- **Reuse:** no input pin beyond the role (the source must hash to the role's `source_sha256`; no imported field code).

### 2b. `stio_chg_q` (research_fields_mgr13f.py; short-term institutional trading)

- **Definition** (`yz-stio-chg-q-v1`) on the `thirteenf/` stage the engine already pins (schema
  `atx.alpha-panel.thirteenf/v1`): positions per (filer, security) of the effective filings by the deadline after the
  house row screens (`F13_ROWS_RULE`); churn CR (Gaspar-Massa-Matos 2005, the minimum of buys and sells at quarter-end
  13F prices over the mean of the two quarter books; Yan-Zhang use the minimum); a share-basis change (split) is detected
  from the holders (median continuing ratio outside [1/1.2, 1.2], offset by the price ratio within [1/1.5, 1.5], at
  least 5 holders) and restated; short-term set ST(q) = top tercile (average-tie rank > 2m/3) of CR averaged over
  q-3..q; value at anchor P = STIO(P, ST(P-1)) - STIO(P-1, ST(P-1)), STIO = their shares / `shares_out` at the quarter
  end (each quarter in its own share basis).
- **Clock:** `F13_CLOCK` (quarter visible at its last deadline filing + 46 h, read at t-1 22:00 UTC, stale 150 days).
  Value of P reads quarters P-5..P only.
- **Options:** its own `--mgr13f-stage DIR --mgr13f-stage-sha256 PIN` (the holdings wrapper consumes `--thirteenf`);
  requires `shares_out` (hence `si_shares`) in the same run. Reuse pins: stage manifest, seal, `imported_code` (closure
  of the names imported from research_fields_holdings.py).

### 2c. `div_init_omit` (research_fields_divevent.py; dividend initiations and omissions)

- **Definition** (`mtw-div-init-omit-v1`) on XDATA's ledger (`research_fields_xdata.div_events`: cash yields 1 bp-4%,
  no kept gap, not a jump cell): +1 in the 252 sessions after an initiation (an ex-date with none in the prior 504
  sessions while the line traded), -1 in the 252 sessions after an omission is detected (a quarterly payer, at least 3
  ex-dates in the year to its last one L, reaches L + 126 sessions without another), 0 otherwise; NaN with fewer than
  504 sessions of ledger. Lag 1 (ledger rows <= t-1).
- **Reuse pins:** the rule calendar (`session_calendar`) and `imported_code` (research_fields_price.py and
  research_fields_xdata.py names).

### 2d. Registration (every constant fixed blind)

| field | constant | value | basis |
|---|---|---|---|
| iv_skew_21 | `SKEW_COLUMN` | `shD1` | 21 trading days = 30 calendar days, the horizon of XZZ (2010) and Yan (2011) |
| | `LAG_SESSIONS` | 1 | the vendor IV delivery clock (XDATA's `beta_dvol_21` precedent) |
| | `ORIENT_TICKER`, `WINDOW`, `MIN_DAYS` | SPY, 21, 17 | the index smile is put-rich; one month; the house 80% ratio |
| | `SLOPE_ABS_MAX` | 5.0 | mechanical: the IV domain's upper bound |
| stio_chg_q | `CHURN_QUARTERS`, `ST_FRACTION` | 4, 2/3 | Yan-Zhang (2009): previous four quarters, top tercile |
| | churn | min(buys, sells) / mean book | Gaspar-Massa-Matos (2005); minimum per Yan-Zhang |
| | `BASIS_SHARE_TOL`, `BASIS_VALUE_TOL`, `BASIS_MIN_HOLDERS` | 1.2, 1.5, 5 | mechanical: a 6:5 split or more; value moved < 50%; enough holders for a median |
| | price screen, IO bound, staleness | [1/10, 10], 2, 150 d | the house 13F rules (`F13_*`) |
| div_init_omit | `INIT_GAP_SESSIONS` | 504 | two years without a payment (MTW 1995, as recalled; root verifies) |
| | `EVENT_SESSIONS` | 252 | MTW's one-year post-event window |
| | `OMIT_GAP_SESSIONS`, `PAYER_MIN` | 126, 3 | mechanical: two quarterly payments missed; XDATA's quarterly-payer rule |

### 2e. Candidate signals (frozen; suggested registration, not registered)

| id | DSL | DSL SHA-256 | theme | tier | sign | citation |
|---|---|---|---|---|---|---|
| `smile_slope` | `rank(decay_linear((-1 * iv_skew_21), 21))` | `063202690eb1f981d789a31206f080318d73bdd7477aff6fe261f1ad32159609` | options_implied | C+ | +1 (long flat smiles) | Xing, Zhang and Zhao (2010, JFQA); Yan (2011, JFE) |
| `stio_trade` | `rank(stio_chg_q)` | `e845ba078144883ba13eefd48bfce4d1016dbf35e889515d64b46b4f6ccc619e` | ownership_flow | C+ | +1 | Yan and Zhang (2009, RFS 22(2)); Gaspar, Massa and Matos (2005, JFE) |
| `div_event` | `rank(div_init_omit)` | `2dbe474ef4f26f2d0dd8e3a845a57bdfaae26728b5d58fa747a665ec1540bbe6` | filing_events (alternative: earnings_momentum) | C+ | +1 | Michaely, Thaler and Womack (1995, JF 50(2)) |

Orthogonality by construction: no roster theme reads the option smile (options_implied reads ATM levels and IV minus
realized vol); `inst_*` members read all-filer ownership levels, breadth and conviction, never the manager horizon;
no member reads dividend events (`net_payout` reads TTM payout levels, `div_season` the payment month). Fields table
rows (L2; root copies the clock and basis from the build of record):

```json
"iv_skew_21": {"formula_id": "xzz-yan-skew-shd1-spy-oriented-v1", "origin": "fields_ydata", "producer": "atx-engine/tools/research_fields_ivshape.py", "clock": "ivshape-lag1-v1 (sessions t-21..t-1 only; vendor IV delivered 22:00 America/Chicago)", "basis": "vendor shD1 of session t-1 x the sign of SPY's median shD1 over t-21..t-1"},
"stio_chg_q": {"formula_id": "yz-stio-chg-q-v1", "origin": "fields_ydata", "producer": "atx-engine/tools/research_fields_mgr13f.py", "clock": "13f-quarter-asof45-v1 (F13_CLOCK)", "basis": "change over the anchor 13F quarter of the shares_out fraction held by the previous quarter's top-tercile-churn filers"},
"div_init_omit": {"formula_id": "mtw-div-init-omit-v1", "origin": "fields_ydata", "producer": "atx-engine/tools/research_fields_divevent.py", "clock": "divevent-lag1-v1 (ledger rows <= t-1)", "basis": "+1 a year after an initiation, -1 a year after a detected omission, on the XDATA ex-date ledger"}
```

add-alpha argv (form of v8x-prereg section 14; `PY="C:/Program Files/Python312/python.exe"`; only `--parent`,
`--name`, `--parent-spec`, `--fields` filled by root):

```bash
"$PY" scripts/research_cycle.py add-alpha --id smile_slope --dsl "rank(decay_linear((-1 * iv_skew_21), 21))" --theme options_implied --tier C+ --prior-sign 1 --citation "Xing, Zhang and Zhao (2010, JFQA) What does the individual option volatility smirk tell us about future equity returns?; Yan (2011, JFE) Jump risk and the cross section of stock returns" --origin prior --prior-sign-source "Xing-Zhang-Zhao 2010; Yan 2011" --form "R(decay_linear(x, 21))" --formula "-iv_skew_21 (xzz-yan-skew-shd1-spy-oriented-v1), decayed 21: the vendor 21-day ATM volatility slope (TickerHistory3 shD1: the volatility difference across standardized moneyness -0.5 and +0.5) of session t-1, signed by SPY's median slope over t-21..t-1 so that a put-rich smile is positive; long flat smiles, short steep put skews" --domain "NaN without a valid session t-1 cell (unique key, atmCenI_21d in [0.02, 5], 0 < |slope| <= 5) or with fewer than 17 SPY slopes in t-21..t-1; values in [-5, 5]" --deviation "vendor fitted-curve slope at 21 trading days in standardized moneyness, not XZZ's OTM put minus ATM call IV nor Yan's delta -0.2 put minus delta 0.5 call (OptionMetrics); sign fixed by SPY's put-rich smile, not by the vendor's undocumented convention; lag 1 for the vendor IV clock; house 21-session decay" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id stio_trade --dsl "rank(stio_chg_q)" --theme ownership_flow --tier C+ --prior-sign 1 --citation "Yan and Zhang (2009, RFS 22(2)) Institutional investors and equity returns: are short-term institutions better informed?; Gaspar, Massa and Matos (2005, JFE) churn rate" --origin prior --prior-sign-source "Yan-Zhang 2009" --form "R(x)" --formula "stio_chg_q (yz-stio-chg-q-v1): change over the anchor 13F quarter P of the fraction of shares outstanding held by the filers classified short-term at P-1 (top tercile of the Gaspar-Massa-Matos minimum churn averaged over P-4..P-1, computed from their own 13F books); long names short-term institutions bought" --domain "NaN without a fresh visible 13F quarter with positions in the security (F13 clock, 150 days), without the previous calendar quarter, without a short-term classification (4 consecutive churn quarters, 3 classified filers), without shares_out, or with a level above 2" --deviation "13F filers of every type; churn on each filer's mapped book with a holder-detected share-basis change instead of CRSP adjustment factors; minimum churn at quarter-end 13F prices (median implied value / shares); shares outstanding = the house A8 90-day lagged vendor count; quarter visible after its 45-day deadline + 46 h, forward-filled until the next" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id div_event --dsl "rank(div_init_omit)" --theme filing_events --tier C+ --prior-sign 1 --citation "Michaely, Thaler and Womack (1995, JF 50(2)) Price reactions to dividend initiations and omissions: overreaction or drift?" --origin prior --prior-sign-source "Michaely-Thaler-Womack 1995" --form "R(x)" --formula "div_init_omit (mtw-div-init-omit-v1): +1 in the 252 sessions after a dividend initiation (an ex-date with none in the prior 504 sessions while trading), -1 in the 252 sessions after an omission is detected (a quarterly payer's last ex-date + 126 sessions), else 0; long initiators, short omitters" --domain "NaN with fewer than 504 sessions of the line's ledger; values -1, 0, 1 (mostly 0: ties at the middle rank)" --deviation "ex-dates from the vendor cumulative factor (XDATA ledger, cash yields 1 bp to 4%), not CRSP distribution codes or announcement dates; an omission is detected 126 sessions after the last payment, not at the board's announcement; the 504-session gap and 252-session window as recalled from MTW" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
```

## 3. New return sources not in the 12 themes, re-ranked (in-house data that only lacks a reader first)

The 12: value, profitability_quality, investment_issuance, earnings_momentum, price_momentum, low_risk,
short_interest, reversal_seasonality, options_implied, ownership_flow, filing_events, price_volume.

| rank | source and signal class | in the warehouse? (`origin/main` = `feat/tier1-v3-warehouse` + 50) | what the engine lacks (reader / export contract) | PIT lag | history | prior |
|---|---|---|---|---|---|---|
| 1 | **Merger-arbitrage spread**: long pending targets (Mitchell-Pulvino 2001 JF; Baker-Savasoglu 2002 JFE) | YES: `sec_filings/filings.parquet` (every EDGAR filing per CIK incl. PREM14A, DEFM14A, SC 14D9, 25-NSE, 15-12G; ALPHA_PANEL_SEC.md); already pinned by the engine's SEC module | only a reader: BUILT (section 4, `deal_pending`) | EDGAR acceptance, usable the next session | filings 2009 on | deal premium after the announcement jump; low volatility, limited breadth (pending targets) |
| 2 | **Common-ownership lead-lag**: returns of stocks sharing 13F owners predict each other across industries (Gao, Moulton and Ng, J. Financial Intermediation, "Institutional ownership and return predictability across economically unrelated stocks"; Anton-Polk 2014 JF with active funds) | YES: `thirteenf/` manager-level holdings (124 M rows, 2013q2 on), read by `research_fields_holdings` and `research_fields_mgr13f.quarter_positions` | a pairwise reader: per quarter, the stock-by-stock common-owner matrix (about 6k x 6k, dense under index funds) and the connected-portfolio return; the paper's construction was not read (abstract only: weekly horizon, about 19 bp a week 1980-2010), so not built blind | 13F clock (47-150 days) on holdings; daily returns | 2013q2 on | weekly horizon: turnover-heavy for the slow book |
| 3 | **Fund flow pressure**: flow-induced trading (Lou 2012 RFS), fire sales (Coval-Stafford 2007 JFE) | PARTLY: `nport.py` on main; schema committed (ALPHA_PANEL_OWNERSHIP.md S5.2: `fund_ownership.parquet` per (security_id, period_q) with `flow_induced_shares`, `available_at`; clock `nport-acceptance-v1`; stale 180 d); 16 of 27 quarters parsed, build not run | atx-db: parse 11 quarters, build, publish a manifest; engine: a stage reader (pin, `period_q` anchor, seal on `available_at`) | report date + 60 days | 2019q4 on (FIT from 2020q1: all of TRAIN) | price pressure +1 quarter, reversal later |
| 4 | **Filing-text change** (Lazy Prices, Cohen-Malloy-Nguyen 2020 JF) | PARTLY: TXT code on main (`filing_text`, `text_features`, `tnic`); 7% landed (2,286 of 32,411 filings) | atx-db: landing, `text_features`, a consumer export; engine: reader | acceptance + 1 session | 10-K / 10-Q 2009 on once landed | slow drift, high capacity |
| 5 | **Pension underfunding** (Franzoni-Marin 2006 JF) | PARTLY: FSDS notes landing 2019q1 on (`fund_notes.py`); warehouse-v2 companyfacts modules (`fundamental_statements.py`, `xbrl_catalog.py`) | atx-db: `fundamentals_notes/` build or a companyfacts export (`DefinedBenefitPlanFundedStatusOfPlan` with its acceptance clock); engine: issuer-route reader | filing acceptance | 2019 on (notes) | mispricing of pension liabilities |
| 6 | **Analyst revisions and consensus surprise** (Chan-Jegadeesh-Lakonishok 1996; Diether-Malloy-Scherbina 2002) | NO (atx-db: "Not in any source: ... consensus") | a purchase (Zacks / I/B/E/S / FactSet detail with estimate dates) and an export | estimate date | 1980s on | the largest family the book lacks |

Not ranked: `vwap` (price_volume theme; section 1), gold alpha panels (re-computations of sources already bound),
the Databento tail (after the seal), the smile slope (options_implied: section 2), borrow fees (short_interest theme).

## 4. The top in-house item: `deal_pending` (research_fields_deals.py)

The schema is fixed in a committed document (atx-db/docs/ALPHA_PANEL_SEC.md on `origin/main`, stage `sec_filings`,
`filings.parquet`: `cik, accession, form, filing_date, acceptance_utc, ..., is_amendment, ..., available_at`, one row
per CIK a filing is filed under) and the engine already binds the stage, so the reader is written (no contract
document needed):

- **Definition** (`sec-deal-target-pending-v1`): deal filings = original PREM14A, DEFM14A, PREM14C, DEFM14C, SC 14D9,
  SC14D9C under the CIK; a merger proxy whose CIK filed an original S-4 / S-4EF within the 365 days before it is the
  share-issuing acquirer's and is dropped; each deal filing opens [u, min(u_r, u + 252)), u its usable session
  (`SEC_CLOCK`), u_r the usable session of the CIK's first later 25 / 25-NSE / 15-12B / 15-12G / 15-15D / 15F-*; value 1
  while any window is open, else 0; NaN unless the primary link is a domestic periodic filer (the v9 `NT_PRESENT` rule).
- **Inputs and pins:** the SEC module's `--sec-stages`, `--sec-filings-sha256`, `--sec-identity-bridge(-sha256)` (no
  new option); reuse pins = stage manifest, bridge, `imported_code` (research_fields_sec.py and research_fields_v9.py
  names). Seal: refusal on a seal mismatch; rows available on or after `SEAL_NS` dropped and counted.
- **Constants:** 252 sessions (past a merger agreement's usual outside date; mechanical, as recalled) and 365 days
  (acquirer S-4 lookback); the 400-day presence rule is the v9 field's.
- **Candidate (suggested; a new theme is an owner / PM ruling, as XWQ-a):** `deal_target`, `rank(deal_pending)`
  (SHA-256 `815952591ce849a82926f3144c0714ee5eba0925d0d7c82971842202e71aaf05`), theme `merger_arbitrage` (new; L1 text:
  "Risk-arbitrage spread: targets of pending mergers and tender offers, from the target's own SEC merger filings until
  delisting or deregistration"), tier C+, sign +1, Mitchell and Pulvino (2001, JF); Baker and Savasoglu (2002, JFE).
  Argv (same form):

```bash
"$PY" scripts/research_cycle.py add-alpha --id deal_target --dsl "rank(deal_pending)" --theme merger_arbitrage --tier C+ --prior-sign 1 --citation "Mitchell and Pulvino (2001, JF) Characteristics of risk and return in risk arbitrage; Baker and Savasoglu (2002, JFE) Limited arbitrage in mergers and acquisitions" --origin prior --prior-sign-source "Mitchell-Pulvino 2001; Baker-Savasoglu 2002" --form "R(x)" --formula "deal_pending (sec-deal-target-pending-v1): 1 while the issuer is the target of a pending merger or tender offer (an original PREM14A, DEFM14A, PREM14C, DEFM14C, SC 14D9 or SC14D9C under its CIK, not an acquirer's proxy with its own S-4 in the prior 365 days, open until the first later Form 25, 25-NSE or 15 or 252 sessions), else 0; long pending targets" --domain "NaN unless the line's primary link is a domestic periodic filer (a 10-K / 10-Q family filing within 400 days); values 0 and 1" --deviation "target identified by form type under the CIK, not by a deal database; no acquirer hedge for stock deals; withdrawn deals end at 252 sessions; the merger proxy follows the announcement by weeks, so the spread is captured, not the announcement jump" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
```

## 5. How root verifies

1. Tests, from `atx-engine/tools`: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   test_research_fields_ivshape.py test_research_fields_mgr13f.py test_research_fields_divevent.py
   test_research_fields_deals.py` -> 29 passed (ivshape 7, mgr13f 9, divevent 5, deals 8). Each field: values equal an
   oracle written in the test (plain loops / sets / the hand-listed calendar), a look-ahead probe (rows <= cut
   bit-identical when everything after the cut moves) and its teeth (a same-session variant moves the cut row), seal and
   input refusals before any manifest, `--reuse` copies identical bytes, CLI equals API (ivshape, mgr13f), the plain
   builder registers nothing. Whole directory (`pytest -q -p no:cacheprovider .` in `atx-engine/tools` at `5a79dc45`):
   329 passed, 6 subtests passed (335 s).
2. Identity: `git diff --stat 798d3b23 HEAD -- atx-engine` lists only new files (four modules, their tests, the draft
   entry `prepare_research_fields_ydata.py`); `prepare_research_fields.py` and every other module are untouched, and the
   plain builder never imports the new modules (tested), so every v8 field list, manifest byte, producer fingerprint and
   reuse count is unchanged; flag absent = the entry not used.
3. Build (root, not run here), the X fields build of record plus the four fields in one process registering every
   draft entry (XWQ section 7 pattern, adding `import prepare_research_fields_ydata as y; y.register(vars(b))`), with
   that build's argv plus `--fields <its names>,iv_skew_21,stio_chg_q,div_init_omit,deal_pending --reuse <its dir>
   --reuse-sha256 <pin> --price-source C:/Users/natha/Downloads/TickerHistory3.parquet --mgr13f-stage
   <alpha_panel/v1>/thirteenf --mgr13f-stage-sha256 <the pin its --thirteenf-sha256 uses>` (the SEC options are already
   in its argv). Expected: the base names reused, these four computed; `seal.exclusive_end` 2024-01-01;
   `source_checks.ivshape.orientation_line.security_id` = SPY's vendor line; `source_checks.mgr13f.thirteenf.per_quarter`
   with `short_term_filers` about a third of the classified filers. Memory [est] for the 4-year role: iv_skew_21 ~0.1
   GiB (f32 matrices) plus one TickerHistory3 scan with the ticker column (1-2 min, as XDATA's SPY scan); div_init_omit
   ~0.4 GiB (the price panel with 763 pre-sessions and 16 B per cell of ledger); stio_chg_q two quarters of positions
   (~3 M rows each, ~0.3-0.5 GiB peak), 29 quarters of holdings parts; deal_pending as `nt_first_126`.

## 6. Owner / PM decisions

1. **VWAP:** buy a consolidated trade or minute-bar history with traded value from 2017-06 (data ask doc), or keep
   XWQ's 43 V formulas out.
2. **Themes:** a new theme `merger_arbitrage` for `deal_target` (or file it under filing_events); `div_event` filed
   under filing_events here (alternatives: earnings_momentum, or a new `payout_events`).
3. **TickerHistory3 exclusions:** the v8 builder excludes `hEMove, iEMove, wkD1, shD1, qtrD1, lnD1` as "forward-horizon
   moves; look-ahead". The vendor dictionary says `wkD1..lnD1` are ATM vol slopes, `iEMove` the earnings move implied by
   the session's ATM term structure, `hEMove` realized on past announcements: observed at the session, subject only to
   the IV delivery clock. Ruling asked: accept `shD1` at lag 1 for `iv_skew_21` (the exclusion list itself is not
   changed by this lane).
4. **Budget:** four admission trials (`smile_slope`, `stio_trade`, `div_event`, `deal_target`); XWQ records X
   hand-written at 27 of at most 33.
5. **One hypothesis, two data routes:** if the ORATS put-wing IV is bought, v9's `iv_skew` and `smile_slope` are one
   hypothesis: keep one.

## 7. Open risks

1. Recalled, not re-read: MTW's two-year gap and one-year window; the 252-session deal horizon; Yan-Zhang's
   specifics beyond the abstract (a later test exists by title only: "Are All Short-Term Institutional Investors
   Informed?", FAJ 80(1)). XSIG called Yan-Zhang "a 1980-2003 result with no later test". Root verifies the citations.
2. `iv_skew_21`: the slope's units (vol points or a multiple of ATM vol) are not documented; the SPY orientation fails
   (NaN rows) only if SPY's slopes are missing for more than 4 of 21 sessions.
3. `stio_chg_q`: churn is computed on mapped books; the basis rule can miss a split with a large same-quarter move
   (min(buys, sells) limits the damage); the holdings read loop is re-expressed in this module rather than shared with
   `research_fields_holdings.build_13f` (sharing it would move that module's producer fingerprints).
4. `deal_pending`: acquirer proxies without an own S-4, withdrawn deals (up to 252 sessions flagged), a Form 25 for
   another class; delisting at completion relies on the `-dlret` role in the NAV replay.
5. `div_init_omit` is sparse (mostly 0); omissions are seen months after the announcement.
6. R-12's `iv_vol_of_vol` reads same-date `iv_atm_21d`, which the open IV-clock note says is one session early: a
   repair question for XIMP / the PM, not acted on here.

## 8. Hygiene

- **Read in the worktree:** lane-rules.md; task-X-briefs.md; task-XDATA-report.md (all); task-XWQ-report.md (all);
  v8x-prereg.md section 14; task-LIB2-report.md and task-XSIG-report.md (registrations and exclusions, to avoid
  collisions); research_fields_{ohlc, xdata, holdings, sec, v9, price (signatures)}.py; prepare_research_fields.py (run,
  reuse, Role, FieldWriter, RoleRows); the ohlc / xdata / holdings / v9-NT tests (fixtures); the TickerHistory3 audit.
- **Pool 2 (`build-equity/`), names and manifests only:** directory listing; `train-2020-2023-lo3/` file names;
  `train-2020-2023-lo3-fields-v13/manifest.json` (field names, excluded columns, seal, role binding, visibility mark);
  `w0-vwap-closure/` file names.
- **TickerHistory3.parquet:** the footer schema (71 names and types), no row.
- **Git objects (`origin/main`):** ALPHA_PANEL_IDENTITY_SECURITY.md (D9), ALPHA_PANEL_STATUS.md (open issues),
  ALPHA_PANEL_REQUEST_V7_RESPONSE.md (D9), ALPHA_PANEL_OWNERSHIP.md, ALPHA_PANEL_SHORTFLOW.md (D1), ALPHA_PANEL_SEC.md
  (sec_filings); `vwap` lines of schema / loader sources; `provider_coverage.py` Databento lines; `git log` for
  Databento files.
- **Not opened:** the atx-db working tree (incl. untracked `databento_tail.py`); any data row; any return, IC, Sharpe,
  turnover or NAV output; `atx-db/research/2026-06-27-tradeable-alpha-results.md` (matched a grep by name only).
- **Web:** SpiderRock TickerHistory3 dictionary and live-surface documentation (slope definition); abstracts of
  Yan-Zhang (2009), Gao-Moulton-Ng and Baltussen et al. (2018).

Cross-lane edits: none (new files only).

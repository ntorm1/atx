# Data ask: a point-in-time daily VWAP (lane YDATA, platform v8, 2026-10-02)

To: atx-db (warehouse) and the owner (source purchase). From: lane YDATA. Status: OPEN, nothing to build engine-side
until a stage exists.

## 1. Finding: no daily VWAP and no input for one is in house

| place looked | what it holds | VWAP or its inputs? |
|---|---|---|
| TickerHistory3 parquet (the engine's only price source; footer read: 71 columns) | `open, high, low, close, volume, shares, closePr, closeUnadjPr, returnFactor, totalReturn, cumulReturnFactor`, IV and earnings columns | NO. The vendor dictionary (SpiderRock TickerHistory3) lists no VWAP, average price, dollar volume or trade count; `rvVar` is "reserved for future use" |
| engine store `C:/atx-wt/pool-2/build-equity/train-2020-2023-lo3` (role) and `...-fields-v13` (75 fields; manifests read) | role: `close, raw_close, volume, present, member`; fields: no price-volume aggregate | NO. `raw_close x volume` is the house dollar-volume proxy (atx-impl `kEquityDollarAdvDsl`), not traded dollars |
| engine `vwap` (atx-engine `alpha::VwapRule`, atx-impl `--vwap-rule`) | `raw-daily-close-v2` (the raw close) or `adjusted-typical-v1` ((high + low + close) / 3) | a PROXY; the code says so ("VWAP is a daily price proxy, not an intraday observation") |
| warehouse on `origin/main` (= `feat/tier1-v3-warehouse` + 50 commits) | `equity_daily_bars.vwap DOUBLE` exists in the schema (`schema.py`, `schema_contract.py`, `api/catalog.py`, DATA_DICTIONARY) | NO. Every loader writes it NULL: `ticker_history.py:434` (`"vwap": pd.NA`), `pricing_bulk.py:268`, `ticker_history_incremental.py:815` (`NULL::DOUBLE AS vwap`). ALPHA_PANEL_IDENTITY_SECURITY.md D9: "Not available: VWAP and trade counts; the vendor file has neither". ALPHA_PANEL_STATUS.md: "Not in any source: ... VWAP, trade count". The panel's `dollar_volume` is raw close x volume |
| Databento in house | atx-core `load_equs_summary_zip` and `python/scripts/extract_databento_equs_ohlcv_1d.py`: EQUS.SUMMARY `ohlcv-1d` (ts, symbol, OHLC, volume); `pull_equity_l1_1m_to_parquet`: `bbo-1m` / `cbbo-1m` quotes; atx-vol OPRA pulls (options, SPY) | NO. OHLCV and quote schemas carry no VWAP and no traded dollars. `atx-db/src/atx_db/alpha_panel/databento_tail.py` exists only as an untracked file in another session's working tree (`C:/atx`), in no commit; not opened (lane rule). By its name and the gold-panel docs it extends prices after the TickerHistory3 end (2026-06-15), i.e. after the seal |

Consequence: the 43 formulas of Kakushadze (2016) that read `vwap` (XWQ section 4) cannot be computed as printed. A
proxy ((high + low + close) / 3, or the close) is not the paper's input; the paper does not use one. **Nothing is built
under the name `vwap`.** XWQ's 46 exact formulas stand; the 43 wait on this ask.

## 2. What is needed (the exact consumer schema)

Stage `daily_vwap/` in the alpha-panel build root, schema `atx.alpha-panel.daily-vwap/v1`, one row per (session, line):

| column | type | unit | rule |
|---|---|---|---|
| `session_date` | date32 | NYSE session | the trading date the trades belong to |
| `security_id` | int64 | TickerHistory3 `securityID` | the warehouse's PIT symbol -> line map (security master); unmapped rows dropped and counted in the manifest |
| `vwap_rth_raw` | float64 | USD, unadjusted | sum(price x size) / sum(size) over the session's regular-hours (09:30:00-16:00:00 America/New_York) consolidated-tape trades that update consolidated volume (CTA / UTP volume-eligible sale conditions); cancelled and corrected trades removed, late reports counted at their execution time |
| `dollar_volume_rth_raw` | float64 | USD | sum(price x size) over the same trades |
| `volume_rth` | float64 | shares | sum(size) over the same trades (unadjusted) |
| `trade_count_rth` | int64 | trades | count of the same trades |
| `vwap_all_raw`, `volume_all` | float64 | USD, shares | optional: the same over all session trades incl. extended hours (04:00-20:00 ET) |
| `available_at` | timestamp[us, UTC] | instant | when the row was public. Same-session use needs `available_at < session_date 22:00 UTC`; otherwise the engine lags the field one session |
| `source`, `rule_id`, `vintage_risk` | string | - | vendor and condition-code rule; `vintage_risk` set when the history is a later rebuild |

Manifest (`manifest.json`, written last): `schema`, `status: complete`, `clock_rule`, `files` (bytes, SHA-256 per
`year=YYYY/daily_vwap.parquet`), the input receipts, and the consumer seal: no row with `session_date` or
`available_at` on or after 2024-01-01 in the files a research consumer binds (or year partitions, so the engine never
opens a sealed one, as `rw.partition_is_sealed`). Coverage: from 2017-06-01 at the latest (the role starts 2018-06-01;
the longest Kakushadze window is 250 sessions), better from 2012-03-26 (TickerHistory3's start), through the TickerHistory3
end. Quality checks published in the manifest: share of member cells with a row; `low <= vwap_rth_raw <= high` of the
TickerHistory3 bar of the same (session, line) (count of violations); `volume_rth / TickerHistory3 volume` quantiles.

Candidate sources (owner decision; none priced or verified here): a consolidated trade history (SIP trades or
minute bars with traded value) from a market-data vendor, or WRDS TAQ for research use. Databento's EQUS.SUMMARY
`ohlcv-1d`, already in house, does not carry it.

## 3. Engine side once the stage publishes (not built now)

A field `vwap_adj` in a new opt-in module, stamped and clocked exactly as `open_adj` (XWQ section 7,
`research_fields_ohlc.py`): row t = `vwap_rth_raw` of (session t, line) x `close.f64[t] / raw_close.f64[t]` (the role
close's own adjustment of that row, so `vwap_adj / close` = `vwap_rth_raw / raw close` of the same session);
`LAG_SESSIONS = 0` only if the manifest's clock proves `available_at < 22:00 UTC` of the session for every row used,
else 1; cell rule: present, finite positive close and raw close, a unique row, `low <= vwap <= high` of the same
vendor bar; reader-side seal from `research_window`; stage manifest SHA-256 pinned on the command line and recorded per
entry; synthetic tests with an in-test oracle and a look-ahead probe shown to fail on a lag -1 variant. XWQ's
transcription table (`xwq_check.py`, class V rows) then becomes checkable; their selection follows XWQ's rule.

# universe (`universe/`)

Per line-session universe flags, CRSP-style share / exchange codes and index-proxy membership (tier1-v4 Task 2,
gap analysis 2 item 1, rule `universe-all-common-v1`, module `atx_db.alpha_panel.universe`). A standalone joinable
stage: one row per `panel` row; the panel and every other input are read, never edited. Manifest
`universe/manifest.json` (schema `atx.alpha-panel.universe/v1`, `gate_status` included); measurements
`validation/universe.json` (gates, per-year counts, 30-field coverage); hand-check sample
`validation/universe_hand_check_sample.json`, verdicts `atx-db/docs/datasets/universe_hand_check.json`.

- Path: `data/alpha_panel/v1/universe/year=YYYY/universe.parquet` (one file per year).
- Keys: `(session_date, security_id)`, unique (checked per year in the build: no fan-out from the interval inputs).
- Span: 2018-01-02 .. 2026-09-18 (the panel's span); delisted lines included up to their last panel session.
- Rows and bytes per year:

| year | 2018 | 2019 | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 | 2026 | total |
|---|---|---|---|---|---|---|---|---|---|---|
| rows | 2,006,722 | 2,110,686 | 2,192,161 | 2,616,995 | 2,765,910 | 2,615,921 | 2,625,233 | 2,783,952 | 2,151,262 | 21,868,842 |
| bytes | 15,944,865 | 19,235,676 | 19,933,322 | 23,058,649 | 25,950,748 | 24,538,023 | 24,273,320 | 25,344,332 | 19,842,576 | 198,121,511 |

Build: `G(0.7) atx_db.alpha_panel.universe build` (about 10 min; resumable per year while input manifests and code
are unchanged; peak 0.66 GiB at DuckDB 380 MB).

## Columns

| name | type | unit | definition | source | clock | staleness |
|---|---|---|---|---|---|---|
| session_date | DATE | date | calendar session | panel | - | daily |
| security_id | BIGINT | id | vendor line | panel | - | - |
| cik | BIGINT | id | issuer: panel `cik`, else `identity/link_table_v3` as of the cutoff (interval holding d) | panel, link_table_v3 | link `available_at` < cutoff | daily |
| shrcd_like | INTEGER | code | CRSP-style share code of the line (11 US common, 12 FPI common, 18 REIT, 31 ADR, 71 LP, 73 ETP / fund, NULL other) | `share_exchange_history` (identity_v3 codes) | run `available_at` < cutoff | daily |
| exchcd_like | INTEGER | code | CRSP-style exchange code (1 NYSE, 2 NYSE American, 3 Nasdaq, 4 Arca, 5 BZX, 6 IEX, 0 OTC) | codes; else repaired FINRA class | as above / FINRA dissemination < d | daily |
| exchcd_basis | VARCHAR | - | `identity_v3` (from the codes) or `finra_class_repair` (codes have no exchange: FINRA class SC / BZX) | - | - | - |
| is_adr | BOOLEAN | flag | panel `is_adr` or `is_adr_likely` (line is an ADR / ADS) | panel | panel | daily |
| is_reit | BOOLEAN | flag | issuer is a REIT: panel `is_reit` or issuer SIC 6798 (any share type of a REIT, preferreds included) | panel, classification | SIC clock < cutoff | daily |
| is_spac | BOOLEAN | flag | blank-check issuer: panel `is_spac` or issuer SIC 6770 as of the cutoff (units, warrants, shares) | panel, classification | SIC clock < cutoff | daily |
| is_cef_etf | BOOLEAN | flag | ETF / ETP / closed-end fund / BDC: panel `is_etf`, `security_type` fund, filer regime `fund` (N-CSR), or BDC (814- file number) | panel, sec_filings | filing clock < cutoff | daily |
| is_foreign_issuer | BOOLEAN | flag | panel `is_fpi`, regime `fpi_20f` / `canadian_40f`, or incorporation outside the US (cover page as of the cutoff, else profile snapshot) | panel, sec_filings, identity_cover | clocks < cutoff | daily |
| filer_regime | VARCHAR | - | SEC filer regime of `cik` as of the cutoff: `domestic`, `fpi_20f`, `canadian_40f`, `fund`, `none`; NULL unknown / unlinked | sec_filings/filer_regime | `available_at` < cutoff | daily |
| me | DOUBLE | USD | `market/shares_daily.shrout` x raw close of d (NULL when shares unknown) | shares_daily, panel | d close | daily |
| univ_all_common | BOOLEAN | flag | rule `universe-all-common-v1` (below) | all of the above | d 22:00 | daily |
| univ_r3000p / univ_r1000p / univ_r2000p / univ_sp500p | BOOLEAN | flag | `indexes` proxy membership of d (false when not a member) | indexes | rank date 22:00 < cutoff | annual / quarterly recon |
| available_at | TIMESTAMP | UTC | session_date 22:00 | - | - | - |

## Rule `universe-all-common-v1`

1. The `market_index` all_common rule, imported (`market_index.common_filter`: `security_type` common /
   common_unverified, not ADR / ADR-likely / FPI / REIT / LP / royalty trust; `is_operating`; not `is_index`;
   listing exchange in `market_index.LISTED` = NYSE, NYSE American, Nasdaq, NYSE Arca, Cboe BZX), evaluated on the
   repaired listing exchange (below).
2. Raw close >= USD 1 and `shares_daily.shrout` > 0 (the gap-analysis spec).
3. CRSP share code 10/11 refinements: the vendor ticker is not a preferred / warrant / unit / right
   (`security_master.SYMBOL_SUFFIX`: `.PR*`, `.P?`, `.WS*`, `.WT*`, `.W`, `.U`, `.UN`, `.RT`, `.R`); `filer_regime` =
   `domestic` (a 10-K filer: excludes 20-F / 40-F filers, N-CSR funds, non-filers and unlinked lines); not
   `is_foreign_issuer` (CRSP second digit 2), `is_spac`, `is_cef_etf` (CRSP 14 / 73 analogues) or `is_reit` (18).

Why step 3 exists: on NYSE the bare market_index rule counts 1,531-1,814 lines a month against 1,167-1,241 in the
French ME-breakpoint file (+30% to +48%, 2018-05 .. 2022-12): `common_unverified` (FINRA-name) lines include bank
and REIT preferreds with `.PR` vendor tickers (`BAC.PRN`, `GS.PRK`), warrants (`AMBC.WS`), unlinked ADRs and
foreign ordinary shares (`IBN`, `CNI`, `SNP`), closed-end funds (`GAM`, `TY`) and BDCs (`TSLX`, `FSK`).

Listing-exchange repair: the panel `exchange` is the security master's map of the FINRA market class of the
panel's own as-of FINRA row; that map (`security_master.EXCHANGE_BY_CLASS`) lacks FINRA's `SC` (Nasdaq Capital
Market) and `BZX` (Cboe BZX), so those lines carry a NULL exchange in the panel and in `share_exchange_history` and
fall out of the market_index all_common and `indexes` eligibility. The stage maps the same FINRA row (dissemination
before d, at most 45 days old, the panel's freshness rule) with `SC` -> XNAS, `BZX` -> BATS, `IEX` -> IEXG. Effect
on all_common: 492-706 repaired lines a day (yearly means 2018-2025); the market_index rule with the panel exchange
gives 3,235-3,833 a day in 2020-2025 (with the $1 and shares screens), 4,048-4,613 with the repair.

## PIT rule

`available_at` = 22:00 UTC of `session_date`. A row uses its session's panel row and `shares_daily` shares (prices
through the close of d) and issuer facts as of `cutoff` = `lake_cutoff(d)` = 22:00 UTC of the previous calendar
session, strict `<` (the first calendar session, 2018-01-02, uses d - 1 day): codes via the single consumption path
`share_exchange_history_as_of(lake_cutoff(d)) WHERE d BETWEEN valid_from AND valid_to` (Task 1 fix round 1, rule
`share-exchange-runs-v2`: a run is stamped before the cutoff of its first session, so the new code shows from that
session); link_table_v3, filer regime and SIC as of the cutoff; cover-page incorporation as of the cutoff with the
`issuer_profile` snapshot as a non-PIT fallback (before the 2019 covers; incorporation rarely changes); BDC filings
and `indexes_membership_as_of(lake_cutoff(d))`.

Two qualifications carried from the inputs:

- The codes and the panel flags inherit the panel's visibility of its own inputs (Task 1 qualification): SIC and
  filer regime as of 22:00 UTC of the previous session (strict), FINRA names as of `dissemination_date < d` with
  `available_at` = dissemination date 22:00 UTC (it can equal the previous session's cutoff), the panel CIK link
  `available_at <= d 22:00 UTC`, and the Nasdaq directory snapshot (2026-09-18) as a non-PIT fallback for
  `security_type` / `is_etf`. The universe is not more point in time than the panel row.
- Consumption: a decision at session s reads `universe_as_of(lake_cutoff(s))`, whose latest row per line is the
  row of s-2 under the strict `<` (a row is stamped at its session's 22:00, the lake convention for daily market
  data, like `market_liquidity`). Research panels keyed by `session_date` join on `(session_date, security_id)`:
  the flags of d then use the close of d, the CRSP formation-date convention (form at the close of d, hold from
  d+1).

## Gates (GC9)

Commands: `G(x)` = `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb x
--wait-minutes 240 -- .venv/Scripts/python.exe -m`, from `C:/atx/atx-db` with `PYTHONPATH=src`. Every number below
is in `validation/universe.json` written by `G(0.7) atx_db.alpha_panel.universe build`.

| criterion | result | measured |
|---|---|---|
| all_common names/day, yearly mean in [3,200, 4,800] each year 2020-2025 (ruling T2-a) | PASS | 2020 3,312, 2021 3,681, 2022 3,731, 2023 3,561, 2024 3,497, 2025 3,428 |
| NYSE all_common lines within +-5% of the French ME-breakpoint NYSE count each month <= 2022 | FAIL | 46/60 months inside; outside: 2018-01 -13.1%, 2018-02 -10.0%, 2018-03 -9.7%, 2018-04 -10.2%, 2020-06 -6.7%, 2021-11 +5.1%, 2021-12 +5.5%, 2022-01 +5.8%, 2022-02 +5.9%, 2022-03 +6.6%, 2022-04 +6.5%, 2022-05 +6.5%, 2022-06 +5.1%, 2022-08 +5.5% |
| R2000 proxy companies in [1,900, 2,100] on each reconstitution's first effective session | PASS | 2018-06-25 1,987, 2019-07-01 1,994, 2020-06-29 1,981, 2021-06-28 1,969, 2022-06-27 1,973, 2023-06-26 1,989, 2024-07-01 1,992, 2025-06-30 1,987, 2026-06-29 1,962 |
| shrcd_like and exchcd_like on >= 99% of all_common cells, each year 2020-2025 | PASS | 2020 100.0000%, 2021 100.0000%, 2022 100.0000%, 2023 100.0000%, 2024 100.0000%, 2025 100.0000% |
| flags >= 95% precise on a 100-line hand check | PASS | 97/100 = 0.97; is_adr 9/12, is_cef_etf 12/12, is_foreign_issuer 12/12, is_reit 12/12, is_spac 12/12, univ_all_common 40/40 |
| 30-field coverage per year on univ_r3000p and univ_all_common republished | done | tables below |

### all_common names a day (mean / min / max per year)

| series | 2018 | 2019 | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 | 2026 |
|---|---|---|---|---|---|---|---|---|---|
| all_common | 3,214 (0-3,467) | 3,338 (3,268-3,394) | 3,312 (3,061-3,492) | 3,681 (3,494-3,870) | 3,731 (3,561-3,867) | 3,561 (3,443-3,701) | 3,497 (3,444-3,542) | 3,428 (3,314-3,515) | 3,331 (3,286-3,379) |
| without the $1 screen | 3,332 (0-3,599) | 3,519 (3,470-3,577) | 3,485 (3,220-3,574) | 3,739 (3,557-3,994) | 4,021 (3,980-4,052) | 3,942 (3,867-4,015) | 3,814 (3,748-3,884) | 3,674 (3,583-3,753) | 3,568 (3,536-3,597) |
| market_index rule, panel exchange, $1 + shares | 3,272 (0-3,744) | 3,615 (3,552-3,710) | 3,538 (3,255-3,639) | 3,731 (3,603-3,921) | 3,832 (3,692-3,932) | 3,599 (3,463-3,768) | 3,391 (3,313-3,475) | 3,235 (3,144-3,350) | 3,072 (3,008-3,163) |
| market_index rule, repaired exchange, $1 + shares | 3,819 (0-4,351) | 4,196 (4,099-4,305) | 4,150 (3,828-4,336) | 4,539 (4,339-4,796) | 4,613 (4,424-4,802) | 4,361 (4,179-4,588) | 4,189 (4,114-4,253) | 4,048 (3,922-4,191) | 3,822 (3,727-3,953) |
| market_index rule as published (no $1 / shares screen) | 3,870 (0-4,315) | 3,910 (3,834-4,023) | 3,852 (3,538-3,947) | 4,102 (3,920-4,352) | 4,250 (4,097-4,381) | 3,978 (3,815-4,138) | 3,729 (3,632-3,828) | 3,532 (3,390-3,655) | 3,327 (3,256-3,415) |
| all_common lines on a repaired exchange | 492 (0-533) | 501 (476-518) | 533 (455-613) | 687 (615-729) | 650 (595-720) | 645 (602-688) | 678 (646-705) | 706 (624-769) | 686 (663-709) |

### NYSE all_common lines vs the French ME-breakpoint NYSE count (month-end session)

| month | French | lines | dev | companies | dev |
|---|---|---|---|---|---|
| 2018-01 | 1,228 | 1,067 | -13.1% | 1,061 | -13.6% |
| 2018-02 | 1,226 | 1,104 | -10.0% | 1,101 | -10.2% |
| 2018-03 | 1,225 | 1,106 | -9.7% | 1,103 | -10.0% |
| 2018-04 | 1,224 | 1,099 | -10.2% | 1,099 | -10.2% |
| 2018-05 | 1,224 | 1,280 | +4.6% | 1,267 | +3.5% |
| 2018-06 | 1,225 | 1,281 | +4.6% | 1,268 | +3.5% |
| 2018-07 | 1,226 | 1,273 | +3.8% | 1,259 | +2.7% |
| 2018-08 | 1,225 | 1,280 | +4.5% | 1,269 | +3.6% |
| 2018-09 | 1,226 | 1,285 | +4.8% | 1,271 | +3.7% |
| 2018-10 | 1,225 | 1,272 | +3.8% | 1,258 | +2.7% |
| 2018-11 | 1,221 | 1,275 | +4.4% | 1,259 | +3.1% |
| 2018-12 | 1,215 | 1,257 | +3.5% | 1,245 | +2.5% |
| 2019-01 | 1,211 | 1,258 | +3.9% | 1,244 | +2.7% |
| 2019-02 | 1,206 | 1,254 | +4.0% | 1,239 | +2.7% |
| 2019-03 | 1,204 | 1,253 | +4.1% | 1,238 | +2.8% |
| 2019-04 | 1,204 | 1,252 | +4.0% | 1,238 | +2.8% |
| 2019-05 | 1,209 | 1,246 | +3.1% | 1,234 | +2.1% |
| 2019-06 | 1,213 | 1,252 | +3.2% | 1,239 | +2.1% |
| 2019-07 | 1,213 | 1,243 | +2.5% | 1,229 | +1.3% |
| 2019-08 | 1,214 | 1,237 | +1.9% | 1,223 | +0.7% |
| 2019-09 | 1,210 | 1,230 | +1.7% | 1,219 | +0.7% |
| 2019-10 | 1,206 | 1,225 | +1.6% | 1,213 | +0.6% |
| 2019-11 | 1,205 | 1,223 | +1.5% | 1,211 | +0.5% |
| 2019-12 | 1,194 | 1,215 | +1.8% | 1,202 | +0.7% |
| 2020-01 | 1,192 | 1,208 | +1.3% | 1,195 | +0.2% |
| 2020-02 | 1,190 | 1,203 | +1.1% | 1,187 | -0.2% |
| 2020-03 | 1,182 | 1,177 | -0.4% | 1,162 | -1.7% |
| 2020-04 | 1,182 | 1,179 | -0.2% | 1,167 | -1.3% |
| 2020-05 | 1,177 | 1,182 | +0.4% | 1,169 | -0.7% |
| 2020-06 | 1,173 | 1,095 | -6.7% | 1,086 | -7.4% |
| 2020-07 | 1,167 | 1,178 | +0.9% | 1,166 | -0.1% |
| 2020-08 | 1,168 | 1,186 | +1.5% | 1,174 | +0.5% |
| 2020-09 | 1,171 | 1,183 | +1.0% | 1,171 | +0.0% |
| 2020-10 | 1,169 | 1,172 | +0.3% | 1,161 | -0.7% |
| 2020-11 | 1,168 | 1,198 | +2.6% | 1,187 | +1.6% |
| 2020-12 | 1,170 | 1,207 | +3.2% | 1,194 | +2.1% |
| 2021-01 | 1,167 | 1,202 | +3.0% | 1,190 | +2.0% |
| 2021-02 | 1,170 | 1,211 | +3.5% | 1,198 | +2.4% |
| 2021-03 | 1,182 | 1,215 | +2.8% | 1,201 | +1.6% |
| 2021-04 | 1,180 | 1,213 | +2.8% | 1,202 | +1.9% |
| 2021-05 | 1,184 | 1,224 | +3.4% | 1,212 | +2.4% |
| 2021-06 | 1,195 | 1,229 | +2.9% | 1,212 | +1.4% |
| 2021-07 | 1,207 | 1,232 | +2.1% | 1,214 | +0.6% |
| 2021-08 | 1,211 | 1,254 | +3.5% | 1,236 | +2.1% |
| 2021-09 | 1,213 | 1,266 | +4.4% | 1,249 | +3.0% |
| 2021-10 | 1,222 | 1,265 | +3.5% | 1,249 | +2.2% |
| 2021-11 | 1,229 | 1,292 | +5.1% | 1,277 | +3.9% |
| 2021-12 | 1,230 | 1,297 | +5.5% | 1,281 | +4.2% |
| 2022-01 | 1,230 | 1,301 | +5.8% | 1,286 | +4.5% |
| 2022-02 | 1,229 | 1,302 | +5.9% | 1,285 | +4.6% |
| 2022-03 | 1,228 | 1,309 | +6.6% | 1,293 | +5.3% |
| 2022-04 | 1,228 | 1,308 | +6.5% | 1,290 | +5.1% |
| 2022-05 | 1,228 | 1,308 | +6.5% | 1,291 | +5.1% |
| 2022-06 | 1,241 | 1,304 | +5.1% | 1,289 | +3.9% |
| 2022-07 | 1,239 | 1,300 | +4.9% | 1,284 | +3.6% |
| 2022-08 | 1,236 | 1,304 | +5.5% | 1,287 | +4.1% |
| 2022-09 | 1,239 | 1,297 | +4.7% | 1,281 | +3.4% |
| 2022-10 | 1,229 | 1,286 | +4.6% | 1,269 | +3.2% |
| 2022-11 | 1,224 | 1,273 | +4.0% | 1,258 | +2.8% |
| 2022-12 | 1,219 | 1,266 | +3.9% | 1,250 | +2.5% |

### Per-year mean daily counts of flags and proxies

| year | sessions | n_rows | all_common | r3000p | r3000p_in_all_common | r1000p | r2000p | sp500p | shrcd_11 | is_adr | is_reit | is_spac | is_cef_etf | is_foreign_issuer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2018 | 251 | 7,995 | 3,214 | 1,552 | 1,260 | 523 | 1,029 | 404 | 4,627 | 292 | 285 | 78 | 2,161 | 607 |
| 2019 | 252 | 8,376 | 3,338 | 2,958 | 2,348 | 1,000 | 1,957 | 529 | 4,655 | 344 | 299 | 117 | 2,379 | 820 |
| 2020 | 253 | 8,665 | 3,312 | 3,001 | 2,324 | 1,020 | 1,982 | 529 | 4,638 | 370 | 294 | 212 | 2,493 | 918 |
| 2021 | 252 | 10,385 | 3,681 | 3,004 | 2,375 | 1,028 | 1,975 | 527 | 5,194 | 396 | 298 | 860 | 2,717 | 1,198 |
| 2022 | 251 | 11,020 | 3,731 | 2,982 | 2,397 | 1,025 | 1,957 | 527 | 5,452 | 417 | 291 | 909 | 3,002 | 1,458 |
| 2023 | 250 | 10,464 | 3,561 | 2,982 | 2,402 | 1,012 | 1,970 | 525 | 5,107 | 408 | 275 | 415 | 3,325 | 1,379 |
| 2024 | 252 | 10,418 | 3,497 | 2,981 | 2,413 | 1,008 | 1,973 | 526 | 4,822 | 396 | 272 | 176 | 3,818 | 1,321 |
| 2025 | 250 | 11,136 | 3,428 | 2,982 | 2,406 | 1,013 | 1,969 | 526 | 4,609 | 384 | 276 | 179 | 4,602 | 1,433 |
| 2026 | 179 | 12,018 | 3,331 | 2,927 | 2,377 | 1,012 | 1,915 | 524 | 4,328 | 348 | 263 | 279 | 5,586 | 1,574 |

## Coverage of 30 core fields (share of cells with a value)

Field sources: panel columns (Compustat names in brackets are the closest analogue: `atq` = panel `at`, `ceqq` =
`seq` (ceqq = seq - pstk), `ibq` = `ni_q`, `oancf` = `cfo_ttm`, `saleq` = `sale_q`), `market_liquidity.adv_63`,
`borrow_proxy.si_to_io`, universe `me` / `cik`; `beta_input` = at least 126 unguarded daily returns in the line's
last 252 panel sessions, `idio_vol_input` = at least 15 in the last 21 (input availability: the betas and
idiosyncratic volatilities themselves are Task 6's `risk_inputs`).

**univ_all_common** (cells: 2020 837,851, 2021 927,553, 2022 936,399, 2023 890,200, 2024 881,168, 2025 857,101)

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---|---|---|---|---|---|
| adv_63 | market_liquidity adv_63 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| atq | panel at | 0.984 | 0.983 | 0.985 | 0.989 | 0.992 | 0.995 |
| be | panel be (book equity) | 0.984 | 0.984 | 0.985 | 0.990 | 0.992 | 0.995 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.981 | 0.963 | 0.977 | 0.990 | 0.987 | 0.987 |
| borrow_proxy | borrow_proxy si_to_io | 0.980 | 0.973 | 0.980 | 0.983 | 0.992 | 0.992 |
| capx_ttm | panel capx_ttm | 0.918 | 0.887 | 0.901 | 0.930 | 0.922 | 0.922 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.984 | 0.984 | 0.985 | 0.990 | 0.992 | 0.995 |
| che | panel che | 0.982 | 0.980 | 0.983 | 0.987 | 0.989 | 0.992 |
| cik | universe cik (panel link, else link_table_v3 as of) | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| debt | panel debt | 0.985 | 0.984 | 0.986 | 0.990 | 0.993 | 0.995 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.926 | 0.915 | 0.926 | 0.928 | 0.925 | 0.923 |
| ff49 | panel grp_ff49 | 0.984 | 0.983 | 0.985 | 0.989 | 0.991 | 0.994 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.859 | 0.966 | 0.968 | 0.968 | 0.945 | 0.943 |
| gp_ttm | panel gp_ttm | 0.668 | 0.640 | 0.659 | 0.678 | 0.679 | 0.689 |
| ibq | panel ni_q (quarterly net income) | 0.984 | 0.982 | 0.984 | 0.988 | 0.991 | 0.993 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| inst_ownership | panel inst_shares (13F) | 0.980 | 0.973 | 0.981 | 0.983 | 0.992 | 0.992 |
| iv_atm | panel iv_atm_21d | 0.736 | 0.775 | 0.802 | 0.794 | 0.779 | 0.780 |
| me | universe me (shares_daily shrout x raw close) | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| ni_ttm | panel ni_ttm | 0.965 | 0.938 | 0.956 | 0.977 | 0.978 | 0.979 |
| oancf | panel cfo_ttm | 0.954 | 0.930 | 0.950 | 0.975 | 0.974 | 0.974 |
| oi_ttm | panel oi_ttm | 0.822 | 0.807 | 0.830 | 0.852 | 0.853 | 0.857 |
| prc | panel raw_close | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| ret | panel ret, unguarded | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.935 | 0.903 | 0.920 | 0.940 | 0.944 | 0.947 |
| saleq | panel sale_q | 0.958 | 0.950 | 0.954 | 0.959 | 0.962 | 0.965 |
| short_interest | panel si_shares (FINRA) | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.990 | 0.994 | 0.992 | 0.993 | 0.995 | 0.998 |
| sic | panel sic | 0.986 | 0.986 | 0.987 | 0.991 | 0.994 | 0.996 |
| sue | panel sue | 0.948 | 0.910 | 0.904 | 0.960 | 0.965 | 0.966 |

**univ_r3000p** (cells: 2020 759,242, 2021 756,875, 2022 748,368, 2023 745,421, 2024 751,240, 2025 745,529)

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---|---|---|---|---|---|
| adv_63 | market_liquidity adv_63 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| atq | panel at | 0.899 | 0.909 | 0.922 | 0.930 | 0.935 | 0.937 |
| be | panel be (book equity) | 0.898 | 0.908 | 0.921 | 0.930 | 0.935 | 0.937 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 1.000 | 1.000 | 0.999 | 1.000 | 0.999 | 1.000 |
| borrow_proxy | borrow_proxy si_to_io | 0.987 | 0.984 | 0.982 | 0.983 | 0.985 | 0.984 |
| capx_ttm | panel capx_ttm | 0.852 | 0.858 | 0.871 | 0.884 | 0.885 | 0.884 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.898 | 0.908 | 0.921 | 0.930 | 0.935 | 0.937 |
| che | panel che | 0.897 | 0.906 | 0.919 | 0.928 | 0.933 | 0.935 |
| cik | universe cik (panel link, else link_table_v3 as of) | 0.947 | 0.954 | 0.968 | 0.973 | 0.977 | 0.975 |
| debt | panel debt | 0.900 | 0.909 | 0.922 | 0.930 | 0.936 | 0.937 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.885 | 0.894 | 0.909 | 0.919 | 0.922 | 0.922 |
| ff49 | panel grp_ff49 | 0.900 | 0.910 | 0.923 | 0.924 | 0.928 | 0.930 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.885 | 0.982 | 0.983 | 0.986 | 0.962 | 0.953 |
| gp_ttm | panel gp_ttm | 0.644 | 0.646 | 0.665 | 0.673 | 0.675 | 0.679 |
| ibq | panel ni_q (quarterly net income) | 0.899 | 0.907 | 0.920 | 0.929 | 0.935 | 0.936 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| inst_ownership | panel inst_shares (13F) | 0.987 | 0.984 | 0.981 | 0.983 | 0.985 | 0.984 |
| iv_atm | panel iv_atm_21d | 0.821 | 0.855 | 0.886 | 0.885 | 0.877 | 0.876 |
| me | universe me (shares_daily shrout x raw close) | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| ni_ttm | panel ni_ttm | 0.896 | 0.902 | 0.915 | 0.926 | 0.933 | 0.933 |
| oancf | panel cfo_ttm | 0.886 | 0.897 | 0.912 | 0.924 | 0.929 | 0.928 |
| oi_ttm | panel oi_ttm | 0.763 | 0.771 | 0.792 | 0.800 | 0.801 | 0.801 |
| prc | panel raw_close | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| ret | panel ret, unguarded | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.871 | 0.874 | 0.885 | 0.892 | 0.901 | 0.904 |
| saleq | panel sale_q | 0.878 | 0.885 | 0.896 | 0.900 | 0.907 | 0.909 |
| short_interest | panel si_shares (FINRA) | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.961 | 0.965 | 0.970 | 0.969 | 0.970 | 0.969 |
| sic | panel sic | 0.902 | 0.912 | 0.924 | 0.926 | 0.931 | 0.932 |
| sue | panel sue | 0.892 | 0.895 | 0.897 | 0.915 | 0.930 | 0.929 |

**member_equity** (cells: 2020 580,695, 2021 611,044, 2022 584,009, 2023 575,461, 2024 573,063, 2025 553,772)

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---|---|---|---|---|---|
| adv_63 | market_liquidity adv_63 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| atq | panel at | 0.911 | 0.911 | 0.921 | 0.926 | 0.929 | 0.929 |
| be | panel be (book equity) | 0.908 | 0.909 | 0.917 | 0.923 | 0.927 | 0.928 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.989 | 0.976 | 0.991 | 0.995 | 0.992 | 0.992 |
| borrow_proxy | borrow_proxy si_to_io | 0.996 | 0.993 | 0.997 | 0.998 | 0.998 | 0.999 |
| capx_ttm | panel capx_ttm | 0.859 | 0.839 | 0.862 | 0.880 | 0.881 | 0.883 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.908 | 0.909 | 0.917 | 0.923 | 0.927 | 0.928 |
| che | panel che | 0.910 | 0.910 | 0.918 | 0.924 | 0.928 | 0.928 |
| cik | universe cik (panel link, else link_table_v3 as of) | 0.980 | 0.981 | 0.988 | 0.993 | 0.995 | 0.995 |
| debt | panel debt | 0.912 | 0.912 | 0.921 | 0.926 | 0.930 | 0.930 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.847 | 0.836 | 0.856 | 0.861 | 0.862 | 0.856 |
| ff49 | panel grp_ff49 | 0.959 | 0.958 | 0.966 | 0.972 | 0.978 | 0.980 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.900 | 0.992 | 0.994 | 0.994 | 0.980 | 0.981 |
| gp_ttm | panel gp_ttm | 0.694 | 0.689 | 0.711 | 0.725 | 0.726 | 0.730 |
| ibq | panel ni_q (quarterly net income) | 0.860 | 0.852 | 0.863 | 0.868 | 0.869 | 0.866 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| inst_ownership | panel inst_shares (13F) | 0.996 | 0.993 | 0.997 | 0.998 | 0.998 | 0.999 |
| iv_atm | panel iv_atm_21d | 0.957 | 0.972 | 0.984 | 0.982 | 0.979 | 0.979 |
| me | universe me (shares_daily shrout x raw close) | 0.995 | 0.993 | 0.997 | 0.999 | 0.998 | 0.996 |
| ni_ttm | panel ni_ttm | 0.898 | 0.880 | 0.901 | 0.918 | 0.921 | 0.921 |
| oancf | panel cfo_ttm | 0.889 | 0.873 | 0.898 | 0.915 | 0.915 | 0.914 |
| oi_ttm | panel oi_ttm | 0.806 | 0.795 | 0.813 | 0.828 | 0.829 | 0.830 |
| prc | panel raw_close | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| ret | panel ret, unguarded | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.881 | 0.859 | 0.882 | 0.899 | 0.905 | 0.906 |
| saleq | panel sale_q | 0.846 | 0.836 | 0.850 | 0.854 | 0.857 | 0.854 |
| short_interest | panel si_shares (FINRA) | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.998 | 0.999 | 0.999 | 0.999 | 0.999 | 0.999 |
| sic | panel sic | 0.961 | 0.960 | 0.968 | 0.974 | 0.980 | 0.982 |
| sue | panel sue | 0.838 | 0.801 | 0.826 | 0.855 | 0.856 | 0.851 |

## Known limits

1. **French NYSE gate FAIL in 14 of 60 months** (binding accuracy check, reported not hidden; `gate_status.pass` =
   false). Causes: (a) 2018-01 .. 2018-04 (-9.7% to -13.1%): `market/shares_daily` holds shares for about 3,700-3,900
   lines a session before May 2018 against 6,700-7,700 after, and all_common requires `shrout`; (b) 2020-06 (-6.7%):
   the vendor file is short on 2020-06-30 (panel 7,934 rows against 8,635 on 2020-06-29); (c) 2021-11 .. 2022-08
   except 2022-07 (+5.1% to +6.6% in lines, +3.9% to +5.3% in companies): no residual class explains it after the
   refinements (about 3 low-volatility USD 10 SPAC lines with a non-6770 SIC and 2-7 LP / LLC registrants on NYSE);
   the excess builds with the 2021 IPO / de-SPAC wave (NYSE lines added 2020-12 .. 2022-03: 183, removed 82, against
   a French net +58), whose CRSP share-code assignment cannot be observed without CRSP. The 46 other months are
   within +-4.9% (2019-07 .. 2021-10 within +-4.4% except 2020-06).
2. **Upstream defect (repaired here, not fixed at the source):** `security_master.EXCHANGE_BY_CLASS` lacks the FINRA
   classes `SC` (Nasdaq Capital Market) and `BZX`, so the panel `exchange`, `share_exchange_history.exchcd`, the
   market_index all_common universe and the `indexes` eligibility all drop those lines (492-706 all_common lines a
   day). The fix belongs in `security_master.py` followed by panel, codes, market_index and indexes rebuilds; until
   then the index proxies never hold a Nasdaq Capital Market company.
3. 2018-01-02 .. 2018-01-10 (7 sessions) have no all_common lines: the first FINRA dissemination is 2018-01-10, so
   the panel's security flags are NULL before it (warm-up, outside the 2020+ window).
4. `is_adr` (the panel's flags) was 9/12 precise in the hand check: the `is_adr_likely` heuristic (20-F filer with a
   `common_unverified` line) also marks directly listed foreign ordinary shares (LeddarTech, TELUS International)
   and a FINRA "ADR"-typed warrant line (Polestar warrants); it misses some ADRs (Fresh2). all_common excludes all of
   them either way (foreign issuers).
5. `shrcd_like` is identity_v3's code of the panel typing, and all_common does not rely on it: preferreds typed
   `common_unverified` carry 11 / 18 (e.g. `CHMI.PRB` 18) and funds / BDCs typed `fund` carry 73 (the panel's
   `is_etf` includes funds, so 14 never occurs). shrcd 11 lines number 4,609-5,452 a day (2020-2025) against
   3,312-3,731 all_common lines.
6. Foreign incorporation (CRSP second digit 2 analogue) removes 10-K filers incorporated abroad (e.g. Medtronic,
   Accenture, Schlumberger, Carnival, Royal Caribbean, Jazz, Credo); `filer_regime = 'domestic' AND
   is_foreign_issuer` selects them. Before the 2019 cover pages the `issuer_profile` snapshot is a non-PIT fallback.
7. `is_reit` is issuer level (a REIT's preferred lines are flagged); `is_spac` relies on the filed SIC 6770: SPACs
   filing under another SIC (e.g. `NSTB`, `IPVF` with 6211) stay in all_common until the panel name rule catches them.
8. Fundamentals coverage is measured on the published panel fields (fundamentals v9 through the v1 link table);
   `compustat_std` (Task 3) is not joined. `me` is `shares_daily.shrout` x raw close, not the panel's vendor-share
   `me_line`.
9. `univ_r*p` come from `indexes` unchanged (see 2): R3000P also holds REITs, foreign-incorporated 10-K filers and
   unlinked lines, so 2,324-2,413 of its about 2,980 lines a day are all_common (2020-2025).
10. Not registered in `stagelake` (no `LAKE_STAGES` literal: three inputs have no registered stage);
    `universe.registry.json` is the stub Task 17 folds in.

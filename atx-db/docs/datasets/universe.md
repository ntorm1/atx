# universe (`universe/`)

Published snapshot: tier1-v4 Task 2 fix round 2 (Task 2x remains approved). Fix round 3 source replay and universe publication are pending after two low-commit guard stops; the previous universe source snapshot is stale and downstream consumers remain on hold. Rule `universe-all-common-v3`, schema `atx.alpha-panel.universe/v1`; one joinable row per frozen panel key `(session_date, security_id)`. Delisted lines remain through their last panel session. Registry stub `universe.registry.json`; manifest `universe/manifest.json`; validation `validation/universe.json`.

Build command from `C:/atx/atx-db` with `PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`:
`.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build`.

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
| is_reit | BOOLEAN | flag | latest explicit issuer-bound original REIT status; fallback panel `is_reit` or SIC 6798; termination overrides stale flags (preferreds included) | issuer_classes, panel, classification | original availability / SIC clock < cutoff | sparse original status observations; 2023-2026 refresh required |
| is_spac | BOOLEAN | flag | blank-check issuer: panel `is_spac` or issuer SIC 6770 as of the cutoff with zero/missing panel sale_ttm (retained SIC with positive revenue is operating) | panel, classification | SIC clock < cutoff | daily |
| is_cef_etf | BOOLEAN | flag | ETF / ETP / closed-end fund / BDC: panel `is_etf`, `security_type` fund, filer regime `fund` (N-CSR), or latest visible BDC election/support event (814- file number), revoked by latest withdrawal | panel, sec_filings | filing clock < cutoff | daily |
| is_foreign_issuer | BOOLEAN | flag | panel `is_fpi`, regime `fpi_20f` / `canadian_40f`, or incorporation outside the US (dated cover or target-bound original header; unknown without dated evidence) | panel, sec_filings, identity_cover, issuer_history, issuer_classes | clocks < cutoff | daily |
| filer_regime | VARCHAR | - | SEC filer regime of `cik` as of the cutoff: `domestic`, `fpi_20f`, `canadian_40f`, `fund`, `none`; NULL unknown / unlinked | sec_filings/filer_regime | `available_at` < cutoff | daily |
| me | DOUBLE | USD | `market/shares_daily.shrout` x raw close of d (NULL when shares unknown) | shares_daily, panel | d close | daily |
| univ_all_common | BOOLEAN | flag | rule `universe-all-common-v3` (below) | all of the above | d 22:00 | daily |
| univ_r3000p / univ_r1000p / univ_r2000p / univ_sp500p | BOOLEAN | flag | `indexes` proxy membership of d (false when not a member) | indexes | rank date 22:00 < cutoff | annual / quarterly recon |
| available_at | TIMESTAMP | UTC | session_date 22:00 | - | - | - |



## Rule and timing

The imported market_index common filter requires operating common/common_unverified lines, excludes ADR/FPI/REIT/LP/royalty trust and indexes, and requires NYSE/Amex/Nasdaq/Arca/BZX listing. Universe adds raw close >= USD 1, positive known shares_daily shrout, no preferred/warrant/unit/right ticker suffix, domestic filer regime, and no foreign issuer, blank check, fund/BDC or REIT flag.

NULL frozen-panel exchanges are repaired only by dated fresh FINRA listing evidence: dissemination_date < session d and age <=45 calendar days. The authoritative security_master.EXCHANGE_BY_CLASS now includes SC -> XNAS, BZX -> BATS and IEX -> IEXG. Codes, market_index, indexes and universe use this mapping; all company caps, ranks and membership intervals were recomputed, with no panel rebuild.

available_at = session d 22:00 UTC. Dated incorporation, SIC, filer regime, links, BDC status and index membership require available_at < lake_cutoff(d), the previous calendar session at 22:00 UTC. Incorporation is unknown when no dated cover or target-bound original header is visible; an undated issuer_profile snapshot is never a historical fallback. BDC status is the latest qualifying event strictly before the cutoff, with withdrawal prevailing on a tied timestamp. SIC 6770 only supplements SPAC when sale_ttm is zero/missing, matching the panel rule. Code intervals additionally hold session d and are stamped one second before their first-session cutoff.

T1-b remains the accepted qualification: frozen-panel FINRA classifications can be visible at the prior cutoff, its CIK links can use d 22:00, and Nasdaq directory classifications are a current snapshot. No extra incorporation exception is introduced. Daily-stage consumption uses universe_as_of(lake_cutoff(s)); the strict inequality means the latest stored market row is generally s-2. Formation-date research joins use d close for holding from d+1.

## Measured gates

| gate | result | measurement |
|---|---|---|
| codes_on_all_common | PASS | 100% present in every year 2020–2025; presence is not independent semantic classification proof |
| daily_band | PASS | 2020–2025 yearly means: 2020: 3326.9, 2021: 3679.7, 2022: 3724.8, 2023: 3555.4, 2024: 3493.0, 2025: 3424.5 |
| french_nyse | FAIL | 10/60 outside unchanged +/-5%; max absolute deviation 13.36%; no missing months |
| hand_check | PASS | 97/100 pooled; ADR 9/12; 64 changed common/REIT/foreign claims independently rechecked |
| r2000 | PASS | All nine proxy reconstitutions within [1900,2100]: {'2018-06-25': 1987, '2019-07-01': 1993, '2020-06-29': 1980, '2021-06-28': 1972, '2022-06-27': 1980, '2023-06-26': 1990, '2024-07-01': 1992, '2025-06-30': 1988, '2026-06-29': 1972} |

## Published rows

| year | rows | bytes |
|---|---:|---:|
| 2018 | 2,006,722 | 15,820,151 |
| 2019 | 2,110,686 | 19,091,036 |
| 2020 | 2,192,161 | 19,784,496 |
| 2021 | 2,616,995 | 22,864,338 |
| 2022 | 2,765,910 | 25,732,851 |
| 2023 | 2,615,921 | 24,347,373 |
| 2024 | 2,625,233 | 24,101,238 |
| 2025 | 2,783,952 | 25,163,321 |
| 2026 | 2,151,262 | 19,681,777 |

## Mean daily line counts

| year | all_common | is_adr | is_cef_etf | is_foreign_issuer | is_reit | is_spac | n_rows | r1000p | r2000p | r3000p | r3000p_in_all_common | sessions | shrcd_11 | sp500p |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2018 | 3268.7 | 292.2 | 2161.3 | 524.0 | 289.0 | 75.3 | 7994.9 | 523.9 | 1028.8 | 1552.8 | 1292.9 | 251 | 4627.2 | 404.5 |
| 2019 | 3384.7 | 344.1 | 2379.3 | 743.0 | 304.7 | 112.5 | 8375.7 | 1001.0 | 1957.4 | 2958.4 | 2399.3 | 252 | 4655.2 | 529.1 |
| 2020 | 3326.9 | 369.8 | 2492.6 | 879.4 | 303.6 | 208.3 | 8664.7 | 1019.0 | 1981.8 | 3000.8 | 2361.5 | 253 | 4637.8 | 529.0 |
| 2021 | 3679.7 | 395.8 | 2716.6 | 1162.1 | 308.3 | 857.6 | 10384.9 | 1027.6 | 1973.5 | 3001.1 | 2418.9 | 252 | 5194.5 | 526.9 |
| 2022 | 3724.8 | 417.3 | 3001.6 | 1456.8 | 301.9 | 905.3 | 11019.6 | 1024.2 | 1960.0 | 2984.3 | 2429.0 | 251 | 5452.4 | 527.1 |
| 2023 | 3555.4 | 408.5 | 3325.2 | 1378.7 | 285.2 | 410.7 | 10463.7 | 1009.8 | 1974.1 | 2983.9 | 2420.6 | 250 | 5107.3 | 524.7 |
| 2024 | 3493.0 | 395.6 | 3817.9 | 1320.5 | 280.8 | 171.6 | 10417.6 | 1008.0 | 1973.1 | 2981.1 | 2436.0 | 252 | 4822.2 | 525.3 |
| 2025 | 3424.5 | 384.3 | 4602.2 | 1432.3 | 284.9 | 176.1 | 11135.8 | 1012.5 | 1971.6 | 2984.0 | 2437.2 | 250 | 4609.0 | 526.1 |
| 2026 | 3326.1 | 347.8 | 5586.5 | 1572.7 | 271.0 | 277.8 | 12018.2 | 1008.3 | 1926.5 | 2934.8 | 2406.6 | 179 | 4328.1 | 524.5 |

## French NYSE comparison (value only, before 2023)

| month | NYSE common lines | French documented count | residual lines | deviation |
|---|---:|---:|---:|---:|
| 2018-01 | 1064 | 1228 | -164 | -13.36% |
| 2018-02 | 1100 | 1226 | -126 | -10.28% |
| 2018-03 | 1102 | 1225 | -123 | -10.04% |
| 2018-04 | 1095 | 1224 | -129 | -10.54% |
| 2018-05 | 1276 | 1224 | +52 | +4.25% |
| 2018-06 | 1277 | 1225 | +52 | +4.24% |
| 2018-07 | 1269 | 1226 | +43 | +3.51% |
| 2018-08 | 1276 | 1225 | +51 | +4.16% |
| 2018-09 | 1281 | 1226 | +55 | +4.49% |
| 2018-10 | 1268 | 1225 | +43 | +3.51% |
| 2018-11 | 1271 | 1221 | +50 | +4.10% |
| 2018-12 | 1252 | 1215 | +37 | +3.05% |
| 2019-01 | 1253 | 1211 | +42 | +3.47% |
| 2019-02 | 1249 | 1206 | +43 | +3.57% |
| 2019-03 | 1247 | 1204 | +43 | +3.57% |
| 2019-04 | 1246 | 1204 | +42 | +3.49% |
| 2019-05 | 1241 | 1209 | +32 | +2.65% |
| 2019-06 | 1246 | 1213 | +33 | +2.72% |
| 2019-07 | 1236 | 1213 | +23 | +1.90% |
| 2019-08 | 1230 | 1214 | +16 | +1.32% |
| 2019-09 | 1222 | 1210 | +12 | +0.99% |
| 2019-10 | 1217 | 1206 | +11 | +0.91% |
| 2019-11 | 1216 | 1205 | +11 | +0.91% |
| 2019-12 | 1210 | 1194 | +16 | +1.34% |
| 2020-01 | 1203 | 1192 | +11 | +0.92% |
| 2020-02 | 1198 | 1190 | +8 | +0.67% |
| 2020-03 | 1169 | 1182 | -13 | -1.10% |
| 2020-04 | 1171 | 1182 | -11 | -0.93% |
| 2020-05 | 1173 | 1177 | -4 | -0.34% |
| 2020-06 | 1086 | 1173 | -87 | -7.42% |
| 2020-07 | 1169 | 1167 | +2 | +0.17% |
| 2020-08 | 1177 | 1168 | +9 | +0.77% |
| 2020-09 | 1174 | 1171 | +3 | +0.26% |
| 2020-10 | 1163 | 1169 | -6 | -0.51% |
| 2020-11 | 1189 | 1168 | +21 | +1.80% |
| 2020-12 | 1198 | 1170 | +28 | +2.39% |
| 2021-01 | 1193 | 1167 | +26 | +2.23% |
| 2021-02 | 1203 | 1170 | +33 | +2.82% |
| 2021-03 | 1205 | 1182 | +23 | +1.95% |
| 2021-04 | 1203 | 1180 | +23 | +1.95% |
| 2021-05 | 1213 | 1184 | +29 | +2.45% |
| 2021-06 | 1219 | 1195 | +24 | +2.01% |
| 2021-07 | 1222 | 1207 | +15 | +1.24% |
| 2021-08 | 1245 | 1211 | +34 | +2.81% |
| 2021-09 | 1257 | 1213 | +44 | +3.63% |
| 2021-10 | 1256 | 1222 | +34 | +2.78% |
| 2021-11 | 1283 | 1229 | +54 | +4.39% |
| 2021-12 | 1288 | 1230 | +58 | +4.72% |
| 2022-01 | 1292 | 1230 | +62 | +5.04% |
| 2022-02 | 1293 | 1229 | +64 | +5.21% |
| 2022-03 | 1301 | 1228 | +73 | +5.94% |
| 2022-04 | 1301 | 1228 | +73 | +5.94% |
| 2022-05 | 1301 | 1228 | +73 | +5.94% |
| 2022-06 | 1297 | 1241 | +56 | +4.51% |
| 2022-07 | 1293 | 1239 | +54 | +4.36% |
| 2022-08 | 1297 | 1236 | +61 | +4.94% |
| 2022-09 | 1290 | 1239 | +51 | +4.12% |
| 2022-10 | 1279 | 1229 | +50 | +4.07% |
| 2022-11 | 1266 | 1224 | +42 | +3.43% |
| 2022-12 | 1259 | 1219 | +40 | +3.28% |

## Coverage: 30 core fields

Value/presence coverage spans every year 2020–2025 and all three masks below despite aggregate market returns stopping at 2022. beta_input and idio_vol_input measure input sufficiency, not actual derived values; Task 6 publishes actual-field coverage. No post-2022 aggregate return values are computed or inspected here.

### univ_all_common

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---:|---:|---:|---:|---:|---:|
| adv_63 | market_liquidity adv_63 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| atq | panel at | 0.9843 | 0.9833 | 0.9853 | 0.9893 | 0.9923 | 0.9947 |
| be | panel be (book equity) | 0.9843 | 0.9838 | 0.9853 | 0.9897 | 0.9922 | 0.9951 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.9812 | 0.9627 | 0.9764 | 0.99 | 0.9874 | 0.9868 |
| borrow_proxy | borrow_proxy si_to_io | 0.9798 | 0.9727 | 0.9804 | 0.9832 | 0.9916 | 0.9924 |
| capx_ttm | panel capx_ttm | 0.9185 | 0.8868 | 0.9016 | 0.9307 | 0.922 | 0.9229 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.9843 | 0.9838 | 0.9853 | 0.9897 | 0.9922 | 0.9951 |
| che | panel che | 0.9818 | 0.9804 | 0.9825 | 0.9866 | 0.9893 | 0.9921 |
| cik | universe cik (panel link, else link_table_v3 as of) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| debt | panel debt | 0.9855 | 0.9842 | 0.9858 | 0.9895 | 0.9928 | 0.9952 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.9245 | 0.9144 | 0.9261 | 0.9273 | 0.9246 | 0.9224 |
| ff49 | panel grp_ff49 | 0.9839 | 0.9828 | 0.9846 | 0.9885 | 0.9912 | 0.9944 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.8588 | 0.966 | 0.968 | 0.9683 | 0.9444 | 0.9428 |
| gp_ttm | panel gp_ttm | 0.6676 | 0.6397 | 0.6594 | 0.6781 | 0.6798 | 0.689 |
| ibq | panel ni_q (quarterly net income) | 0.9842 | 0.9819 | 0.9835 | 0.988 | 0.9906 | 0.993 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 0.9999 | 0.9998 | 0.9998 | 0.9999 | 0.9999 | 0.9999 |
| inst_ownership | panel inst_shares (13F) | 0.9798 | 0.9727 | 0.9804 | 0.9832 | 0.9917 | 0.9924 |
| iv_atm | panel iv_atm_21d | 0.7337 | 0.7742 | 0.8021 | 0.7935 | 0.7783 | 0.7792 |
| me | universe me (shares_daily shrout x raw close) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| ni_ttm | panel ni_ttm | 0.9647 | 0.9382 | 0.9556 | 0.9772 | 0.9783 | 0.9792 |
| oancf | panel cfo_ttm | 0.9541 | 0.9303 | 0.9503 | 0.9751 | 0.9743 | 0.9743 |
| oi_ttm | panel oi_ttm | 0.8221 | 0.8069 | 0.83 | 0.8518 | 0.8525 | 0.8568 |
| prc | panel raw_close | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| ret | panel ret, unguarded | 1.0 | 1.0 | 1.0 | 0.9999 | 0.9999 | 0.9999 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.9339 | 0.9028 | 0.9204 | 0.9405 | 0.944 | 0.9471 |
| saleq | panel sale_q | 0.9567 | 0.9491 | 0.9543 | 0.9595 | 0.9624 | 0.9656 |
| short_interest | panel si_shares (FINRA) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.9896 | 0.9941 | 0.9923 | 0.9927 | 0.9946 | 0.9978 |
| sic | panel sic | 0.9864 | 0.9856 | 0.9871 | 0.9908 | 0.9938 | 0.9964 |
| sue | panel sue | 0.9476 | 0.9102 | 0.9033 | 0.9596 | 0.9643 | 0.9663 |
| denominator cells | line-session cells | 841696 | 927281 | 934920 | 888859 | 880228 | 856121 |

### univ_r3000p

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---:|---:|---:|---:|---:|---:|
| adv_63 | market_liquidity adv_63 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| atq | panel at | 0.9093 | 0.9227 | 0.9339 | 0.94 | 0.9458 | 0.9485 |
| be | panel be (book equity) | 0.9086 | 0.922 | 0.933 | 0.9395 | 0.9454 | 0.9488 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.9995 | 0.9994 | 0.9994 | 0.9996 | 0.9995 | 0.9992 |
| borrow_proxy | borrow_proxy si_to_io | 0.9881 | 0.9862 | 0.983 | 0.9839 | 0.9867 | 0.986 |
| capx_ttm | panel capx_ttm | 0.8607 | 0.8693 | 0.8817 | 0.8943 | 0.8952 | 0.895 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.9086 | 0.922 | 0.933 | 0.9395 | 0.9454 | 0.9488 |
| che | panel che | 0.9071 | 0.9202 | 0.9307 | 0.9373 | 0.9433 | 0.9466 |
| cik | universe cik (panel link, else link_table_v3 as of) | 0.9538 | 0.9622 | 0.9755 | 0.9783 | 0.9815 | 0.9813 |
| debt | panel debt | 0.9099 | 0.9229 | 0.934 | 0.9401 | 0.9459 | 0.9488 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.8891 | 0.9035 | 0.9178 | 0.9244 | 0.9294 | 0.9308 |
| ff49 | panel grp_ff49 | 0.9112 | 0.9242 | 0.9343 | 0.9347 | 0.9395 | 0.9412 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.8858 | 0.9811 | 0.9829 | 0.9863 | 0.9624 | 0.9558 |
| gp_ttm | panel gp_ttm | 0.6475 | 0.6553 | 0.6763 | 0.6832 | 0.6801 | 0.6797 |
| ibq | panel ni_q (quarterly net income) | 0.9089 | 0.9215 | 0.9319 | 0.9383 | 0.945 | 0.9478 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| inst_ownership | panel inst_shares (13F) | 0.9878 | 0.9858 | 0.9826 | 0.9839 | 0.9867 | 0.9858 |
| iv_atm | panel iv_atm_21d | 0.8272 | 0.871 | 0.8993 | 0.8968 | 0.8929 | 0.8911 |
| me | universe me (shares_daily shrout x raw close) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| ni_ttm | panel ni_ttm | 0.9064 | 0.9155 | 0.9266 | 0.9352 | 0.9432 | 0.9446 |
| oancf | panel cfo_ttm | 0.8965 | 0.9102 | 0.9241 | 0.9333 | 0.9391 | 0.94 |
| oi_ttm | panel oi_ttm | 0.7692 | 0.7841 | 0.8054 | 0.8103 | 0.8112 | 0.8081 |
| prc | panel raw_close | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| ret | panel ret, unguarded | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.8809 | 0.8867 | 0.8972 | 0.9022 | 0.9115 | 0.9139 |
| saleq | panel sale_q | 0.8885 | 0.8994 | 0.9086 | 0.9105 | 0.9182 | 0.9199 |
| short_interest | panel si_shares (FINRA) | 0.9999 | 1.0 | 0.9999 | 0.9999 | 0.9999 | 0.9999 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.9644 | 0.9709 | 0.9739 | 0.9727 | 0.974 | 0.9742 |
| sic | panel sic | 0.9127 | 0.926 | 0.936 | 0.936 | 0.9414 | 0.9433 |
| sue | panel sue | 0.9014 | 0.9071 | 0.909 | 0.9244 | 0.9395 | 0.9402 |
| denominator cells | line-session cells | 759214 | 756269 | 749057 | 745968 | 751245 | 746012 |

### member_equity

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---:|---:|---:|---:|---:|---:|
| adv_63 | market_liquidity adv_63 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| atq | panel at | 0.9108 | 0.9115 | 0.9209 | 0.9258 | 0.9295 | 0.9295 |
| be | panel be (book equity) | 0.9084 | 0.9092 | 0.9169 | 0.9229 | 0.9273 | 0.9284 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.9887 | 0.976 | 0.9908 | 0.9948 | 0.9924 | 0.9923 |
| borrow_proxy | borrow_proxy si_to_io | 0.9958 | 0.9933 | 0.9974 | 0.9981 | 0.9984 | 0.999 |
| capx_ttm | panel capx_ttm | 0.8594 | 0.839 | 0.8615 | 0.8804 | 0.8814 | 0.8833 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.9084 | 0.9092 | 0.9169 | 0.9229 | 0.9273 | 0.9284 |
| che | panel che | 0.9098 | 0.9098 | 0.9181 | 0.9245 | 0.9276 | 0.9282 |
| cik | universe cik (panel link, else link_table_v3 as of) | 0.9801 | 0.9806 | 0.9883 | 0.9933 | 0.9954 | 0.9946 |
| debt | panel debt | 0.9123 | 0.9118 | 0.9212 | 0.9261 | 0.93 | 0.93 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.8468 | 0.8364 | 0.8562 | 0.8608 | 0.8619 | 0.8561 |
| ff49 | panel grp_ff49 | 0.9593 | 0.9583 | 0.9664 | 0.972 | 0.9783 | 0.98 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.8997 | 0.9918 | 0.994 | 0.994 | 0.9799 | 0.981 |
| gp_ttm | panel gp_ttm | 0.6936 | 0.6885 | 0.711 | 0.7251 | 0.7264 | 0.73 |
| ibq | panel ni_q (quarterly net income) | 0.8599 | 0.8516 | 0.8631 | 0.8682 | 0.8689 | 0.8663 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| inst_ownership | panel inst_shares (13F) | 0.9958 | 0.9933 | 0.9974 | 0.9981 | 0.9984 | 0.999 |
| iv_atm | panel iv_atm_21d | 0.957 | 0.9715 | 0.9837 | 0.9819 | 0.9788 | 0.9793 |
| me | universe me (shares_daily shrout x raw close) | 0.9952 | 0.9929 | 0.9971 | 0.9986 | 0.9982 | 0.9961 |
| ni_ttm | panel ni_ttm | 0.8981 | 0.8797 | 0.901 | 0.918 | 0.9211 | 0.9213 |
| oancf | panel cfo_ttm | 0.8887 | 0.8725 | 0.8984 | 0.9148 | 0.9149 | 0.9141 |
| oi_ttm | panel oi_ttm | 0.8056 | 0.7947 | 0.8129 | 0.828 | 0.829 | 0.83 |
| prc | panel raw_close | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| ret | panel ret, unguarded | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.881 | 0.8592 | 0.8818 | 0.8993 | 0.905 | 0.9061 |
| saleq | panel sale_q | 0.846 | 0.836 | 0.8497 | 0.854 | 0.8565 | 0.8542 |
| short_interest | panel si_shares (FINRA) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.9975 | 0.9992 | 0.9992 | 0.9993 | 0.9992 | 0.9985 |
| sic | panel sic | 0.9606 | 0.9602 | 0.9681 | 0.9737 | 0.9801 | 0.9818 |
| sue | panel sue | 0.8382 | 0.8012 | 0.8258 | 0.8552 | 0.8558 | 0.8505 |
| denominator cells | line-session cells | 580695 | 611044 | 584009 | 575461 | 573063 | 553772 |

## Known limits and residuals

The French accuracy threshold remains +/-5% and the daily band 3,200–4,800. French remains FAIL: Jan–Apr 2018, June 2020 and Jan–May 2022. Month-by-month overlapping source counters and exclusive exclusion classes are preserved in `.superpowers/sdd/tier1-v4/t2r2-french-reconciliation.json`; they partition our observations but cannot identify missing/extra licensed benchmark constituents. The benchmark's documented line-count convention remains binding; company counts are diagnostic only.

Early 2018 has 277/233/230/215 otherwise-eligible NYSE observations missing positive shares versus deficits 164/126/123/129. These are measured shares-source limitations; no shares or prices are synthesized. June 2020 source continuity recheck finds 89 of 1,155 neighboring-month common lines lack June30 panel, price and original vendor bars (t2r2-vendor-coverage.json). This does not identify exact French missing constituents. No existing authorized independent June 2020 price replacement was established. The 2022 excess remains unexplained at constituent level: incorporation is known on admitted lines, while nonasserting class disclosures remain unknown rather than negative proof. A deterministic independent 28-line-date audit found two demonstrated REIT admission errors and 26 cases without a demonstrated error; it is not population precision or French overlap attribution. Zero same-FINRA-classifier suspects is internal consistency only.

The bounded new source stages publish 1,346 incorporation observations and 49 explicit REIT status observations across 11 issuers. Header/class sources are sparse and stop in 2022. Full 2018–2026 output carries latest known assertions with existing fallback; no uninterrupted status or 2023–2026 conversion/termination coverage is certified. Subsequent source refresh must include dated positive/negative status changes and unknowns. An explicit REIT termination clears stale SIC/panel exclusion only after disclosure availability; historical effective dates are not backdated.

Index flags remain unlicensed proxies without float adjustment and with incomplete IPO/spin-off additions. Task 2x market aggregate returns remain through 2022 only under R5; membership, annual coverage and universe flags span latest. Presence of identity share codes does not prove independent semantic agreement with newly observed class flags; downstream code refresh must review dated class evidence.

Hand judgments bind the exact current seed/rule/sample hash. All 64 affected common/REIT/foreign claims were independently rechecked from originals, and 36 unaffected unchanged ADR/fund/SPAC judgments retained. Pooled 97/100 does not conceal ADR 9/12. Original audit disclosures are evidence only, never backdated model inputs. See task-2-report.md Fix round 2 and standalone issuer_history/issuer_classes docs for provenance, source completeness and remaining obligations.

## Input manifest provenance

| input | SHA-256 |
|---|---|
| classification | c86e761d8c60be50f7796bcacf6da494352390fe84c157470fc4115c51eb5b35 |
| identity/link_table_v3_manifest.json | d461c1d031ddcbbd12ac0edcf8ac6cd3bf27d3420cb0af3e0724b75cf95a2139 |
| identity_cover | be2268beda4fafd3ec33f99b7712983ac8b26d3874b5ff82f95e6e90abe38c32 |
| indexes | 83fef29088798ad1a04dbad4198c4c64a3a30c6138bf01accfb947ea6984ffce |
| issuer_classes | 35c906ba83c05bebc3fc9c4bd403a05393068f8379cfb08f95be5094c3d1958d |
| issuer_history | 46c83410ee7ed899b2aac239335c97d9983a27346df990cc0b4719d98d6f8939 |
| market/market_shares_manifest.json | 41af29e0fa817b0dff57028e027537f850825c18d11c6ed7a449ef5e6a8b0990 |
| panel | c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4 |
| sec_filings | 5190fe99e4c2f1f13218d966a67d06995d51b7a31a73151dad2686e829aed693 |
| security_master | 3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a |
| security_master/share_exchange_history_manifest.json | 5c3c3bd7817ee69ef4f796f8b7789a35eb68d23c67fa28f8f7b1b2b8e9dc82cd |

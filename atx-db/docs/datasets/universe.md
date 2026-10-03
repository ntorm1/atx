# universe (`universe/`)

Rebuilt 2026-10-03, tier1-v4 Task 2 fix round 1 and Task 2x. Rule `universe-all-common-v2`, schema `atx.alpha-panel.universe/v1`; one joinable row per frozen panel key `(session_date, security_id)`. Delisted lines remain through their last panel session. Registry stub `universe.registry.json`; manifest `universe/manifest.json`; validation `validation/universe.json`.

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
| is_reit | BOOLEAN | flag | issuer is a REIT: panel `is_reit` or issuer SIC 6798 (any share type of a REIT, preferreds included) | panel, classification | SIC clock < cutoff | daily |
| is_spac | BOOLEAN | flag | blank-check issuer: panel `is_spac` or issuer SIC 6770 as of the cutoff with zero/missing panel sale_ttm (retained SIC with positive revenue is operating) | panel, classification | SIC clock < cutoff | daily |
| is_cef_etf | BOOLEAN | flag | ETF / ETP / closed-end fund / BDC: panel `is_etf`, `security_type` fund, filer regime `fund` (N-CSR), or latest visible BDC election/support event (814- file number), revoked by latest withdrawal | panel, sec_filings | filing clock < cutoff | daily |
| is_foreign_issuer | BOOLEAN | flag | panel `is_fpi`, regime `fpi_20f` / `canadian_40f`, or incorporation outside the US (cover page strictly before cutoff; unknown without dated evidence) | panel, sec_filings, identity_cover | clocks < cutoff | daily |
| filer_regime | VARCHAR | - | SEC filer regime of `cik` as of the cutoff: `domestic`, `fpi_20f`, `canadian_40f`, `fund`, `none`; NULL unknown / unlinked | sec_filings/filer_regime | `available_at` < cutoff | daily |
| me | DOUBLE | USD | `market/shares_daily.shrout` x raw close of d (NULL when shares unknown) | shares_daily, panel | d close | daily |
| univ_all_common | BOOLEAN | flag | rule `universe-all-common-v2` (below) | all of the above | d 22:00 | daily |
| univ_r3000p / univ_r1000p / univ_r2000p / univ_sp500p | BOOLEAN | flag | `indexes` proxy membership of d (false when not a member) | indexes | rank date 22:00 < cutoff | annual / quarterly recon |
| available_at | TIMESTAMP | UTC | session_date 22:00 | - | - | - |



## Rule and timing

The imported market_index common filter requires operating common/common_unverified lines, excludes ADR/FPI/REIT/LP/royalty trust and indexes, and requires NYSE/Amex/Nasdaq/Arca/BZX listing. Universe adds raw close >= USD 1, positive known shares_daily shrout, no preferred/warrant/unit/right ticker suffix, domestic filer regime, and no foreign issuer, blank check, fund/BDC or REIT flag.

NULL frozen-panel exchanges are repaired only by dated fresh FINRA listing evidence: dissemination_date < session d and age <=45 calendar days. The authoritative security_master.EXCHANGE_BY_CLASS now includes SC -> XNAS, BZX -> BATS and IEX -> IEXG. Codes, market_index, indexes and universe use this mapping; all company caps, ranks and membership intervals were recomputed, with no panel rebuild.

available_at = session d 22:00 UTC. Dated incorporation, SIC, filer regime, links, BDC status and index membership require available_at < lake_cutoff(d), the previous calendar session at 22:00 UTC. Incorporation is unknown when no dated cover is visible; an undated issuer_profile snapshot is never a historical fallback. BDC status is the latest qualifying event strictly before the cutoff, with withdrawal prevailing on a tied timestamp. SIC 6770 only supplements SPAC when sale_ttm is zero/missing, matching the panel rule. Code intervals additionally hold session d and are stamped one second before their first-session cutoff.

T1-b remains the accepted qualification: frozen-panel FINRA classifications can be visible at the prior cutoff, its CIK links can use d 22:00, and Nasdaq directory classifications are a current snapshot. No extra incorporation exception is introduced. Daily-stage consumption uses universe_as_of(lake_cutoff(s)); the strict inequality means the latest stored market row is generally s-2. Formation-date research joins use d close for holding from d+1.

## Measured gates

| gate | result | measurement |
|---|---|---|
| codes_on_all_common | PASS | {"pass": true, "per_year": {"2020": 1.0, "2021": 1.0, "2022": 1.0, "2023": 1.0, "2024": 1.0, "2025": 1.0}} |
| daily_band | PASS | {"pass": true, "per_year": {"2020": {"max": 3505, "mean": 3340.3, "min": 3089, "sessions": 253}, "2021": {"max": 3872, "mean": 3689.3, "min": 3507, "sessions": 252}, "2022": {"max": 3869, "mean": 3732.8, "min": 3561, "sessions": 251}, "2023": {"max": 3701, "mean": 3562.4, "min": 3446, "sessions": 250}, "2024": {"max": 3545, "mean": 3499.0, "min": 3445, "sessions": 252}, "2025": {"max": 3517, "mean": 3430.5, "min": 3316, "sessions": 250}}} |
| french_nyse | FAIL | {"max_abs_dev": 0.1069, "missing_months": [], "months": 60, "months_outside": 27, "outside": {"2018-01": -0.0814, "2018-05": 0.1062, "2018-06": 0.1053, "2018-07": 0.0979, "2018-08": 0.1045, "2018-09": 0.1069, "2018-10": 0.0971, "2018-11": 0.1057, "2018-12": 0.0963, "2019-01": 0.1007, "2019-02": 0.1012, "2019-03": 0.1038, "2019-04": 0.1013, "2019-05": 0.0918, "2019-06": 0.0948, "2019-07": 0.0758, "2020-06": -0.0622, "2021-11": 0.0513, "2021-12": 0.0545, "2022-01": 0.0577, "2022-02": 0.0594, "2022-03": 0.0668, "2022-04": 0.066, "2022-05": 0.066, "2022-06": 0.0516, "2022-07": 0.05, "2022-08": 0.0558}, "pass": false} |
| hand_check | PASS | {"by_stratum": {"is_adr": {"correct": 9, "lines": 12}, "is_cef_etf": {"correct": 12, "lines": 12}, "is_foreign_issuer": {"correct": 12, "lines": 12}, "is_reit": {"correct": 12, "lines": 12}, "is_spac": {"correct": 12, "lines": 12}, "univ_all_common": {"correct": 40, "lines": 40}}, "correct": 97, "lines": 100, "pass": true, "precision": 0.97, "sample_seed": 20261002} |
| r2000 | PASS | {"companies": {"2018-06-25": 1987, "2019-07-01": 1993, "2020-06-29": 1980, "2021-06-28": 1972, "2022-06-27": 1980, "2023-06-26": 1990, "2024-07-01": 1992, "2025-06-30": 1988, "2026-06-29": 1972}, "outside": {}, "pass": true} |


## Published rows

| year | rows | bytes |
|---|---|---|
| 2018 | 2006722 | 15813869 |
| 2019 | 2110686 | 19088007 |
| 2020 | 2192161 | 19783137 |
| 2021 | 2616995 | 22863117 |
| 2022 | 2765910 | 25731721 |
| 2023 | 2615921 | 24346647 |
| 2024 | 2625233 | 24100337 |
| 2025 | 2783952 | 25162589 |
| 2026 | 2151262 | 19681143 |


## Mean daily line counts

| year | sessions | all_common | r3000p | r1000p | r2000p | sp500p | r3000p_in_all_common | is_adr | is_reit | is_spac | is_cef_etf | is_foreign_issuer |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2018 | 251 | 3341.9 | 1552.8 | 523.9 | 1028.8 | 404.5 | 1331.8 | 292.2 | 285.2 | 75.3 | 2161.3 | 451.0 |
| 2019 | 252 | 3442.3 | 2958.4 | 1001.0 | 1957.4 | 529.1 | 2450.9 | 344.1 | 298.8 | 112.5 | 2379.3 | 688.2 |
| 2020 | 253 | 3340.3 | 3000.8 | 1019.0 | 1981.8 | 529.0 | 2373.5 | 369.8 | 294.2 | 208.3 | 2492.6 | 872.9 |
| 2021 | 252 | 3689.3 | 3001.1 | 1027.6 | 1973.5 | 526.9 | 2427.9 | 395.8 | 297.5 | 857.6 | 2716.6 | 1160.8 |
| 2022 | 251 | 3732.8 | 2984.3 | 1024.2 | 1960.0 | 527.1 | 2437.1 | 417.3 | 290.8 | 905.3 | 3001.6 | 1457.0 |
| 2023 | 250 | 3562.4 | 2983.9 | 1009.8 | 1974.1 | 524.7 | 2427.6 | 408.5 | 275.2 | 410.7 | 3325.2 | 1378.7 |
| 2024 | 252 | 3499.0 | 2981.1 | 1008.0 | 1973.1 | 525.3 | 2442.0 | 395.6 | 271.7 | 171.6 | 3817.9 | 1320.5 |
| 2025 | 250 | 3430.5 | 2984.0 | 1012.5 | 1971.6 | 526.1 | 2443.2 | 384.3 | 275.9 | 176.1 | 4602.2 | 1432.3 |
| 2026 | 179 | 3332.1 | 2934.8 | 1008.3 | 1926.5 | 524.5 | 2412.6 | 347.8 | 262.6 | 277.8 | 5586.5 | 1572.7 |


## French NYSE comparison (value only, before 2023)

| month | French firms | our lines | our companies | line deviation | company deviation |
|---|---|---|---|---|---|
| 2018-01 | 1228 | 1128 | 1122 | -0.0814 | -0.0863 |
| 2018-02 | 1226 | 1169 | 1166 | -0.0465 | -0.0489 |
| 2018-03 | 1225 | 1172 | 1169 | -0.0433 | -0.0457 |
| 2018-04 | 1224 | 1165 | 1165 | -0.0482 | -0.0482 |
| 2018-05 | 1224 | 1354 | 1339 | 0.1062 | 0.094 |
| 2018-06 | 1225 | 1354 | 1339 | 0.1053 | 0.0931 |
| 2018-07 | 1226 | 1346 | 1330 | 0.0979 | 0.0848 |
| 2018-08 | 1225 | 1353 | 1340 | 0.1045 | 0.0939 |
| 2018-09 | 1226 | 1357 | 1341 | 0.1069 | 0.0938 |
| 2018-10 | 1225 | 1344 | 1328 | 0.0971 | 0.0841 |
| 2018-11 | 1221 | 1350 | 1332 | 0.1057 | 0.0909 |
| 2018-12 | 1215 | 1332 | 1318 | 0.0963 | 0.0848 |
| 2019-01 | 1211 | 1333 | 1317 | 0.1007 | 0.0875 |
| 2019-02 | 1206 | 1328 | 1311 | 0.1012 | 0.0871 |
| 2019-03 | 1204 | 1329 | 1312 | 0.1038 | 0.0897 |
| 2019-04 | 1204 | 1326 | 1310 | 0.1013 | 0.088 |
| 2019-05 | 1209 | 1320 | 1306 | 0.0918 | 0.0802 |
| 2019-06 | 1213 | 1328 | 1313 | 0.0948 | 0.0824 |
| 2019-07 | 1213 | 1305 | 1289 | 0.0758 | 0.0627 |
| 2019-08 | 1214 | 1258 | 1243 | 0.0362 | 0.0239 |
| 2019-09 | 1210 | 1247 | 1236 | 0.0306 | 0.0215 |
| 2019-10 | 1206 | 1241 | 1229 | 0.029 | 0.0191 |
| 2019-11 | 1205 | 1234 | 1222 | 0.0241 | 0.0141 |
| 2019-12 | 1194 | 1227 | 1214 | 0.0276 | 0.0168 |
| 2020-01 | 1192 | 1220 | 1207 | 0.0235 | 0.0126 |
| 2020-02 | 1190 | 1214 | 1198 | 0.0202 | 0.0067 |
| 2020-03 | 1182 | 1184 | 1169 | 0.0017 | -0.011 |
| 2020-04 | 1182 | 1186 | 1174 | 0.0034 | -0.0068 |
| 2020-05 | 1177 | 1188 | 1175 | 0.0093 | -0.0017 |
| 2020-06 | 1173 | 1100 | 1091 | -0.0622 | -0.0699 |
| 2020-07 | 1167 | 1184 | 1172 | 0.0146 | 0.0043 |
| 2020-08 | 1168 | 1187 | 1175 | 0.0163 | 0.006 |
| 2020-09 | 1171 | 1184 | 1172 | 0.0111 | 0.0009 |
| 2020-10 | 1169 | 1173 | 1162 | 0.0034 | -0.006 |
| 2020-11 | 1168 | 1199 | 1188 | 0.0265 | 0.0171 |
| 2020-12 | 1170 | 1208 | 1195 | 0.0325 | 0.0214 |
| 2021-01 | 1167 | 1203 | 1191 | 0.0308 | 0.0206 |
| 2021-02 | 1170 | 1212 | 1199 | 0.0359 | 0.0248 |
| 2021-03 | 1182 | 1216 | 1202 | 0.0288 | 0.0169 |
| 2021-04 | 1180 | 1214 | 1203 | 0.0288 | 0.0195 |
| 2021-05 | 1184 | 1224 | 1212 | 0.0338 | 0.0236 |
| 2021-06 | 1195 | 1229 | 1212 | 0.0285 | 0.0142 |
| 2021-07 | 1207 | 1232 | 1214 | 0.0207 | 0.0058 |
| 2021-08 | 1211 | 1254 | 1236 | 0.0355 | 0.0206 |
| 2021-09 | 1213 | 1266 | 1249 | 0.0437 | 0.0297 |
| 2021-10 | 1222 | 1265 | 1249 | 0.0352 | 0.0221 |
| 2021-11 | 1229 | 1292 | 1277 | 0.0513 | 0.0391 |
| 2021-12 | 1230 | 1297 | 1281 | 0.0545 | 0.0415 |
| 2022-01 | 1230 | 1301 | 1286 | 0.0577 | 0.0455 |
| 2022-02 | 1229 | 1302 | 1285 | 0.0594 | 0.0456 |
| 2022-03 | 1228 | 1310 | 1294 | 0.0668 | 0.0537 |
| 2022-04 | 1228 | 1309 | 1291 | 0.066 | 0.0513 |
| 2022-05 | 1228 | 1309 | 1292 | 0.066 | 0.0521 |
| 2022-06 | 1241 | 1305 | 1290 | 0.0516 | 0.0395 |
| 2022-07 | 1239 | 1301 | 1285 | 0.05 | 0.0371 |
| 2022-08 | 1236 | 1305 | 1288 | 0.0558 | 0.0421 |
| 2022-09 | 1239 | 1298 | 1282 | 0.0476 | 0.0347 |
| 2022-10 | 1229 | 1287 | 1270 | 0.0472 | 0.0334 |
| 2022-11 | 1224 | 1273 | 1258 | 0.04 | 0.0278 |
| 2022-12 | 1219 | 1266 | 1250 | 0.0386 | 0.0254 |


## Coverage: 30 core fields

beta_input and idio_vol_input measure return-input sufficiency, not actual beta or idiosyncratic-volatility values. Task 6 must publish actual-field coverage. All coverage after 2022 is value/presence checking; no return evaluation is used.

### univ_all_common

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---|---|---|---|---|---|
| ret | panel ret, unguarded | 1.0 | 1.0 | 1.0 | 0.9999 | 0.9999 | 0.9999 |
| prc | panel raw_close | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| me | universe me (shares_daily shrout x raw close) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| adv_63 | market_liquidity adv_63 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.934 | 0.9029 | 0.9203 | 0.9404 | 0.9439 | 0.9469 |
| saleq | panel sale_q | 0.9568 | 0.949 | 0.9541 | 0.9593 | 0.9622 | 0.9654 |
| atq | panel at | 0.9842 | 0.9834 | 0.9854 | 0.9893 | 0.9923 | 0.9947 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.9844 | 0.9838 | 0.9853 | 0.9898 | 0.9922 | 0.9951 |
| ibq | panel ni_q (quarterly net income) | 0.9841 | 0.9819 | 0.9835 | 0.988 | 0.9906 | 0.993 |
| oancf | panel cfo_ttm | 0.9541 | 0.9304 | 0.9504 | 0.9752 | 0.9744 | 0.9744 |
| sic | panel sic | 0.9865 | 0.9856 | 0.9871 | 0.9908 | 0.9938 | 0.9964 |
| ff49 | panel grp_ff49 | 0.984 | 0.9828 | 0.9846 | 0.9886 | 0.9912 | 0.9944 |
| inst_ownership | panel inst_shares (13F) | 0.9798 | 0.9728 | 0.9805 | 0.9832 | 0.9917 | 0.9924 |
| short_interest | panel si_shares (FINRA) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.9896 | 0.9941 | 0.9923 | 0.9927 | 0.9946 | 0.9978 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.8589 | 0.9661 | 0.9681 | 0.9683 | 0.9445 | 0.9429 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.9248 | 0.9146 | 0.9262 | 0.9275 | 0.9247 | 0.9225 |
| iv_atm | panel iv_atm_21d | 0.7337 | 0.7743 | 0.8024 | 0.7939 | 0.7786 | 0.7796 |
| borrow_proxy | borrow_proxy si_to_io | 0.9799 | 0.9728 | 0.9804 | 0.9832 | 0.9916 | 0.9924 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.9813 | 0.9628 | 0.9765 | 0.99 | 0.9874 | 0.9868 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 0.9999 | 0.9998 | 0.9998 | 0.9999 | 0.9999 | 0.9999 |
| cik | universe cik (panel link, else link_table_v3 as of) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| be | panel be (book equity) | 0.9844 | 0.9838 | 0.9853 | 0.9898 | 0.9922 | 0.9951 |
| ni_ttm | panel ni_ttm | 0.9646 | 0.9383 | 0.9557 | 0.9772 | 0.9783 | 0.9792 |
| gp_ttm | panel gp_ttm | 0.6671 | 0.6395 | 0.6593 | 0.6779 | 0.6795 | 0.6886 |
| oi_ttm | panel oi_ttm | 0.8219 | 0.8068 | 0.8298 | 0.8515 | 0.8525 | 0.8567 |
| debt | panel debt | 0.9854 | 0.9842 | 0.9858 | 0.9896 | 0.9928 | 0.9952 |
| che | panel che | 0.9818 | 0.9804 | 0.9825 | 0.9866 | 0.9893 | 0.9921 |
| capx_ttm | panel capx_ttm | 0.9174 | 0.8864 | 0.9013 | 0.9303 | 0.9219 | 0.9225 |
| sue | panel sue | 0.9475 | 0.9104 | 0.9035 | 0.9597 | 0.9644 | 0.9663 |


### univ_r3000p

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---|---|---|---|---|---|
| ret | panel ret, unguarded | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| prc | panel raw_close | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| me | universe me (shares_daily shrout x raw close) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| adv_63 | market_liquidity adv_63 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.8809 | 0.8867 | 0.8972 | 0.9022 | 0.9115 | 0.9139 |
| saleq | panel sale_q | 0.8885 | 0.8994 | 0.9086 | 0.9105 | 0.9182 | 0.9199 |
| atq | panel at | 0.9093 | 0.9227 | 0.9339 | 0.94 | 0.9458 | 0.9485 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.9086 | 0.922 | 0.933 | 0.9395 | 0.9454 | 0.9488 |
| ibq | panel ni_q (quarterly net income) | 0.9089 | 0.9215 | 0.9319 | 0.9383 | 0.945 | 0.9478 |
| oancf | panel cfo_ttm | 0.8965 | 0.9102 | 0.9241 | 0.9333 | 0.9391 | 0.94 |
| sic | panel sic | 0.9127 | 0.926 | 0.936 | 0.936 | 0.9414 | 0.9433 |
| ff49 | panel grp_ff49 | 0.9112 | 0.9242 | 0.9343 | 0.9347 | 0.9395 | 0.9412 |
| inst_ownership | panel inst_shares (13F) | 0.9878 | 0.9858 | 0.9826 | 0.9839 | 0.9867 | 0.9858 |
| short_interest | panel si_shares (FINRA) | 0.9999 | 1.0 | 0.9999 | 0.9999 | 0.9999 | 0.9999 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.9644 | 0.9709 | 0.9739 | 0.9727 | 0.974 | 0.9742 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.8858 | 0.9811 | 0.9829 | 0.9863 | 0.9624 | 0.9558 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.8891 | 0.9035 | 0.9178 | 0.9244 | 0.9294 | 0.9308 |
| iv_atm | panel iv_atm_21d | 0.8272 | 0.871 | 0.8993 | 0.8968 | 0.8929 | 0.8911 |
| borrow_proxy | borrow_proxy si_to_io | 0.9881 | 0.9862 | 0.983 | 0.9839 | 0.9867 | 0.986 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.9995 | 0.9994 | 0.9994 | 0.9996 | 0.9995 | 0.9992 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| cik | universe cik (panel link, else link_table_v3 as of) | 0.9538 | 0.9622 | 0.9755 | 0.9783 | 0.9815 | 0.9813 |
| be | panel be (book equity) | 0.9086 | 0.922 | 0.933 | 0.9395 | 0.9454 | 0.9488 |
| ni_ttm | panel ni_ttm | 0.9064 | 0.9155 | 0.9266 | 0.9352 | 0.9432 | 0.9446 |
| gp_ttm | panel gp_ttm | 0.6475 | 0.6553 | 0.6763 | 0.6832 | 0.6801 | 0.6797 |
| oi_ttm | panel oi_ttm | 0.7692 | 0.7841 | 0.8054 | 0.8103 | 0.8112 | 0.8081 |
| debt | panel debt | 0.9099 | 0.9229 | 0.934 | 0.9401 | 0.9459 | 0.9488 |
| che | panel che | 0.9071 | 0.9202 | 0.9307 | 0.9373 | 0.9433 | 0.9466 |
| capx_ttm | panel capx_ttm | 0.8607 | 0.8693 | 0.8817 | 0.8943 | 0.8952 | 0.895 |
| sue | panel sue | 0.9014 | 0.9071 | 0.909 | 0.9244 | 0.9395 | 0.9402 |


### member_equity

| field | source | 2020 | 2021 | 2022 | 2023 | 2024 | 2025 |
|---|---|---|---|---|---|---|---|
| ret | panel ret, unguarded | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| prc | panel raw_close | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| me | universe me (shares_daily shrout x raw close) | 0.9952 | 0.9929 | 0.9971 | 0.9986 | 0.9982 | 0.9961 |
| adv_63 | market_liquidity adv_63 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| sale_ttm | panel sale_ttm (saleq, trailing 4 quarters) | 0.881 | 0.8592 | 0.8818 | 0.8993 | 0.905 | 0.9061 |
| saleq | panel sale_q | 0.846 | 0.836 | 0.8497 | 0.854 | 0.8565 | 0.8542 |
| atq | panel at | 0.9108 | 0.9115 | 0.9209 | 0.9258 | 0.9295 | 0.9295 |
| ceqq | panel seq (ceqq = seq - pstk) | 0.9084 | 0.9092 | 0.9169 | 0.9229 | 0.9273 | 0.9284 |
| ibq | panel ni_q (quarterly net income) | 0.8599 | 0.8516 | 0.8631 | 0.8682 | 0.8689 | 0.8663 |
| oancf | panel cfo_ttm | 0.8887 | 0.8725 | 0.8984 | 0.9148 | 0.9149 | 0.9141 |
| sic | panel sic | 0.9606 | 0.9602 | 0.9681 | 0.9737 | 0.9801 | 0.9818 |
| ff49 | panel grp_ff49 | 0.9593 | 0.9583 | 0.9664 | 0.972 | 0.9783 | 0.98 |
| inst_ownership | panel inst_shares (13F) | 0.9958 | 0.9933 | 0.9974 | 0.9981 | 0.9984 | 0.999 |
| short_interest | panel si_shares (FINRA) | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| short_volume | panel sv_short_volume (FINRA RegSHO daily) | 0.9975 | 0.9992 | 0.9992 | 0.9993 | 0.9992 | 0.9985 |
| ftd | panel ftd_quantity (SEC fails-to-deliver) | 0.8997 | 0.9918 | 0.994 | 0.994 | 0.9799 | 0.981 |
| earnings_date | panel earn_last_utc (8-K 2.02) | 0.8468 | 0.8364 | 0.8562 | 0.8608 | 0.8619 | 0.8561 |
| iv_atm | panel iv_atm_21d | 0.957 | 0.9715 | 0.9837 | 0.9819 | 0.9788 | 0.9793 |
| borrow_proxy | borrow_proxy si_to_io | 0.9958 | 0.9933 | 0.9974 | 0.9981 | 0.9984 | 0.999 |
| beta_input | >= 126 unguarded daily returns in the line's last 252 panel sessions | 0.9887 | 0.976 | 0.9908 | 0.9948 | 0.9924 | 0.9923 |
| idio_vol_input | >= 15 unguarded daily returns in the line's last 21 panel sessions | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 |
| cik | universe cik (panel link, else link_table_v3 as of) | 0.9801 | 0.9806 | 0.9883 | 0.9933 | 0.9954 | 0.9946 |
| be | panel be (book equity) | 0.9084 | 0.9092 | 0.9169 | 0.9229 | 0.9273 | 0.9284 |
| ni_ttm | panel ni_ttm | 0.8981 | 0.8797 | 0.901 | 0.918 | 0.9211 | 0.9213 |
| gp_ttm | panel gp_ttm | 0.6936 | 0.6885 | 0.711 | 0.7251 | 0.7264 | 0.73 |
| oi_ttm | panel oi_ttm | 0.8056 | 0.7947 | 0.8129 | 0.828 | 0.829 | 0.83 |
| debt | panel debt | 0.9123 | 0.9118 | 0.9212 | 0.9261 | 0.93 | 0.93 |
| che | panel che | 0.9098 | 0.9098 | 0.9181 | 0.9245 | 0.9276 | 0.9282 |
| capx_ttm | panel capx_ttm | 0.8594 | 0.839 | 0.8615 | 0.8804 | 0.8814 | 0.8833 |
| sue | panel sue | 0.8382 | 0.8012 | 0.8258 | 0.8552 | 0.8558 | 0.8505 |


## Known limits and residuals

The French accuracy threshold remains +/-5% and the daily band remains 3,200-4,800. Failed observations remain FAIL in the manifest; source/class diagnostics and month-by-month residuals are in `.superpowers/sdd/tier1-v4/t2-french-reconciliation.json` and task-2-report.md Fix round 1. Exclusive exclusion classes partition our observed NYSE lines; they do not identify the unknown CRSP counterpart of a residual. A licensed line-level comparator is unavailable. Missing historical incorporation is an explicit source-coverage limit.

Index flags are proxies with no licensed official-list overlap, no float adjustment, and incomplete IPO/spin-off additions. They include REITs and other classes deliberately outside all_common. Market-index aggregate returns stop at 2022 under R5; membership and universe/value coverage continue through the latest panel session.

ADR precision is reported per stratum and cannot be inferred from the pooled result. Hand verdicts must match unique sample identities, seed, rule and sample hash; classifier changes invalidate old verdicts until evidence is rechecked. No automatic inheritance of the prior 97/100 claim is permitted.

## Input manifest provenance

| input | SHA-256 |
|---|---|
| classification | c86e761d8c60be50f7796bcacf6da494352390fe84c157470fc4115c51eb5b35 |
| identity/link_table_v3_manifest.json | d461c1d031ddcbbd12ac0edcf8ac6cd3bf27d3420cb0af3e0724b75cf95a2139 |
| identity_cover | be2268beda4fafd3ec33f99b7712983ac8b26d3874b5ff82f95e6e90abe38c32 |
| indexes | 83fef29088798ad1a04dbad4198c4c64a3a30c6138bf01accfb947ea6984ffce |
| market/market_shares_manifest.json | 41af29e0fa817b0dff57028e027537f850825c18d11c6ed7a449ef5e6a8b0990 |
| panel | c19a4d850c3ed9f59d1202e40c7d77b3147def81c45bd676302008a75d5fb2b4 |
| sec_filings | 5190fe99e4c2f1f13218d966a67d06995d51b7a31a73151dad2686e829aed693 |
| security_master | 3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a |
| security_master/share_exchange_history_manifest.json | 5c3c3bd7817ee69ef4f796f8b7789a35eb68d23c67fa28f8f7b1b2b8e9dc82cd |


June30 source gap: 90 NYSE lines present on neighboring May/July2020 common month ends have no June30 panel/price/vendor bar. This is diagnostic source continuity evidence, not benchmark identity proof or permission to backfill. Historical SEC header remediation candidates and unresolved 2021-22 classification hypotheses are documented in task-2-report.md Fix round 1; French FAIL remains.

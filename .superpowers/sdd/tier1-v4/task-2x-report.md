# Task 2x report: SC/BZX repair inside Task 2 fix round 1

Status: DONE_WITH_CONCERNS. Codes, market_index, indexes and universe rebuilt; no foreign panel code/stage rebuilt. Parent Task 2 French accuracy gate still FAIL, with no waiver. Agent/model selection: Codex, gpt-6.1-sol under controller R1; GC6 historical trailer retained.

Code commit `d306e7768f7d72293aa600718a396814c98caba9`; dependent Task 2 code commit `f0ab20f50a5d9fa4e4e8d0a68b23beeba90f51f4`. Final evidence/docs/report commit listed in final response (specific commits only).

## Canonical mapping and timing

Primary [Nasdaq report contract](https://www.nasdaqtrader.com/content/home/help/tsiqtxtkey.pdf) defines historical SC as Nasdaq Capital Market. [FINRA Regulatory Notice 19-33](https://www.finra.org/sites/default/files/2019-10/Regulatory-Notice-19-33.pdf) identifies H as Cboe BZX, and [SEC exchange directory](https://www.sec.gov/about/divisions-offices/division-trading-markets/national-securities-exchanges) identifies Cboe BZX as formerly Bats BZX. The existing warehouse uses XNAS for Nasdaq tiers and BATS for BZX; those MIC conventions are preserved. Source field remains FINRA marketClassCode, not a trading venue or unconditional fallback.

Shared EXCHANGE_BY_CLASS now includes SC -> XNAS, BZX -> BATS (and existing universe IEX repair centrally). Consumers replace only NULL panel exchange from source dissemination_date < session/rank date and <=45-day freshness. Unsupported/stale/source-absent values stay unknown. Existing finra_names.exchange and panel.exchange remain frozen; repair reads market_class directly. Codes preserve first-session stamp and T1-b qualification. Market/index/universe input hashes include source manifests and source-code mapping fingerprints.

Rank computations rerun full eligible company ME aggregation, Russell ranks/bands and S&P buffers; no names were added to previously computed ranks. No return evaluation after 2022.

## R5 inherited-builder issue and correction

Initial `market_index build --end 2022-12-31` at 16:03:57Z computed/published aggregate-return product rows through 2026 because --end only bounded checks. Reported to controller; R5 explicitly forbids portfolio-return products after 2022. Corrected load_universe filters rows before aggregation and rejects later end, then republished at 16:06:07Z. Current output has 5,122 rows and no periods >=2023. Original pre-repair artifact metadata is preserved in t2-before-audit.json; the initial interim run is identified by command/time/guard receipt and 9,022 output rows. Its interim output SHA was not captured before supersession; no interim return values were inspected. No post-2022 return values were inspected, compared or tuned. Downstream Task 6 factor-return products cannot extend beyond 2022 without new authorization; membership and value/presence coverage continue through 2026.

## Gates

| criterion | PASS/FAIL | measurement | command |
|---|---|---|---|
| market replication (3 gates) | PASS | {"mkt_all_common_monthly_rho_2013": 0.9983966080273335, "mkt_rf_monthly_rho": 0.9997303917927841, "smb_monthly_rho": 0.9861019625328911} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.market_index build --end 2022-12-31 |
| indexes R2000 recon count | PASS | {"companies": {"2018-06-22": 2000, "2019-06-28": 2012, "2020-06-26": 2002, "2021-06-25": 1989, "2022-06-24": 1995, "2023-06-23": 2016, "2024-06-28": 2016, "2025-06-27": 2007, "2026-06-26": 2004}, "criterion": "R2000 proxy companies in [1900, 2100] at every reconstitution", "lines": {"2018-06-22": 2019, "2019-06-28": 2080, "2020-06-26": 2056, "2021-06-25": 2054, "2022-06-24": 2048, "2023-06-23": 2066, "2024-06-28": 2065, "2025-06-27": 2056, "2026-06-26": 2046}, "official_overlap": "GAP: FTSE Russell / S&P DJI constituent lists are licensed (not measured)", "outside": {}, "pass": true} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.indexes build |
| daily_band | PASS | {"2020": 3340.3, "2021": 3689.3, "2022": 3732.8, "2023": 3562.4, "2024": 3499.0, "2025": 3430.5} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| French +/-5% | FAIL | 33/60 inside; missing=[] | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| R2000 effective companies | PASS | {"2018-06-25": 1987, "2019-07-01": 1993, "2020-06-29": 1980, "2021-06-28": 1972, "2022-06-27": 1980, "2023-06-26": 1990, "2024-07-01": 1992, "2025-06-30": 1988, "2026-06-29": 1972} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| codes >=99% | PASS | {"2020": 1.0, "2021": 1.0, "2022": 1.0, "2023": 1.0, "2024": 1.0, "2025": 1.0} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| hand precision >=95% | PASS | 97/100; per stratum {'is_adr': {'correct': 9, 'lines': 12}, 'is_cef_etf': {'correct': 12, 'lines': 12}, 'is_foreign_issuer': {'correct': 12, 'lines': 12}, 'is_reit': {'correct': 12, 'lines': 12}, 'is_spac': {'correct': 12, 'lines': 12}, 'univ_all_common': {'correct': 40, 'lines': 40}} | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |


Inherited combined gates: 8 PASS / 1 FAIL (French); technical verifications below are additional evidence.

## Before/after exchange source classes

Baseline panel NULLs are intentionally unchanged. Before source-SC membership exclusion follows old NULL panel-exchange eligibility; original Task 2 report records zero SC R2000 representation. After publication counts are measured from repaired codes/universe flags, with dated source classes on each session. Counts include all observed lines, not just eligible common.

| year | class | source cells | distinct lines | panel NULL before/after | fresh NULL candidates | published exchcd NULL after | all_common cells after | R2000 cells after | R3000 cells after |
|---|---|---|---|---|---|---|---|---|---|
| 2018 | BZX | 55534 | 308 | 55534 | 54356 | 1178 | 56 | 131 | 187 |
| 2018 | IEX | 45 | 1 | 45 | 45 | 0 | 0 | 0 | 45 |
| 2018 | SC | 197883 | 1074 | 197883 | 196833 | 1050 | 126846 | 14214 | 14693 |
| 2018 | UNKNOWN | 72574 | 9373 | 72574 | 0 | 72574 | 0 | 0 | 0 |
| 2019 | BZX | 73800 | 382 | 73800 | 72458 | 1342 | 252 | 252 | 504 |
| 2019 | IEX | 206 | 1 | 206 | 206 | 0 | 0 | 0 | 124 |
| 2019 | SC | 222082 | 1197 | 222082 | 219548 | 2534 | 129733 | 29260 | 30014 |
| 2019 | UNKNOWN | 14744 | 968 | 14744 | 0 | 14744 | 0 | 0 | 0 |
| 2020 | BZX | 89648 | 456 | 89648 | 88798 | 850 | 253 | 382 | 635 |
| 2020 | SC | 253805 | 1469 | 253805 | 251523 | 2282 | 137261 | 36371 | 36992 |
| 2020 | UNKNOWN | 22059 | 1551 | 22059 | 0 | 22059 | 0 | 0 | 0 |
| 2021 | BZX | 119795 | 554 | 119795 | 118528 | 1267 | 252 | 132 | 384 |
| 2021 | SC | 440147 | 2561 | 440147 | 436538 | 3609 | 174316 | 41142 | 41617 |
| 2021 | UNKNOWN | 46981 | 3361 | 46981 | 0 | 46981 | 0 | 0 | 0 |
| 2022 | BZX | 136495 | 620 | 136495 | 134279 | 2216 | 251 | 0 | 251 |
| 2022 | SC | 439371 | 2435 | 439371 | 430989 | 8382 | 163170 | 36821 | 37586 |
| 2022 | UNKNOWN | 20945 | 1469 | 20945 | 0 | 20945 | 0 | 0 | 0 |
| 2023 | BZX | 152380 | 711 | 152380 | 150019 | 2361 | 252 | 0 | 250 |
| 2023 | SC | 365582 | 2150 | 365582 | 362365 | 3217 | 161091 | 34478 | 35430 |
| 2023 | UNKNOWN | 17816 | 1072 | 17816 | 0 | 17816 | 0 | 0 | 0 |
| 2024 | BZX | 184242 | 899 | 184242 | 183050 | 1192 | 286 | 0 | 252 |
| 2024 | SC | 353226 | 1932 | 353226 | 351180 | 2046 | 170853 | 38737 | 39895 |
| 2024 | UNKNOWN | 21746 | 1433 | 21746 | 0 | 21746 | 0 | 0 | 0 |
| 2025 | BZX | 240222 | 1228 | 240222 | 239122 | 1100 | 250 | 129 | 379 |
| 2025 | SC | 366044 | 1895 | 366044 | 364770 | 1274 | 176230 | 44100 | 45544 |
| 2025 | UNKNOWN | 31242 | 2262 | 31242 | 0 | 31242 | 0 | 0 | 0 |
| 2026 | BZX | 234863 | 1713 | 234863 | 233906 | 957 | 179 | 41 | 220 |
| 2026 | SC | 259280 | 1756 | 259280 | 258677 | 603 | 122692 | 35316 | 36265 |
| 2026 | UNKNOWN | 25883 | 2039 | 25883 | 0 | 25883 | 0 | 0 | 0 |


## SC representation at rank-date recomputed R2000 reconstitutions

| effective session | SC lines | SC companies |
|---|---|---|
| 2018-06-25 | 112 | 110 |
| 2019-07-01 | 131 | 129 |
| 2020-06-29 | 169 | 168 |
| 2021-06-28 | 180 | 176 |
| 2022-06-27 | 146 | 143 |
| 2023-06-26 | 156 | 153 |
| 2024-07-01 | 166 | 165 |
| 2025-06-30 | 194 | 193 |
| 2026-06-29 | 231 | 231 |


## Keys, PIT and alignment

| artifact | rows | distinct keys | bytes |
|---|---|---|---|
| indexes/constituents.parquet | 73175 | 73175 | 1026539 |
| indexes/recon_summary.parquet | 61 | 61 | 6971 |
| market/index_returns.parquet | 5122 | 5122 | 78032 |
| security_master/share_exchange_history.parquet | 399328 | 399328 | 2307647 |


Codes cutoff violations 0; interval overlaps 0; index/universe alignment mismatch per year {"2018": 0, "2019": 0, "2020": 0, "2021": 0, "2022": 0, "2023": 0, "2024": 0, "2025": 0, "2026": 0}.

Universe row/key evidence: {"2018": [2006722, 2006722], "2019": [2110686, 2110686], "2020": [2192161, 2192161], "2021": [2616995, 2616995], "2022": [2765910, 2765910], "2023": [2615921, 2615921], "2024": [2625233, 2625233], "2025": [2783952, 2783952], "2026": [2151262, 2151262]}.

## Commands, test evidence and provenance

| job | exact command |
|---|---|
| before audit | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_audit.py before |
| codes | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.identity_v3 codes |
| market R5 final | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.market_index build --end 2022-12-31 |
| indexes | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.indexes build |
| universe final | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.universe build |
| French reconciliation | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_reconcile.py |
| verification | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_verify.py |
| hand evidence binding | .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.4 --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t2_handcheck.py |


| guard receipt | status | cap GiB | peak GiB | wall s | exit |
|---|---|---|---|---|---|
| 20261003T160034-3528 | completed | 0.8 | 0.3918 | 14.0 | 0 |
| 20261003T160212-17664 | completed | 0.8 | 0.4613 | 27.0 | 0 |
| 20261003T160357-6664 | completed | 0.9 | 0.5447 | 26.0 | 0 |
| 20261003T160448-16944 | completed | 0.9 | 0.3929 | 45.0 | 0 |
| 20261003T160607-11508 | completed | 0.9 | 0.5385 | 30.0 | 0 |
| 20261003T160722-15108 | completed | 0.9 | 0.6555 | 265.2 | 0 |
| 20261003T161245-14416 | completed | 0.9 | 0.5236 | 192.2 | 0 |
| 20261003T161717-13484 | completed | 0.4 | 0.037 | 1.0 | 0 |
| 20261003T162105-14416 | completed | 0.9 | 0.6603 | 263.2 | 0 |
| 20261003T162800-12600 | completed | 0.9 | 0.6045 | 93.1 | 0 |
| 20261003T162956-15928 | completed | 0.8 | 0.4347 | 44.0 | 0 |


Tests: 61 passed offline in <10 seconds via the exact pytest command in task-2-report.md Fix round 1. R5 source-filter/output/rejection tests, SC/BZX and unsupported/stale/same-day regressions, market and index eligibility, universe cutoff transitions, identity clocks and issuer future append cases all covered.

Sources: no new raw lake landing, zero downloaded source-data bytes. Mapping verification used primary Nasdaq/FINRA/SEC contracts linked above; existing FINRA semi-monthly and SEC filing/covers/vendor shares/French landings and their receipt/terms/rate-limit provenance are inherited from Task 1/2 reports. Historical original commits, frozen panel SHA and all before/after input hashes/code fingerprints are recorded in task-2-report.md Fix round 1, t2-before-audit.json and t2-after-verification.json. Guard scratch files are file-backed spill, not foreign panel outputs.

Residuals: French FAIL remains unresolved; exact failed months and 60-month exclusive-class/source-coverage reconciliation are in task-2-report.md and t2-french-reconciliation.json. Same-classifier checks are internal consistency only. No claim official-list overlap passes (licensed-source GAP remains), no price/share/link/unknown-exchange coverage invented. Precision retains visible ADR 9/12. Documentation/stubs reflect current product bounds and all source inputs; shared registries untouched.

### Final source-coverage and dated-header assessment

June 2020 residual is -73 lines. A diagnostic cohort of 1,170 NYSE common lines present at both May29 and July31 month ends has 90 missing June30 panel rows; all 90 also lack June30 price-stage and original vendor bars. This is a measured vendor-source coverage hole. Neighboring future observations are diagnostic only and do not backfill features; aggregate French counts cannot prove which candidates belong to the benchmark. Evidence: t2-vendor-coverage.json and its producer.

Missing-incorporation admitted NYSE month-end cohorts contain 1,420 distinct CIKs in 2018 and 1,359 in 2019, union 1,454. All 1,454 have a current CIK file in existing submissions.zip; that archive can supply accession discovery, but its current incorporation profile is not historical evidence. Existing sec_text primary-document metadata covers 2,588 pre2020 filings for 1,277 cohort CIKs, earliest 2018-01-19. Selected parsed sections contain the literal STATE OF INCORPORATION in 15 pre2020 rows for nine CIKs. These are candidates, not verified original submission-header facts: completeness, state value, issuer identity and filing acceptance/publication clocks still require inspection. No complete-header cache or usable pre2018 header coverage has been established by this bounded assessment. No acquisition launched.

A primary EDGAR submission header containing STATE OF INCORPORATION, the issuer CIK and original acceptance timestamp is a feasible dated source after validating publication/revision semantics. One suitable pre-first-eligibility accession per cohort issuer implies a minimum 1,454 candidate header requests before cache reuse; state changes require more, so this is a lower bound rather than a fetch plan. Inspect the nine existing text candidates first; use cached archive accession discovery to locate earlier filings and bounded missing-header requests. Never assign a later filing state to earlier sessions. SEC header backfill should be separately authorized/reviewed and preserve strict cutoff and unresolved unknowns.

For 2021-22 excess, missing incorporation is near zero and the same FINRA classifier's zero suspects proves only internal consistency. Independent candidates are dated original SEC cover/security descriptions and prospectuses for NYSE listed classes, contemporaneous 814-N/814-NW and fund status, completed-SPAC versus retained SIC6770 chronology, foreign domicile/FPI filing history, duplicate share classes, and dated exchange/link validity. French uses its documented count unit; company counts remain a diagnostic alternative. The aggregate benchmark has no constituent identities, so these hypotheses are not established false inclusions. Preserve French FAIL pending controller ruling and independently measured remediation.

Guard commands (all from atx-db with PYTHONPATH=src): `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/SCRIPT`. Vendor coverage: CAP=0.8, SCRIPT=t2_vendor_coverage.py, receipt 20261003T163345-14836, peak 0.0738 GiB, 2 seconds. Header cohort/schema/hash: CAP=0.9, SCRIPT=t2_header_assessment.py, receipt 20261003T163832-2172, peak 0.8454 GiB, 13 seconds. Header text-cache: CAP=0.6, SCRIPT=t2_header_cache_check.py, receipt 20261003T163926-16608, peak 0.2476 GiB, 14 seconds. All completed with exit 0 and no cap hit.

All four published manifest LF-normalized code fingerprints match current modules (individual comparisons in t2-header-assessment.json). Owned directory disk bytes: universe 196,586,801; indexes 3,187,407; market 1,761,260,397 (includes inherited non-return market files). This is directory footprint, not new raw bytes. No foreign output modified.

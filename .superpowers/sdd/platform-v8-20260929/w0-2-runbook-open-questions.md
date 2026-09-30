# W0-2 runbook: open questions (one line each; could not be settled from receipts and code)

1. regsho_threshold stage was republished (live manifest fb073c62..., fields-v9 and L9 pin 68f431f0...): pin the new one and accept that regsho_threshold_days63 coverage will miss the ".02 of the 3-year build" acceptance (no v7.1 candidate reads it), or ask atx-db for the old bytes?
2. build_fundamental_events.py:73 LAST_SUB_QUARTER = "2024q4" is a second seal constant outside W0-1's file list: which lane derives it from the window (must be 2023q4 before R5)?
3. research_fields_sec.py:574-577 (INS_SKIP_MARGIN_DAYS 31) opens insider transactions/year=2024/2024q1.parquet for a role ending 2023-12-29: which lane adds the seal guard before R10/R11?
4. backtest_integrity.py:47 TRAIN_END_EXCLUSIVE = 2023-01-01 (nav_summ ledger append) refuses every 4-year series and is outside W0-1's list and grep gate: who changes it before B0 summ, and does V-1's move of nav_summ.py take it along?
5. Fundamental events for lo3: rebuild on the sealed r4 CIK scope (fundamental-events-v3; strict; lo3 fund coverage may shift for CIKs whose only r4 evidence is dated 2024) or keep fundamental-events-v2 (continuity with lo3-fields-v7; scope used 2024 link evidence)?
6. Is rebuilding identity-bridge-r4 and fundamental events wanted even though every value on 2018-2023 sessions is identical under the reader filter (the rebuild only removes 2024 rows and 2024-only CIKs)?
7. Instrument count of the 4-year union is unknown until R2 (estimate 6,100, range 5,950-6,300; hard bound 8,000); the admission table assumes it.
8. v7.1 max compiled slots: the request says 7; the L8 report says 8 and only 8 reproduces the recorded 1,553,063,994 B: which should the prereg quote?
9. me_company (sum over the issuer's role lines) and sv_ratio126 (ticker map over role lines) can change 2020-2022 values when the union grows: does ruling W0-a apply to candidates that read them (10 value candidates and sv_flow), or should the overlap test exclude them?
10. Floating-point order: will mkt_ret and the VM cross-sectional reductions stay bit-identical when non-member columns are interleaved into the instrument axis (numpy pairwise sums over a wider row)?
11. Additional mass sessions: the 4-year factor-break scan could flag a 2023 session or push a non-mass session past 50 cells with the new columns; the plan stops there, but what is the fallback (rule v2, or accept a changed repair)?
12. research_cycle.py has one runner block: IC-only caps (300 s / 2,560 MiB) need a per-phase override (A-3?) or apply to every bounded phase as drafted.
13. dsr_n "ledger+1" needs A-3; before A-3 the spec needs an integer (38 for B0a, 39 for B0b if B0a is ledgered first).
14. W0-3's protocol line in trials.jsonl carries no "cell"; research_cycle.ledger_cells (research_cycle.py:585) raises on it: A-3 must skip non-cell kinds, or the line needs another file.
15. summ.cells_from_ledger mixes 37 three-year series with 4-year cells: are PBO, effective-N and V[SR_n] over unequal windows acceptable until W0-4 step 4 re-runs exist?
16. Caps for the fields builds (est. 150-200 s, ~1 GB): the runbook uses 600 s / 2,560 MiB; OD-2 covers only the IC pass, so root must rule (or split into two runs).
17. Host memory: the cold u pass is estimated at ~1.6 GB RSS; with the atx-db session active (3.5 GB free per Ruling E-6) the 512 MiB floor may stop the run.
18. B0c when B0a (lo1) wins: linked-operating-v1 refuses --delisting/--delisting-returns (prepare_recent_research.py:665-670, 962-964); is B0c then on lo2, or does lo1 get a delisting option?
19. compare_window_overlap.py interface in the brief has no session cutoff; "sessions before 2022-09-30" needs a flag (e.g. --before) or post-filtering.
20. How --kind signal maps candidate-cache entries between the 3-year and 4-year caches (content-keyed by DSL sha + field payload sha, and B-1/C-1 add role/window keys).
21. Will the candidate cache filled by R13 (pre B/C merges) still hit for B0a after B-1/C-1 change the cache key format?
22. Builder commands for the atx-db stages (earnings_calendar, insider, sec_filings, thirteenf, ftd, regsho_threshold, security_master, short_volume_ext, fundamentals, delisting, identity-bridge-v2-pit) and the FINRA SI asof producer are not in pool-2 receipts.
23. No receipts exist for lo1-fields-v8/v9, the lo3 role and lo3-fields-v7 (built direct); R8/R10/R11 argv are reconstructed from W5a/W5b/U2/L9 reports, v70-lo3.json and the manifests; the fields-v9 --max-rss-mib/--max-seconds actually used are unknown.
24. Sealed-row counters keep keys named "..._2025_..." and, after W0-1, record counts of 2024+ rows in every manifest: rename, suppress, or accept as metadata?
25. Whole-file SHA pinning of multi-year atx-db stages reads their 2024-2026 bytes: is that compliant with "no tool opens a file dated 2024+", or are sealed stage copies required?
26. prepare_identity_bridge.py:97-98 default --check roles include recent-fast-validation-2023-2024-v1 (2024 sessions): should W0-1 change DEFAULT_ROLES (the runbook always passes --role)?
27. The optional bridge --check opens C:/atx/atx-db/data/warehouse.duckdb while the atx-db session is active: allowed, or skip?
28. NAV --max-bytes 1073741824 admission at 1,405 x ~6,100 was not computed (3-year peak 347 MiB suggests it fits).
29. Monitor on the 4-year cells runs without holdings_days/bias (3-year v7-l3-holdings, v7-l4-risk): acceptable for B0, or rebuild them on the new roles first?
30. Coverage acceptance for lo3 fields v9: no 3-year lo3 v9 exists; the 22 W5a/W5b fields have no lo3 reference (compare to lo1 v9, or report only?).
31. Report seal regex (E-4 pending): new artifact names avoid "2024", but candidate-cache hex folder names can still contain it.
32. fit_composition_weights HOLD_BEGIN_NS (2022-01-01) is not in W0-1's list; it only drives screen v3-admit-v1 (B0 uses v4-prior-v1), so presumed harmless: confirm.
33. Validation-window artifacts (recent-fast-validation-2023-2024-v1 and fields v1-v6, VAL u/NAV dirs, 4.1 GB) and recent-projection-v1 physically hold 2024 data: keep, archive or delete (owner)?
34. Disclosure: my first stage-manifest filter printed per-year coverage counts (2009-2026) from earnings_calendar/manifest.json and sec_filings/manifest.json, including 2024-2026 counts (data coverage, not returns or IC); nothing uses them; record in the ledger?

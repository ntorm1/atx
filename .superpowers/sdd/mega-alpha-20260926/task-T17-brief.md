# T17 — point-in-time / look-ahead audit of the v3 data path (read-only)

Context: frozen daily v3 book: TRAIN (2020-22) in-sample gross SR 2.74 -> VALIDATION (2023-24) gross
0.16. Before blaming only overfitting, rule out a TRAIN-specific information leak. Top frozen weights:
si_change_10_s21 (.156, sign -1), vol_of_vol_21_126_s63 (.096), si_change_42_s21 (.077),
year_forward_month_s21 (.062), iv_level_21_s21 (.057), lagged_market_corr_240_s21, low_downside_126_s63,
year_forward_week_s63, earn_car_63_s1 (earnings drift), volume_spike_21_126_s21 (library
`atx-impl/strategies/pv_fields_ic121_v3.json`).

Read-only audit. Work in C:/atx-wt/pool-2 (HEAD d63a7058) READ-ONLY; no edits, builds, commits, or
real-data reads. Write only your report file.

For every data input that reaches a signal or a P&L, establish WHEN it is knowable vs WHEN the engine
uses it. Trace the actual code, cite file:line.

1. Fields producer `atx-engine/tools/prepare_research_fields.py` (+ its source loaders): short interest
   (settlement date vs FINRA publication date ~8 business days later — which one stamps the row, and is
   a publication lag applied?), days-to-cover, SI/shares_out, implied vol (as-of time of the quote vs
   the decision close), IV term slope/change, earnings (announcement date/time-of-day; after-close
   announcements must not be usable at that day's close; earn_car windows), shares_out (period end vs
   filing/acceptance date), market return, size group / mktcap. Any forward-fill / back-fill / interpolation
   or full-sample statistic (e.g. bounds, medians, units rules, factor-break correction) that uses data
   after the decision date? The factor-break repair and units rules: do they use future sessions?
2. Role preparation (TRAIN role v2 was repaired with factor-break-v2; VALIDATION role v1 was not):
   `atx-engine/tools/` or `scripts/` producer of `recent-fast-*` roles (find it), adjusted close/volume
   construction (split factors applied backward are fine for returns; flag anything that makes the
   TRAIN signal inputs differ in kind from VALIDATION's), universe/membership (lagged 63d raw USD ADV
   top 3000?) — is membership decided with future information (survivorship: names chosen because they
   exist later)? Is delisting handled (delisting returns) in both roles?
3. IC runner signal timing (`atx-impl` strategy IC runner, DSL VM, `year_forward_*` seasonality
   operators, `lagged_*`): signal at decision d uses only data <= close of d? Any negative shift or
   centered window? Forward label r[d+2] alignment. Candidate cache keys — could a TRAIN cache entry
   have been produced by an older, leakier field version and reused (key includes field-manifest SHA?)
4. NAV replay timing (`atx-engine` book/replay, targets tool `nav`): decision d -> trade d+1 -> earn
   d+2; stale5 semantics; what price fills at; is the neutralizer (price-risk-v1) fit with same-day or
   future data?
5. TRAIN vs VALIDATION asymmetries of ANY kind (different role versions v2 vs v1, different field
   producer blobs, different coverage rules, missing-predictor handling, capacity).

Deliverable `task-T17-report.md` (same dir as this brief): a table
`input | source | knowable-at | used-at | lag applied | verdict (OK / LEAK / RISK / UNKNOWN) | evidence
file:line`, a section on TRAIN-vs-VAL asymmetries, and, for every RISK/UNKNOWN that needs data to settle,
an exact small bounded Python check for the ROOT to run (read-only, <60 s, what result would confirm or
clear it). Reply with the verdict counts and the top 3 risks only.

# Task W5a: PIT fields from the new SEC-derived atx-db stages (earnings calendar, Form 4, 8-K metadata)

**Pool:** C:/atx-wt/pool-8. `git status` clean, then `git checkout -B feat/platform-v7-w5a-secfields-20260928 <BASE>`, BASE =
`git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules: never build C++, never run the pipeline/IC/NAV or any returns-conditioned
statistic, never spawn subagents. C:/atx is READ-ONLY: you may read C:/atx/atx-db/docs/ALPHA_PANEL*.md and open files under
C:/atx/atx-db/data/alpha_panel/v1/{earnings_calendar,insider,sec_filings,export/identity-bridge-v2-pit} to learn schemas and
to run your builder on them (fields only; never join to returns). Never read validation/VAL/2023/2024/2025 statistics.
Python "C:/Program Files/Python312/python.exe" (pyarrow/pandas as the builder already uses). Trailer `Co-Authored-By:
Claude Opus 5.5 <noreply@anthropic.com>`. Report task-W5a-report.md (<= 40 lines) in the pool-2 sprint dir; reply < 12
lines. Coordinate-free with lane W5b: you add fields to a NEW module `atx-engine/tools/research_fields_sec.py` registered
from prepare_research_fields.py through one small registry hook (<= 15 lines in the main file); W5b does the same with
its own module and hook lines placed under yours; do not otherwise edit prepare_research_fields.py.

**Read first:** docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md (what we asked: D2, D11, D12), C:/atx/atx-db/docs/
ALPHA_PANEL_REQUEST_V7_RESPONSE.md, ALPHA_PANEL_SEC.md, ALPHA_PANEL_STATUS.md; literature-v7.md S6 (earnings-date and
insider families: Savor-Wilson 2016, Barber et al. 2013, Johnson-So 2018, Cohen-Malloy-Pomorski 2012); atx-engine/tools/
prepare_research_fields.py (PIT rules: `available_at < 22:00 UTC of d-1`, lag conventions, manifest schema, coverage
recording, the sv_ratio126 implementation as the template for a new source), the identity mapping used for FINRA and
fundamentals (map atx-db CIK/security ids to role instruments through export/identity-bridge-v2-pit, PIT).

**Fields (each with formula id, lag, min-history, NaN rule; all visible at d only from data available before 22:00 UTC d-1):**
1. Earnings calendar (D2): `ea_days_to_expected` (sessions until the expected next announcement; NaN if none),
   `ea_days_since` (sessions since the last 8-K 2.02 reaction session), `ea_window_pre5` (1 if 1..5 sessions before the
   expected date), `ea_window_post3` (1 if 0..3 sessions after the last reaction session), `ea_delay_days` (Johnson-So:
   announced date minus expected date for the last announcement, signed), `ea_time_of_day` (BMO/AMC code of the last
   announcement).
2. Form 4 (D11): over trailing 126 sessions, `ins_net_buy_ratio` = (open-market buys - sells in shares) / shares_out;
   `ins_n_buyers`, `ins_n_sellers` (distinct insiders); `ins_opportunistic_net` = net buying by insiders classified
   opportunistic under Cohen-Malloy-Pomorski (an insider is "routine" if they traded in the same calendar month in each
   of the prior 3 years; requires 2015+ history: state coverage), `ins_cluster_buy` (>= 3 distinct buyers within 21
   sessions). Transactions are visible from their filing acceptance time (`available_at`), not the trade date.
3. 8-K metadata (D12): `k8_count_63` (8-K filings in 63 sessions), `k8_item_material_21` (items 1.01/2.01/2.05/2.06/
   4.02/5.02 flag in 21 sessions), `k8_days_since_any`.
Manifest: per field the source stage manifest SHA, formula id, coverage per year on member rows (TRAIN years only for
the numbers you print; the builder may compute later years but print nothing about them). Existing fields byte-identical
when the new ones are not requested (test as the sv_ratio126 lane did).

**Tests:** synthetic parquet fixtures for each stage; PIT tests (a filing accepted at 22:30 UTC d-1 is NOT visible at d);
routine/opportunistic classification on a planted history; byte-identity of other fields; manifest fields present.

**Root acceptance:** builder run on the lo1 role with the new fields under the builder caps (700 MiB / 1800 s: state your
estimate), coverage table for 2020-2022, and zero change to the 41 existing payloads. Report: field table (id, formula,
lag, coverage), file:line, root command lines, concerns (e.g. FPI 6-K gap, ADR names).

# task-9 report: announcement-clock earnings (calendar v2, EX-99 actuals, SUE family) — STOPPED

Status: **STOPPED (controller / owner request) during orientation.** Nothing built, nothing fetched, no code written.

## Stopped at

**Done (read-only orientation):**
- Read IMPLEMENTER.md, task-9-brief.md, global-constraints.md, gap-analysis §2 item 7 (binding spec),
  tier1-v3 LANE-RULES.md, foreign-dirty-files.txt, tier1-v3 task-EVT-report.md, progress.md, task-3/10/16 briefs.
- State checks (read-only, 2026-10-02):
  - `atx-db/data/alpha_panel/v1/_tmp/events/fetch_fpi.txt` exists (1.0 MB, 12,138 URLs per the EVT report). It was
    never fetched.
  - `atx-db/data/raw/sec_events/fetch-ledger.jsonl` has 17,103 lines. That is the EVT lane's SEC count. The spec
    says 25,762/60,000 used across lanes, and the cap is `ATX_EVT_SEC_CAP`.
  - `data/alpha_panel/v1/earnings_calendar_v2/` does not exist yet (never built).
  - `earnings_calendar_v2.py` (359 lines) and `events_sources.py` (639 lines) are clean (no uncommitted edits).
    `ea_actuals.py` does not exist.

**Not done (whole deliverable):**
1. FPI 6-K fetch (`fetch_fpi.txt`, ≤ 4 req/s), catalog rebuild, calendar v2 parse and build. Gate:
   `member_coverage[year].covered_share` ≥ 0.95 for 2020+, FPIs included.
2. New `alpha_panel/ea_actuals.py`: parse revenue, diluted EPS and net income from the EX-99 corpus
   (`data/raw/sec-earnings-release`, 1.3 GB), with available_at = 8-K acceptance time. Derived columns:
   - `sue_eps_ann`: actual minus the seasonal random walk, scaled by the std of the prior 8 surprises.
   - `sue_rev_ann`.
   - `ea_lag_days`: rdq to the 10-Q.
   - `ea_ret_3d`: a feature only; no IC or return statistic ≥ 2023.
   Also tests, `docs/datasets/ea_actuals.md` and `ea_actuals.registry.json`.
3. Gates not measured:
   - actuals ≥ 97% precise on 200 releases vs the later 10-Q (within 1%). This needs a manual row-by-row read.
   - recall ≥ 85% of 2.02 releases 2020-2025.
   - SUE on ≥ 90% of member cells.
   - calendar covered_share (gate 1 above).
4. Re-measure Task 3's rdq share against calendar v2 (same-ET-date rule), per year 2020-2025. This is reporting
   only; compustat_std.py is not to be edited.

**Uncommitted files:** none (this report is the only file written, and it is committed).

**Running processes I started:** none (no PIDs).

**Exact next command to resume** (from `C:/atx/atx-db`, `PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`):
first confirm the shared SEC budget with the controller (other lanes hit SEC). Then run the FPI fetch, which is
resumable and skips ledger URLs:

```
.venv/Scripts/python.exe -m atx_db.alpha_panel.events_sources fetch data/alpha_panel/v1/_tmp/events/fetch_fpi.txt
```

Then follow the EVT-report resume steps 2 and 6:
1. `G ... events_sources catalog`
2. `earnings_calendar_v2 --phase parse`
3. `G ... earnings_calendar_v2 --phase build`

After that, write `ea_actuals.py` (tests first).

## Gates

| criterion | status | measured | command |
|---|---|---|---|
| calendar v2 covered_share ≥ 0.95 / yr 2020+ (FPIs incl.) | NOT RUN | — | — |
| actuals ≥ 97% precise, 200 releases vs 10-Q (1%) | NOT RUN | — | — |
| actuals recall ≥ 85% of 2.02 releases 2020-2025 | NOT RUN | — | — |
| SUE on ≥ 90% of member cells | NOT RUN | — | — |
| Task 3 rdq share re-measured vs calendar v2 | NOT RUN | — | — |

## Tests

None run (no code written).

## Files changed

- `.superpowers/sdd/tier1-v4/task-9-report.md` (this file).

## Sources / disk

None landed. SEC requests used by this task: 0.

# AR3 annual coverage independent review

Reviewed commit `938965096a56f229efec8836c23225a70aaf632e` on 2026-09-20 using Codex. Read-only source review against the AR3 brief, program rulings, implementation report and focused test assertions. No tests, warehouse queries, source archive scans, network calls or production edits were performed.

Decision: **two Important findings; no Critical findings.** Repair these before production cohort measurement. Per the program process, the implementer's focused validation and repair report are sufficient; another review pass is not required.

## Important 1 — Year-end membership must use the table's actual inclusive end

`atx-db/src/atx_db/item_coverage_cohort.py:89` uses `ranking_date < u.valid_to`. The existing US-listed interval producer writes the **last included session**: `universe_us_listed.py:266` closes a state-changing interval at `rank_to_date[next_rank - 1]`, and the existing accessor (`:639`) and terminal-policy lookup (`delisting.py:1821`) both use `valid_to >= date`. The interval regression tests explicitly assert that the first interval ends one session before the next begins.

Consequently, a valid common-stock interval ending on the annual ranking session is excluded from the new cohort. This can change the top-3000 constituents or mark a sufficient cohort undersized. The AR3 brief incorrectly describes this particular table as half-open, and the new fixture follows that mistaken description rather than the producer.

Repair the new reader to include `ranking_date = valid_to`; update its contract comment, documentation/report and boundary fixture accordingly. Do not change the established interval writer as part of this repair. Exchange-listing and other identifier tables may still have their own half-open semantics.

## Important 2 — New runtime cohort writes read the wall clock

`atx-db/src/atx_db/item_coverage_cohort.py:149,176` explicitly populate `source_loaded_at` with `now()` in both new runtime insert statements. The program requires deterministic code and no new library clock reads; the implementation accepts an explicit `as_of_date` but still changes these persisted values on identical reruns.

Accept a caller-supplied load/measurement timestamp or use a documented deterministic timestamp derived from the existing explicit options, and bind it in both inserts. Keep migration audit defaults separate from the runtime fix. A small focused assertion that supplied timestamps survive both tables is adequate; no broad clock cleanup is requested.

## What passed static review

- Source and listing availability are restricted to the ranking session plus 22 hours before market ranking; positive finite caps and deterministic security-ID tie breaking produce a separately named annual cohort.
- Explicit years retain missing sessions, undersized cohorts and the incomplete current year. Current metadata is not backdated. Actual listing evidence remains an operational prerequisite, as the report states.
- Numerators join exact `(security_id, fiscal_year)` membership; production aggregation stays in DuckDB. Registered zero-fact items, absent years and zero-valued observations remain visible. Legacy interval support checks `is_member`.
- The annual gate retains 110 items, 90 percent and every completed FY2015+ year, with exactly 3000 constituents. Provider coverage and DQC share the SQL implementation; null status, missing years and stale year evidence cannot pass.
- Cohort replacement and invalidation of prior measurements occur in one transaction, including a same-date rebuild. Measurement replacement deletes an empty requested slice without deleting unrelated years, bases or items.
- No public schema condition is flipped. Existing 109 passing focused checks are implementation evidence, not live cohort or coverage certification.

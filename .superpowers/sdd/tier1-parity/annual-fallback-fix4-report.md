# AF1 fix4: aggregate-result type narrowing

Date: 2026-09-20. Status: actual source stable for root strict mypy/Ruff retry.

Read `annual-fallback-final-mypy.log`. Its six errors were Optional `fetchone()`
results indexed without narrowing in `derived_metrics.py`: input events,
dependency events, target events, annual item events, annual target events,
and the old publication scope. Four count lookups predated AF1; two were its
annual additions.

Only `atx-db/src/atx_db/derived_metrics.py` and this report changed. Each of the
six lookups now stores its existing `fetchone()` result in a local variable,
asserts that the row is not None, then performs the same `int(row[0])`
conversion. Every query is an aggregate `SELECT count(*)` without grouping and
therefore returns one row even for an empty input relation.

The fix has no type ignores, new helper/API names, SQL changes, extra queries,
schema/catalog changes or altered control flow for valid database results.
Query arguments, ordering and execution counts remain identical. Candidate,
frame and publication limits, per-security atomic publication, 1GB memory and
one-thread settings are unchanged. Other root and CF5 edits were untouched.

Source SHA-256 before this fix:
`6c80c099f9029482ee5498e323fc7f89afe908909cb81788e5e7a9a9cdcedecd`.

Source SHA-256 after this fix:
`f953220ea651dd4f60bb1e205046cee2966f89f1aad9582412bd805e20978729`.

Root reported all six fix3 focused selectors passing with peak0.789284GiB and
Ruff passing for all eleven AF1 paths before this mechanical narrowing. Those
are root-owned results. The remaining verification is root's strict mypy over
`derived_metrics.py`, `_derived_annual.py`, `_derived_pit.py`, and
`migrations/bodies_0316.py`, plus Ruff for the changed file.

No new runtime regression was added for this type-only correction. No tests,
imports, DB connections, probes, runtime commands, lint/typechecks, extra
reviews or commits were executed by this implementer. Root acceptance remains
pending its requested typecheck/lint retry; no source edits remain planned.

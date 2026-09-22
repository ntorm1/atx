# AP1 fix1: calendar TTM fixture oracle

Root's focused runtime receipt reported 47 passed / 1 failed, with a 0.953 GiB
guard peak. The failure was the new publication fixture's calendar TTM assertion,
recorded in `fundamentals-publication-tests.log`. This fix was investigated and
edited statically; no test, collection, lint/type, database, or other runtime work
was launched by this implementer.

The calendar SQL intentionally aggregates up to four visible quarters for every
anchor period. It retains partial windows and sets `is_complete` only when
`quarter_count = 4` and coverage is 330–380 days. Revision chains are partitioned
by the calendar-period revision group, so each of the fixture's four periods has
its own latest revision. The existing
`test_calendar_aligned_ttm_emits_shared_calendar_quarter_for_offset_fye` already
asserts that a three-quarter partial window remains stored with
`is_complete = False`.

Only the warranted oracle in live
`atx-db/tests/test_fundamental_publication.py` was changed. It now asserts all four
latest calendar outputs in deterministic calendar-period order:

| Period | Quarters | Coverage days | Complete | Value |
| --- | --- | --- | --- | --- |
| 2024Q1 | 1 | 91 | false | 100 |
| 2024Q2 | 2 | 182 | false | 300 |
| 2024Q3 | 3 | 274 | false | 600 |
| 2024Q4 | 4 | 366 | true | 1000 |

The fiscal TTM assertion remains the single complete 1,000 row. The fixture data,
full-payload rerun comparison, production arithmetic, publication implementation,
and original draft artifacts were not edited. No source fix or Critical issue
was identified.

Root's pending serial rerun selector:

```text
tests/test_fundamental_publication.py::test_all_eight_sql_writers_cross_file_reopens_with_unchanged_payload
```

The correction is not yet runtime-verified by this implementer. Root retains the
runtime slot and the lint/type checks already in progress.

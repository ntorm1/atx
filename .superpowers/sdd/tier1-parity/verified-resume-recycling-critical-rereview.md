# VR1 Critical re-review — C1 closure

## Scope and reviewed artifacts

This is the requested scoped Critical re-review only: C1 (the configured
reopen path in the primary verified-resume test), plus any Critical regression
introduced by its repair. It is static-only. No imports, tests, database
access, profiler, live workload, source edit, or patch application was run.

| Artifact | SHA-256 |
| --- | --- |
| Frozen draft `atx-db/src/atx_db/fundamentals.py` | `31989CC785D403EDB9B828680EB1EA00C942C05D499680D25403EA8C613AA2D0` |
| Repaired draft `atx-db/tests/test_companyfacts_archive_repair.py` | `79D2904FB36E08B2CFC7D51B7FD1E44415AF00B9648E4D7849173571DE3B9C08` |
| Final `integration-final.patch` | `0D268C57499B9738DA4FD85AC4B0ABD2DB011479C2BF3185065CE7E872FB4E34` |

I also read the draft `verified-resume-recycling-fix-report.md` and
`packaging-fix-report.md`. Per the scoped instruction, the previously accepted
Important work was not re-reviewed.

## C1 closure: closed

The repaired primary test sets both required eligibility fields and matching
active-session settings before the observed resume at
[`verified-resume-recycling-draft/atx-db/tests/test_companyfacts_archive_repair.py:516`](verified-resume-recycling-draft/atx-db/tests/test_companyfacts_archive_repair.py:516)
through line 522. This satisfies the guard in
`reopen_companyfacts_store`, which only recycles a file-backed store with both
`analytical_memory_limit` and `analytical_threads` set
(`atx-db/src/atx_db/_companyfacts_resume.py:278-289`).

The repair also observes `reopen` at lines 547-556: it checks that `close`
released the connection, invokes the original reopen, then verifies the saved
memory/thread/preserve-order settings and absence of temporary tables/views.
The close observer remains active at lines 540-545, and the required four
state assertion remains unchanged at line 561:
`[(200, 0), (200, 100), (200, 200), (201, 201)]`.

Static control flow therefore reaches real configured-persistent
`close`/`reopen` calls instead of the prior `False` guard return. C1 is closed.

## Critical regressions from the C1 repair

None found. The repair is test-local: it configures the existing file-backed
fixture and adds observers around the existing close/reopen calls. It does not
alter the frozen production source draft or the counter, archive proof,
lineage, receipt, transaction, candidate, empty, or unavailable paths.

## Static validation limit

This establishes only that the repaired test is wired to the eligible reopen
path and its assertions match the reviewed control flow. Runtime behavior,
DuckDB setting normalization, focused-test results, and any performance or
memory claim require the root-owned focused checks after integration.

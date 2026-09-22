# VR1 static review — verified resume recycling

## Scope and evidence

One independent static review of the frozen VR1 draft only. I read the task
brief, prefix audit, draft report, `integration.patch`, and the complete
relevant draft source/test regions. I did not run Python, imports, tests,
lint, database commands, profilers, or any live workload; I made no source,
test, or patch edits. The reported packaging failure of `integration.patch` is
outside this semantic review and is intentionally not a finding here.

Reviewed SHA-256 values:

| Artifact | SHA-256 |
| --- | --- |
| Live baseline `atx-db/src/atx_db/fundamentals.py` | `A73F51625BF8933744256D8BA1A7BB4A2C67BAD33F2D86DBC5BD932BAA8CC868` |
| Live baseline `atx-db/tests/test_companyfacts_archive_repair.py` | `6DFB5880253584F323DF2F78462DD24F20F6B6EF895856D082A6C1CDBECA74FE` |
| Draft `atx-db/src/atx_db/fundamentals.py` | `31989CC785D403EDB9B828680EB1EA00C942C05D499680D25403EA8C613AA2D0` |
| Draft `atx-db/tests/test_companyfacts_archive_repair.py` | `329E24CC9E73E7CE03BE6B74CEAD5EC620EBD78379F266691CE99B2BEAE2A1BE` |
| Frozen `integration.patch` | `E998A2927AA5FD45F8E36B5E73A1D3B694101468773C9DDD90EE25F706DED80F` |

## Critical

1. **The primary new recycling test cannot exercise a reopen, so its asserted
   four closes and `connection_reopens == 4` are statically impossible.**
   [`verified-resume-recycling-draft/atx-db/tests/test_companyfacts_archive_repair.py:438`](verified-resume-recycling-draft/atx-db/tests/test_companyfacts_archive_repair.py:438)
   configures neither `tmp_store.analytical_memory_limit` nor
   `tmp_store.analytical_threads` before it observes `close` at lines 467–479.
   `tmp_store` constructs a new `DuckDBStore` whose two fields default to
   `None` (`atx-db/src/atx_db/connection.py:62-73`), and
   `reopen_companyfacts_store` returns `False` without calling `close` when
   either is `None` (`atx-db/src/atx_db/_companyfacts_resume.py:278-289`).
   The existing reopening tests explicitly establish both settings first at
   lines 379–382 and 418–421. Thus this test records no close observations and
   reports zero actual reopens, rather than its expected four.

   Fix: before installing the `close` observer, give this file-backed fixture
   the same configured analytical session as the existing reopening tests:
   set `analytical_memory_limit` and `analytical_threads`, apply the matching
   `SET memory_limit` and `SET threads` statements (and the normal
   `preserve_insertion_order` setting if the test asserts session persistence),
   then retain the existing close-state assertions. This keeps the coverage on
   the only eligible configured-persistent path and lets it prove the
   post-proof, raw-triggered, verified-triggered, and final reopens.

## Important

1. **The mixed test mentions an empty replay but does not distinguish whether
   that empty replacement advances the raw counter or causes the final partial
   reopen.**
   [`verified-resume-recycling-draft/atx-db/tests/test_companyfacts_archive_repair.py:441`](verified-resume-recycling-draft/atx-db/tests/test_companyfacts_archive_repair.py:441)
   creates ten non-empty raw replays, line 443 adds the empty CIK 202, and the
   resumed run also performs the non-empty CIK 201 replacement. At the final
   observation in line 479, the raw counter is already nonzero because of CIK
   201. A regression that failed to increment the raw counter for CIK 202 would
   produce the same observations, reopen count, empty-target count, and final
   candidate count. The existing raw-cap test at lines 378–403 covers only
   loaded replacements.

   Fix: add or reshape a focused configured-store case with nine committed raw
   replacements followed by one empty replacement and a subsequent target. The
   subsequent target must observe a cap-triggered reopen, proving the empty
   transaction is the tenth raw replacement. Also keep a case where the only
   work since the preceding reopen is an empty replacement, so the final
   partial-work reopen is attributable to the empty path. Preserve the real
   archive/resume fixture and its candidate, receipt, fact, and point checks.

## Minor

No minor findings.

## Static assessment after the required fixes

The source change itself is narrowly placed: raw replacements advance only
after `_replace_facts` returns at draft `fundamentals.py:1313-1320`; verified
members advance only after their candidate transaction exits at lines
1169-1185; either reached counter calls the existing guarded reopen helper and
resets both at lines 1157-1162; and the final partial-work reopen tests either
counter at lines 1327-1328. The post-proof reopen, archive/lineage/multiset
verification, receipt handling, unavailable-placeholder path, per-issuer
transaction boundaries, and raw fact/point/candidate replacement code are
unchanged by the draft patch. Static evidence supports those invariants but
does not support a live performance or memory-capacity claim.

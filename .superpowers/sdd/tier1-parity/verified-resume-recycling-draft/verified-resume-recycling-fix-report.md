# VR1 review-repair and final packaging report

## Frozen source and changed draft test

| Artifact | SHA-256 |
| --- | --- |
| Live `atx-db/src/atx_db/fundamentals.py` | `A73F51625BF8933744256D8BA1A7BB4A2C67BAD33F2D86DBC5BD932BAA8CC868` |
| Frozen draft `atx-db/src/atx_db/fundamentals.py` | `31989CC785D403EDB9B828680EB1EA00C942C05D499680D25403EA8C613AA2D0` |
| Live `atx-db/tests/test_companyfacts_archive_repair.py` | `6DFB5880253584F323DF2F78462DD24F20F6B6EF895856D082A6C1CDBECA74FE` |
| Repaired draft `atx-db/tests/test_companyfacts_archive_repair.py` | `79D2904FB36E08B2CFC7D51B7FD1E44415AF00B9648E4D7849173571DE3B9C08` |
| Final standard-context patch `integration-final.patch` | `0D268C57499B9738DA4FD85AC4B0ABD2DB011479C2BF3185065CE7E872FB4E34` |

## Review finding resolution

### C1 — Critical: configured reopen path

The primary verified-resume test now sets `analytical_memory_limit` and
`analytical_threads`, applies matching DuckDB session settings, and sets
`preserve_insertion_order=false` before resume. Its reopen observer verifies
that every actual reopen starts closed, restores the configured session, and
has no temporary table or view. The existing four close-state assertion is
unchanged:

`[(200, 0), (200, 100), (200, 200), (201, 201)]`.

### I1 — Important: empty raw replacement cadence

`test_empty_replacement_is_tenth_raw_commit_before_next_target` uses the real
offline archive fixture with nine loaded raw issuers, one empty issuer, and an
unavailable next member. The observed close state includes exactly ten receipts
before that next member can be recorded, proving the empty transaction supplied
the tenth raw count. It retains status/receipt assertions.

`test_empty_replacement_alone_triggers_final_partial_reopen` uses one real
empty archive member. Its only close occurs after the empty receipt is written,
proving the final partial reopen is attributable to empty raw work.

The original real resume fixture still covers receipt admission and ownership,
candidate recovery, verified/raw separation, raw-triggered reset, verified
100-member reset, and final partial reopen. Its candidate-failure rollback test
is unchanged.

## Focused selectors for root

```powershell
pytest atx-db/tests/test_companyfacts_archive_repair.py::test_verified_resume_uses_independent_bounded_recycling_and_preserves_owned_rows atx-db/tests/test_companyfacts_archive_repair.py::test_empty_replacement_is_tenth_raw_commit_before_next_target atx-db/tests/test_companyfacts_archive_repair.py::test_empty_replacement_alone_triggers_final_partial_reopen atx-db/tests/test_companyfacts_archive_repair.py::test_verified_candidate_reconciliation_failure_rolls_back_without_counting_work atx-db/tests/test_companyfacts_archive_repair.py::test_reopen_after_ten_commits_preserves_caps_and_leaves_no_relations atx-db/tests/test_companyfacts_archive_repair.py::test_resume_reopens_after_proof_before_candidate_recovery
```

## Integration

When archive ownership permits, root should check and apply only
`integration-final.patch` from this draft directory. It was checked without
application in both default Git mode and `core.autocrlf=false`. No runtime,
imports, tests, lint, database work, profiler, or live edit occurred in this
repair. No performance or capacity claim is made.

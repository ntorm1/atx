# CF5 independent static review

Reviewed the five frozen CF5 source/test paths against base `3ce0db14`, the
repair brief/report, and archive3's retained failure evidence. Dependency source
and schema were read only to establish contracts. No tests, imports, database
connections, probes, network requests, source edits, commits or subagents were
used. This is the requested single review pass.

## Critical

None identified.

## Important

1. **The skip proof ignores additional points owned outside the resume lineage.**
   `atx-db/src/atx_db/_companyfacts_resume.py:210` filters the point scan to
   lineage run IDs, and `:219-223` checks only the resulting run/security groups.
   A loaded receipt with its expected fact and point rows therefore still passes
   if an additional stale SEC point for the same security and filing keys remains
   under a different or NULL run ID. The all-owner fact count at `:177-180` does
   not detect a point-only extra. The verified branch at
   `atx-db/src/atx_db/fundamentals.py:1159-1180` then skips the issuer, preserving
   that extra row, whereas an ordinary replacement would remove it. This is an
   unsafe skip under contradictory retained point evidence. Verify relevant
   point multiplicity across owners before authorizing the skip; retain bounded
   security/issuer aggregates. Add foreign-run and NULL-run extra-point fixtures
   to `atx-db/tests/test_companyfacts_archive_repair.py:128`; its current damage
   cases only change/delete the expected lineage rows.

2. **A source-incomplete pass cannot resume from its own newest dataset UUID.**
   `atx-db/src/atx_db/_companyfacts_resume.py:69-71` accepts only dataset status
   `failed`. However, source errors accumulate while the loader returns normally
   at `atx-db/src/atx_db/fundamentals.py:1373`; the existing `Dataset.run` contract
   records that return as `succeeded`. Activation subsequently reports failure
   at `atx-db/src/atx_db/activation.py:588-589`. Thus a full pass that commits a
   large new tail but encounters one malformed/mismatched member cannot use its
   UUID for the next verified resume. Referencing archive3's older UUID replays
   the newly completed tail because its receipts belong to the rejected newer
   owner. Make the durable source-incomplete outcome eligible for a safe resume,
   while continuing to retry its error receipts. The mixed fixture at
   `atx-db/tests/test_companyfacts_archive_repair.py:75-109` already ends in this
   state; extend it with a second resume from the returned UUID and assert that
   the completed tail is skipped and the source error is retried.

## Minor

None identified.

## Other conclusions and validation limits

The recorded archive3 path/options and old receipt shape are compatible with
the new verifier. Shared fact/point projection types match the retained schema.
Normal issuer replacement places facts, points, candidate replacement and its
successful receipt in one transaction. Loader relations are released before
the configured persistent store is recycled; proof buffers and candidate output
are bounded by identities and issuer work. Missing payload CIK handling retains
exact member provenance and rejects explicit invalid/conflicting values.
Activation forwards its configured User-Agent explicitly.

Root reported all 42 new cases passed and 125/126 focused cases passed, with the
remaining existing activation test affected by the explicit dummy-UA environment
precondition. The reviewer did not run or independently verify those results.
Small fixtures do not establish full-archive runtime or peak memory under the
production 1 GB/one-thread budget.

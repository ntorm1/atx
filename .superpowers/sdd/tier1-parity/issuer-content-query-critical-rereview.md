# Issuer-content Critical-only re-review

Review basis: static re-review of
`issuer-content-query-draft/integration-final.patch` (SHA-256
`c2edf0faadde4e990ab3bfb5a01ca83e0d61c3627971fc43cc4b24e54d426829`), its
current draft mirrors, `fix-report.md`, `task-state.md`, and the prior
`issuer-content-query-review.md`.  No imports, tests, database access,
network access, or production-source edits were performed while the loader is
active.

## Critical

### C1 — the repaired collision fixture cannot execute, so it cannot prove the required no-leak assertion

The repaired owner-discovery query unconditionally reads
`fundamental_fact_revisions.as_of_date`:

```python
# atx-db/src/atx_db/api/service.py:479-494
FROM fundamental_fact_revisions
WHERE coalesce(available_at, source_loaded_at) <= ?
  AND coalesce(as_of_date, CAST(coalesce(available_at, source_loaded_at) AS DATE))
      <= CAST(? AS DATE)
```

However, the restored fixture creates that table with only `security_id`,
`cik`, `available_at`, and `source_loaded_at`
(`atx-db/tests/test_issuer_content_query.py:32-37`; the final-patch equivalent
is lines 720-725).  Its inserts also supply only those four values
(`:77-84`; final patch `:764-771`).  The first collision test calls
`issuer_content_range` (`:123-128`), which calls `_issuer_owner_ids`
(`atx-db/src/atx_db/api/service.py:555-556`) before any assertion.  DuckDB
therefore cannot bind `as_of_date`, and the fixture reaches none of its
collision/no-leak assertions at `:130-142`.

This was introduced by the repair: the original `integration.patch` owner
lookup filtered `fundamental_fact_revisions` only by normalized CIK and
availability (lines 38-43), whereas the final patch adds the `as_of_date`
predicate (final-patch lines 49-52) without updating the fixture schema.

Impact: the focused test required by the task state fails before proving that
CIK-B's `cross-cik-leak` derived row cannot appear in the CIK-A response; it
also cannot exercise the retained later NULL state.  Add `as_of_date DATE` to
the fixture table and provide corresponding values in its rows (or otherwise
make the query and fixture schema consistent), then run the focused test only
after the writer is terminal.

## Original Critical disposition

The production-query closure is otherwise present statically, but cannot be
accepted until C1 restores an executable fixture.

* Complete visible owner CIK sets: `_issuer_owner_ids` builds `visible` from
  every content-clock-visible `fundamental_fact_revisions` row, selects owners
  that have the requested normalized CIK, then returns every non-null
  normalized CIK for those owners (`atx-db/src/atx_db/api/service.py:473-498`).
  Derived eligibility is exactly `owner_map[owner] == (normalized_cik,)`
  (`:555-559`).
* Collision exclusion and machine-readable state: for a derived schema,
  `_issuer_content_rows` replaces the owner list with `derived_owners` and
  returns no rows if that list is empty (`:584-591`).  Metadata publishes
  `issuer_owner_ciks`, `derived_issuer_owner_ids`,
  `excluded_derived_owner_ids`, and switches the status to
  `ambiguous_owner_cik_collision` (`:667-684`).
* Exact-CIK relations: only derived metrics omit the CIK condition; every
  CIK-bearing issuer table receives
  `_normalized_cik_sql('b.cik') = ?` (`:609-616`).
* NULL and clock behavior relevant to this closure remains intact: derived
  rows are content-clock-filtered before revision ranking (`:642-649`), then
  the requested period range is applied after `_revision_rank = 1`
  (`:650-651`); there is no non-NULL predicate.  Owner discovery uses the same
  content clock (`:479-494`).  Ticker lookup remains a separate
  `issuer_lookup_as_of` input (`:540-551`), so this repair does not merge the
  lookup and content clocks.

The intended fixture includes the two visible raw mappings for
`OWNER_COLLIDING` (`tests/test_issuer_content_query.py:79-83`), the CIK-B-only
derived value `999.0` (`:102-107`), and assertions for the collision metadata
and CIK-A-only NULL result (`:130-142`).  Those are the correct target
assertions, but C1 prevents them from executing.

## Narrow non-Critical follow-up

Both owner-CIK discovery implementations select raw visible rows without SQL
deduplication and only deduplicate after `fetchall()`: the service query uses
`SELECT visible.security_id, visible.normalized_cik`
(`atx-db/src/atx_db/api/service.py:488-498`) and the as-of helper does the
same (`atx-db/src/atx_db/asof/fundamentals.py:388-398`). A source owner with
many visible fact revisions therefore transfers duplicate owner/CIK pairs into
Python before the final `set`. This does not change the exclusivity result,
but `SELECT DISTINCT visible.security_id, visible.normalized_cik` would bound
the transferred result to owner/normalized-CIK pairs. This is a performance
follow-up only and does not alter C1 or the original Critical disposition.

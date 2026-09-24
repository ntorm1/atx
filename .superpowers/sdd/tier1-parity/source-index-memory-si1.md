# SI1: optional source-index memory repair

Date: 2026-09-24. Branch: `feat/tier1-parity`.

Research/production question: can the full retained CompanyFacts archive finish
issuer-by-issuer replacement under a bounded DuckDB memory limit while keeping
filing history, provenance and fact/point equivalence intact?

## Evidence and scope

- The archive13 attempt with a 512 MB DuckDB limit verified 9,462 predecessor
  targets / 39,457,715 rows, then failed at the first new raw replacement COMMIT
  (488 MiB limit in the reported OOM). The warehouse contains 47,941,000 source
  facts and points. This establishes a production write blocker, not proven
  causality by any individual index.
- The pre-repair `schema.py` defined both source tables without a PK/UNIQUE
  constraint, and created exactly the three optional nonunique indexes below. The source loader
  already replaces one CIK in a transaction, with matching point deletions and
  an atomic source receipt. Those semantics are unchanged.
- Migration 0318 already removes optional fundamentals indexes after checking
  catalog table identity and uniqueness. Migration 0314 provides the analogous
  price-publication precedent. Migration 0316 is annual provenance, not an index
  removal migration.
- Root's `source-index-catalog1.json` confirms all three expected `main` index
  names, table names and two-column expressions, each nonunique/nonprimary.
  Live constraints are only NOT NULL: five on `fundamental_points`, nine on
  `sec_company_facts`. Schema remains 322. The read left warehouse size
  (12,883,341,312 bytes) and mtime unchanged; its guard receipt reports a
  0.101497650 GiB process-job memory peak under a 1 GiB cap.
- [DuckDB's current indexing guide](https://duckdb.org/docs/current/guides/performance/indexing)
  states that ART index scans require a single-column, non-expression index;
  indexes do not accelerate joins or aggregations, and their loaded buffers
  cannot currently be evicted by the buffer manager. All three candidate indexes
  contain two columns. This supports retiring them as a targeted physical repair
  without claiming that index residency alone caused the measured OOM.

## Implemented candidate

Migration 0326, `bounded_source_indexes`, validates every present candidate
against the expected table and rejects UNIQUE/PRIMARY indexes before dropping
any index. It then drops only:

| Index | Table |
| --- | --- |
| `idx_sec_company_facts_security_asof` | `sec_company_facts` |
| `idx_sec_company_facts_entity_asof` | `sec_company_facts` |
| `idx_fundamental_points_metric_asof` | `fundamental_points` |

The same three CREATE INDEX statements are removed from base bootstrap.
Historical migration bodies remain unchanged: their past creations are followed
by 0326 during a fresh bootstrap. No rows, columns, table definitions,
constraints, source clocks, receipts or resume-proof rules are changed. No
source table is rewritten. The migration is idempotent.

Removing optional indexes can change query latency. Their names are not used
by query code, and their removal does not change SQL results; production scan
and write timings remain to be measured. This is a plausible physical memory
repair, not evidence that the entire archive now fits in 512 MB. Sustained host
headroom, memory guards and full-universe scope remain required.

## Acceptance and current verification

`tests/test_source_index_memory.py` covers:

- Original/amended EPS and missing-value rows remain identical, with correct
  historical as-of visibility; type/default/NOT NULL metadata is unchanged.
- Required NOT NULL, unrelated PK and UNIQUE constraints still reject invalid
  writes, and unrelated indexes survive.
- Repeated migration calls are harmless; a reserved name on a UNIQUE index or
  wrong table is refused before any candidate is dropped.
- The real current-schema bootstrap records 0326, and explicit bootstrap reentry
  preserves populated source history without recreating the retired indexes.

Root ran the three isolated SI1 tests together with SM1's six focused tests:
`source-memory-focused1.log` records **9 passed, 1 deselected in 2.38 s**.
`source-memory-focused1-memory.json` records a successful 1 GiB guarded job,
with **0.595832825 GiB** native peak. Ruff passed for the owned files.

The remaining real-bootstrap integration check subsequently passed with related
connection/recovery contracts and the public API snapshot:
`source-memory-integration1.log` records **17 passed in 155.07 s**. Its guard
receipt records **0.840614319 GiB** native peak under a **1.5 GiB** cap. This
includes the actual fresh schema, source-history preservation and bootstrap
reentry assertion. Root's one independent review,
`source-index-memory-review.md`, is clean; its then-pending integration check is
now satisfied by this receipt. No further review is due for this unchanged code.

Governed migration with backup and measured production archive completion remain
pending. The SI1 implementer launched no runtime.

Static seed audit against real schema and historical ALTERs confirms that `_seed`
supplies every required source column without a default. Both omitted required
`source_loaded_at` columns default to `now()`. Later-added source `as_of_date`
and point `item_id` columns are nullable. The subsequent real-bootstrap runtime
also exercised this seed successfully.

Static diff inspection confirms the base schema edit is exactly three line removals.
No public snapshot change is needed: migration body modules remain hidden by the
existing migration package facade pattern.

Post-migration acceptance query (expected zero rows; count source tables before
and after using the bounded runtime audit, then resume the existing full archive):

```sql
SELECT table_name, index_name, is_unique, is_primary
FROM duckdb_indexes()
WHERE database_name = current_database()
  AND schema_name = current_schema()
  AND index_name IN (
    'idx_sec_company_facts_security_asof',
    'idx_sec_company_facts_entity_asof',
    'idx_fundamental_points_metric_asof'
  );
```

The production outcome is only accepted after the repaired source writer commits
new issuer replacements under its declared cap, preserves raw/point equivalence
and source receipt durability, and the full pinned archive completes or exposes
a separately measured blocker. No release or memory-floor reduction is claimed
by this candidate alone. `stash@{0}` and all unrelated work are untouched.

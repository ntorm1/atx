# Concept catalog memory review

Date: 2026-09-20. First independent Codex source review of helper commit `8a95a3ec9a06ee9cef623b8676ecaef258a20e4c` and facade commit `c401fb31d02f5c0f8cec0751307f86a8b6862a5f`, including both implementation reports, test source, the prior implementation, schema, and transaction helper.

**Verdict: accepted within the catalog scope. Critical: none. Important: none. Minor: none.**

`xbrl_catalog.refresh_concept_catalog` groups raw facts inside DuckDB by the existing `(source, taxonomy, concept)` key before fetching results. Python receives concept-level rows and their distinct categorical metadata, then formats the existing JSON/category fields. The raw fact frame and its pandas grouping are gone. `fundamentals.refresh_xbrl_concept_catalog` delegates directly to the helper, so existing activation/public callers reach this implementation without changing their interface.

The aggregate preserves the inspected catalog contract: NULL/empty taxonomy/concept exclusion; independent first-non-NULL label and description selection using physical `rowid`; sorted distinct nonempty unit/form/fiscal-period strings with the existing JSON serializer; the existing category heuristic and precedence; total fact counts; distinct security/accession counts excluding NULL while retaining empty strings; and native date/timestamp min/max. Source and taxonomy remain part of grouping and publication identity. The SQL makes the prior physical first-value convention explicit rather than depending on parallel result order.

Publication stages only aggregate rows. Temporary-table creation, staging inserts, matching-key deletion, and final insertion occur inside `DuckDBStore.transaction`, which rolls back on exceptions. Absent catalog keys remain untouched, and an entirely empty aggregate returns before mutation, matching the prior upsert behavior. The `finally` cleanup removes the staging table. The regression source includes a destination-constraint failure after deletion and asserts rollback plus temporary-table cleanup.

The 16 reported helper cases cover metadata/count/date/JSON semantics, category branches, source/taxonomy separation, empty/absent inputs, repeated results, rollback, and a 100,000-fact aggregation with DataFrame fetch methods forbidden. The separately reported facade case proves exact delegation using a store sentinel with no SQL/DataFrame API. These tests were inspected, not rerun.

The controller-authorized read-only evidence recorded in `catalog-memory-report.md` reports the exact helper SELECT aggregating 31,590,760 facts into 242 rows in 26.031 seconds under a 1 GB DuckDB limit and one thread, at approximately 1.04 GiB peak working set / 1.05 GiB peak commit. This is prior evidence, not a measurement performed by this reviewer. The helper leaves memory/thread settings to its caller, and a DuckDB query limit is not a process memory cap.

No tests, database connections, network requests, or live writes were performed for this review. Acceptance covers this catalog aggregation and facade only; it does not certify later activation stages' memory use. The separately identified calendarization materialization remains a controller-owned follow-up.

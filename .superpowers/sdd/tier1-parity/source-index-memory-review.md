# SI1 independent review — 2026-09-24

Root's one independent review is clean. Migration0326 validates all expected
index identities before the first drop and refuses unique, primary or
wrong-table collisions. Exactly three optional source indexes are removed;
the table definitions, source rows, NOT NULL and logical identity constraints
are untouched. Fresh bootstrap omits exactly the same three creation lines.
Registry integration is sequential and the module remains internal.

The live read-only catalog receipt source-index-catalog1.json confirms the
expected main-schema, nonunique/nonprimary indexes, nine fact NOT NULL and
five point NOT NULL constraints, schema322 and an unchanged warehouse file.
Three isolated physical-contract/PIT/refusal tests passed in the combined
nine-test batch; scoped Ruff passed. Real bootstrap/reentry acceptance remains
pending at review. It subsequently passed in source-memory-integration1's
17passes155.07seconds,0.840614319GiB peak under1.5GiB. Public API snapshot also
passed. Accepted; no Critical repair/re-review. No live write-performance
benefit is yet claimed.

DuckDB's current indexing guide documents that multicolumn ART indexes are
ineligible for index scans and their buffers cannot be evicted. This supports
the physical-design choice but does not substitute for actual load evidence:
https://duckdb.org/docs/current/guides/performance/indexing

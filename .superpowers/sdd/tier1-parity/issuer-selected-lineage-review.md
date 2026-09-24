# IQ2 independent static review

Scope: the specified uncommitted issuer service, as-of, root lineage, docs, and tests. No runtime, database, Python, network, or source edits were performed. The codebase graph tools were unavailable, so I read the named diff and targeted source directly.

## Important

1. **The focused proof does not cover several required failure paths.** `test_issuer_selected_lineage.py:18-108` has one service/as-of scenario with one direct capex leaf per root. It exercises a foreign CIK, a valid direct leaf, a later NULL revision, and an expired first root. It does not exercise a selected metric dependency, a source/definition/clock mismatch, a pre-0323 missing-column controlled error, or aggregate batch splitting. Its one rejected row before `limit=1` does not cross a 64-row page. Add small targeted cases using the same fixture style: a dependency whose selected leaf or definition fails; a tampered source/definition/clock; a schema without the 0323 ref columns; and at least 64 rejected candidates before one valid row. Assert API and as-of outcomes where the surfaces overlap. These are behavior checks for the new branches in `derived_lineage.py:398-459`, `api/service.py:729-771`, and `asof/fundamentals.py:521-570`, rather than mirrors of internal code.

2. **The fixture does not enforce the required tiny DuckDB resource envelope.** Both `test_issuer_content_query.py:18` and `test_issuer_selected_lineage.py:22` open a file database through `duckdb.connect(str(path))` without `memory_limit='256MB'` and `threads=1`. Set those options on every writable fixture connection and, if the read-only serving connections use their own connection defaults, set the fixture database's persistent configuration or use an appropriately scoped connection mechanism. The brief explicitly requires isolated 256MB/one-thread fixtures; the process-tree guard alone does not establish that property.

## Moderate

3. **As-of loses its DataFrame column contract when every candidate is rejected.** `asof/fundamentals.py:558` returns a bare `pd.DataFrame()` when `frames` is empty. Before this change, a visible-owner query returned the SQL result's columns even for zero rows. For a legacy-only or foreign-only owner, callers cannot inspect `derived_value_id` or other expected columns, and `asof["derived_value_id"]` raises. Preserve the query's column names from the first fetched page when the accepted set is empty; retain the diagnostic attrs.

4. **The as-of truncation flag can claim a missing tail at exactly 50,000 accepted rows.** The loop condition at `asof/fundamentals.py:522` stops as soon as the accepted count reaches `max_rows`, and the `while ... else` at `:556-557` sets `derived_lineage_scan_limited=True` even if that final page exhausted the SQL result. Probe one further bounded candidate page or distinguish reaching a cap from proving that unseen candidates exist. This matters because callers use the flag to decide whether a result is complete.

No selected foreign numeric leak or old-value resurrection was apparent in the inspected code path: whole revisions are ranked before the period filter, and both readers pass selected root IDs to the shared DL1 helper. Runtime validity is pending the root-owned guarded check.

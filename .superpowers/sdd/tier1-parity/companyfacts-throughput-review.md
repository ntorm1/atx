# Companyfacts CIK spelling throughput review

Fresh independent static review on `feat/tier1-parity`, 2026-09-20. Reviewed the uncommitted changes in `atx-db/src/atx_db/fundamentals.py`, new `atx-db/tests/test_companyfacts_cik_spellings.py`, and the paired throughput brief and implementation report. Read the existing target normalization, transaction context manager, issuer-cleanup tests and ZIP tests only as supporting context. Graph tools were unavailable in the exposed catalog, so discovery used source reads.

This reviewer performed no imports, tests, database connections, runtime probes or live-job operations, and made no implementation edits or commits. Runtime validation remains exclusively with the root controller. The separate malformed SEC member investigation is outside this review.

## Findings

- **Critical: none.**
- **Important: none.**

The patch is acceptable on static review. No correction or second review pass is requested. Root-run tests and production throughput/memory measurements remain necessary validation; this review does not claim they passed or that the optimization produced a measured speedup.

## Correctness assessment

At `fundamentals.py:956-973`, the inventory groups exact raw strings under SQL-derived BIGINT keys. Its `regexp_full_match(trim(cik), '[0-9]+')` and `try_cast(cik AS BIGINT)` expressions are the previous matching rules. NULLs, empty or rejected strings, and BIGINT overflow cannot enter an alias tuple. Original strings are retained verbatim, with no Python replacement for SQL whitespace, digit or cast semantics. SQL DISTINCT removes duplicate fact spellings before fetching; no per-fact keys or rows enter the global Python dictionary. All returned aliases are retained without a cap.

At `fundamentals.py:1241-1265` and `1306-1308`, old-key staging and raw deletion use the same predicate and bound parameter list. An alias tuple therefore selects precisely the inventory's accepted stored spellings for the requested numeric CIK. `None` preserves the independent caller's fresh numeric lookup; `()` deliberately produces `FALSE` and a typed empty old-key table. There is no interpolated raw CIK value. Unrelated CIK strings do not acquire deletion eligibility through this change.

The existing point DELETE, source restriction, exact identity/key relation and all six NULL-safe comparisons at `fundamentals.py:1267-1305` are unchanged from the issuer-key repair. Legacy ticker discovery and candidate cleanup continue to normalize their own CIK columns, so an alias present only in either smaller table is still handled. The deferred point-ID prefilter is correctly outside this patch's implemented scope.

At `fundamentals.py:1036` and `1121-1138`, the inventory is lazy and local to each `load` call. Source failures bypass its creation and replacement. `resolve_companyfacts_targets` canonicalizes and deduplicates targets, normalization inserts that target CIK, and identifier resolution does not rewrite it. A nonempty successful replacement appends the known canonical spelling only after `_replace_facts` returns. That return follows transaction commit in `connection.py:269-279`; either insertion failure propagates before cache advancement. Successful empty replacements add no spelling. Retaining deleted aliases is harmless within the stated writer contract, and the next load/retry rebuilds the inventory.

The cache assumes no independent writer introduces a previously unseen raw spelling during the load. Such a writer could leave rows outside the snapshot's aliases. The brief explicitly excludes that concurrency model, and the implementation report and source identify the serial writer path. This is an operational assumption, not a persistent-cache defect. A future concurrent-writer requirement would need synchronization or fresh matching.

## Test and resource assessment

The new eight parametrized cases cover cached/default alias equivalence, duplicate spellings, excluded strings and overflow, independent ticker/candidate spellings, empty-tuple behavior, rollback after both insertion stages, successful retry, cache advancement and freshness across loads, and failure without cache advancement. The rollback fixture inserts the newly canonical CIK before raising, so it checks rollback of the insertion as well as restoration of deletions. Existing ZIP tests exercise the cached load path with another CIK sharing filing keys, successful allowlist-empty cleanup, and historical versus fallback point IDs. Existing cleanup tests retain the richer NULL-key and 250,000-row/64 MB scenarios. These observations come from reading tests, not executing them.

Python memory scales with distinct valid raw CIK spellings and issuer keys, including transient fetched rows, lists and tuple construction, rather than fact-row count. The documented controller observation of 6,533 distinct existing raw CIKs is compatible with this design; it is not a fixed upper bound. The inventory still needs a SQL scan/aggregation, and direct IN predicates only permit storage pruning. The unchanged point DELETE and other per-target work can still dominate runtime.

Root should record the already planned focused-suite results and comparable production batches with inventory cardinality/time and peak memory before claiming throughput improvement. The root-reported interrupted archive2 state (987,977 attempt rows across 146 CIKs; 31,837,696 total old plus new facts) is retained production state, not a completed archive or a performance result for this uncommitted patch. No additional implementation change is requested by this review.

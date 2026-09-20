# Companyfacts issuer cleanup repair

## Failure and scope

The full 20,390-CIK archive replacement failed in the first issuer's point cleanup after 68.375 seconds. `activation-companyfacts-archive1-launch2.err` identifies the correlated `DELETE FROM fundamental_points` in `SecCompanyFactsDataset._replace_facts`; DuckDB was at 953.5/953.6 MiB under the 1 GB, one-thread limit. The memory guard recorded a 1.838 GiB native peak under its 3 GiB cap. The failure inspection preserved 31,590,760 raw facts and the same number of points, plus 31,959,271 prices through 2026-09-18.

This repair changes only `atx-db/src/atx_db/fundamentals.py` and adds `atx-db/tests/test_companyfacts_issuer_cleanup.py`. No activation, job, schema, migration, index, or memory-limit changes are included. The separate static audit is `companyfacts-runtime-path-audit.md`.

## Implementation

Inside the existing per-CIK transaction, three DuckDB temporary relations now materialize:

1. The requested issuer's distinct old `(security_id, accession_number, taxonomy, concept, unit, period_end, period_start)` tuples, using the unchanged guarded numeric CIK predicate.
2. Deduplicated non-NULL legacy IDs: the passed ID, `cik_security_id(cik)`, and matching ticker IDs using the same numeric CIK predicate.
3. The union of each old fact's own ID/key tuple and each legacy ID crossed with only the issuer's distinct filing keys.

The point deletion uses `DELETE ... USING`, ordinary security-ID equality, the existing source filter, and `IS NOT DISTINCT FROM` for all six filing-key fields. It contains no correlated subquery or identity OR. Old resolved IDs remain paired only with their own fact keys; they are not crossed with every old key. No old warehouse history is fetched into Python.

The raw-fact deletion, candidate cleanup, and both inserts remain in their existing order and transaction. Successful replacements drop all three temporary relations before commit; exceptions roll back their transaction-local creation as well as every deletion/insertion. Empty replacements still remove matching old evidence; absent old facts produce no point deletion.

The deletion-key relation is bounded by the issuer's own distinct ID/key count plus its distinct filing-key count times its deduplicated legacy-ID count. This removes the demonstrated correlated query shape. It does not establish production throughput or eliminate every full-table scan; those require the guarded production retry.

## Regression coverage

The new focused file uses an isolated reduced legacy schema permitting NULL identities and all nullable filing keys; the existing archive tests exercise current warehouse contracts.

- Exact old ID/key ownership, fallback IDs, deduplicated ticker IDs, NULL identities, all six NULL-safe filing fields, other sources, and unrelated issuers sharing filing keys.
- Padded, unpadded, whitespace-padded, and extra-leading-zero CIK evidence; junk/signed CIK values stay untouched. Candidate dataset/method/key-type scope is preserved.
- Empty replacement, repeated replacement, and legacy IDs without old fact evidence.
- 250,000 unrelated wide-key raw facts and points with a one-key target, replacement under a 64 MB query-memory limit, and complete unrelated-row retention.
- Injected failure after either insertion, full before/after snapshots for raw facts/points/candidates, temporary-table absence after rollback, and two successful retries with changed IDs.

## Validation status

Fresh independent static review (`companyfacts-issuer-cleanup-review.md`) found no Critical or Important findings and confirmed exact identity/key, NULL, issuer/source, and transaction semantics.

The root controller ran the focused suite using the repository virtual environment:

```text
C:/atx/atx-db/.venv/Scripts/python.exe -m pytest -n 0 -q tests/test_companyfacts_issuer_cleanup.py tests/test_companyfacts_zip.py tests/test_fundamentals_spine_link.py
```

All 38 cases passed, exit code 0. The 2.5 GiB process guard recorded a native peak of 0.6891555786132812 GiB. This includes the new test's separate 64 MB DuckDB query-memory limit and 250,000 unrelated rows in each history surface, plus both rollback/retry cases. Receipts: `companyfacts-issuer-cleanup-tests.log` and `companyfacts-issuer-cleanup-tests-memory.json`; the corresponding `.err` file is empty.

The implementation agent ran `ruff check src/atx_db/fundamentals.py tests/test_companyfacts_issuer_cleanup.py` (all checks passed) and `git diff --check`. No application imports, tests, database access, probes, or live jobs were run by the implementation agent; runtime validation stayed with the root controller.

The full 20,390-CIK production archive retry remains root-controlled and pending, at the original 1 GB query-memory limit, one thread, and 3 GiB process guard. Focused-test success does not establish production throughput or archive completion.

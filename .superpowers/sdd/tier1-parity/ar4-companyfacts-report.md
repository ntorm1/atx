# AR4: offline archive-wide companyfacts targets

Review corrections supersede the initial identity/replacement details below; see `ar4-companyfacts-fix1-report.md`.

Implemented on `feat/tier1-parity`, 2026-09-20. Code and offline fixture validation only; no live warehouse connection, network request, writer interruption, migration, or LLM API call. The recorded local inventory of 20,390 CIK members is an archive count, not a loaded-CIK or historical US-listed count.

## Behavior

- `SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=...)` enumerates only exact root-level `CIK[0-9]{10}.json` members, sorts/deduplicates normalized CIKs, then applies append-missing filtering and offset/limit. Other target modes also normalize/deduplicate CIKs before paging, including ticker aliases.
- Archive mode requires a local ZIP before creating any SEC session. The bulk CLI rejects `--download` and `--factor-ids` with this selector. Bad JSON, malformed supported fact structures, absent members, and payload/member CIK mismatches become visible failures without HTTP fallback. Database errors still stop the load.
- After review C1, archive targets begin in the distinct source-only `SEC-COMPANYFACTS-UNRESOLVED-CIK-##########` namespace. Actual dated resolutions may retain traded `SEC-CIK-##########` IDs; undated fallback may not. The existing per-fact valid/available identifier-history resolver is retained; archive ingestion disables its undated current-ticker and current-security entity fallback. Facts and fundamental points receive identical PIT security IDs. Archive point symbols are NULL rather than invented trading symbols. No securities/listings/price mappings are created.
- Unknown security or entity links remain visible in the candidate ledger and separate target/fact-row counts. Dated history can resolve an issuer across different securities/entities over time. Unresolved CIK source identities do not establish a historical traded security, US listing, or common-equity classification. Listed-universe eligibility remains a separate prerequisite.
- Replacement is the archive default. Successful empty normalization removes that CIK's stale raw facts and old points; source failures preserve prior data. Per-CIK transactions are retained. After review I1/I3, old points are cleared by prior CIK-scoped identities and NULL-safe fact keys, with normalized CIK equivalence. Candidate evidence is cleared/replaced for successfully refreshed CIKs.
- `--append-missing` (legacy alias `--skip-loaded`) is explicitly limited: any existing fact causes a skip. It does not prove completion for the current archive or allowlist. No receipt table or migration was introduced; `previously_completed_targets=null` and `completion_evidence=no_persisted_archive_allowlist_receipts` make the limitation explicit. A replacement retry rereads every selected member, including successful and empty targets. Offset/limit can bound deliberate replacement windows, with omitted-window counts visible.
- Details include archive total/unique-CIK/duplicate/ignored member counts; preselection valid targets; selected/window-excluded/existing-skipped targets; successful nonempty and empty targets; empty reasons; failed targets/error types; unresolved security/entity targets and fact rows; fact counts; archive SHA-256 and allowlist/taxonomy SHA-256. The ZIP is hashed once per load, never once per member. The first 50 empty/failure examples accompany aggregate counts; raw_source_files retains each selected member's current outcome, actual archive hash, allowlist hash, and dataset run ID.
- Empty archive, all-existing-skipped, empty page, supported-empty, and failed-target outcomes are distinct. Successful processing is not a listed-coverage assertion (`listed_security_coverage_verified=false`).
- The bulk CLI prints failure evidence and exits 1 for failed targets or no valid targets. Activation raises `ActivationStageError` after collecting failures, retaining successful CIK writes; emitted JSON and the durable activation error column contain outcome details, and the ledger row count retains partial rows. Existing-skipped and supported-empty loads retain honest detail instead of being presented as refreshed coverage.
- Activation adds `--companyfacts-symbol-source archive_members`, `--companyfacts-append-missing`, and `--companyfacts-replace-existing`; archive default is replacement and existing current-ticker activation retains append-missing behavior. Job option parsing now retains ZIP, paging, failure, deferred-surface, and progress options. The wrapper enables INFO logging at the CLI edge and its stale fixed stage list is replaced with a reference to the authoritative stage order.

## Validation

- `python -m pytest tests/test_companyfacts_zip.py tests/test_companyfacts_resilience.py tests/test_fundamentals_spine_link.py tests/test_activation_stages_b.py -n 0 -q`: 39 passed, exit 0. Fixture coverage includes exact/deduplicated inventory, noncurrent CIKs and ticker aliases, no HTTP fallback, malformed/mismatched members, unsupported/allowlist-empty targets, append-missing versus replacement, idempotency/empty replacement, PIT security/entity transitions, CIK-only fallback, and durable failed-stage partial accounting.
- `python -m pytest tests/test_companyfacts_cli.py -n 0 -q`: 3 passed, exit 0 (final CLI options and logging). Five initial pure checks also passed; two were subsequently included in the 39-test covering run.
- `python -m mypy --strict --follow-imports=silent src/atx_db/fundamentals.py src/atx_db/activation.py`: clean. No new source module or schema table.
- Ruff clean on loader, activation, both scripts, and five touched/new test files. `jobs.py` was checked and retains the four preexisting findings already recorded by S4 T10: I001, UP035, B010, B007. Controller explicitly instructed no unrelated scheduler cleanup.
- `git diff --check`: clean. Shared DB test slot released to controller/AR6 after the covering run. One independent review requested after the pathspec commit; no review verdict is inferred here.

## Exact supplementary operator command (not executed)

After run4 PID 22840 releases the writer, independent code review, and the controller's governed migration step with preserved backups, run from `C:/atx/atx-db`:

```powershell
& .\.venv\Scripts\python.exe scripts/build_companyfacts_bulk.py --db-path data/warehouse.duckdb --companyfacts-zip data/cache/companyfacts.zip --symbol-source archive_members --defer-derived-surfaces --as-of-date 2026-09-20 --progress-every 25 --memory-limit 1GB --threads 1
```

This command is fully offline and needs no SEC contact. Its output contains the actual generated dataset run UUID. Do not add `--append-missing` to this final-allowlist replacement. Inspect the output counts/error records; resolve source failures before treating the supplement as finished. Persisted receipts are unavailable, so retries of this command replace all selected CIKs and reread the archive members. The full archive may include unsupported taxonomies and unresolved/non-US/nontraded issuers; its size is never the historical-equity denominator.

The command deliberately defers global derived surfaces. Follow with the controller's activation-run5 from `statement_points --force` after all required upstream supplements and activation integration land. Run5 does not itself select/ingest archive CIKs. No live loaded, unresolved, failure, or historical-US-listed count was measured in this task.

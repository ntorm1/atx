# AR4 review fix round 1

Addresses `.superpowers/sdd/tier1-parity/ar4-companyfacts-review.md`: C1 Critical and I1?I3 Important. No live database connection/write, network, migration, production process change, or activation/jobs edit. The controller granted the serial fixture DB test slot. Critical rereview remains required; this implementation report is not that verdict.

## C1 ? isolate unresolved issuer source identities

Archive fallback IDs now use reserved source-only `SEC-COMPANYFACTS-UNRESOLVED-CIK-##########`, distinct from the real traded-security namespace `SEC-CIK-##########`. This supersedes the original brief's submissions-helper reuse, which the review demonstrated was unsafe. The existing per-fact dated resolver can still assign any authoritative security ID, including actual `SEC-CIK-*`; no blanket prefix exclusion is introduced. Unresolved raw facts, points, and candidate evidence retain the isolated identity, and no corresponding security/listing/bar row is invented.

The regression seeds an actual current `SEC-CIK-0000320193` security/ticker and its dated history. A 2018 fact predating that history stays isolated while a 2024 fact resolves to the actual security/entity. The real fact-revision and statement refresh leaves the older statement symbol NULL; a security-ID/availability join to actual equity bars returns only the resolved later fact. This checks the concrete downstream path identified by the review, not just output counters.

## I1 ? issuer-scoped, NULL-safe point replacement

The point delete now requires both a prior identity belonging to the normalized target CIK and the prior raw fact's keys. Accepted prior identities are the raw fact's PIT ID, the loader's target/source fallback, the legacy padded SEC-CIK fallback, and SEC ticker IDs explicitly recorded for that same CIK. It no longer deletes points solely because another issuer shares accession/concept/unit/period keys. Nullable accession, taxonomy, metric, unit, period-end, and period-start comparisons use `IS NOT DISTINCT FROM`. Deletes and inserts stay in the existing per-CIK transaction.

Regressions cover two distinct issuers with identical filing keys and a formerly resolved point with NULL accession that must be removed when the historical mapping becomes unavailable. Repeated replacement retains exactly the intended point set.

## I2 ? validate selected fact structure before replacement

Normalization validates each selected supported taxonomy/concept container, required dictionary-valued `units`, list-valued per-unit facts, and object-valued fact entries. Validation runs before any replacement, including for empty containers and mixed valid/malformed concepts. The existing source-failure path records malformed members and preserves all prior raw facts, points, and candidate evidence. Valid unsupported-taxonomy/allowlist-empty results remain separate outcomes; list-valued empty fact containers are still structurally valid.

Eight parameterized regressions cover missing units, empty dictionary in place of a unit list, wrong units type, and a non-object fact, each alone and mixed with a valid selected concept. Every case preserves prior issuer rows and records one failed target with no successful/empty completion.

## I3 ? normalize all prior issuer slices

Prior-fact lookup/deletion and loader-owned CIK candidate cleanup use digit-guarded numeric CIK equivalence, so unpadded and padded legacy values form one issuer slice. Newly stored CIKs remain padded. Point cleanup uses that same prior raw scope and includes the historical loader fallback as required.

The regression converts an existing issuer to an unpadded raw/candidate key, a raw historical ID, and a legacy fallback point ID while retaining an unrelated CIK. Append-missing recognizes the existing issuer; replacement removes its legacy rows, leaves one padded slice, preserves the unrelated issuer, and is idempotent.

## Operator memory limits

Per controller follow-up, `build_companyfacts_bulk.py` exposes `--memory-limit` and `--threads`, defaults to `1GB` and `1`, and calls the existing analytical-session configurator immediately after opening the store, before loading or any factor-target query. A pure CLI test proves parsing/defaults and configuration-before-load ordering. DuckDB query limits complement, rather than replace, the controller's external process guard. This change makes no process-RSS guarantee.

## Validation

One covering serial fixture run:

```text
python ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 4 --receipt ../.superpowers/sdd/tier1-parity/ar4-fix-tests-memory.json -- python -m pytest tests/test_companyfacts_zip.py tests/test_companyfacts_resilience.py tests/test_fundamentals_spine_link.py tests/test_companyfacts_cli.py tests/test_activation_stages_b.py -o addopts= -n 0 -q
55 passed in 19.96s
Guard completed, returncode 0, native_peak_job_memory_gb 0.6942481994628906
```

The receipt records Windows Job Object peak job memory; this is not an RSS measurement. No full suite or concurrent heavy test run was launched. Ruff on the touched source/script/test files and strict mypy on `fundamentals.py` pass. `git diff --check` is clean. The existing independent-review requirement for C1 is still pending.

## Updated exact supplementary command (not executed)

After review acceptance and the controller's governed migration/input sequence, from `C:/atx/atx-db`:

```powershell
& .\.venv\Scripts\python.exe scripts/build_companyfacts_bulk.py --db-path data/warehouse.duckdb --companyfacts-zip data/cache/companyfacts.zip --symbol-source archive_members --defer-derived-surfaces --as-of-date 2026-09-20 --progress-every 25 --memory-limit 1GB --threads 1
```

Keep full replacement (do not add append-missing) for the final allowlist. Inspect actual output/failure evidence, then rebuild derived surfaces in run5. The 20,390 recorded archive members remain an input inventory, not loaded or historical US-listed coverage. The command remains fully offline; no SEC contact is required.

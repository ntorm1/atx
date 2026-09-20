# Production continuation after CF5 and AF1

Prepared 2026-09-20 22:49 UTC. Commands below are queued, not execution evidence.
Use the project Python for the guard and its child, a fresh receipt for every
attempt, one workload at a time, and a 3 GiB job cap. DuckDB stays at 1 GB and
one thread. No bulk download stage is needed; retained archive bytes are the
evidence for both verified resumes. All User-Agent values use the dummy address.

Prerequisites: AF1 Important fixes verified and committed, exact private-module
snapshot pins updated, data dictionary regenerated, and committed-HEAD import,
module-boundary and schema-contract checks passed. CF5 is committed b9572b27;
53 post-review focused cases passed with peak job memory 0.703762 GiB.

## Companyfacts

Run through `run_memory_guarded.py --job-gb 3` from `C:/atx/atx-db`:

```text
C:/atx/atx-db/.venv/Scripts/python.exe scripts/warehouse_activate.py
--db-path data/warehouse.duckdb --as-of-date 2026-09-20
--only companyfacts_load --companyfacts-symbol-source archive_members
--companyfacts-replace-existing
--companyfacts-resume-from-run-id 758f7d5c-73ae-4b66-a8c9-f1996afa163a
--memory-limit 1GB --threads 1 --backup-keep 100 --force
--run-id activation-companyfacts-archive4
--sec-user-agent "atx-db/0.1 atx-research@example.com"
```

The CLI will govern pending 0315/0316 migrations before opening the loader.
Migration code itself configures 1 GB/one thread and keeps the pre-migration
backup. The verifier checks retained receipts and full fact/point fingerprints;
it must finish successfully before issuer mutation. Observe proof phase and
member logs, then inspect final activation and dataset ledgers. No skip is
inferred from a maximum CIK or the earlier progress counter.

## Submissions, then full downstream ladder

After companyfacts is terminal and its result is handled, use the same guard and
CLI defaults with `--only submissions_load`, `--submissions-batch-size 50`,
`--submissions-resume-from-run-id 04cf947d-53bb-49b7-a276-b3c74a2a52c8`,
`--force`, and a fresh activation ID. Keep the existing all-forms/history source.
Do not run `sec_bulk_download`, which would invalidate the retained proof.

Then use `--start-stage statement_points --force --shards 16` with run ID
`activation-run5`, the same database/as-of-date/resource settings, and no resume
flag. This forces all newly implemented downstream stages over the universe.
Fix actual stage failures as bounded tasks rather than skipping stages.

After forward labels exist, evaluate the already-built eight CF1 hypotheses;
report holdout decile spreads, label coverage, HAC uncertainty, Holm adjustment,
and transaction-cost sensitivity. Current custom-feature rows are not evidence
of significant or production-eligible signals. Measure live coverage/quality,
publish observed numbers, and change schema conditions only on measured gates.
Publish the full-universe release only when its actual release conditions pass.

The full non-slow suite and Codex whole-branch review remain sprint gates.
Ask the user before merging main. Preserve and remind them about stash@{0}.

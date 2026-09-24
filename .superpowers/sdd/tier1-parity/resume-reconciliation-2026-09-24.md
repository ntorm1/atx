# Production resume reconciliation - 2026-09-24

Read-only source/receipt reconciliation for root. No tests, Python imports,
warehouse access, production process, or commit were performed by this agent.
Root owns current runtime observations and PG1 repair validation. Historical
sprint ledgers remain completed evidence; their old task ownership and gate
notes are superseded by the program and continuation receipts.

## Exact verification remaining

`quant-platform-head6` exported `09fed6066da85169c5b4796941a55c6108588299`.
Its actual result was **110 passed, one default slow skip, two failures** in
the 113 progress outcomes, process exit 1, peak 0.890263 GiB under the 1.5 GiB
guard. This was a test failure, not a headroom stop. Both failures are in
`tests/test_derived_pit_revisions.py`: the temporary reorder table survives into
a connection-recycling call, and the upgrade assertion assumes migration HEAD
0316 after later registered migrations exist. Root assigned PG1 separately.

The `atx-db` Git subtree of that tested commit and current initial HEAD
`2e0d738f2a866b1f253e400ee386ef3e0674cd15` is identical:
`2166d90d282891d43d63233f850daeab5d6a808f`. The merge brought no `atx-db` content
change. Existing successful head6 module-boundary, schema-v2, derived,
annual/PIT and forward-publication checks therefore remain exact-source
evidence. Rerun the repaired failures only, then the required full non-slow
suite at the release gate. A complete passing head6 batch is not yet claimed.

Root subsequently ran the PG1 chunk selector through
`verify_pit_gate_repair.py`: `pit-gate-chunk1-memory.json` is completed, exit 0,
peak 0.707485 GiB; the log records one pass in 4.19 seconds, including 0.68
seconds test call and 0.20 seconds setup. Candidate test SHA256 is
`6be6adfd02148dd6b3334d343da931e998b234cf07931fcba5063b8773d6e0a8`.
Only the repaired populated-0314 upgrade selector remains pending in this
prerequisite. Root reports the one independent PG1 static review clean.

After a fresh sustained 6 GiB physical / 8 GiB commit window for 120 seconds,
the minimal guarded command is (all three evidence paths must be new):

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 1.5 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\pit-gate-upgrade1-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\pit-gate-upgrade1.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\pit-gate-upgrade1.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\verify_pit_gate_repair.py upgrade
```

This controller exports committed source and overlays only candidate test bytes,
records their hash, and validates/reuses an exact completed schema template.
It is candidate verification until root commits the tested repair. Do not
change another session's files to make the verification export.

## Completed schema cache and timing evidence

The head6 export retains a completed 59,781,120-byte template at:

`C:/Users/natha/AppData/Local/Temp/atx-tier1-head-6bzfpb4d/atx-db/.pytest_cache/db_schema_templates/19b2c3a07814e600498c7d79/`

Its 24-byte `warehouse_template.duckdb.ready` contains exactly the directory
key. A PowerShell SHA256 implementation of the existing fixture algorithm,
over its 164 source/seed inputs with CPython `cpython-312` and installed DuckDB
package 1.5.5, reproduced the key. A fresh Git archive has those same bytes.
The working-tree source computes `4c1e64b71f0fd1e049dc0860` because of EOL-only
byte differences. Never rename the head6 cache to that different fingerprint.
The controller correctly copies the template and marker under the exact
export fingerprint, retaining the old evidence.

Head6 receipt file creation-to-final-write was 484.328 seconds. Template file
activity was 01:16:23.077..01:17:56.435 UTC (93.359 seconds). Its populated-0314
test database activity was 01:22:19.196..01:23:48.176 UTC (88.980 seconds).
These are filesystem timing proxies, not instrumented test durations. The
populated-0314 test deliberately bootstraps that older schema in a separate
database and cannot substitute the ready HEAD template. Its sustained-headroom
requirement remains in force despite cheap template reuse for the chunk test.

## Existing review disposition

`whole-branch-review-2026-09-24.md` reviewed candidate `727e6b90` and records
one Important label-evidence finding and one Moderate empty-column finding.
They are accepted in `09fed606` and `9e0b4ffa`, with 17 focused checks and
scoped Ruff. No Critical finding or repeated whole-branch review is due from
that receipt. Record later repair SHAs and affected paths; do not turn the
completed review into a second review cycle.

## Production commands match the initial current HEAD

Static parser, option mapping, dispatch and migration registration match
`production-resume-sequence-2026-09-21.md`:

- Registered head is 0325. Latest stored recovery evidence says live 0322;
  pending bodies are 0323 selected input references, 0324 signal panels, and
  0325 evaluation/forward-label provenance. Root's fresh pipeline-status read
  independently reconfirms pending 0323..0325. Governed migration creates its
  own connection at 1GB / one thread before SQL and on reopen. Retain
  `--backup-keep 100` and inspect current free disk before the full-file backup.
- Full CompanyFacts predecessor is dataset UUID
  `513cfbbc-096a-4186-9666-b6cc5170c4ad`, not the activation label. Archive16
  recovery committed both terminal failed ledger states at
  2026-09-23T22:19:32.614888Z; CHECKPOINT passed. Retained facts/points were
  47,941,000 each and that attempt inserted no rows. Do not repeat recovery.
  `--only companyfacts_load --companyfacts-symbol-source archive_members
  --companyfacts-replace-existing --companyfacts-resume-from-run-id UUID
  --force` satisfies the full-archive resume validator without a download stage.
- Full submissions predecessor is dataset UUID
  `04cf947d-53bb-49b7-a276-b3c74a2a52c8`. The implemented activation dispatch
  passes `forms=None`, batch size 50, unrestricted CIKs and the resume UUID.
  `--only submissions_load` preserves the pre-attempt archive receipt. The
  existing source verifier checks retained lineage and archive content; the
  old count/boundary alone is not sufficient resume proof.
- The next scoped source wave uses `--only earnings_release_facts
  --earnings-release-cik 0000093410 --earnings-release-history-end 2026-09-20`.
  These flags exist and the source dispatch uses the approved dummy SEC contact.
- Full run5 starts at `statement_points --force --shards 16`, with no CIK
  restriction. The current suffix includes derived/market data, compatibility
  projections, delisting/universe/terminal returns, calendar/forward labels,
  item/provider coverage, price metrics and final quality. Reconciliation shards
  remain sequential.

Keep the pinned 2026-09-20 snapshot, 1GB / one-thread production settings,
2 GiB production guard and fresh receipt/log/error filenames. Process liveness
and latest capacity are root-owned observations. None of this static audit is
evidence of full ingestion, live migration, materialized desk queries, satisfied
quality/SLO thresholds, historical listing proof, a release or alpha results.

Preserve `stash@{0}`, the unrelated risk test and all retained source/backup
evidence. Ask before any merge to main.

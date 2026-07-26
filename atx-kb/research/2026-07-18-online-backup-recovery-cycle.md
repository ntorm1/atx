# WAL-aware online backup and restored-copy verification

Date: 2026-07-18

## Primary SQLite references

- Online Backup API: https://sqlite.org/backup.html
- C backup interface contract: https://sqlite.org/c3ref/backup_finish.html
- Write-ahead logging: https://www.sqlite.org/wal.html
- WAL-mode file lifecycle and recovery: https://sqlite.org/walformat.html

SQLite's online backup interface copies a live source into a consistent
destination snapshot while holding source read locks only during bounded step
calls. Concurrent external writes can restart the copy, but a completed backup
remains consistent and current. `SQLITE_BUSY` and `SQLITE_LOCKED` are retryable
step results; importantly, they do not make a later `sqlite3_backup_finish()`
fail. A caller that ignores `sqlite3_backup_step()` can therefore report false
success without completing the copy. Raw copying is also unsafe in WAL mode
because committed transactions may exist only in the `-wal` file.

## Correctness changes

- atx-core now checks every backup step, copies a bounded number of pages,
  retries busy/locked results with finite limits and delay, reports progress,
  and succeeds only on `SQLITE_DONE` followed by successful finish.
- A busy destination rolls back and returns `Unavailable`; the old destination
  remains intact. Invalid options fail before creating backup state.
- atx-kb and atx-db write to sibling `.partial` files and never overwrite a
  destination or stale partial.
- Each candidate is reopened through its domain schema and integrity gates.
  atx-kb verifies content hashes, observations, exact claim support, graph/FTS,
  vector revisions, and finalized HNSW checksums. atx-db verifies SQLite/FKs,
  workspace leases, task DAG/run state, and temporal facts.
- Verification WAL is explicitly checkpointed with `TRUNCATE`; a busy checkpoint
  fails publication. Nonessential sidecars are removed only after checkpoint.
- A same-directory hard-link operation atomically exposes the verified file and
  refuses an existing target. A crash may leave the complete partial name, but
  cannot publish a partial destination.
- Both CLIs expose `backup <source> <new-destination>` and return JSON page,
  step, and contention telemetry.

## Regression and live evidence

- Five focused backup tests pass: completed-copy reporting, destination-lock
  failure, a concurrent WAL writer, restored atx-kb graph/provenance, restored
  multi-workspace atx-db state, no sidecar dependency, and overwrite refusal.
- The complete targeted knowledge/database group passes 41 tests.
- Retrieval quality remains green on 110 documents and 41 queries with every
  threshold met. The default 1,024-vector ANN gate retains recall@10 1.0,
  4.006848x distance-work reduction, and 5.385020x warm speedup.
- Live CLI snapshots restored and verified successfully: the research store
  copied 137 pages and the coordination store copied 33 pages, both without a
  busy retry.

## Remaining disaster-recovery gates

- Inject process termination and I/O faults at every step, checkpoint, verify,
  hard-link, and partial-cleanup boundary.
- Add signed manifests with database identity, schema version, logical content
  digest, creation sequence/time, and parent-backup lineage.
- Define retention, encrypted off-host replication, key rotation, restore drills,
  and recovery-point/recovery-time objectives.
- Coordinate the independently consistent atx-kb and atx-db snapshots with an
  evidence-link watermark so a restored pair can prove that every episode's
  exact knowledge observation is present.

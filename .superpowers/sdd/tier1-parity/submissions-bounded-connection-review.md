# SEC submissions bounded connection lifecycle: static review

Reviewed once on 2026-09-20 on `feat/tier1-parity`: the pending changes to `atx-db/src/atx_db/sec_submissions.py`, `atx-db/tests/test_sec_submissions_bulk.py`, and `submissions-bounded-connection-report.md`. Supporting reads covered the existing transaction, insertion, connection lifecycle, dataset-run, and activation call paths.

**Critical findings: none. Important findings: none.**

- The recycle attempt occurs only after `_replace_submission_rows()` returns. Both DataFrame registrations are removed in `finally` blocks before the transaction context executes COMMIT. A failed write or COMMIT cannot advance the flush counter or reach recycling. The concatenated batch frame is deleted before the attempt.
- The helper excludes anonymous/named in-memory paths, paths without an existing file, and stores without both recorded analytical caps. It checks noninternal temporary tables and views before closing. These conditions preserve the intended in-memory and caller-registered/table cases; no broader concurrent-session ownership claim is made.
- Existing `close()` checkpoints before closing. Existing `reopen()` restores recorded memory/thread caps, disabled progress reporting, insertion-order tuning, UTC, and the warehouse's absolute spill-directory setup. It does not initialize or migrate. Dataset-run completion continues through `store.con`, so it uses the replacement connection.
- The flush refactor preserves concatenation, accession-key deduplication, transactional replacement, and invocation row counting. All-form selection, CIK scope/mapping, history traversal, recent-before-history precedence, source lineage, run IDs, and source/quality recording remain unchanged. New log messages contain counts only.
- The added cases exercise real file-backed recycling, committed-row survival and replay replacement, restored settings, initialization exclusion, in-memory preservation, caller session-state guards, and progress with no selected rows. The expected committed counts and final rows agree with the fixture's recent/history records.

The implementation report accurately describes preventive connection-lifecycle control. It does not claim a measured OOM fix or a hard process-memory bound, and it records that observed native memory can fall as well as rise. Eight flushes is an unmeasured cadence; actual memory benefit and checkpoint/reopen overhead remain unverified. The active process retains its already-imported implementation.

This was a static review only. No imports, tests, database opens, process probes, or production commands were run. Controller-owned focused tests and lint remain pending until the current writer releases the exclusive workload slot. No implementation edits or commit were made. Codebase graph tools were unavailable in this session, so source inspection used direct reads and `rg`. Review used Codex only and made no external LLM API calls.

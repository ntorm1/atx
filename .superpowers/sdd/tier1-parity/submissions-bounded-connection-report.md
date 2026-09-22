# SEC submissions bounded connection lifecycle

Status: implemented; independent static review clean and controller-owned focused verification passed. The implementer performed no imports, tests, database opens, probes, production commands, or changes to the active process. `git diff --check` and touched-file Ruff passed on the two changed Python files.

This is preventive bounded lifecycle control, not a measured fix of an actual stage failure. The controller reported that the existing all-form production submissions load was still committing healthy batches after roughly 41 minutes, with native private memory increasing from approximately 1.87 GB at 18:08 UTC to 2.37 GB at 18:44 UTC under its 1 GB DuckDB budget and 3 GiB process guard. A later controller observation showed private memory dropping from 2,442,285,056 bytes at 18:49 UTC to 2,034,946,048 bytes at 18:51:54 UTC. Memory fluctuates despite the earlier rising retained high-water trend; these observations do not prove a leak or inevitable OOM. No errors or completed-stage output had been observed at that point. This task did not independently measure the process. The already running process could not pick up these edits and was left under its existing guard without interruption to deploy this change.

The controller subsequently reported that the old process was stopped by the independent guard for low host headroom: approximately 1.096 GiB physical and 2.921 GiB commit headroom. This was not a demonstrated worker OOM. The warehouse retained 8,504,213 submissions rows. These retained partial rows do not establish completion of the full archive, and this preventive change was not active in that process.

## Implementation

`SecSubmissionsBulkDataset.load()` now attempts a checkpoint/close/reopen after every eight successful, nonempty batch flushes. The flush still performs the identical concatenation, accession-key deduplication, and transactional delete/insert. Counters advance only after `_replace_submission_rows()` has returned from COMMIT; both loader DataFrame registrations have already been removed and all loader query results have been consumed. The concatenated DataFrame is released before recycling.

Recycling is limited to an existing file-backed store with both analytical memory/thread settings already recorded. Anonymous and named in-memory paths are explicitly excluded. Any noninternal temporary table or view, including a caller-registered DataFrame, prevents recycling. Unconfigured stores retain their current connection. The load owns its session while running; this is not an ownership framework for arbitrary caller-managed cursors or concurrent connection use. An existing outer transaction still fails at the writer's nested BEGIN before any successful flush/recycle; no new transaction handling was introduced.

The existing `DuckDBStore.close()` issues CHECKPOINT before closing. Existing `reopen()` reapplies the recorded memory/thread caps, disabled progress bar, insertion-order setting, UTC clock, and absolute spill directory. It does not call initialization or migrations. Neither store method was changed. No schema, migration, registry, activation, job, or load-scope setting was changed.

The change bounds the number of committed batches whose native connection state can accumulate in configured production callers. It does not bound single-issuer JSON size, a single batch's row/byte count, Python/archive metadata, or process memory; the existing independent process guard remains necessary. Eight is a conservative fixed cadence, not a measured optimum. Checkpoint/reopen overhead and any reduction in native retention still need production measurement in a future process. Reopen failures propagate rather than silently rerunning committed input.

Progress logging reports only processed main-member counts, total archive main members, CIKs with selected rows, rows committed by this invocation, and flush count. It emits after each successful flush, every 2,000 processed main members between flushes, and on completion, with an additional count-only message after a recycle. A CIK with no selected forms still advances processing progress; CIK scope exclusions do not. No source payload, contact value, source path, or issuer identifier is included in these messages.

All forms/CIKs/history traversal, recent-before-history precedence, accession replacement, source lineage, run IDs, source recording, quality recording, and result fields retain their existing behavior. Committed-row counters describe accepted rows in this invocation, not the net change in total warehouse rows on replay.

## Focused verification

Seven new parametrized cases in `tests/test_sec_submissions_bulk.py`, alongside its three existing cases, cover:

- A real file-backed close/reopen after each test batch: unrelated rows survive, overlapping accessions are replaced, recent/history/all-form rows survive repeated loads, session caps/UTC/spill directory survive, and initialization is forbidden during recycling.
- Anonymous and named in-memory stores retain the same connection and all rows on replay.
- Registered DataFrames, temporary tables, and unconfigured stores retain caller session state and loaded rows.
- Progress continues through form-filtered members with zero committed rows, and recycle logs contain counts without source payload/path details.

After the production writer exited and the exclusive workload slot was free, the controller ran the correct project virtual environment (DuckDB 1.5.5) under the existing 2.5 GiB process guard:

```text
C:/atx/atx-db/.venv/Scripts/python.exe -m pytest -n 0 -q tests/test_sec_submissions_bulk.py tests/test_sec_submissions.py
```

All 11 cases passed, exit 0. The guard measured native peak job memory of 0.6701545715332031 GiB. Evidence is in `submissions-lifecycle-tests-memory.json`, `submissions-lifecycle-tests.log`, and `submissions-lifecycle-tests.err`; those controller-owned receipts are outside this commit's scope. No broad suite or production memory benchmark was run. The implementer subsequently ran `.venv/Scripts/ruff.exe check src/atx_db/sec_submissions.py tests/test_sec_submissions_bulk.py`; all checks passed.

The independent Codex static review in `submissions-bounded-connection-review.md` reported no Critical or Important findings. Its description of pending execution records the review-time state; the controller results above complete that verification.

Commit scope: this report, the independent review, `atx-db/src/atx_db/sec_submissions.py`, and `atx-db/tests/test_sec_submissions_bulk.py`. Required literal trailer: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`. Implementation/review uses Codex only; this task incurred no LLM API spend.

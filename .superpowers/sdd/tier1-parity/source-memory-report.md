# SM1 implementation report

Status: six new focused checks, scoped Ruff, and 17 integration checks passed in
root's serialized slots; root's independent review is clean. Accepted for commit.
Production qualification remains pending. No production readiness or measured
memory reduction is claimed.

## Change

The resume verifier already aggregates SHA-256 multiset evidence with bounded
Python state; it does not retain raw fact or point rows. Its avoidable lifetime
cost was keeping inventory, full fact scan, and full point scan on one connection
until all verification had completed. It now closes/reopens after consumed CIK
counts, after consumed fact fingerprints, and after consumed point fingerprints.
It records three successful releases in `resume_proof_connection_reopens` for a
configured persistent store with loaded receipts. In-memory/unconfigured stores
retain the previous no-recycle policy, and caller-owned temporary objects still
fail closed before being discarded.

`DuckDBStore.reopen` now supplies recorded memory/thread/insertion-order caps at
the actual `duckdb.connect` call. Previously it supplied them only through SET
after opening, leaving allocation during open/recovery outside the intended cap.
The absolute spill directory is also supplied when reopening a persistent store.
Unconfigured opens, session UTC, and normal writable checkpoints are preserved.
Read-only close releases its connection without attempting CHECKPOINT.

All verifier SQL and proof projections are unchanged. Three additional closes
occur before candidate recovery or issuer replacement; the existing post-proof
recycle remains. Normal loader recycling and proof recycling have separate
receipt fields, avoiding a misleading change to the loader cadence counters.

## Evidence and limits

- Static diff inspection and `git diff --check`: clean (only repository LF/CRLF
  conversion notices).
- New minimal persistent-store tests cover active connect-time settings,
  read-only file preservation, shared historical security verification, filing
  availability preservation, altered point values, and foreign-owner duplicates.
- Root ran all six SM1 checks in a combined nine-test batch: all passed in
  2.38 seconds. `source-memory-focused1-memory.json` records return code 0,
  native peak job memory 0.595832825GiB under a 1GiB process limit, and terminal
  host headroom 4.627285GiB physical/10.359180GiB commit. The combined batch also
  includes three separate source-index-memory checks; its peak is not an
  isolated SM1 measurement or a full-universe production proof.
- Root reports scoped Ruff passed on all SM1 code/test files.
- `source-memory-integration1.log` records 17 passes in 155.07 seconds: all five
  existing connection tests, both affected archive lifecycle tests, all eight
  contradictory completion-proof cases, the real source-index 0326 bootstrap
  integration, and the public API snapshot. Its memory receipt records return
  code 0 and native peak 0.840614319GiB under a 1.5GiB process cap, with terminal
  headroom 4.542736GiB physical/10.215649GiB commit. This is focused integration
  evidence; the full non-slow release gate remains separate.
- Root's one independent review (`source-memory-review.md`) is clean: all
  receipt, ownership, multiset, identity, and temporary-object proofs remain.
  Serialized single-writer use remains required; phase recycling does not
  establish a concurrent-reader snapshot contract.
- Existing archive repair tests require three extra pre-mutation lifecycle
  observations; economic/source assertions are unchanged.
- No Python, tests, profiler, source archive read, or database mutation was run
  by this implementer before root's resource slot.

The previous archive13 receipt used 512MB/one thread and completed its proof
over 39,457,715 verified rows; its later OOM was in candidate processing.
Root reports its native job peak as 1.199386GiB. That receipt predates the
47,941,000-row warehouse and cannot establish a current peak or an A/B improvement.
Root will measure the actual full production resume, including its unchanged
complete proof, rather than duplicate the long proof in a separate read-only
pass. Full-source write behavior requires its own evidence before any lower
launch requirement can be represented as qualified. The prior read-only probe
proposal has therefore been superseded; the required full-universe proof and
native resource measurements remain.

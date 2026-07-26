# Exact artifact certification for task results and temporal facts

Date: 2026-07-18

## Research basis

Primary references:

- W3C PROV-AQ: https://www.w3.org/TR/prov-aq/
- W3C PROV-DM: https://www.w3.org/TR/2013/PR-prov-dm-20130312/
- SQLite foreign keys: https://www.sqlite.org/foreignkeys.html

PROV distinguishes a general entity from a more specific entity whose fixed
aspects identify the state being described. It also treats provenance records
as assertions that can identify a particular version or specialization rather
than only a mutable logical resource. For atx, a source ID alone identifies the
logical evidence object, while `(source_id, observation_id, content_hash)`
identifies the exact immutable artifact that an agent observed or produced.

SQLite foreign keys cannot cross schema boundaries. Because atx-db and atx-kb
remain separate WAL databases, exact cross-store evidence integrity must be
validated by the application and rechecked during recovery; it cannot be
expressed as a native foreign key.

## Schema-v6 task result certification

- `complete_task_verified()` validates that the requested observation belongs
  to the immutable atx-kb source before consuming the worker's fenced lease.
- A completed task stores the exact observation, source SHA-256, and
  `result_evidence_status=verified`. Source-only completion is retained as an
  explicit unverified import path; a result without a source is `none`.
- Strict retries are idempotent only when the lease transition token and the
  entire certified evidence tuple match. A different result cannot be smuggled
  through a retry after completion.
- Cross-store verification detects a missing source, missing exact observation,
  or certified content-hash drift. Local integrity rejects contradictory task
  status and evidence metadata.

## Schema-v7 temporal fact certification

- `put_verified_fact()` validates an exact observation and stores its source
  hash on the inserted bitemporal fact version.
- Each valid-time segment owns its evidence tuple. When a replacement assertion
  splits an earlier interval, preserved left or right segments inherit the
  earlier assertion's source, observation, hash, and certification state.
- Source-only legacy facts migrate to `unverified`; facts without an evidence
  source migrate to `none`. Fresh databases enforce all three metadata states
  with table checks, and runtime integrity repeats those checks for upgraded
  databases whose columns were added with `ALTER TABLE`.
- `fact-put-verified` exposes the strict path in the CLI. Paired-backup
  verification now checks certified task results and facts in every workspace,
  in addition to episodes.

## Regression and live evidence

All 47 focused SHA-256, atx-kb, atx-db, migration, backup, concurrency, and CLI
tests pass. New tests prove exact task completion, mismatched-observation
rejection, exact fact certification, evidence preservation through interval
splits, and legacy source-only fact migration to `unverified`.

The deterministic retrieval gate remains green on 110 documents and 41
queries, with perfect lexical, filtered, vector, hybrid, citation, abstention,
and ranking-determinism gates. The default ANN benchmark retains recall@10 of
1.0, 4.006848x distance-work reduction, and 4.677975x warm-latency speedup.

A disposable CLI round trip certified a temporal fact and published a paired
backup that passed external manifest-digest verification. Its manifest SHA-256
was `5cff89bd67f2cdf01388f4a3adb6c09bc36c9edaddeefd33cc51b953e90eb469`,
with coordination event watermark 2 and knowledge observation watermark 1.

## Remaining gates

- Require strict evidence APIs by default at higher-level agent boundaries and
  capability-gate source-only import/recovery operations.
- Add signer identity, authorization decision, policy version, and optional
  cryptographic attestations to evidence certification events.
- Batch exact-observation validation without weakening per-artifact failure
  reporting or allowing partial task completion.
- Cache immutable source lookups during all-workspace recovery verification and
  measure pair-validation cost at millions of certified references.

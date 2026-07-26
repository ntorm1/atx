# Write-time certification for agent episode evidence

Date: 2026-07-18

## Storage constraint

Primary references:

- SQLite foreign keys: https://www.sqlite.org/foreignkeys.html
- SQLite ATTACH behavior: https://www.sqlite.org/lang_attach.html

SQLite explicitly states that foreign keys may not cross schema boundaries.
atx-db and atx-kb are deliberately separate WAL databases, so an episode cannot
use a native foreign key to its knowledge source and observation. Attaching the
files would not fix that constraint and would also weaken cross-file crash
atomicity under WAL.

## Schema-v5 certification model

- `record_episode()` remains an explicit unverified import/recovery path.
- `record_verified_episode()` first retrieves the immutable atx-kb source and
  proves that the requested observation belongs to it. Only then does atx-db
  commit the episode with `evidence_status=verified`, the source content
  SHA-256, and a canonical UTC verification time.
- Knowledge sources and observations are immutable and cannot be deleted, so a
  successful validation cannot be invalidated by a later target mutation.
- A strict retry with the same idempotency key can promote a matching unverified
  row without changing its identity or payload. It cannot downgrade a verified
  row or reuse the key for different evidence.
- Verification checks the exact observation for every episode and additionally
  compares certified content hashes. atx-db integrity rejects malformed or
  contradictory evidence-status metadata.
- Schema v4-to-v5 migration marks existing episodes unverified. The migration is
  introspective and tolerates minimal legacy fixtures where the episodes table
  was absent, while fresh schema-v5 databases enforce the full table CHECK.
- `episode-record-verified` exposes the strict path in the CLI and returns the
  certification status and content hash as JSON.

## Regression evidence

All 23 AgentDatabase/CLI tests pass. New coverage proves exact certification,
source-hash persistence, canonical verification time, idempotent strict retry,
promotion of an unverified row, rejection of a mismatched observation, evidence
link verification, and schema integrity. Existing schema-v2 and schema-v3
migration fixtures still reach the current schema, and paired-backup tests
continue to enforce file-wide restored evidence consistency.

## Remaining gates

- Make verified recording the default in higher-level agent APIs and require an
  explicit capability for the unverified import path.
- Add a batch certification API with bounded knowledge lookups and an audit
  event for every promotion or rejection.
- Extend facts and task result artifacts from source-only IDs to exact certified
  `(source_id, observation_id, content_hash)` references.
- Add authorization, signer identity, and policy version to certification
  metadata so evidence trust decisions remain auditable across agents.

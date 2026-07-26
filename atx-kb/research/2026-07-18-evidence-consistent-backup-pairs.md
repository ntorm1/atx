# Evidence-consistent coordination and knowledge backup pairs

Date: 2026-07-18

## Primary SQLite constraints

- ATTACH DATABASE: https://sqlite.org/lang_attach.html
- WAL limitations: https://www3.sqlite.org/wal.html
- Super-journal lifecycle: https://www.sqlite.org/tempfiles.html
- Atomic commit: https://sqlite.org/atomiccommit.html

SQLite documents that transactions spanning attached databases are atomic as a
set only when the main database is file-backed and not in WAL mode. Under WAL,
each database commits atomically, but a host crash can roll one file forward and
another back. atx-kb and atx-db intentionally use WAL for concurrent embedded
agents, so presenting ATTACH as a cross-file recovery transaction would be a
false guarantee.

## Monotonic-reference protocol

Episode references flow from atx-db into immutable, append-only atx-kb source
observations. The safe ordering is therefore:

1. Snapshot the coordination database.
2. Snapshot the knowledge database later.
3. Enumerate every workspace in the restored coordination file.
4. Verify each workspace's domain invariants and every episode's exact source
   and observation against the later knowledge snapshot.
5. Stream SHA-256 over both stable files and record selected-workspace event and
   episode watermarks plus the global knowledge-observation watermark.
6. Commit a `atx-backup-pair-v1` SQLite manifest with relative filenames and
   digests, then atomically publish it. The manifest is the recovery commit
   point; unmanifested files are incomplete attempts.

The later knowledge snapshot may contain a superset of observations, but cannot
omit a valid immutable observation present when the earlier coordination
snapshot was taken. New episodes created after the coordination snapshot are
outside that recovery point and do not create dangling references inside it.

## Integrity and authenticity boundary

- atx-core now provides incremental SHA-256 plus a constant-memory file digest,
  verified against published empty and `abc` vectors and chunked-update tests.
- Pair verification rejects absolute or traversing manifest filenames, checks
  the SQLite manifest, hashes both files, checks watermarks and every domain
  invariant, verifies file-wide evidence links, and rehashes after verification
  to prove the checks did not mutate either snapshot.
- A supplied expected manifest SHA-256 anchors the manifest itself. The CLI
  returns this digest so an operator can sign it or store it separately.
  Without that external anchor, internal digests detect accidental corruption
  but do not authenticate against an attacker able to rewrite the whole set.
- Failure before manifest publication removes newly created pair files. A crash
  may leave complete unmanifested files, which recovery tooling must ignore.

## Regression and live evidence

All 47 focused SHA-256, backup, knowledge, coordination, migration, concurrency,
and CLI tests pass. Pair-specific tests cover a valid exact-observation restore,
external manifest anchoring, overwrite refusal, a hidden second-workspace orphan
that blocks publication and cleans outputs, database tampering, digest mismatch,
and stable post-verification hashes.

The live smoke pair verified twice, including the external manifest anchor:

- coordination SHA-256: `5d50df62cbf93e9287327e73a5f0c250a3aa46e2b2422598f444ed8b2a219d32`
- knowledge SHA-256: `0a3e655de8afa6fe7be2d9d4674803e1d65a915b3e8ee11f6f28669aa01ccbb1`
- manifest SHA-256: `dbd69e7650a49fddaae5028298ccf545ee671c2c5e8a8b2b52ec9cd544d5d88f`
- event watermark: 55
- episode count: 8
- knowledge observation watermark: 12

## Remaining recovery gates

- Fault-inject termination and I/O errors before and after every snapshot,
  digest, manifest transaction, hard-link publication, and cleanup boundary.
- Add manifest lineage, retention class, logical database identity, software
  build identity, and optional Ed25519 signing/key identifiers.
- Replicate the three-file recovery unit off-host with encryption and perform
  scheduled restore drills against explicit recovery-point/time objectives.
- Bound all-workspace verification cost and add a streaming evidence-link join
  for snapshots with millions of episodes.

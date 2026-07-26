# Vector build recovery and scale hardening — cycle 2

Date: 2026-07-18

## Failure model reviewed

An index builder can terminate after committing its `building` manifest, while loading the revision snapshot, while constructing the graph, or while persisting nodes and edges. No partial generation may become query-visible, and abandoned state must remain diagnosable and recoverable without mutating an active generation.

## Changes compiled into atx-kb schema v4

- Finalized HNSW generations remain immutable and query-visible only after an explicit activation.
- Every error after manifest creation is caught at the build boundary. Partial graph rows are deleted while the generation is still mutable, and the manifest transitions to `failed` with a bounded failure reason.
- `recover_abandoned_vector_indexes()` fences building generations older than an operator-supplied age, removes their partial nodes/edges, and retains failed manifests for inspection. Repeated recovery is idempotent.
- Integrity checks exclude intentionally partial building/failed graphs from finalized checksum rules while continuing to enforce SQLite and foreign-key integrity.
- HNSW construction now reuses an epoch-marked visited array instead of clearing an array proportional to total nodes for every layer search.
- Node and edge persistence reuses compiled SQLite statements rather than preparing one statement per row.
- The CLI exposes build/list/activate/retire/recover lifecycle operations as machine-readable JSON.

## Regression evidence

- 34 targeted knowledge, coordination, CLI, quality, and benchmark gates pass.
- Abandoned generation test injects a durable partial building graph, recovers it, verifies idempotence, checks the retained failure reason, and runs full integrity verification.
- Versioned-generation test covers exact equivalence, raw ANN diagnostics, post-cutoff mutation merge, filtered exact fallback, activation retirement, idempotent retirement, and checksum verification.
- `atx-kb-vector-benchmark-v1`: recall@10 1.000000, exact-distance work reduction 4.006848x, warm latency speedup 5.049521x on the deterministic 1,024-vector tier.
- The live research store migrated from schema v3 to v4 and verified successfully.

## Remaining scale risks

- A single persistence transaction still scales with the entire generation; bounded batch commits are needed for very large graphs.
- The warm graph cache currently retains a full generation without an explicit byte budget or eviction policy.
- The checked gate is still a 1,024-vector synthetic tier. 100k/1m tiers, real embedding corpora, latency percentiles, peak memory, and restart/cold-cache measurements remain mandatory.

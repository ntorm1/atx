# Bounded persistence and 100k vector evidence — cycle 3

Date: 2026-07-18

## Scale changes

- HNSW nodes persist in fenced transactions of at most 4,096 rows.
- HNSW edges persist in fenced transactions of at most 32,768 rows.
- Every batch rechecks that the manifest remains `building`; finalization verifies durable node and edge counts before transitioning to `ready`.
- Partial committed batches remain invisible to search and are removable by abandoned-build recovery.
- The immutable graph cache now reports measured bytes and has a configurable per-connection byte limit. Auto/approximate planning selects exact search when the conservative generation estimate exceeds that limit.
- The benchmark runner accepts corpus, dimension, query, top-k, and ef-search parameters. The opt-in `atx-kb-vector-benchmark-100k` target reproduces the large tier without adding five minutes to ordinary CI.

## 100k evidence tier

Configuration: 100,000 provenance-preserving one-chunk sources, 48-dimensional normalized deterministic embeddings, HNSW M=16, efConstruction=128, efSearch=64, eight top-10 queries.

Observed release-build result:

- pass: true
- ANN recall@10: 0.987500 (gate 0.98)
- exact-distance work reduction: 53.177346x (gate 4x)
- warm latency speedup: 137.616100x (large-tier gate 2x)
- exact average latency: 238.197988 ms
- ANN average latency: 1.730887 ms
- graph: 100,000 nodes, 3,307,472 directed layered edges
- measured warm cache: 69,785,272 bytes
- provenance/vector ingest: 133.886288 seconds
- versioned graph build and bounded persistence: 147.315383 seconds

The run includes source submission, extraction, vector revision transactions, persisted immutable graph construction, integrity verification, exact queries, and ANN queries. It is not an isolated HNSW-only microbenchmark.

## Interpretation and open falsification

This clears the first serious scale gate and demonstrates that the persisted embedded path can meet high recall with large exact-versus-ANN separation. It does not establish universal superiority: the corpus is synthetic, queries are near stored vectors, and only one hardware/software environment was measured. Next gates must include real model embeddings, heterogeneous BEIR-style corpora, cold-cache/reopen latency, p50/p95/p99 distributions, concurrent readers during batch construction, a 1m tier, and comparative runs against current Qdrant/Vespa configurations.

# Vector-system production patterns (2026-07-18)

Primary sources:

- Qdrant indexing: https://qdrant.tech/documentation/manage-data/indexing/
- Qdrant distributed deployment: https://qdrant.tech/documentation/scaling/distributed_deployment/
- Qdrant quantization: https://qdrant.tech/documentation/quantization/
- Weaviate vector indexing: https://docs.weaviate.io/weaviate/concepts/vector-index

Qdrant uses HNSW for dense approximate-nearest-neighbor search, with a
full-scan threshold below which exact search is preferred. Its payload indexes
estimate filter cardinality and add filter-aware edges to HNSW. Current Qdrant
also offers ACORN traversal for restrictive compound filters. This implies that
a competitive embedded engine needs a query planner, not a single vector path:
exact scan for small/selective candidate sets, ANN for large sets, and filter
cardinality estimates to choose between them.

Production durability requires more than a valid database file. Qdrant writes
updates through a WAL, applies them to searchable unoptimized segments, then
builds indexes asynchronously. Replication can recover from WAL deltas and
snapshots transfer prebuilt indexes. Quantization reduces memory by 4x to 32x
or more at a measurable recall cost. Weaviate likewise switches between flat
and HNSW indexes and supports asynchronous vector indexing.

Immediate atx gaps: no ANN candidate generator, no query planner, no vector
quantization, no bulk ingestion, no snapshot API, and no retrieval latency or
recall benchmark. Exact search remains the correct ground-truth oracle and
small-corpus path.

Measurable gates:

- Exact top-k remains byte-deterministic and is the recall oracle.
- ANN recall@10 must be at least 0.98 on deterministic clustered fixtures.
- ANN p95 latency must beat exact scan by at least 3x at 100,000 vectors.
- Snapshot/restore must reproduce all source, graph, FTS, and vector results.
- Filtered retrieval must never cross tenant/namespace boundaries.

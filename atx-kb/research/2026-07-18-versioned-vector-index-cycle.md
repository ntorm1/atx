# Versioned vector indexes — implementation cycle 1

Date: 2026-07-18

## Primary-source findings

- Qdrant documents payload-aware HNSW indexes, immutable configuration choices, and optimizer-driven index construction. Its hybrid-query API also makes multistage retrieval and fusion explicit.
  - https://qdrant.tech/documentation/manage-data/indexing/
  - https://qdrant.tech/documentation/search/hybrid-queries/
- Vespa documents the accuracy/latency role of `hnsw.exploreAdditionalHits` and recommends exact nearest-neighbor search when filters are highly selective.
  - https://docs.vespa.ai/en/querying/approximate-nn-hnsw.html
- Qdrant's quantization documentation reinforces the importance of rescoring approximate candidates with original vectors when accuracy matters.
  - https://qdrant.tech/documentation/manage-data/quantization/
- BEIR shows that retrieval quality must be evaluated across heterogeneous datasets rather than inferred from one convenient benchmark.
  - https://arxiv.org/abs/2104.08663

## Decisions compiled into atx-kb

1. Persist HNSW as immutable generations with explicit `building`, `ready`, `active`, `retired`, and `failed` lifecycle states.
2. Assign every chunk-vector mutation a globally monotonic revision. A generation records its cutoff revision and freezes the exact vector bytes and revision used to construct it.
3. Keep activation separate from construction and atomically retire the prior active generation for the same model and dimensions.
4. Traverse stale snapshot nodes for connectivity, but only return a node when its current chunk revision still matches the frozen revision.
5. Exact-scan every post-cutoff mutation and merge that delta with ANN candidates before ranking.
6. Recompute cosine scores from the current full-precision vector bytes before returning candidates.
7. Fall back to an exact scan for filtered production requests or underfilled ANN results. Expose a raw-ANN mode only for evaluation.
8. Check finalized manifests, graph shape, vector bytes, revision bounds, entry point, edge count, and deterministic checksum during integrity verification.

## Evaluation result

The deterministic `atx-kb-vector-benchmark-v1` gate currently uses 1,024 normalized 48-dimensional vectors and 32 top-10 queries. Cycle-1 results:

- raw ANN recall@10: 1.0000 (gate: 0.98)
- exact-distance work reduction: 4.0068x (gate: 4.0x)
- warm query latency speedup: 5.7476x (gate: 1.0x)
- lifecycle, revision-delta, filtered fallback, checksum, and exact-equivalence tests: passing

These results validate the architecture and provide a regression floor; they are not yet evidence for 100k/1m-vector leadership.

## Next falsification targets

- Add 100k and 1m corpus tiers with per-corpus recall@10 gates and latency distributions.
- Replace construction-time full-size visited-vector clearing with generation counters for million-node builds.
- Add crash injection after the `building` manifest commit and during graph persistence, plus abandoned-build recovery.
- Add background construction on a separate connection and bounded cache memory/accounting.
- Evaluate filtered ANN traversal, quantized navigation with full-vector rescoring, and real embedding corpora.

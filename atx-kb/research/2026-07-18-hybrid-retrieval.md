# Hybrid retrieval and reranking patterns (2026-07-18)

Primary sources:

- Vespa hybrid-search tutorial: https://docs.vespa.ai/en/learn/tutorials/hybrid-search
- Qdrant hybrid queries: https://qdrant.tech/documentation/search/hybrid-queries/
- Weaviate hybrid search: https://docs.weaviate.io/weaviate/concepts/search/hybrid-search
- LanceDB hybrid search: https://docs.lancedb.com/search/hybrid-search
- LanceDB reranking: https://docs.lancedb.com/reranking

Leading systems combine BM25 and dense retrieval because exact identifiers and
semantic similarity fail on different queries. Qdrant and LanceDB default to
reciprocal-rank fusion (RRF) where raw score scales are incompatible. Weaviate
also offers normalized relative-score fusion. Vespa exposes phased ranking and
publishes retrieval quality with NDCG@10, making evaluation part of the search
design instead of relying on anecdotal examples.

Competitive systems expose reranker interfaces and multi-vector retrieval.
LanceDB supports model rerankers and multiple vector columns; Qdrant supports
named dense, sparse, and multivector representations. A production agent store
should allow retriever/reranker policies to be selected per query and should
record the policy and component ranks for reproducibility.

Immediate atx gaps: fixed RRF weights, no query-adaptive fusion, no reranker
callback, one vector per chunk, no query trace, and no golden relevance suite.

Measurable gates:

- Report MRR, recall@k, precision@k, and NDCG@10 for lexical, vector, graph,
  and fused modes independently.
- Hybrid NDCG@10 must never regress below the better single retriever on the
  checked-in golden fixture.
- Every hit must expose component rank/score and a reproducible query trace.
- Reranking must be pluggable without coupling atx-kb to a model dependency.

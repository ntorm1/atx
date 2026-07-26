# Filter-aware retrieval planning cycle (2026-07-18)

Primary comparison points:

- Qdrant indexing and filter-aware HNSW:
  https://qdrant.tech/documentation/manage-data/indexing/
- Vespa nearest-neighbor filtering:
  https://docs.vespa.ai/en/querying/approximate-nn-hnsw.html

The initial lexical path asked FTS5 for a global fixed-size top set and then
discarded sources that failed tag or metadata filters. A sufficiently large set
of stronger ineligible rows could consume that pre-limit and hide every eligible
result. The vector path also constructed eligibility by first loading every
source ID and repeatedly intersecting in memory.

The corrected planner builds indexed `EXISTS` predicates for every required tag
and key/value condition. Those predicates are pushed into the FTS candidate SQL,
so the ranking limit applies only after eligibility. The same predicate plan
selects vector/graph eligibility without an all-source scan. Search rejects empty,
invalid UTF-8, oversized, or excessive filter predicates before SQL compilation.

The deterministic evaluation fixture now includes 32 high-BM25 excluded sources,
a candidate limit of five, and one weaker eligible source. This exceeds the old
global pre-limit while remaining fast. Filtered Recall@1 remains 1.0 and leakage
remains zero across the full 110-document, 41-query quality suite; all other
quality gates remain green.

# atx-kb architecture

## Research basis

The design was cross-checked against current primary documentation before
implementation:

- [Qdrant's data model](https://qdrant.tech/documentation/overview/) associates
  vectors with structured payloads, supports indexed metadata filtering, and
  segments stored data. `atx-kb` mirrors the useful embedded subset with chunk
  vectors plus indexed tag/metadata tables.
- [Qdrant hybrid queries](https://qdrant.tech/documentation/search/hybrid-queries/)
  and [Milvus reranking](https://milvus.io/docs/reranking.md) both use RRF to
  combine result sets whose raw score scales are incompatible. `atx-kb` fuses
  BM25, cosine, and graph ranks with `k=60`, while retaining component scores
  for evaluation.
- [Milvus schema guidance](https://milvus.io/docs/schema-hands-on.md) separates
  scalar metadata from dense/sparse vector fields and applies scalar filters
  during retrieval. `atx-kb` makes tag and key/value filtering part of candidate
  eligibility, never a post-answer decoration.
- [Microsoft GraphRAG indexing](https://microsoft.github.io/graphrag/index/overview/)
  derives text units, entities, relationships, claims, summaries, and vector
  representations from raw documents. `atx-kb` persists the corresponding
  artifacts as normalized tables and records their provenance in a DAG.
- [GraphRAG query modes](https://microsoft.github.io/graphrag/) distinguish
  baseline vector search from local graph fan-out. `atx-kb` performs bounded
  fan-out over explicit source edges and shared entities as a third retrieval
  leg.

The systems above are distributed platforms or LLM pipelines. This project is
an in-process C++ component, so it deliberately adopts their data and retrieval
patterns without introducing a service, Python runtime, model dependency, or
new package.

## Storage model and invariants

| Artifact | Storage | Invariant |
| --- | --- | --- |
| Raw source | `sources.raw_text` | Exact bytes retained; SHA-256 unique |
| Observations | `source_observations` | Repeat URI/agent/time provenance retained after byte deduplication |
| Summary | `sources.summary` + summary node | Extractive and traceable to source |
| Chunks | `chunks` | Ordered, source-owned, independently citable |
| Lexical index | `chunks_fts` | External-content FTS5 index kept by triggers |
| Vectors | chunk BLOB/model/dimension | Finite, nonzero, normalized floats |
| Vector revisions | `vector_clock`, `chunks.vector_revision` | Globally monotonic mutation sequence |
| ANN generations | generation/node/edge tables | Immutable after ready; one active per model/dimension |
| Metadata | `source_tags`, `source_metadata` | Indexed and filterable |
| Keywords/entities | normalized linkage tables | Shared entities connect sources |
| Claims | `claims` | Exact text linked to a verified chunk byte range |
| Graph | `nodes`, `edges` | Provenance on every edge; explicit links cannot cycle |

Foreign keys, strict SQLite tables, uniqueness constraints, transactions, WAL,
and FTS/SQLite integrity checks enforce the durable boundary. A failed
submission rolls back every derived artifact.

## Ingestion

1. Validate the submission and compute SHA-256 over the exact raw text.
2. Reuse an existing source for duplicate content while recording a new
   observation and its tags/metadata.
3. Segment sentences into overlapping, citation-sized chunks.
4. Rank informative sentences for a concise extractive summary.
5. Extract weighted keywords, proper-name/topic entities, and summary claims.
6. Produce dependency-free local feature vectors.
7. Persist source, derived artifacts, FTS entries, and DAG edges atomically.

The local extractors are deterministic and auditable. They do not pretend to be
an LLM: agents can attach model-quality embeddings through the public API and
can add explicit evidence-bearing relationships after their own research.

## Retrieval

Each request independently ranks:

1. FTS5/BM25 chunks for precise token matches.
2. Same-model vectors by exact cosine or immutable HNSW plus an exact revision delta.
3. Sources reached through explicit links or shared entities.

Tag and metadata predicates constrain all three legs. Weighted RRF combines the
rank positions, avoiding a fragile linear mixture of unbounded BM25 and bounded
cosine scores. Hits carry the exact source ID, chunk ID, URI, evidence text,
and per-leg scores. Weak vector candidates below the request threshold are
excluded and final hits are source-deduplicated by default. `build_context()`
emits a strict character-budgeted `atx-evidence-v2` envelope whose query and
evidence strings are JSON escaped. The envelope identifies evidence as
untrusted data, prevents source text from forging framing/citation syntax, and
records an explicit abstention when no retrieval leg produces evidence.

The source/derivation graph remains a DAG. Immediate write transactions cover
deduplication and cycle-check-plus-insert operations, so separate agent
connections cannot commit duplicate derivations or race a graph cycle.

## Versioned vector retrieval

An HNSW build first commits a `building` manifest with a revision cutoff, then
constructs a deterministic graph from ascending chunk IDs and frozen
full-precision vectors. Persistence and transition to `ready` are atomic.
Activation separately retires the prior active generation for the same model
and dimensions. Ready, active, and retired nodes/edges are guarded against
mutation, and a warm in-process cache avoids reloading an immutable graph on
each query. Node persistence commits in 4,096-row batches and edge persistence
in 32,768-row batches, then verifies durable counts before transitioning the
manifest to ready. The cache has explicit byte accounting and a configurable
per-connection limit; the planner selects exact search when a generation would
exceed that limit.

Search traverses frozen nodes, hydrates candidates in one SQLite query, rejects
stale node revisions, rescales current full-precision vectors, and exact-scans
all matching chunks newer than the generation cutoff. Auto mode chooses exact
search for small or filtered sets; production fallback exact-scans filtered or
underfilled ANN results. Raw approximate mode is available for falsifiable
recall evaluation and reports whether the result is complete.

Construction and query traversal use epoch-marked visited arrays, avoiding an
`O(total_nodes)` clear for every HNSW layer. A crashed builder leaves a durable
manifest; recovery deletes partial graph rows while they are still mutable and
retains a failed manifest with the reason. Integrity verification recomputes
finalized graph checksums and checks revision, vector, entry-point, edge-count,
and lifecycle invariants.

## Backup and restore boundary

Raw copying of the main SQLite file is unsafe while WAL frames may contain
committed changes. `KnowledgeBase::backup_to()` instead uses the SQLite online
backup API, copies in bounded steps with finite busy/locked retries, and treats
only `SQLITE_DONE` plus a successful finish as completion. The candidate copy
is reopened and passed through schema initialization, SQLite/FK checks, content
hash and exact-support checks, graph-cycle detection, FTS verification, and
finalized HNSW checksums. Only then is the sibling partial file exposed at the
requested new name through an atomic no-overwrite hard link.

RRF weights and chunk size are intentionally explicit constants. Production
deployments should build a golden query/relevance set and tune these values with
NDCG/recall measurements rather than guessing from raw component scores.

`atx-kb-eval` is the first checked-in golden gate. Its fixed-seed quality suite
covers lexical, selective-filter, oracle-vector, hybrid-conflict, citation,
context-budget, and abstention behavior. The filter fixture deliberately places
more strong ineligible rows ahead of the eligible source than the candidate
limit; filter predicates are therefore compiled into the FTS candidate SQL, not
applied after a global ranking limit. Report output is deterministic JSON and
gate mode fails the build when any threshold regresses.

`atx-kb-vector-benchmark` is the deterministic exact-versus-ANN gate. Its first
tier checks recall@10 >= 0.98, at least 4x fewer exact distance evaluations, and
no warm-latency regression. Larger 100k/1m and heterogeneous real-embedding
tiers remain required before making leadership claims. The opt-in 100k tier is
available as `atx-kb-vector-benchmark-100k`; the first recorded run reached
0.9875 recall@10 with 100,000 vectors and a 69,785,272-byte measured cache,
while exact and ANN queries averaged 238.20 ms and 1.73 ms respectively. The
1m and heterogeneous real-embedding tiers remain open.

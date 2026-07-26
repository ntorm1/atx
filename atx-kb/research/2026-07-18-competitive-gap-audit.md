# Competitive gap and evidence-integrity audit (2026-07-18)

## Primary references

- Qdrant hybrid queries: https://qdrant.tech/documentation/search/hybrid-queries/
- Qdrant indexing: https://qdrant.tech/documentation/manage-data/indexing/
- Vespa phased ranking: https://docs.vespa.ai/en/ranking/phased-ranking.html
- GraphRAG query overview: https://microsoft.github.io/graphrag/query/overview/
- Graphiti repository and paper: https://github.com/getzep/graphiti and
  https://arxiv.org/abs/2501.13956
- BEIR: https://arxiv.org/abs/2104.08663
- RAGChecker: https://arxiv.org/abs/2408.08067

## Review result

The first atx-kb implementation established a dependency-free embedded baseline,
but the highest-risk gaps were evidence integrity rather than approximate nearest
neighbor speed. Prompt-ready Markdown allowed untrusted source text to forge
headings and citations. Extracted claims were associated with a summary index
instead of a verified source span. Duplicate bytes discarded the provenance of
later observations. Weak vector candidates could be returned without a minimum
similarity threshold, and concurrent deduplication or graph cycle checks could
race across connections.

Evidence-integrity v2 addresses that tranche before scale work:

- immutable content is separated from repeat source observations;
- every retained claim has an exact chunk byte range and integrity verification;
- context is a bounded, line-delimited JSON evidence envelope with explicit
  untrusted-data framing and escaped citation/control characters;
- retrieval has a vector similarity floor, source deduplication, and explicit
  abstention when no retrieval leg finds evidence;
- strict UTF-8 and resource limits protect ingestion and query boundaries;
- immediate SQLite write transactions make deduplication and DAG cycle checks
  safe across concurrent writers;
- the v1-to-v2 migration removes unsupported legacy claims instead of inventing
  provenance.

The next production gates are a reproducible retrieval/evidence evaluation
harness, filter-aware candidate planning, scalable vector indexing, model-version
lifecycle, and temporal multi-agent memory. Claims of superiority must be scoped
to measured workloads; the defensible initial target is the strongest embedded,
dependency-light, provenance-first agent knowledge engine rather than universal
distributed or GPU leadership.

## Verified cycle result

The live research database migrated from schema v1 to v2 and passed all integrity
checks. Unsupported legacy claims fell from eight to four because only exact
support spans survived. Eight concurrent submissions of identical content
produced one immutable source and eight distinct observations. The focused
atx-core and atx-kb suite passed after the migration and concurrency changes.

The new atx-db control plane adds workspace-isolated runs and agents,
dependency-aware tasks, atomic expiring leases, retry limits, append-only pollable
events, idempotent episode links to atx-kb observations, optimistic agent
revisions, and bitemporal facts. Its initial tests cover eight-agent contention,
dependency release, stale lease rejection, retry exhaustion, workspace isolation,
idempotency, temporal reconstruction, and database integrity.

# Atomic revision-stamped hybrid retrieval

Date: 2026-07-18

## Review finding

ATX already preserves immutable source bytes, exact chunk citations, observation
provenance, hybrid lexical/vector/graph retrieval, and bitemporal coordination
facts. The next strategic capability is a unified temporal recall plane across
those stores. Current `atx-kb` retrieval is not yet a sound cutoff for that
work: `search_detailed` performs its filter, lexical, vector, graph, hydration,
and entity reads as separate autocommit statements. A concurrent submission,
embedding update, link update, or ANN activation can therefore be observed by
only some retrieval legs.

SQLite WAL readers receive one stable view only while a read transaction stays
open. A `BEGIN` followed by multiple `SELECT` statements continues to see the
same historical snapshot even when another connection commits:
https://www.sqlite.org/isolation.html

This matters beyond conventional search plumbing. GraphRAG explicitly uses
text-unit links as provenance from derived knowledge back to source text, so a
hybrid context must not combine graph and text artifacts from different
commits:
https://microsoft.github.io/graphrag/index/default_dataflow/

Recent long-term-memory results also make retrieval a first-class evaluation
surface. MemMachine reports larger gains from retrieval-depth and context
formatting changes than from sentence chunking, while preserving full episodic
ground truth:
https://arxiv.org/abs/2604.04853
LongMemEval separately evaluates information extraction, multi-session
reasoning, temporal reasoning, knowledge updates, and abstention:
https://proceedings.iclr.cc/paper_files/paper/2025/hash/d813d324dbf0598bbdc9c8e79740ed01-Abstract-Conference.html

## Selected bounded increment

Schema v5 adds a singleton durable `knowledge_state_revision`. Search-visible
mutations advance it transactionally:

- every source observation insertion, which covers both new content and
  duplicate-content provenance/filter updates;
- every chunk embedding replacement;
- every explicit source-link insert, update, or delete; and
- every vector-index state transition that enters or leaves `active`.

The revision is a monotonic identity for durable knowledge state, not a promise
of byte-identical responses. Request parameters, approximate-search settings,
and warm-cache diagnostics remain part of the caller and process context.
Revision increments are conservative and need not correspond one-for-one with
public calls.

Every `search_detailed` call:

1. validates the complete request before opening a transaction;
2. begins one deferred read transaction;
3. makes the first stepped statement capture one UTC `observed_at` and the
   durable state revision, thereby pinning the SQLite snapshot;
4. runs every eligibility, lexical, exact/ANN vector, hydration, graph, and
   entity query inside that transaction; and
5. commits only after the complete ranked response is materialized.

`SearchResponse` exposes the captured snapshot marker. `build_context` uses the
detailed response rather than discarding it and emits an `atx-evidence-v3`
snapshot record before the query/evidence records. The context budget includes
that record. Existing `search()` remains source-compatible and continues to
return only hits.

## Deliberate limits

The revision identifies one snapshot of one `atx-kb` file. It does not provide
historical reads, a watch journal, or a cache-validity guarantee. It also does
not make an `atx-kb` search atomic with an `atx-db` fact/episode read: the two
objects own separate SQLite connections to separate files. A future unified
recall API must return both cutoffs and either use an explicit attached/read
transaction boundary or state clearly that it observed a two-store cut rather
than one instant.

This cycle does not add source trust, audience visibility, consolidation, fleet
pagination, scheduler finalizers, or event retention. Those remain independent
contracts and must not be smuggled into the meaning of a knowledge revision.

## Blocking evaluation

- A concurrent writer repeatedly submits uniquely identifiable matching
  sources while a reader searches from a separate connection. For every
  response, its revision and complete matching-source set must describe the
  same committed prefix.
- New and duplicate submissions, embedding replacement, explicit-link changes,
  and ANN activation advance the revision; pure reads and failed mutations do
  not.
- Revision overflow aborts the source mutation and leaves both state and
  provenance unchanged.
- A simulated schema-v4 database migrates to a valid baseline revision. Online
  backup/restore preserves the exact revision and produces the same snapshot
  marker on the restored search.
- Missing state or trigger corruption fails integrity verification rather than
  returning an unstamped response.
- Context-v3 snapshot fields cannot be forged by untrusted query/source text,
  remain inside the character budget, and preserve citation/abstention rules.
- Existing KB/DB/CLI tests, all 15 quality thresholds, ANN recall/work/latency
  gates, SHA-256 conformance, formatting, and whitespace checks remain blocking.

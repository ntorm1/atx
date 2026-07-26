# atx-kb

`atx-kb` is an embedded C++20 research knowledge base for agent workloads. A
submission stores the exact raw source, records every distinct observation of
that content, and atomically derives chunks, an
extractive summary, keywords, entities, claims, embeddings, metadata indexes,
and provenance edges. Retrieval combines SQLite FTS5, vectors, and graph
expansion with reciprocal-rank fusion (RRF), and always returns source/chunk
citations.

It adds no dependency: it uses the SQLite/FTS5 library already vendored by
`atx-core` and the C++ standard library.

## Data flow

```text
                         +-> FTS5 (BM25) --------+
agent -> raw submission -+-> chunks + vectors ---+-> RRF -> cited hits -> context pack
                         +-> summary / claims ----+
                         +-> entities / DAG ------+
                         +-> tags / metadata filters
```

Raw text is immutable and content-addressed by SHA-256. All derived rows are
written in one transaction, so a source is either fully indexed or absent.

## Agent CLI

```powershell
# Submit a file. Use - instead of a path to read stdin.
build/bin/atx-kb submit research.sqlite paper.txt `
  --title "Paper title" --uri "https://example/paper" `
  --agent "research-agent-7" --tag rag --meta status=reviewed

# JSON retrieval results include source_id, chunk_id, component/fused score,
# URI, title, and exact evidence text.
build/bin/atx-kb search research.sqlite "filtered hybrid retrieval" --limit 8

# A bounded atx-evidence-v2 envelope: JSON-escaped evidence records, stable
# citation IDs, explicit untrusted-content framing, and abstention when empty.
build/bin/atx-kb context research.sqlite "What supports the result?" --limit 8

build/bin/atx-kb show research.sqlite src_...
build/bin/atx-kb link research.sqlite src_parent src_child extends `
  --evidence "The child source explicitly extends section 4"
build/bin/atx-kb stats research.sqlite
build/bin/atx-kb verify research.sqlite
build/bin/atx-kb backup research.sqlite research-2026-07-18.sqlite
```

The command output is machine-readable JSON except `context`, which emits a
line-delimited evidence envelope. Do not remove its safety framing when placing
it in an agent prompt.

## C++ API

```cpp
#include "atx/kb/knowledge_base.hpp"

auto opened = atx::kb::KnowledgeBase::open("research.sqlite");
if (!opened) { /* handle opened.error() */ }

atx::kb::Submission source;
source.title = "Study";
source.raw_text = raw_text;
source.uri = canonical_uri;
source.submitted_by = agent_id;
source.tags = {"volatility", "reviewed"};
source.metadata = {{"dataset", "options-2026"}};
auto submitted = opened->submit(source);

atx::kb::SearchRequest query;
query.query = "evidence for volatility risk premium";
query.require_tags = {"reviewed"};
query.metadata_equals = {{"dataset", "options-2026"}};
query.min_vector_similarity = 0.2; // explicit weak-match abstention policy
auto evidence = opened->search(query);
```

The default `atx-hash-v1` vectors are deterministic local feature embeddings,
so the database works offline. An agent with a semantic embedding model can
replace each chunk vector with `set_chunk_embedding()` and pass the matching
model/dimensions in `SearchRequest`. Vectors are normalized at the boundary.

For larger collections, build and explicitly activate an immutable HNSW
generation. Every vector mutation receives a monotonic revision; changes after
the generation cutoff are exact-scanned and merged with ANN candidates.
Filtered or underfilled production searches fall back to an exact scan.

```cpp
atx::kb::VectorIndexBuildOptions index;
index.embedding_model = "semantic-v1";
index.dimensions = 768;
auto built = opened->build_vector_index(index);
if (built) {
  opened->activate_vector_index(built->id);
}

auto detailed = opened->search_detailed(query); // includes ANN/fallback diagnostics
opened->recover_abandoned_vector_indexes();     // marks old building manifests failed
opened->set_vector_index_cache_limit(2ULL * 1024 * 1024 * 1024); // explicit memory budget
auto backup = opened->backup_to("research-backup.sqlite"); // online + restored-copy verify
```

One `KnowledgeBase` owns one SQLite connection and must stay on one thread.
Concurrent agents should open one connection per thread/process; WAL mode and a
busy timeout are configured automatically.

`backup` uses SQLite's WAL-aware online backup API in bounded page steps. It
writes to a sibling `.partial` file, opens that copy through the full atx-kb
schema and integrity verifier, and publishes it with an atomic no-overwrite
hard link after explicitly checkpointing the verifier's WAL. The destination
must be a new path. A failed copy or verification never replaces an earlier
backup.

Schema v4 migrates v1-v3 databases in place. The v2 migration retains only
claims whose text can be located exactly in a supporting chunk; v3 adds vector
revisions and immutable generations; v4 adds recoverable failure manifests.
Run `verify` after upgrading a durable database.

## Build and test

```powershell
cmake --build build --target atx-kb-cli atx-kb-tests
ctest --test-dir build -R KnowledgeBase --output-on-failure

# Deterministic retrieval/evidence gate. JSON report mode never hides a red
# metric; --gate additionally returns nonzero when any threshold fails.
cmake --build build --target atx-kb-eval
build/bin/atx-kb-eval --suite quality --gate

# Exact-versus-ANN recall, work, and warm-latency regression gate.
cmake --build build --target atx-kb-vector-benchmark
build/bin/atx-kb-vector-benchmark

# Opt-in, approximately five-minute 100k provenance + vector + HNSW tier.
cmake --build build --target atx-kb-vector-benchmark-100k
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for schema, retrieval, research basis,
and scale boundaries.

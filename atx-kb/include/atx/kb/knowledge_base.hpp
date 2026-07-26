#pragma once

// atx::kb is an embedded, provenance-first research knowledge base. It keeps
// submitted source text losslessly, derives retrieval artifacts transactionally,
// and returns evidence-bearing hybrid (FTS + vector + graph) search results.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"

namespace atx::kb {

struct MetadataValue {
  std::string key;
  std::string value;
};

struct Submission {
  std::string title;
  std::string raw_text;
  std::string uri;
  std::string mime_type{"text/plain"};
  std::string author;
  std::string published_at;
  std::string submitted_by;
  std::vector<std::string> tags;
  std::vector<MetadataValue> metadata;
};

struct SubmitResult {
  std::string source_id;
  std::int64_t observation_id{};
  std::string content_hash;
  std::string summary;
  std::vector<std::string> keywords;
  std::vector<std::string> entities;
  std::int64_t chunk_count{};
  std::int64_t claim_count{};
  bool deduplicated{};
};

struct SourceObservation {
  std::int64_t id{};
  std::string title;
  std::string uri;
  std::string mime_type;
  std::string author;
  std::string published_at;
  std::string submitted_by;
  std::string observed_at;
  std::vector<std::string> tags;
  std::vector<MetadataValue> metadata;
};

struct ChunkRecord {
  std::int64_t id{};
  std::int64_t ordinal{};
  std::int64_t token_count{};
  std::int64_t vector_dimensions{};
  std::string vector_model;
  std::string text;
};

struct ClaimRecord {
  std::int64_t id{};
  std::int64_t chunk_id{};
  std::int64_t support_start{};
  std::int64_t support_length{};
  std::string text;
  double confidence{};
};

struct SourceRecord {
  std::string id;
  std::string content_hash;
  std::string title;
  std::string raw_text;
  std::string summary;
  std::string uri;
  std::string mime_type;
  std::string author;
  std::string published_at;
  std::string submitted_by;
  std::string created_at;
  std::vector<std::string> tags;
  std::vector<MetadataValue> metadata;
  std::vector<std::string> keywords;
  std::vector<std::string> entities;
  std::vector<ChunkRecord> chunks;
  std::vector<ClaimRecord> claims;
  std::vector<SourceObservation> observations;
};

enum class VectorSearchMode { Auto, Exact, Approximate };

struct SearchRequest {
  std::string query;
  // Empty uses the built-in deterministic atx-hash-v1 embedding. Agents can
  // provide model embeddings here after replacing chunk vectors with the same
  // model via set_chunk_embedding().
  std::vector<float> query_embedding;
  std::string embedding_model{"atx-hash-v1"};
  std::vector<std::string> require_tags;
  std::vector<MetadataValue> metadata_equals;
  std::size_t limit{10};
  std::size_t candidate_limit{100};
  std::size_t graph_depth{1};
  VectorSearchMode vector_mode{VectorSearchMode::Auto};
  // HNSW exploration width. Approximate search uses at least candidate_limit
  // internally; larger values trade latency for recall.
  std::size_t vector_ef_search{100};
  // Production requests preserve completeness by scanning exactly whenever an
  // ANN generation is absent, too stale, filtered, or returns too few hits.
  // Evaluators can disable this to measure raw ANN recall.
  bool allow_exact_vector_fallback{true};
  // Cosine threshold for the vector candidate leg. The default deliberately
  // abstains on weak/negative matches instead of returning arbitrary vectors.
  double min_vector_similarity{0.35};
  // Context assembly retains hits close to the best fused score. Search still
  // returns the full requested ranking; this threshold keeps weak one-leg
  // candidates from being presented to an agent as answer evidence.
  double min_context_score_ratio{0.75};
  bool deduplicate_sources{true};
};

struct SearchHit {
  std::string source_id;
  std::int64_t chunk_id{};
  std::int64_t chunk_ordinal{};
  std::string title;
  std::string uri;
  std::string text;
  double score{};
  double lexical_score{};
  double vector_score{};
  double graph_score{};
  std::vector<std::string> matched_entities;
};

struct VectorSearchDiagnostics {
  VectorSearchMode requested_mode{VectorSearchMode::Auto};
  VectorSearchMode used_mode{VectorSearchMode::Exact};
  std::int64_t generation_id{};
  std::int64_t cutoff_revision{};
  std::size_t indexed_nodes_examined{};
  std::size_t delta_nodes_examined{};
  std::size_t cache_bytes{};
  bool cache_hit{};
  bool exact_fallback{};
  bool complete{};
};

struct KnowledgeStateSnapshot {
  std::string observed_at;
  std::int64_t revision{};
};

struct SearchResponse {
  std::vector<SearchHit> hits;
  VectorSearchDiagnostics vector;
  KnowledgeStateSnapshot snapshot;
};

struct VectorIndexBuildOptions {
  std::string embedding_model;
  std::int64_t dimensions{};
  std::size_t max_connections{16};
  std::size_t ef_construction{200};
};

struct VectorIndexGeneration {
  std::int64_t id{};
  std::string embedding_model;
  std::int64_t dimensions{};
  std::int64_t cutoff_revision{};
  std::string state;
  std::int64_t entry_chunk_id{};
  std::int64_t max_level{};
  std::int64_t node_count{};
  std::int64_t edge_count{};
  std::string checksum;
  std::string failure_reason;
  std::string created_at;
  std::string activated_at;
};

struct ContextPack {
  std::string format_version{"atx-evidence-v3"};
  KnowledgeStateSnapshot snapshot;
  std::string safety_notice;
  std::string query;
  std::vector<SearchHit> evidence;
  // Length-budgeted, line-delimited JSON evidence envelope. Untrusted strings
  // are JSON escaped so evidence cannot forge citations or control framing.
  std::string markdown;
};

struct SourceLink {
  std::string from_source_id;
  std::string to_source_id;
  std::string relation;
  std::string evidence;
  double confidence{1.0};
};

struct RelatedSource {
  std::string source_id;
  std::string title;
  std::string relation;
  double confidence{};
  std::size_t distance{};
};

struct KnowledgeStats {
  std::int64_t sources{};
  std::int64_t chunks{};
  std::int64_t claims{};
  std::int64_t entities{};
  std::int64_t edges{};
};

class KnowledgeBase {
public:
  [[nodiscard]] static atx::core::Result<KnowledgeBase> open(std::string_view path);
  [[nodiscard]] static atx::core::Result<KnowledgeBase> open_memory();

  KnowledgeBase(KnowledgeBase &&) noexcept = default;
  KnowledgeBase &operator=(KnowledgeBase &&) noexcept = default;
  KnowledgeBase(const KnowledgeBase &) = delete;
  KnowledgeBase &operator=(const KnowledgeBase &) = delete;

  // Submission is idempotent by SHA-256 of the exact raw text. Raw source and
  // all derived artifacts commit atomically.
  [[nodiscard]] atx::core::Result<SubmitResult> submit(const Submission &submission);
  [[nodiscard]] atx::core::Result<SourceRecord> get_source(std::string_view source_id);

  [[nodiscard]] atx::core::Result<std::vector<SearchHit>> search(const SearchRequest &request);
  [[nodiscard]] atx::core::Result<SearchResponse> search_detailed(const SearchRequest &request);
  [[nodiscard]] atx::core::Result<ContextPack> build_context(const SearchRequest &request,
                                                             std::size_t max_characters = 12'000);

  // Replaces one derived chunk vector. All vectors used in one search leg must
  // share the query's model and dimensions.
  [[nodiscard]] atx::core::Status set_chunk_embedding(std::int64_t chunk_id,
                                                      std::span<const float> embedding,
                                                      std::string_view model);

  // Builds a deterministic immutable HNSW generation over a revision snapshot.
  // Activation is explicit and atomic; vector mutations after the snapshot are
  // searched as an exact delta until a newer generation is activated.
  [[nodiscard]] atx::core::Result<VectorIndexGeneration>
  build_vector_index(const VectorIndexBuildOptions &options);
  [[nodiscard]] atx::core::Status activate_vector_index(std::int64_t generation_id);
  [[nodiscard]] atx::core::Status retire_vector_index(std::int64_t generation_id);
  [[nodiscard]] atx::core::Result<std::int64_t>
  recover_abandoned_vector_indexes(std::int64_t minimum_age_seconds = 3'600);
  [[nodiscard]] atx::core::Result<std::vector<VectorIndexGeneration>> vector_indexes();
  void set_vector_index_cache_limit(std::size_t bytes) noexcept;
  [[nodiscard]] std::size_t vector_index_cache_limit() const noexcept {
    return vector_index_cache_limit_bytes_;
  }

  // Adds an explicit provenance-bearing edge. The source graph is a DAG: an
  // edge that would introduce a cycle is rejected.
  [[nodiscard]] atx::core::Status link_sources(const SourceLink &link);
  [[nodiscard]] atx::core::Result<std::vector<RelatedSource>>
  related_sources(std::string_view source_id, std::size_t max_depth = 2, std::size_t limit = 50);

  [[nodiscard]] atx::core::Result<KnowledgeStats> stats();
  [[nodiscard]] atx::core::Status verify_integrity();
  // Creates a WAL-aware online snapshot at a new destination path, verifies
  // the restored domain invariants, then publishes it from a sibling partial
  // file. Existing destination and partial files are never overwritten.
  [[nodiscard]] atx::core::Result<atx::core::db::BackupReport>
  backup_to(std::string_view destination_path, const atx::core::db::BackupOptions &options = {});

private:
  explicit KnowledgeBase(atx::core::db::Database database) : database_{std::move(database)} {}

  [[nodiscard]] atx::core::Status initialize();

  atx::core::db::Database database_;
  // Type-erased here to keep the persisted HNSW representation private to the
  // implementation while retaining a warm immutable generation across queries.
  std::shared_ptr<void> vector_index_cache_;
  std::size_t vector_index_cache_limit_bytes_{1U * 1024U * 1024U * 1024U};
};

} // namespace atx::kb

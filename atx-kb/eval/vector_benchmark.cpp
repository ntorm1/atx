#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "atx/kb/knowledge_base.hpp"

namespace {

struct Configuration {
  std::size_t documents{1'024};
  std::size_t dimensions{48};
  std::size_t queries{32};
  std::size_t top_k{10};
  std::size_t ef_search{10};
};

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

[[nodiscard]] std::vector<float> embedding(std::size_t document, std::size_t dimensions) {
  std::vector<float> out(dimensions);
  double norm = 0.0;
  std::uint64_t state = splitmix64(document + 0x4154584bULL);
  for (float &value : out) {
    state = splitmix64(state);
    const double unit = static_cast<double>(state >> 11U) * (1.0 / 9007199254740992.0);
    value = static_cast<float>(unit * 2.0 - 1.0);
    norm += static_cast<double>(value) * static_cast<double>(value);
  }
  const float scale = static_cast<float>(1.0 / std::sqrt(norm));
  for (float &value : out) {
    value *= scale;
  }
  return out;
}

[[nodiscard]] bool parse_size(std::string_view text, std::size_t &out) {
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_configuration(int argc, char **argv, Configuration &configuration) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (index + 1 >= argc) {
      return false;
    }
    std::size_t *target = nullptr;
    if (option == "--documents") {
      target = &configuration.documents;
    } else if (option == "--dimensions") {
      target = &configuration.dimensions;
    } else if (option == "--queries") {
      target = &configuration.queries;
    } else if (option == "--top-k") {
      target = &configuration.top_k;
    } else if (option == "--ef-search") {
      target = &configuration.ef_search;
    } else {
      return false;
    }
    if (!parse_size(argv[++index], *target)) {
      return false;
    }
  }
  return configuration.documents >= 100 && configuration.documents <= 1'000'000 &&
         configuration.dimensions >= 4 && configuration.dimensions <= 8'192 &&
         configuration.queries >= 1 && configuration.queries <= 1'000 && configuration.top_k >= 1 &&
         configuration.top_k <= 100 && configuration.ef_search >= configuration.top_k &&
         configuration.ef_search <= 100'000;
}

[[nodiscard]] double recall_at_k(const std::vector<atx::kb::SearchHit> &exact,
                                 const std::vector<atx::kb::SearchHit> &approximate) {
  std::unordered_set<std::int64_t> expected;
  for (const auto &hit : exact) {
    expected.insert(hit.chunk_id);
  }
  std::size_t matches = 0;
  for (const auto &hit : approximate) {
    matches += expected.contains(hit.chunk_id) ? 1U : 0U;
  }
  return expected.empty() ? 1.0
                          : static_cast<double>(matches) / static_cast<double>(expected.size());
}

} // namespace

int main(int argc, char **argv) {
  Configuration configuration;
  if (!parse_configuration(argc, argv, configuration)) {
    std::cerr << "usage: atx-kb-vector-benchmark [--documents N] [--dimensions N] "
                 "[--queries N] [--top-k N] [--ef-search N]\n";
    return 2;
  }
  auto opened = atx::kb::KnowledgeBase::open_memory();
  if (!opened) {
    std::cerr << opened.error().to_string() << '\n';
    return 2;
  }
  auto kb = std::move(*opened);
  const auto ingest_start = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < configuration.documents; ++index) {
    atx::kb::Submission input;
    input.title = "ANN benchmark document " + std::to_string(index);
    input.raw_text =
        "Deterministic persisted vector benchmark evidence record " + std::to_string(index) + ".";
    input.submitted_by = "atx-kb-vector-benchmark";
    auto submitted = kb.submit(input);
    if (!submitted) {
      std::cerr << submitted.error().to_string() << '\n';
      return 2;
    }
    auto source = kb.get_source(submitted->source_id);
    if (!source || source->chunks.size() != 1U) {
      std::cerr << (source ? "benchmark source produced multiple chunks"
                           : source.error().to_string())
                << '\n';
      return 2;
    }
    const auto vector = embedding(index, configuration.dimensions);
    auto status = kb.set_chunk_embedding(source->chunks.front().id, vector, "benchmark-v1");
    if (!status) {
      std::cerr << status.error().to_string() << '\n';
      return 2;
    }
  }
  const auto ingest_time = std::chrono::steady_clock::now() - ingest_start;

  atx::kb::VectorIndexBuildOptions build;
  build.embedding_model = "benchmark-v1";
  build.dimensions = static_cast<std::int64_t>(configuration.dimensions);
  build.max_connections = 16;
  build.ef_construction = 128;
  const auto build_start = std::chrono::steady_clock::now();
  auto generation = kb.build_vector_index(build);
  if (!generation) {
    std::cerr << generation.error().to_string() << '\n';
    return 2;
  }
  auto activated = kb.activate_vector_index(generation->id);
  if (!activated) {
    std::cerr << activated.error().to_string() << '\n';
    return 2;
  }
  const auto build_time = std::chrono::steady_clock::now() - build_start;

  atx::kb::SearchRequest base;
  base.query = "lexicaltokenabsentfrombenchmark";
  base.embedding_model = "benchmark-v1";
  base.min_vector_similarity = -1.0;
  base.graph_depth = 0;
  base.deduplicate_sources = false;
  base.limit = configuration.top_k;
  base.candidate_limit = configuration.top_k;
  base.vector_ef_search = configuration.ef_search;

  auto warm = base;
  warm.query_embedding = embedding(0, configuration.dimensions);
  warm.vector_mode = atx::kb::VectorSearchMode::Approximate;
  warm.allow_exact_vector_fallback = false;
  auto warmed = kb.search_detailed(warm);
  if (!warmed) {
    std::cerr << "failed to warm the immutable vector generation: " << warmed.error().to_string()
              << '\n';
    return 2;
  }

  double recall_sum = 0.0;
  std::size_t exact_examined = 0;
  std::size_t approximate_examined = 0;
  std::chrono::nanoseconds exact_time{};
  std::chrono::nanoseconds approximate_time{};
  for (std::size_t query_index = 0; query_index < configuration.queries; ++query_index) {
    base.query_embedding =
        embedding((query_index * 31U + 7U) % configuration.documents, configuration.dimensions);
    base.vector_mode = atx::kb::VectorSearchMode::Exact;
    const auto exact_start = std::chrono::steady_clock::now();
    auto exact = kb.search_detailed(base);
    exact_time += std::chrono::steady_clock::now() - exact_start;
    if (!exact) {
      std::cerr << exact.error().to_string() << '\n';
      return 2;
    }

    base.vector_mode = atx::kb::VectorSearchMode::Approximate;
    base.allow_exact_vector_fallback = false;
    const auto approximate_start = std::chrono::steady_clock::now();
    auto approximate = kb.search_detailed(base);
    approximate_time += std::chrono::steady_clock::now() - approximate_start;
    if (!approximate) {
      std::cerr << approximate.error().to_string() << '\n';
      return 2;
    }
    recall_sum += recall_at_k(exact->hits, approximate->hits);
    exact_examined += exact->vector.indexed_nodes_examined;
    approximate_examined += approximate->vector.indexed_nodes_examined;
  }

  auto integrity = kb.verify_integrity();
  if (!integrity) {
    std::cerr << integrity.error().to_string() << '\n';
    return 2;
  }
  const double recall = recall_sum / static_cast<double>(configuration.queries);
  const double work_reduction =
      approximate_examined == 0
          ? 0.0
          : static_cast<double>(exact_examined) / static_cast<double>(approximate_examined);
  const double latency_speedup =
      approximate_time.count() == 0
          ? 0.0
          : static_cast<double>(exact_time.count()) / static_cast<double>(approximate_time.count());
  const auto seconds = [](auto duration) {
    return std::chrono::duration<double>(duration).count();
  };
  const double exact_average_ms = std::chrono::duration<double, std::milli>(exact_time).count() /
                                  static_cast<double>(configuration.queries);
  const double approximate_average_ms =
      std::chrono::duration<double, std::milli>(approximate_time).count() /
      static_cast<double>(configuration.queries);
  constexpr double recall_threshold = 0.98;
  constexpr double work_threshold = 4.0;
  const double latency_threshold =
      configuration.documents >= 1'000'000 ? 5.0 : (configuration.documents >= 100'000 ? 2.0 : 1.0);
  const bool passed = recall >= recall_threshold && work_reduction >= work_threshold &&
                      latency_speedup >= latency_threshold;
  std::cout << std::fixed << std::setprecision(6)
            << "{\"suite\":\"atx-kb-vector-benchmark-v1\",\"passed\":"
            << (passed ? "true" : "false") << ",\"documents\":" << configuration.documents
            << ",\"dimensions\":" << configuration.dimensions
            << ",\"queries\":" << configuration.queries << ",\"generation_id\":" << generation->id
            << ",\"nodes\":" << generation->node_count << ",\"edges\":" << generation->edge_count
            << ",\"cache_bytes\":" << warmed->vector.cache_bytes
            << ",\"ingest_seconds\":" << seconds(ingest_time)
            << ",\"build_seconds\":" << seconds(build_time)
            << ",\"exact_average_ms\":" << exact_average_ms
            << ",\"approximate_average_ms\":" << approximate_average_ms << ",\"metrics\":["
            << "{\"name\":\"ann_recall_at_10\",\"value\":" << recall
            << ",\"threshold\":" << recall_threshold << "},"
            << "{\"name\":\"distance_work_reduction\",\"value\":" << work_reduction
            << ",\"threshold\":" << work_threshold << "},"
            << "{\"name\":\"warm_latency_speedup\",\"value\":" << latency_speedup
            << ",\"threshold\":" << latency_threshold << "}]}\n";
  return passed ? 0 : 1;
}

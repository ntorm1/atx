#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "atx/kb/knowledge_base.hpp"

namespace atx::kb::eval {
namespace {

constexpr std::string_view kSuiteVersion = "atx-kb-eval-v1";
constexpr std::uint64_t kDefaultSeed = 0x4154584BULL;
constexpr std::size_t kOracleDimensions = 40;

struct ExpectedSource {
  std::string key;
  int relevance{};
};

struct DocumentFixture {
  std::string key;
  Submission submission;
  std::vector<float> embedding;
  std::string embedding_model;
};

struct QueryFixture {
  std::string id;
  std::string category;
  SearchRequest request;
  std::vector<ExpectedSource> expected;
  std::size_t context_budget{4'096};
  bool expect_abstention{};
};

struct QualityFixture {
  std::vector<DocumentFixture> documents;
  std::vector<QueryFixture> queries;
};

struct Metric {
  std::string name;
  double value{};
  double threshold{};
  bool higher_is_better{true};
  std::size_t samples{};

  [[nodiscard]] bool passed() const noexcept {
    constexpr double tolerance = 1.0e-12;
    return higher_is_better ? value + tolerance >= threshold : value <= threshold + tolerance;
  }
};

struct EvaluationResult {
  std::vector<Metric> metrics;
  std::size_t documents{};
  std::size_t queries{};
  std::size_t chunks{};
};

[[nodiscard]] std::string numbered(std::string_view prefix, std::size_t value) {
  std::string out{prefix};
  if (value < 10) {
    out.push_back('0');
  }
  out += std::to_string(value);
  return out;
}

[[nodiscard]] std::vector<float> unit_vector(std::size_t dimension, float secondary = 0.0F,
                                             std::size_t secondary_dimension = 0) {
  std::vector<float> out(kOracleDimensions, 0.0F);
  out.at(dimension) = 1.0F;
  if (secondary != 0.0F) {
    out.at(secondary_dimension) = secondary;
  }
  return out;
}

[[nodiscard]] Submission submission(std::string title, std::string text) {
  Submission out;
  out.title = std::move(title);
  out.raw_text = std::move(text);
  out.uri = "https://eval.invalid/" + out.title;
  out.submitted_by = "atx-kb-eval-v1";
  out.tags = {"eval-v1"};
  out.metadata = {{"fixture", "quality-v1"}};
  return out;
}

void add_expected(QueryFixture &query, std::string key, int relevance = 3) {
  query.expected.push_back({std::move(key), relevance});
}

[[nodiscard]] QualityFixture make_quality_fixture(std::uint64_t seed) {
  QualityFixture fixture;
  const std::string seed_text = std::to_string(seed);

  // Exact lexical cases pair one rare marker with a high-frequency distractor.
  for (std::size_t i = 0; i < 12; ++i) {
    const std::string suffix = numbered("", i);
    const std::string marker = "lexgold" + suffix;
    const std::string gold_key = "lex-gold-" + suffix;
    const std::string distractor_key = "lex-distractor-" + suffix;
    fixture.documents.push_back(
        {gold_key,
         submission("Lexical Gold " + suffix,
                    "Project Lexical verified the immutable evidence marker " + marker +
                        ". This exact record is the authoritative finding for evaluation seed " +
                        seed_text + "."),
         {},
         {}});
    fixture.documents.push_back(
        {distractor_key,
         submission("Lexical Distractor " + suffix,
                    "Project Lexical background evidence repeats verified finding evidence "
                    "finding for case " +
                        suffix + ", but it never contains the authoritative marker."),
         {},
         {}});

    QueryFixture query;
    query.id = "lexical-" + suffix;
    query.category = "lexical";
    query.request.query = marker;
    query.request.embedding_model = "disabled";
    query.request.limit = 5;
    query.request.graph_depth = 0;
    add_expected(query, gold_key);
    fixture.queries.push_back(std::move(query));
  }

  // Filter cases deliberately make the ineligible source lexically stronger.
  for (std::size_t i = 0; i < 6; ++i) {
    const std::string suffix = numbered("", i);
    const std::string marker = "filtermark" + suffix;
    const std::string target_key = "filter-target-" + suffix;
    auto target =
        submission("Filtered Target " + suffix, "The reviewed tenant record contains " + marker +
                                                    " and is the sole eligible research evidence.");
    target.tags.push_back("tenant-green");
    target.metadata.push_back({"stage", "reviewed"});
    fixture.documents.push_back({target_key, std::move(target), {}, {}});

    auto excluded = submission("Filtered Excluded " + suffix,
                               marker + " " + marker + " " + marker +
                                   " appears frequently in this draft tenant record.");
    excluded.tags.push_back("tenant-blue");
    excluded.metadata.push_back({"stage", "draft"});
    fixture.documents.push_back({"filter-excluded-" + suffix, std::move(excluded), {}, {}});
    if (i == 0) {
      // More ineligible high-BM25 rows than the old global FTS pre-limit. A
      // correct planner pushes eligibility into the candidate SQL and still
      // retrieves the weaker eligible target.
      for (std::size_t distractor = 0; distractor < 32; ++distractor) {
        auto crowded =
            submission("Filtered Crowding Distractor " + std::to_string(distractor),
                       marker + " " + marker + " " + marker + " " + marker +
                           " belongs to excluded draft row " + std::to_string(distractor) + ".");
        crowded.tags.push_back("tenant-blue");
        crowded.metadata.push_back({"stage", "draft"});
        fixture.documents.push_back(
            {"filter-crowding-" + std::to_string(distractor), std::move(crowded), {}, {}});
      }
    }

    QueryFixture query;
    query.id = "filter-" + suffix;
    query.category = "filter";
    query.request.query = marker;
    query.request.embedding_model = "disabled";
    query.request.require_tags = {"TENANT-GREEN"};
    query.request.metadata_equals = {{"stage", "reviewed"}};
    query.request.limit = 5;
    query.request.candidate_limit = 5;
    query.request.graph_depth = 0;
    add_expected(query, target_key);
    fixture.queries.push_back(std::move(query));
  }

  // Oracle-vector cases separate candidate/ranking correctness from model quality.
  for (std::size_t i = 0; i < 8; ++i) {
    const std::string suffix = numbered("", i);
    const std::string gold_key = "vector-gold-" + suffix;
    fixture.documents.push_back(
        {gold_key,
         submission("Vector Gold " + suffix,
                    "This document has deliberately unrelated surface words for oracle case " +
                        suffix + "."),
         unit_vector(i), "eval-oracle-v1"});
    fixture.documents.push_back(
        {"vector-distractor-" + suffix,
         submission("Vector Distractor " + suffix,
                    "This distinct unrelated document tests exact cosine ranking for case " +
                        suffix + "."),
         unit_vector(8 + i), "eval-oracle-v1"});

    QueryFixture query;
    query.id = "vector-" + suffix;
    query.category = "vector";
    query.request.query = "unseen semantic request " + suffix;
    query.request.query_embedding = unit_vector(i);
    query.request.embedding_model = "eval-oracle-v1";
    query.request.limit = 5;
    query.request.graph_depth = 0;
    add_expected(query, gold_key);
    fixture.queries.push_back(std::move(query));
  }

  // Hybrid cases make lexical and vector-only distractors individually strong.
  for (std::size_t i = 0; i < 6; ++i) {
    const std::string suffix = numbered("", i);
    const std::string marker = "hybridmark" + suffix;
    const std::string gold_key = "hybrid-gold-" + suffix;
    fixture.documents.push_back(
        {gold_key,
         submission("Hybrid Gold " + suffix,
                    "The authoritative hybrid evidence contains " + marker + " exactly once."),
         unit_vector(16 + i), "eval-oracle-v1"});
    fixture.documents.push_back({"hybrid-lexical-" + suffix,
                                 submission("Hybrid Lexical Distractor " + suffix,
                                            marker + " " + marker + " " + marker +
                                                " is repeated without semantic support."),
                                 unit_vector(22 + i), "eval-oracle-v1"});
    fixture.documents.push_back({"hybrid-vector-" + suffix,
                                 submission("Hybrid Vector Distractor " + suffix,
                                            "This source is close in vector space for case " +
                                                suffix + " but lacks the requested marker."),
                                 unit_vector(16 + i, 0.75F, 28 + i), "eval-oracle-v1"});

    QueryFixture query;
    query.id = "hybrid-" + suffix;
    query.category = "hybrid";
    query.request.query = marker;
    query.request.query_embedding = unit_vector(16 + i);
    query.request.embedding_model = "eval-oracle-v1";
    query.request.limit = 5;
    query.request.graph_depth = 0;
    add_expected(query, gold_key);
    fixture.queries.push_back(std::move(query));
  }

  // Short evidence plus long titles/URIs exposes whether the final source ledger
  // is included in the advertised context-character budget.
  QueryFixture budget_query;
  budget_query.id = "context-budget";
  budget_query.category = "budget";
  budget_query.request.query = "budgetanchor";
  budget_query.request.embedding_model = "disabled";
  budget_query.request.limit = 8;
  budget_query.request.graph_depth = 0;
  budget_query.context_budget = 512;
  for (std::size_t i = 0; i < 8; ++i) {
    const std::string suffix = numbered("", i);
    const std::string key = "budget-" + suffix;
    auto source = submission(
        "Context Budget Source With A Deliberately Long Stable Title " + suffix,
        "budgetanchor evidence " + suffix + " is short, exact, and independently citable.");
    source.uri = "https://eval.invalid/context-budget/a-deliberately-long-canonical-uri/" + suffix +
                 "/source-record";
    fixture.documents.push_back({key, std::move(source), {}, {}});
    add_expected(budget_query, key, 1);
  }
  fixture.queries.push_back(std::move(budget_query));

  for (std::size_t i = 0; i < 4; ++i) {
    QueryFixture lexical;
    lexical.id = numbered("abstain-lexical-", i);
    lexical.category = "abstain-lexical";
    lexical.request.query = numbered("missinglexicaltoken", i);
    lexical.request.embedding_model = "disabled";
    lexical.request.limit = 5;
    lexical.request.graph_depth = 0;
    lexical.expect_abstention = true;
    fixture.queries.push_back(std::move(lexical));

    QueryFixture local;
    local.id = numbered("abstain-default-", i);
    local.category = "abstain-default";
    local.request.query = numbered("unsupportedquasarrequest", i);
    local.request.limit = 5;
    local.request.graph_depth = 0;
    local.expect_abstention = true;
    fixture.queries.push_back(std::move(local));
  }
  return fixture;
}

[[nodiscard]] double relevance_for(std::span<const ExpectedSource> expected, std::string_view key) {
  const auto found = std::find_if(expected.begin(), expected.end(),
                                  [&](const auto &item) { return item.key == key; });
  return found == expected.end() ? 0.0 : static_cast<double>(found->relevance);
}

[[nodiscard]] double recall_at(std::span<const std::string> ranked,
                               std::span<const ExpectedSource> expected, std::size_t k) {
  if (expected.empty()) {
    return ranked.empty() ? 1.0 : 0.0;
  }
  std::unordered_set<std::string> relevant;
  for (const auto &item : expected) {
    if (item.relevance > 0) {
      relevant.insert(item.key);
    }
  }
  std::size_t found = 0;
  for (std::size_t i = 0; i < std::min(k, ranked.size()); ++i) {
    if (relevant.contains(ranked[i])) {
      ++found;
    }
  }
  return static_cast<double>(found) / static_cast<double>(relevant.size());
}

[[nodiscard]] double ndcg_at(std::span<const std::string> ranked,
                             std::span<const ExpectedSource> expected, std::size_t k) {
  double dcg = 0.0;
  for (std::size_t i = 0; i < std::min(k, ranked.size()); ++i) {
    const double relevance = relevance_for(expected, ranked[i]);
    dcg += (std::exp2(relevance) - 1.0) / std::log2(static_cast<double>(i) + 2.0);
  }
  std::vector<int> ideal;
  ideal.reserve(expected.size());
  for (const auto &item : expected) {
    ideal.push_back(item.relevance);
  }
  std::sort(ideal.begin(), ideal.end(), std::greater<>{});
  double idcg = 0.0;
  for (std::size_t i = 0; i < std::min(k, ideal.size()); ++i) {
    idcg +=
        (std::exp2(static_cast<double>(ideal[i])) - 1.0) / std::log2(static_cast<double>(i) + 2.0);
  }
  return idcg == 0.0 ? (ranked.empty() ? 1.0 : 0.0) : dcg / idcg;
}

[[nodiscard]] std::vector<std::string>
logical_ranking(std::span<const SearchHit> hits,
                const std::unordered_map<std::string, std::string> &source_to_key) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const auto &hit : hits) {
    const auto found = source_to_key.find(hit.source_id);
    if (found != source_to_key.end() && seen.insert(found->second).second) {
      out.push_back(found->second);
    }
  }
  return out;
}

[[nodiscard]] bool same_ranking(std::span<const SearchHit> lhs,
                                std::span<const SearchHit> rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].source_id != rhs[i].source_id || lhs[i].chunk_id != rhs[i].chunk_id ||
        lhs[i].score != rhs[i].score) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] EvaluationResult run_quality(std::uint64_t seed, std::string &error) {
  EvaluationResult result;
  const QualityFixture fixture = make_quality_fixture(seed);
  result.documents = fixture.documents.size();
  result.queries = fixture.queries.size();

  auto opened = KnowledgeBase::open_memory();
  if (!opened) {
    error = opened.error().to_string();
    return result;
  }
  auto kb = std::move(*opened);

  std::unordered_map<std::string, std::string> source_to_key;
  struct StoredChunk {
    std::string source_id;
    std::string text;
  };
  std::unordered_map<std::int64_t, StoredChunk> stored_chunks;

  for (const auto &document : fixture.documents) {
    auto submitted = kb.submit(document.submission);
    if (!submitted) {
      error = "submit " + document.key + ": " + submitted.error().to_string();
      return result;
    }
    source_to_key.emplace(submitted->source_id, document.key);
    auto loaded = kb.get_source(submitted->source_id);
    if (!loaded) {
      error = "load " + document.key + ": " + loaded.error().to_string();
      return result;
    }
    result.chunks += loaded->chunks.size();
    for (const auto &chunk : loaded->chunks) {
      stored_chunks.emplace(chunk.id, StoredChunk{loaded->id, chunk.text});
      if (!document.embedding.empty()) {
        auto status =
            kb.set_chunk_embedding(chunk.id, document.embedding, document.embedding_model);
        if (!status) {
          error = "embedding " + document.key + ": " + status.error().to_string();
          return result;
        }
      }
    }
  }

  double lexical_recall = 0.0;
  double filter_recall = 0.0;
  double vector_recall = 0.0;
  double hybrid_ndcg = 0.0;
  std::size_t lexical_count = 0;
  std::size_t filter_count = 0;
  std::size_t vector_count = 0;
  std::size_t hybrid_count = 0;
  std::size_t filter_hits = 0;
  std::size_t filter_leaks = 0;
  std::size_t citation_count = 0;
  std::size_t valid_citations = 0;
  std::size_t deterministic_count = 0;
  std::size_t deterministic_matches = 0;
  std::size_t lexical_abstentions = 0;
  std::size_t lexical_abstention_successes = 0;
  std::size_t default_abstentions = 0;
  std::size_t default_abstention_successes = 0;
  std::size_t context_budget_count = 0;
  std::size_t context_budget_successes = 0;
  double context_budget_recall_sum = 0.0;
  std::size_t ledger_entries = 0;
  std::size_t valid_ledger_entries = 0;
  std::size_t context_evidence = 0;
  std::size_t relevant_context_evidence = 0;
  double context_recall_sum = 0.0;
  std::size_t context_recall_count = 0;

  for (const auto &query : fixture.queries) {
    auto searched = kb.search(query.request);
    if (!searched) {
      error = "search " + query.id + ": " + searched.error().to_string();
      return result;
    }
    auto repeated = kb.search(query.request);
    if (!repeated) {
      error = "repeat search " + query.id + ": " + repeated.error().to_string();
      return result;
    }
    ++deterministic_count;
    if (same_ranking(*searched, *repeated)) {
      ++deterministic_matches;
    }

    const auto ranking = logical_ranking(*searched, source_to_key);
    if (query.category == "lexical") {
      lexical_recall += recall_at(ranking, query.expected, 1);
      ++lexical_count;
    } else if (query.category == "filter") {
      filter_recall += recall_at(ranking, query.expected, 1);
      ++filter_count;
      for (const auto &key : ranking) {
        ++filter_hits;
        if (relevance_for(query.expected, key) <= 0.0) {
          ++filter_leaks;
        }
      }
    } else if (query.category == "vector") {
      vector_recall += recall_at(ranking, query.expected, 1);
      ++vector_count;
    } else if (query.category == "hybrid") {
      hybrid_ndcg += ndcg_at(ranking, query.expected, 10);
      ++hybrid_count;
    } else if (query.category == "abstain-lexical") {
      ++lexical_abstentions;
      if (searched->empty()) {
        ++lexical_abstention_successes;
      }
    } else if (query.category == "abstain-default") {
      ++default_abstentions;
      if (searched->empty()) {
        ++default_abstention_successes;
      }
    }

    for (const auto &hit : *searched) {
      ++citation_count;
      const auto found = stored_chunks.find(hit.chunk_id);
      if (found != stored_chunks.end() && found->second.source_id == hit.source_id &&
          found->second.text == hit.text) {
        ++valid_citations;
      }
    }

    auto context = kb.build_context(query.request, query.context_budget);
    if (!context) {
      error = "context " + query.id + ": " + context.error().to_string();
      return result;
    }
    if (query.category == "budget") {
      ++context_budget_count;
      if (context->markdown.size() <= query.context_budget) {
        ++context_budget_successes;
      }
      const auto budget_ranking = logical_ranking(context->evidence, source_to_key);
      context_budget_recall_sum += recall_at(budget_ranking, query.expected, budget_ranking.size());
    }
    for (std::size_t i = 0; i < context->evidence.size(); ++i) {
      ++ledger_entries;
      const std::string legacy_label = "[S" + std::to_string(i + 1) + "]";
      const std::string envelope_label = "\"citation\":\"S" + std::to_string(i + 1) + "\"";
      const bool labelled = context->markdown.find(legacy_label) != std::string::npos ||
                            context->markdown.find(envelope_label) != std::string::npos;
      if (labelled && context->markdown.find(context->evidence[i].source_id) != std::string::npos) {
        ++valid_ledger_entries;
      }
    }
    if (!query.expect_abstention && query.category != "budget") {
      std::vector<std::string> context_keys;
      for (const auto &hit : context->evidence) {
        const auto found = source_to_key.find(hit.source_id);
        if (found != source_to_key.end()) {
          context_keys.push_back(found->second);
          ++context_evidence;
          if (relevance_for(query.expected, found->second) > 0.0) {
            ++relevant_context_evidence;
          }
        }
      }
      context_recall_sum += recall_at(context_keys, query.expected, context_keys.size());
      ++context_recall_count;
    }
  }

  const auto ratio = [](std::size_t numerator, std::size_t denominator) {
    return denominator == 0 ? 1.0
                            : static_cast<double>(numerator) / static_cast<double>(denominator);
  };
  const std::vector<std::string> metric_perfect{"a", "b"};
  const std::vector<std::string> metric_reversed{"b", "a"};
  const std::vector<ExpectedSource> metric_qrels{{"a", 3}, {"b", 1}};
  const bool metric_math_ok = std::abs(ndcg_at(metric_perfect, metric_qrels, 10) - 1.0) < 1.0e-12 &&
                              ndcg_at(metric_reversed, metric_qrels, 10) < 1.0 &&
                              std::abs(recall_at(metric_perfect, metric_qrels, 1) - 0.5) < 1.0e-12;
  result.metrics = {
      {"metric_math_self_test", metric_math_ok ? 1.0 : 0.0, 1.0, true, 3},
      {"lexical_recall_at_1", lexical_recall / static_cast<double>(lexical_count), 1.0, true,
       lexical_count},
      {"filtered_recall_at_1", filter_recall / static_cast<double>(filter_count), 1.0, true,
       filter_count},
      {"filter_leakage_rate", ratio(filter_leaks, filter_hits), 0.0, false, filter_hits},
      {"oracle_vector_recall_at_1", vector_recall / static_cast<double>(vector_count), 1.0, true,
       vector_count},
      {"hybrid_ndcg_at_10", hybrid_ndcg / static_cast<double>(hybrid_count), 0.97, true,
       hybrid_count},
      {"citation_validity", ratio(valid_citations, citation_count), 1.0, true, citation_count},
      {"context_ledger_validity", ratio(valid_ledger_entries, ledger_entries), 1.0, true,
       ledger_entries},
      {"context_budget_compliance", ratio(context_budget_successes, context_budget_count), 1.0,
       true, context_budget_count},
      {"context_budget_source_recall",
       context_budget_count == 0
           ? 1.0
           : context_budget_recall_sum / static_cast<double>(context_budget_count),
       0.10, true, context_budget_count},
      {"context_evidence_precision", ratio(relevant_context_evidence, context_evidence), 0.80, true,
       context_evidence},
      {"context_source_recall",
       context_recall_count == 0 ? 1.0
                                 : context_recall_sum / static_cast<double>(context_recall_count),
       0.90, true, context_recall_count},
      {"lexical_abstention_accuracy", ratio(lexical_abstention_successes, lexical_abstentions), 1.0,
       true, lexical_abstentions},
      {"default_abstention_accuracy", ratio(default_abstention_successes, default_abstentions),
       0.95, true, default_abstentions},
      {"repeat_ranking_determinism", ratio(deterministic_matches, deterministic_count), 1.0, true,
       deterministic_count},
  };
  return result;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) >= 0x20U) {
        out.push_back(c);
      }
      break;
    }
  }
  return out;
}

void emit_json(const EvaluationResult &result, std::uint64_t seed, bool gate,
               std::string_view error) {
  const bool metrics_passed = std::all_of(result.metrics.begin(), result.metrics.end(),
                                          [](const auto &metric) { return metric.passed(); });
  const bool passed = error.empty() && metrics_passed;
  std::cout << std::setprecision(10);
  std::cout << "{\"suite\":\"quality\",\"suite_version\":\"" << kSuiteVersion
            << "\",\"seed\":" << seed << ",\"gate\":" << (gate ? "true" : "false")
            << ",\"passed\":" << (passed ? "true" : "false")
            << ",\"all_thresholds_met\":" << (metrics_passed ? "true" : "false")
            << ",\"documents\":" << result.documents << ",\"chunks\":" << result.chunks
            << ",\"queries\":" << result.queries;
  if (!error.empty()) {
    std::cout << ",\"error\":\"" << json_escape(error) << '"';
  }
  std::cout << ",\"metrics\": [";
  for (std::size_t i = 0; i < result.metrics.size(); ++i) {
    const auto &metric = result.metrics[i];
    if (i != 0) {
      std::cout << ',';
    }
    std::cout << "{\"name\":\"" << json_escape(metric.name) << "\",\"value\":" << metric.value
              << ",\"threshold\":" << metric.threshold << ",\"comparison\":\""
              << (metric.higher_is_better ? ">=" : "<=") << "\",\"samples\":" << metric.samples
              << ",\"passed\":" << (metric.passed() ? "true" : "false") << '}';
  }
  std::cout << "]}\n";
}

void usage() { std::cerr << "usage: atx-kb-eval [--suite quality] [--seed N] [--gate]\n"; }

[[nodiscard]] bool parse_seed(std::string_view text, std::uint64_t &seed) noexcept {
  const char *const begin = text.data();
  const char *const end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, seed);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

} // namespace
} // namespace atx::kb::eval

int main(int argc, char **argv) {
  std::string_view suite = "quality";
  std::uint64_t seed = atx::kb::eval::kDefaultSeed;
  bool gate = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument{argv[i]};
    if (argument == "--suite") {
      if (i + 1 >= argc) {
        atx::kb::eval::usage();
        return 2;
      }
      suite = argv[++i];
    } else if (argument == "--seed") {
      if (i + 1 >= argc || !atx::kb::eval::parse_seed(argv[++i], seed)) {
        atx::kb::eval::usage();
        return 2;
      }
    } else if (argument == "--gate") {
      gate = true;
    } else if (argument == "--help" || argument == "-h") {
      atx::kb::eval::usage();
      return 0;
    } else {
      atx::kb::eval::usage();
      return 2;
    }
  }
  if (suite != "quality") {
    atx::kb::eval::usage();
    return 2;
  }

  std::string error;
  const auto result = atx::kb::eval::run_quality(seed, error);
  atx::kb::eval::emit_json(result, seed, gate, error);
  if (!error.empty()) {
    return 2;
  }
  if (gate && std::any_of(result.metrics.begin(), result.metrics.end(),
                          [](const auto &metric) { return !metric.passed(); })) {
    return 1;
  }
  return 0;
}

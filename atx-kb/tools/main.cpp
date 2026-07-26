#include <algorithm>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "atx/kb/knowledge_base.hpp"

namespace {

[[nodiscard]] std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
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

[[nodiscard]] bool parse_size(std::string_view text, std::size_t &out) {
  const char *const begin = text.data();
  const char *const end = begin + text.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_i64(std::string_view text, std::int64_t &out) {
  const char *const begin = text.data();
  const char *const end = begin + text.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool option_value(int argc, char **argv, int &index, std::string &out) {
  if (index + 1 >= argc) {
    return false;
  }
  out = argv[++index];
  return true;
}

[[nodiscard]] std::string read_text(std::string_view path) {
  if (path == "-") {
    return std::string{std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{}};
  }
  std::ifstream stream{std::string{path}, std::ios::binary};
  if (!stream) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void usage() {
  std::cerr << "atx-kb: embedded research DAG/RAG store\n\n"
            << "  atx-kb submit <db> <file|-> [--title T] [--uri U] [--author A]\n"
            << "                [--agent ID] [--tag TAG]... [--meta KEY=VALUE]...\n"
            << "  atx-kb search <db> <query> [--limit N] [--tag TAG]...\n"
            << "  atx-kb context <db> <query> [--limit N] [--max-chars N]\n"
            << "  atx-kb show <db> <source-id>\n"
            << "  atx-kb link <db> <from-id> <to-id> <relation> [--evidence TEXT]\n"
            << "  atx-kb stats <db>\n"
            << "  atx-kb vector-build <db> <model> <dimensions> [--m N]"
               " [--ef-construction N] [--activate]\n"
            << "  atx-kb vector-list <db>\n"
            << "  atx-kb vector-activate <db> <generation-id>\n"
            << "  atx-kb vector-retire <db> <generation-id>\n"
            << "  atx-kb vector-recover <db> [--minimum-age-seconds N]\n"
            << "  atx-kb backup <db> <new-backup-path>\n"
            << "  atx-kb verify <db>\n";
}

template <class T> [[nodiscard]] int report_error(const T &result) {
  std::cerr << result.error().to_string() << '\n';
  return 1;
}

void print_string_array(const std::vector<std::string> &values) {
  std::cout << '[';
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      std::cout << ',';
    }
    std::cout << '"' << json_escape(values[i]) << '"';
  }
  std::cout << ']';
}

[[nodiscard]] int submit_command(atx::kb::KnowledgeBase &kb, int argc, char **argv) {
  if (argc < 4) {
    usage();
    return 2;
  }
  atx::kb::Submission submission;
  submission.raw_text = read_text(argv[3]);
  if (submission.raw_text.empty()) {
    std::cerr << "could not read non-empty input from: " << argv[3] << '\n';
    return 1;
  }
  for (int i = 4; i < argc; ++i) {
    const std::string_view option{argv[i]};
    if (option == "--title") {
      if (!option_value(argc, argv, i, submission.title)) {
        return 2;
      }
    } else if (option == "--uri") {
      if (!option_value(argc, argv, i, submission.uri)) {
        return 2;
      }
    } else if (option == "--author") {
      if (!option_value(argc, argv, i, submission.author)) {
        return 2;
      }
    } else if (option == "--agent") {
      if (!option_value(argc, argv, i, submission.submitted_by)) {
        return 2;
      }
    } else if (option == "--tag") {
      std::string tag;
      if (!option_value(argc, argv, i, tag)) {
        return 2;
      }
      submission.tags.push_back(std::move(tag));
    } else if (option == "--meta") {
      std::string value;
      if (!option_value(argc, argv, i, value)) {
        return 2;
      }
      const auto separator = value.find('=');
      if (separator == std::string::npos || separator == 0) {
        std::cerr << "--meta requires KEY=VALUE\n";
        return 2;
      }
      submission.metadata.push_back({value.substr(0, separator), value.substr(separator + 1)});
    } else {
      std::cerr << "unknown option: " << option << '\n';
      return 2;
    }
  }
  auto result = kb.submit(submission);
  if (!result) {
    return report_error(result);
  }
  std::cout << "{\"source_id\":\"" << json_escape(result->source_id) << "\",\"content_hash\":\""
            << result->content_hash << "\",\"observation_id\":" << result->observation_id
            << ",\"deduplicated\":" << (result->deduplicated ? "true" : "false")
            << ",\"chunks\":" << result->chunk_count << ",\"claims\":" << result->claim_count
            << ",\"summary\":\"" << json_escape(result->summary) << "\"}\n";
  return 0;
}

[[nodiscard]] int search_command(atx::kb::KnowledgeBase &kb, int argc, char **argv, bool context) {
  if (argc < 4) {
    usage();
    return 2;
  }
  atx::kb::SearchRequest request;
  request.query = argv[3];
  std::size_t max_characters = 12'000;
  for (int i = 4; i < argc; ++i) {
    const std::string_view option{argv[i]};
    if (option == "--limit") {
      std::string value;
      if (!option_value(argc, argv, i, value) || !parse_size(value, request.limit)) {
        std::cerr << "--limit requires an integer\n";
        return 2;
      }
    } else if (option == "--max-chars" && context) {
      std::string value;
      if (!option_value(argc, argv, i, value) || !parse_size(value, max_characters)) {
        std::cerr << "--max-chars requires an integer\n";
        return 2;
      }
    } else if (option == "--tag") {
      std::string tag;
      if (!option_value(argc, argv, i, tag)) {
        return 2;
      }
      request.require_tags.push_back(std::move(tag));
    } else {
      std::cerr << "unknown option: " << option << '\n';
      return 2;
    }
  }
  if (context) {
    auto result = kb.build_context(request, max_characters);
    if (!result) {
      return report_error(result);
    }
    std::cout << result->markdown;
    return 0;
  }
  auto result = kb.search(request);
  if (!result) {
    return report_error(result);
  }
  std::cout << "[";
  for (std::size_t i = 0; i < result->size(); ++i) {
    const auto &hit = (*result)[i];
    if (i != 0) {
      std::cout << ',';
    }
    std::cout << "{\"source_id\":\"" << json_escape(hit.source_id)
              << "\",\"chunk_id\":" << hit.chunk_id << ",\"title\":\"" << json_escape(hit.title)
              << "\",\"uri\":\"" << json_escape(hit.uri) << "\",\"score\":" << hit.score
              << ",\"lexical_score\":" << hit.lexical_score
              << ",\"vector_score\":" << hit.vector_score << ",\"graph_score\":" << hit.graph_score
              << ",\"text\":\"" << json_escape(hit.text) << "\"}";
  }
  std::cout << "]\n";
  return 0;
}

[[nodiscard]] int show_command(atx::kb::KnowledgeBase &kb, int argc, char **argv) {
  if (argc != 4) {
    usage();
    return 2;
  }
  auto result = kb.get_source(argv[3]);
  if (!result) {
    return report_error(result);
  }
  std::cout << "{\"id\":\"" << json_escape(result->id) << "\",\"title\":\""
            << json_escape(result->title) << "\",\"uri\":\"" << json_escape(result->uri)
            << "\",\"summary\":\"" << json_escape(result->summary) << "\",\"raw_text\":\""
            << json_escape(result->raw_text) << "\",\"tags\":";
  print_string_array(result->tags);
  std::cout << ",\"keywords\":";
  print_string_array(result->keywords);
  std::cout << ",\"entities\":";
  print_string_array(result->entities);
  std::cout << ",\"metadata\":[";
  for (std::size_t i = 0; i < result->metadata.size(); ++i) {
    if (i != 0) {
      std::cout << ',';
    }
    std::cout << "{\"key\":\"" << json_escape(result->metadata[i].key) << "\",\"value\":\""
              << json_escape(result->metadata[i].value) << "\"}";
  }
  std::cout << "],\"chunks\":[";
  for (std::size_t i = 0; i < result->chunks.size(); ++i) {
    if (i != 0) {
      std::cout << ',';
    }
    const auto &chunk = result->chunks[i];
    std::cout << "{\"id\":" << chunk.id << ",\"ordinal\":" << chunk.ordinal
              << ",\"token_count\":" << chunk.token_count << ",\"vector_model\":\""
              << json_escape(chunk.vector_model)
              << "\",\"vector_dimensions\":" << chunk.vector_dimensions << ",\"text\":\""
              << json_escape(chunk.text) << "\"}";
  }
  std::cout << "],\"claims\":[";
  for (std::size_t i = 0; i < result->claims.size(); ++i) {
    if (i != 0) {
      std::cout << ',';
    }
    const auto &claim = result->claims[i];
    std::cout << "{\"id\":" << claim.id << ",\"chunk_id\":" << claim.chunk_id
              << ",\"support_start\":" << claim.support_start
              << ",\"support_length\":" << claim.support_length
              << ",\"confidence\":" << claim.confidence << ",\"text\":\"" << json_escape(claim.text)
              << "\"}";
  }
  std::cout << "],\"observations\":[";
  for (std::size_t i = 0; i < result->observations.size(); ++i) {
    if (i != 0) {
      std::cout << ',';
    }
    const auto &observation = result->observations[i];
    std::cout << "{\"id\":" << observation.id << ",\"title\":\"" << json_escape(observation.title)
              << "\",\"uri\":\"" << json_escape(observation.uri) << "\",\"submitted_by\":\""
              << json_escape(observation.submitted_by) << "\",\"observed_at\":\""
              << json_escape(observation.observed_at) << "\"}";
  }
  std::cout << "]}\n";
  return 0;
}

[[nodiscard]] int link_command(atx::kb::KnowledgeBase &kb, int argc, char **argv) {
  if (argc < 6) {
    usage();
    return 2;
  }
  atx::kb::SourceLink link{argv[3], argv[4], argv[5], {}, 1.0};
  for (int i = 6; i < argc; ++i) {
    if (std::string_view{argv[i]} == "--evidence") {
      if (!option_value(argc, argv, i, link.evidence)) {
        return 2;
      }
    } else {
      std::cerr << "unknown option: " << argv[i] << '\n';
      return 2;
    }
  }
  auto status = kb.link_sources(link);
  if (!status) {
    return report_error(status);
  }
  std::cout << "{\"linked\":true}\n";
  return 0;
}

[[nodiscard]] int stats_command(atx::kb::KnowledgeBase &kb) {
  auto result = kb.stats();
  if (!result) {
    return report_error(result);
  }
  std::cout << "{\"sources\":" << result->sources << ",\"chunks\":" << result->chunks
            << ",\"claims\":" << result->claims << ",\"entities\":" << result->entities
            << ",\"edges\":" << result->edges << "}\n";
  return 0;
}

void print_vector_generation(const atx::kb::VectorIndexGeneration &generation) {
  std::cout << "{\"id\":" << generation.id << ",\"vector_model\":\""
            << json_escape(generation.embedding_model)
            << "\",\"vector_dimensions\":" << generation.dimensions
            << ",\"cutoff_revision\":" << generation.cutoff_revision << ",\"state\":\""
            << generation.state << "\",\"entry_chunk_id\":" << generation.entry_chunk_id
            << ",\"max_level\":" << generation.max_level
            << ",\"node_count\":" << generation.node_count
            << ",\"edge_count\":" << generation.edge_count << ",\"checksum\":\""
            << generation.checksum << "\",\"failure_reason\":\""
            << json_escape(generation.failure_reason) << "\",\"created_at\":\""
            << generation.created_at << "\",\"activated_at\":\"" << generation.activated_at
            << "\"}";
}

[[nodiscard]] int vector_build_command(atx::kb::KnowledgeBase &kb, int argc, char **argv) {
  if (argc < 5) {
    usage();
    return 2;
  }
  atx::kb::VectorIndexBuildOptions options;
  options.embedding_model = argv[3];
  if (!parse_i64(argv[4], options.dimensions)) {
    std::cerr << "dimensions must be an integer\n";
    return 2;
  }
  bool activate = false;
  for (int index = 5; index < argc; ++index) {
    const std::string_view option{argv[index]};
    std::string parsed;
    if (option == "--m") {
      if (!option_value(argc, argv, index, parsed) ||
          !parse_size(parsed, options.max_connections)) {
        return 2;
      }
    } else if (option == "--ef-construction") {
      if (!option_value(argc, argv, index, parsed) ||
          !parse_size(parsed, options.ef_construction)) {
        return 2;
      }
    } else if (option == "--activate" && !activate) {
      activate = true;
    } else {
      std::cerr << "unknown or repeated vector build option: " << option << '\n';
      return 2;
    }
  }
  auto generation = kb.build_vector_index(options);
  if (!generation) {
    return report_error(generation);
  }
  if (activate) {
    auto status = kb.activate_vector_index(generation->id);
    if (!status) {
      return report_error(status);
    }
    auto generations = kb.vector_indexes();
    if (!generations) {
      return report_error(generations);
    }
    const auto activated =
        std::find_if(generations->begin(), generations->end(),
                     [&](const auto &item) { return item.id == generation->id; });
    if (activated == generations->end()) {
      std::cerr << "activated vector generation disappeared\n";
      return 1;
    }
    *generation = *activated;
  }
  print_vector_generation(*generation);
  std::cout << '\n';
  return 0;
}

[[nodiscard]] int vector_transition_command(atx::kb::KnowledgeBase &kb, int argc, char **argv,
                                            bool activate) {
  if (argc != 4) {
    usage();
    return 2;
  }
  std::int64_t generation_id = 0;
  if (!parse_i64(argv[3], generation_id)) {
    return 2;
  }
  auto status =
      activate ? kb.activate_vector_index(generation_id) : kb.retire_vector_index(generation_id);
  if (!status) {
    return report_error(status);
  }
  std::cout << "{\"ok\":true,\"generation_id\":" << generation_id << "}\n";
  return 0;
}

[[nodiscard]] int vector_recover_command(atx::kb::KnowledgeBase &kb, int argc, char **argv) {
  std::int64_t minimum_age = 3'600;
  if (argc == 5 && std::string_view{argv[3]} == "--minimum-age-seconds") {
    if (!parse_i64(argv[4], minimum_age)) {
      return 2;
    }
  } else if (argc != 3) {
    usage();
    return 2;
  }
  auto recovered = kb.recover_abandoned_vector_indexes(minimum_age);
  if (!recovered) {
    return report_error(recovered);
  }
  std::cout << "{\"recovered\":" << *recovered << "}\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  const std::string_view command{argv[1]};
  const bool known = command == "submit" || command == "search" || command == "context" ||
                     command == "show" || command == "link" || command == "stats" ||
                     command == "verify" || command == "vector-build" || command == "vector-list" ||
                     command == "vector-activate" || command == "vector-retire" ||
                     command == "vector-recover" || command == "backup";
  if (!known) {
    usage();
    return 2;
  }
  auto opened = atx::kb::KnowledgeBase::open(argv[2]);
  if (!opened) {
    return report_error(opened);
  }
  auto kb = std::move(*opened);
  if (command == "submit") {
    return submit_command(kb, argc, argv);
  }
  if (command == "search") {
    return search_command(kb, argc, argv, false);
  }
  if (command == "context") {
    return search_command(kb, argc, argv, true);
  }
  if (command == "show") {
    return show_command(kb, argc, argv);
  }
  if (command == "link") {
    return link_command(kb, argc, argv);
  }
  if (command == "stats") {
    if (argc != 3) {
      usage();
      return 2;
    }
    return stats_command(kb);
  }
  if (command == "vector-build") {
    return vector_build_command(kb, argc, argv);
  }
  if (command == "vector-list") {
    if (argc != 3) {
      usage();
      return 2;
    }
    auto generations = kb.vector_indexes();
    if (!generations) {
      return report_error(generations);
    }
    std::cout << '[';
    for (std::size_t index = 0; index < generations->size(); ++index) {
      if (index != 0) {
        std::cout << ',';
      }
      print_vector_generation((*generations)[index]);
    }
    std::cout << "]\n";
    return 0;
  }
  if (command == "vector-activate") {
    return vector_transition_command(kb, argc, argv, true);
  }
  if (command == "vector-retire") {
    return vector_transition_command(kb, argc, argv, false);
  }
  if (command == "vector-recover") {
    return vector_recover_command(kb, argc, argv);
  }
  if (command == "backup") {
    if (argc != 4) {
      usage();
      return 2;
    }
    auto backup = kb.backup_to(argv[3]);
    if (!backup) {
      return report_error(backup);
    }
    std::cout << "{\"ok\":true,\"path\":\"" << json_escape(argv[3])
              << "\",\"pages\":" << backup->page_count << ",\"steps\":" << backup->steps
              << ",\"busy_retries\":" << backup->busy_retries << "}\n";
    return 0;
  }
  if (command == "verify") {
    if (argc != 3) {
      usage();
      return 2;
    }
    auto status = kb.verify_integrity();
    if (!status) {
      return report_error(status);
    }
    std::cout << "{\"ok\":true}\n";
    return 0;
  }
  usage();
  return 2;
}

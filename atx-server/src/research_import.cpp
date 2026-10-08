#include "research_detail.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <type_traits>

#include "atx/core/sha256.hpp"

namespace atx::server::research_detail {
namespace {
constexpr std::int64_t day_ns = 86'400'000'000'000LL;

bool contains_path(const std::filesystem::path& root, const std::filesystem::path& path) {
  auto base = root.begin();
  auto child = path.begin();
  for (; base != root.end(); ++base, ++child) {
    if (child == path.end() || *base != *child) return false;
  }
  return true;
}

template<class T> T numeric(std::string_view text) {
  if (text.empty()) throw std::invalid_argument("empty numeric CSV cell");
  T value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw std::invalid_argument("malformed numeric CSV cell");
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(value)) throw std::invalid_argument("nonfinite numeric CSV cell");
  }
  return value;
}

std::vector<std::string_view> cells(std::string_view line) {
  if (line.size() > 65536 || line.find('"') != std::string_view::npos)
    throw std::invalid_argument("NAV CSV requires bounded unquoted numeric rows");
  std::vector<std::string_view> result;
  std::size_t begin = 0;
  for (std::size_t field = 0; field < 512; ++field) {
    const auto end = line.find(',', begin);
    result.push_back(line.substr(begin, end == std::string_view::npos ? end : end - begin));
    if (end == std::string_view::npos) return result;
    begin = end + 1;
  }
  throw std::invalid_argument("NAV CSV exceeds column limit");
}

Day parse_day(const std::vector<std::string_view>& row,
              const std::map<std::string, std::size_t>& columns, std::string_view nav_column) {
  const auto key = numeric<std::int64_t>(row.at(columns.at("session_ns")));
  if (key <= 0 || key >= 4'102'444'800'000'000'000LL || key % day_ns != 0)
    throw std::invalid_argument("NAV session_ns must be a UTC midnight between 1970 and 2100");
  const auto date = std::chrono::floor<std::chrono::days>(
      std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{key}});
  const auto year = static_cast<int>(std::chrono::year_month_day{date}.year());
  if (year >= 2023 && year <= 2025)
    throw std::invalid_argument("locked holdout date is forbidden");
  const auto nav = numeric<double>(row.at(columns.at(std::string(nav_column))));
  const auto net_return = numeric<double>(row.at(columns.at("net_return")));
  const auto observed = numeric<int>(row.at(columns.at("return_observation")));
  if (nav < 1e-12 || nav > 1e18 || std::abs(net_return) > 1e6 || net_return <= -1 ||
      (observed != 0 && observed != 1))
    throw std::invalid_argument("invalid NAV valuation, return or observation flag");
  return {key, date_text(date), nav, net_return, observed == 1};
}

Evidence parse_csv(std::string_view text, const Json& request, const ResearchOptions& options,
                   std::stop_token stop, const Progress& progress) {
  const auto start = request.contains("start_date")
      ? std::optional{parse_date(request.at("start_date").get<std::string>())} : std::nullopt;
  const auto end = request.contains("end_date")
      ? std::optional{parse_date(request.at("end_date").get<std::string>())} : std::nullopt;
  if (start && end && *start > *end) throw std::invalid_argument("start_date after end_date");
  const auto header_end = text.find('\n');
  if (header_end == std::string_view::npos) throw std::invalid_argument("NAV CSV has no body");
  auto header = text.substr(0, header_end);
  if (header.ends_with('\r')) header.remove_suffix(1);
  if (header.starts_with("\xEF\xBB\xBF")) header.remove_prefix(3);
  const auto fields = cells(header);
  std::map<std::string, std::size_t> columns;
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].empty() || !columns.emplace(std::string(fields[i]), i).second)
      throw std::invalid_argument("NAV CSV has empty or duplicate header");
  }
  for (const auto* required : {"session_ns", "return_observation", "net_return"})
    if (!columns.contains(required)) throw std::invalid_argument("NAV CSV missing required column");
  const std::string nav_column = columns.contains("pretrade_nav")
      ? "pretrade_nav" : "posttrade_nav";
  if (!columns.contains(nav_column))
    throw std::invalid_argument("NAV CSV missing valuation column");
  Evidence evidence;
  std::size_t offset = header_end + 1;
  std::size_t rows = 0;
  std::int64_t previous = 0;
  for (; rows < options.max_csv_rows && offset < text.size(); ++rows) {
    if (rows % 256 == 0) checkpoint(stop, progress, 0.5, "Validating NAV evidence rows");
    const auto line_end = text.find('\n', offset);
    const auto length = line_end == std::string_view::npos ? line_end : line_end - offset;
    auto line = text.substr(offset, length);
    offset = line_end == std::string_view::npos ? text.size() : line_end + 1;
    if (line.ends_with('\r')) line.remove_suffix(1);
    const auto values = cells(line);
    if (values.size() != fields.size()) throw std::invalid_argument("ragged NAV CSV row");
    auto day = parse_day(values, columns, nav_column);
    if (day.session_ns <= previous)
      throw std::invalid_argument("NAV sessions must be strictly ordered");
    previous = day.session_ns;
    const auto date = std::chrono::sys_days{std::chrono::days{day.session_ns / day_ns}};
    if ((start && date < *start) || (end && date > *end)) continue;
    if (evidence.days.empty()) day.return_observation = false;
    evidence.days.push_back(std::move(day));
  }
  if (offset < text.size()) throw std::invalid_argument("NAV CSV exceeds row limit");
  if (evidence.days.empty())
    throw std::invalid_argument("NAV CSV has no rows in selected window");
  evidence.provenance = {{"synthetic", false}, {"evaluation", "research_unverified"},
      {"data_source", "imported ATX strategy NAV artifact; market observation status unverified"},
      {"engine", "atx-impl strategy_nav_replay artifact import"}, {"nav_column", nav_column},
      {"annualization_sessions", 252}, {"input_rows", rows},
      {"return_convention", "return_observation==1; selected first row is baseline, excluded"},
      {"start_date", evidence.days.front().date}, {"end_date", evidence.days.back().date},
      {"warnings", Json::array({"Trial selection and historical data vintage remain unverified",
          "Trades, positions and factor exposures unavailable in NAV daily CSV"})}};
  if (nav_column != "pretrade_nav")
    evidence.provenance["warnings"].push_back(
        "pretrade_nav absent; using supplied posttrade valuations");
  return evidence;
}
} // namespace

bool forbidden_path(std::string_view name) {
  static const std::regex forbidden(
      "validation|holdout|(^|[^A-Za-z])VAL([^A-Za-z]|$)|2023|2024|2025",
      std::regex::ECMAScript | std::regex::icase);
  return std::regex_search(name.begin(), name.end(), forbidden);
}

std::filesystem::path artifact_path(std::string_view requested, const ResearchOptions& options) {
  if (requested.empty() || requested.size() > 4096 ||
      requested.find('\0') != std::string_view::npos ||
      requested.find(':') != std::string_view::npos || forbidden_path(requested))
    throw std::invalid_argument("forbidden artifact path");
  const std::filesystem::path relative{requested};
  if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
    throw std::invalid_argument("artifact paths must be relative to repository root");
  for (const auto& component : relative) if (component == "..")
    throw std::invalid_argument("artifact path traversal forbidden");
  std::error_code error;
  const auto resolved = std::filesystem::weakly_canonical(options.repo_root / relative, error);
  if (error) throw std::invalid_argument("artifact path cannot be resolved");
  bool allowed = false;
  for (const auto& root : options.artifact_roots) {
    if (contains_path(root, resolved)) {
      if (forbidden_path(resolved.lexically_relative(root).generic_string()))
        throw std::invalid_argument("artifact resolves into locked holdout path");
      allowed = true;
      break;
    }
  }
  if (!allowed) throw std::invalid_argument("artifact outside configured roots");
  if (!std::filesystem::is_regular_file(resolved))
    throw std::invalid_argument("artifact is not an existing regular file");
  return resolved;
}

std::string read_artifact(const std::filesystem::path& path, std::size_t max_bytes,
                          std::stop_token stop) {
  checkpoint(stop, {}, 0, {});
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("cannot open research artifact");
  const auto extent = input.tellg();
  if (extent < 0 || static_cast<std::uint64_t>(extent) > max_bytes)
    throw std::invalid_argument("research artifact exceeds byte budget");
  std::string result(static_cast<std::size_t>(extent), '\0');
  input.seekg(0);
  for (std::size_t offset = 0; offset < result.size();) {
    checkpoint(stop, {}, 0, {});
    const auto count = std::min(std::size_t{65536}, result.size() - offset);
    input.read(result.data() + offset, static_cast<std::streamsize>(count));
    if (!input) throw std::runtime_error("artifact changed or truncated while reading");
    offset += count;
  }
  if (input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("artifact changed extent while reading");
  return result;
}

std::string date_text(std::chrono::sys_days date) {
  const std::chrono::year_month_day parts{date};
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setfill('0') << std::setw(4) << static_cast<int>(parts.year()) << '-'
         << std::setw(2) << static_cast<unsigned>(parts.month()) << '-'
         << std::setw(2) << static_cast<unsigned>(parts.day());
  return output.str();
}

std::chrono::sys_days parse_date(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-')
    throw std::invalid_argument("date must use YYYY-MM-DD");
  const auto year = numeric<int>(text.substr(0, 4));
  const auto month = numeric<unsigned>(text.substr(5, 2));
  const auto day = numeric<unsigned>(text.substr(8, 2));
  if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31)
    throw std::invalid_argument("date outside supported calendar");
  const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{month},
                                       std::chrono::day{day}};
  if (!date.ok()) throw std::invalid_argument("invalid calendar date");
  return std::chrono::sys_days{date};
}

Evidence import_nav(const Json& request, const ResearchOptions& options,
                    std::stop_token stop, const Progress& progress) {
  if (!request.contains("artifact_path"))
    throw std::invalid_argument("artifact_path is required");
  // Validate date syntax before opening any artifact.
  for (const auto* key : {"start_date", "end_date"})
    if (request.contains(key)) (void)parse_date(request.at(key).get<std::string>());
  const auto path = artifact_path(request.at("artifact_path").get<std::string>(), options);
  checkpoint(stop, progress, 0.15, "Reading bounded allowlisted NAV artifact");
  const auto bytes = read_artifact(path, options.max_input_bytes, stop);
  const auto digest = atx::core::sha256_hex(bytes);
  if (!digest) throw std::runtime_error(digest.error().message());
  auto evidence = parse_csv(bytes, request, options, stop, progress);
  evidence.provenance["artifact_path"] =
      path.lexically_relative(options.repo_root).generic_string();
  evidence.provenance["artifact_sha256"] = *digest;
  evidence.provenance["artifact_bytes"] = bytes.size();
  if (request.contains("summary_path")) {
    const auto summary_path = artifact_path(request.at("summary_path").get<std::string>(), options);
    const auto summary_bytes = read_artifact(summary_path,
        std::min(options.max_input_bytes, std::size_t{1024U * 1024U}), stop);
    const auto summary = parse_json(summary_bytes);
    if (!summary.is_object()) throw std::invalid_argument("summary must be a JSON object");
    for (const auto* key : {"role", "evaluation"}) {
      if (summary.contains(key) && summary.at(key).is_string() &&
          forbidden_path(summary.at(key).get<std::string>()))
        throw std::invalid_argument("locked holdout summary role forbidden");
    }
    const auto summary_digest = atx::core::sha256_hex(summary_bytes);
    if (!summary_digest) throw std::runtime_error(summary_digest.error().message());
    evidence.provenance["summary_sha256"] = *summary_digest;
    evidence.provenance["summary_schema"] = summary.value("schema", std::string("unspecified"));
  }
  checkpoint(stop, progress, 0.8, "Evidence validated and hashed");
  return evidence;
}
} // namespace atx::server::research_detail

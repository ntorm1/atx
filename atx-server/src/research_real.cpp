#include "research_detail.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>

#include "atx/core/sha256.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_target_replay.hpp"

namespace atx::server::research_detail {
namespace {
namespace strategy = atx::impl::strategy;
constexpr std::int64_t day_ns = 86'400'000'000'000LL;

std::string external_pin(const Json& request, std::string_view key) {
  const auto value = request.at(std::string(key)).get<std::string>();
  if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
      })) throw std::invalid_argument("external pin must be 64 lowercase hexadecimal characters");
  return value;
}

Json pinned_manifest(const std::filesystem::path& path, std::string_view pin,
                     const ResearchOptions& options, std::stop_token stop) {
  const auto bytes = read_artifact(path,
      std::min(options.max_input_bytes, std::size_t{1024U * 1024U}), stop);
  const auto digest = atx::core::sha256_hex(bytes);
  if (!digest) throw std::runtime_error(digest.error().message());
  if (*digest != pin) throw std::invalid_argument("manifest SHA-256 pin mismatch");
  const auto document = parse_json(bytes);
  if (!document.is_object()) throw std::invalid_argument("manifest must be a JSON object");
  return document;
}

std::uint64_t manifest_integer(const Json& value, std::uint64_t upper,
                                std::string_view name) {
  if (!value.is_number_integer())
    throw std::invalid_argument("manifest integer required: " + std::string(name));
  std::uint64_t result{};
  if (value.is_number_unsigned()) {
    result = value.get<std::uint64_t>();
  } else {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0)
      throw std::invalid_argument("manifest integer must be nonnegative: " + std::string(name));
    result = static_cast<std::uint64_t>(signed_value);
  }
  if (result > upper)
    throw std::invalid_argument("manifest integer outside supported range: " + std::string(name));
  return result;
}

void validate_manifest_geometry(const Json& manifest, bool combined,
                                const ResearchOptions& options) {
  const auto dates = manifest_integer(manifest.at("dates"),
      std::min(std::uint64_t{4096}, static_cast<std::uint64_t>(options.max_csv_rows)), "dates");
  const auto names = manifest_integer(manifest.at("instruments"), 20'000, "instruments");
  const auto begin = manifest_integer(manifest.at("score_begin"), dates, "score_begin");
  const auto end = manifest_integer(manifest.at("score_end"), dates, "score_end");
  if (!dates || !names || begin >= end)
    throw std::invalid_argument("manifest geometry requires nonempty dimensions and score window");
  if (combined) {
    const auto cells = dates * names; // Bounded above by 4096 * 20000.
    (void)manifest_integer(manifest.at("finite_cells"), cells, "finite_cells");
    (void)manifest_integer(manifest.at("member_cells"), cells, "member_cells");
  }
  const auto& files = manifest.at("files");
  if (!files.is_object() || files.empty() || files.size() > 64)
    throw std::invalid_argument("invalid manifest payload file set");
  for (const auto& file : files.items()) {
    if (!file.value().is_object())
      throw std::invalid_argument("manifest payload receipt must be an object");
    (void)manifest_integer(file.value().at("bytes"), options.max_working_bytes, "payload bytes");
  }
}

void validate_payload_paths(const Json& manifest, const std::filesystem::path& directory,
                            const ResearchOptions& options) {
  const auto& files = manifest.at("files");
  if (!files.is_object() || files.empty() || files.size() > 64)
    throw std::invalid_argument("invalid manifest payload file set");
  for (const auto& file : files.items()) {
    const std::filesystem::path name{file.key()};
    if (name.has_parent_path() || name.filename() != name ||
        file.key().find(':') != std::string::npos)
      throw std::invalid_argument("manifest payload filename must be a single relative component");
    const auto relative = (directory / name).lexically_relative(options.repo_root);
    (void)artifact_path(relative.generic_string(), options);
  }
}

void validate_session_dates(const Json& role, const std::filesystem::path& directory,
                            const ResearchOptions& options, std::stop_token stop) {
  if (std::endian::native != std::endian::little)
    throw std::runtime_error("saved NAV artifacts require a little-endian host");
  const auto dates = role.at("dates").get<std::uint64_t>();
  if (!dates || dates > options.max_csv_rows)
    throw std::invalid_argument("pinned role exceeds session row budget");
  const auto relative = (directory / "sessions.i64").lexically_relative(options.repo_root);
  const auto path = artifact_path(relative.generic_string(), options);
  const auto bytes = read_artifact(path, options.max_input_bytes, stop);
  const auto& receipt = role.at("files").at("sessions.i64");
  if (bytes.size() != dates * sizeof(std::int64_t) ||
      receipt.at("bytes").get<std::uint64_t>() != bytes.size())
    throw std::invalid_argument("pinned session extent mismatch");
  const auto digest = atx::core::sha256_hex(bytes);
  if (!digest) throw std::runtime_error(digest.error().message());
  if (*digest != receipt.at("sha256").get<std::string>())
    throw std::invalid_argument("pinned sessions SHA-256 mismatch");
  std::int64_t previous = 0;
  for (std::size_t index = 0; index < static_cast<std::size_t>(dates); ++index) {
    if (index % 256 == 0) checkpoint(stop, {}, 0, {});
    std::int64_t key{};
    std::memcpy(&key, bytes.data() + index * sizeof(key), sizeof(key));
    if (key <= previous || key >= 4'102'444'800'000'000'000LL || key % day_ns != 0)
      throw std::invalid_argument("pinned sessions must be ordered UTC midnights");
    previous = key;
    const auto date = std::chrono::sys_days{std::chrono::days{key / day_ns}};
    const auto year = static_cast<int>(std::chrono::year_month_day{date}.year());
    if (year >= 2023 && year <= 2025)
      throw std::invalid_argument("locked holdout session date is forbidden");
  }
}

double bounded_number(const Json& request, std::string_view name, double fallback,
                       double lower, double upper) {
  const auto value = request.value(std::string(name), fallback);
  if (!std::isfinite(value) || value < lower || value > upper)
    throw std::invalid_argument(std::string(name) + " outside supported range");
  return value;
}

strategy::TargetReplayConfig target_config(const Json& request, const ResearchOptions& options) {
  strategy::TargetReplayConfig config;
  const auto rule = request.value("rule", std::string("baseline-v1"));
  if (rule == "baseline-v1") config.rule = strategy::TargetReplayRule::BaselineTargetV1;
  else if (rule == "monthly-budget-v2")
    config.rule = strategy::TargetReplayRule::MonthlyTargetBudgetV2;
  else if (rule == "aim-partial-v5") config.rule = strategy::TargetReplayRule::AimPartialV5;
  else throw std::invalid_argument("unsupported saved NAV target rule");
  const auto cadence = bounded_number(request, "cadence", 5, 1, 252);
  if (std::floor(cadence) != cadence) throw std::invalid_argument("cadence must be integral");
  config.cadence = static_cast<std::size_t>(cadence);
  config.trade_fraction = bounded_number(request, "trade_fraction", 0.25, 0.001, 1.0);
  config.monthly_budget = bounded_number(request, "monthly_budget", 0.30, 0.001, 2.0);
  config.max_working_bytes = options.max_working_bytes;
  return config;
}

std::filesystem::path output_directory(const ResearchOptions& options) {
  if (options.output_root.empty())
    throw std::invalid_argument("real_nav requires a configured server output root");
  const auto configured = options.output_root.is_absolute()
      ? options.output_root : options.repo_root / options.output_root;
  std::filesystem::create_directories(configured);
  const auto directory = std::filesystem::canonical(configured);
  std::random_device entropy;
  for (std::size_t attempt = 0; attempt < 32; ++attempt) {
    std::string id = "run-";
    for (std::size_t byte = 0; byte < 24; ++byte)
      id.push_back(static_cast<char>('a' + (entropy() % 16U)));
    const auto candidate = directory / id;
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw std::runtime_error("cannot allocate unique server-owned research output directory");
}

Evidence load_published(const std::filesystem::path& directory, const Json& request,
                        const ResearchOptions& options, std::stop_token stop,
                        const Progress& progress) {
  const auto summary_bytes = read_artifact(
      directory / "summary.json", options.max_input_bytes, stop);
  const auto summary = parse_json(summary_bytes);
  if (summary.at("status") != "complete")
    throw std::runtime_error("NAV publication is incomplete");
  const auto primary = summary.at("primary_scenario").get<std::string>();
  if (std::filesystem::path(primary).has_parent_path() || forbidden_path(primary) ||
      primary.find(':') != std::string::npos)
    throw std::runtime_error("invalid NAV primary scenario filename");
  ResearchOptions published = options;
  published.repo_root = directory.parent_path();
  published.artifact_roots = {published.repo_root};
  const auto prefix = directory.filename().generic_string();
  Json import_request{{"artifact_path", prefix + "/daily_" + primary + ".csv"},
                      {"summary_path", prefix + "/summary.json"}};
  for (const auto* field : {"start_date", "end_date"})
    if (request.contains(field)) import_request[field] = request.at(field);
  auto evidence = import_nav(import_request, published, stop, progress);
  evidence.provenance["artifact_namespace"] = "server_output";
  evidence.provenance["output_directory"] = directory.filename().generic_string();
  evidence.provenance["primary_scenario"] = primary;
  evidence.provenance["recipe_sha256"] = summary.at("recipe_sha256");
  evidence.provenance["capacity_qualified"] = summary.at("capacity_qualified");
  evidence.provenance["self_financing_nav"] = summary.at("self_financing_nav");
  evidence.provenance["limitations"] = summary.at("limitations");
  return evidence;
}
} // namespace

Evidence real_nav(const Json& request, const ResearchOptions& options,
                  std::stop_token stop, const Progress& progress) {
  checkpoint(stop, progress, 0.05, "Verifying externally pinned TRAIN research artifacts");
  if (options.output_root.empty())
    throw std::invalid_argument("real_nav requires a configured server output root");
  if (options.max_working_bytes < (16ULL << 20) || options.max_working_bytes > (4ULL << 30))
    throw std::invalid_argument("invalid server research working memory budget");
  const auto start = request.contains("start_date")
      ? std::optional{parse_date(request.at("start_date").get<std::string>())} : std::nullopt;
  const auto end = request.contains("end_date")
      ? std::optional{parse_date(request.at("end_date").get<std::string>())} : std::nullopt;
  if (start && end && *start > *end) throw std::invalid_argument("start_date after end_date");
  const auto combined = artifact_path(request.at("combined_path").get<std::string>(), options);
  const auto role_path = artifact_path(request.at("role_path").get<std::string>(), options);
  const auto combined_pin = external_pin(request, "combined_sha256");
  const auto role_pin = external_pin(request, "role_sha256");
  const auto blend = pinned_manifest(combined, combined_pin, options, stop);
  const auto role = pinned_manifest(role_path, role_pin, options, stop);
  // The existing loader converts JSON numbers directly; prevent float-to-integer UB first.
  validate_manifest_geometry(blend, true, options);
  validate_manifest_geometry(role, false, options);
  if (blend.at("role") != "train")
    throw std::invalid_argument("locked evaluation role forbidden; real_nav accepts TRAIN only");
  if (role.contains("role") && role.at("role") != "train")
    throw std::invalid_argument("locked evaluation role forbidden; real_nav accepts TRAIN only");
  validate_payload_paths(blend, combined.parent_path(), options);
  validate_payload_paths(role, role_path.parent_path(), options);
  validate_session_dates(role, role_path.parent_path(), options, stop);
  strategy::TargetReplayRunConfig config;
  config.combined_path = combined.string(); config.combined_sha256 = combined_pin;
  config.role_path = role_path.string(); config.role_sha256 = role_pin;
  config.target = target_config(request, options);
  checkpoint(stop, progress, 0.25, "Running the existing pinned self-financing NAV pipeline");
  const auto output = output_directory(options);
  config.output_directory = output.string();
  std::ostringstream messages;
  const auto status = strategy::run_nav_replay(config, {}, {}, {}, {}, {}, messages, stop);
  if (!status) {
    if (status.error().code() == atx::core::ErrorCode::Cancelled)
      throw std::runtime_error("research cancelled");
    if (status.error().code() == atx::core::ErrorCode::IoError ||
        status.error().code() == atx::core::ErrorCode::Internal)
      throw std::runtime_error(status.error().message());
    throw std::invalid_argument(status.error().message());
  }
  checkpoint(stop, progress, 0.7, "NAV artifacts published with source and recipe hashes");
  const Progress import_progress = [&](double fraction, std::string_view message) {
    if (progress) progress(0.72 + 0.16 * std::clamp(fraction, 0.0, 1.0), message);
  };
  auto evidence = load_published(output, request, options, stop, import_progress);
  evidence.provenance["engine"] = "atx::impl::strategy::run_nav_replay";
  evidence.provenance["data_source"] =
      "externally pinned TRAIN saved blend and research price role";
  evidence.provenance["combined_sha256"] = combined_pin;
  evidence.provenance["role_sha256"] = role_pin;
  evidence.provenance["source_sha256"] = blend.at("source_sha256");
  evidence.provenance["library_sha256"] = blend.at("library_sha256");
  evidence.provenance["role"] = "train";
  evidence.provenance["common_stock_verified"] = role.at("common_stock_verified");
  evidence.provenance["historical_vintage_verified"] = role.at("historical_vintage_verified");
  evidence.provenance["warnings"].push_back(
      "Costs, capacity and borrow use declared scenarios; observed execution is unverified");
  return evidence;
}
} // namespace atx::server::research_detail

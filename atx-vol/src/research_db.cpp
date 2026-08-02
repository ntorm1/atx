#include "atx/vol/research_db.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"

namespace atx::vol {
using atx::core::Ok;

namespace {

constexpr std::uint32_t kResearchDbFormatVersion = 1u;
constexpr std::string_view kMetaSection = "rdb_meta";
constexpr std::string_view kArtifactIndexSection = "artifact_index";
constexpr std::string_view kHeadIndexSection = "head_index";
constexpr std::string_view kArtifactMetaSection = "artifact_meta";
constexpr std::string_view kParametersSection = "parameters";
constexpr std::string_view kDependenciesSection = "dependencies";
constexpr std::string_view kSignalValuesSection = "signal_values";
constexpr std::size_t kMaxLogicalIdBytes = 512u;
constexpr std::size_t kMaxParameterTextBytes = 16u * 1024u;
constexpr std::size_t kMaxDependencyTextBytes = 1024u;
constexpr std::size_t kMaxRequestItems = 1u << 20u;

[[nodiscard]] constexpr std::int64_t u64_bits(std::uint64_t value) noexcept {
  return std::bit_cast<std::int64_t>(value);
}

[[nodiscard]] constexpr std::uint64_t i64_bits(std::int64_t value) noexcept {
  return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::int64_t wall_clock_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] bool valid_kind(ResearchArtifactKind kind) noexcept {
  return static_cast<std::uint8_t>(kind) <=
         static_cast<std::uint8_t>(ResearchArtifactKind::ExecutionIntent);
}

[[nodiscard]] bool valid_value_kind(ResearchValueKind kind) noexcept {
  return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(ResearchValueKind::Text);
}

[[nodiscard]] bool valid_sha256(std::string_view value) noexcept {
  if (value.size() != 64u) {
    return false;
  }
  return std::all_of(value.begin(), value.end(),
                     [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

[[nodiscard]] bool valid_logical_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxLogicalIdBytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '/' || ch == '_' || ch == '-' || ch == '.' || ch == ':' || ch == '@';
  });
}

[[nodiscard]] bool is_leap_year(int year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] bool valid_iso_date(std::string_view value) noexcept {
  if (value.empty()) {
    return true;
  }
  if (value.size() != 10u || value[4] != '-' || value[7] != '-') {
    return false;
  }
  const auto digit = [&value](std::size_t i) -> int {
    const char ch = value[i];
    return ch >= '0' && ch <= '9' ? static_cast<int>(ch - '0') : -1;
  };
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i != 4u && i != 7u && digit(i) < 0) {
      return false;
    }
  }
  const int year = digit(0u) * 1000 + digit(1u) * 100 + digit(2u) * 10 + digit(3u);
  const int month = digit(5u) * 10 + digit(6u);
  const int day = digit(8u) * 10 + digit(9u);
  constexpr int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year <= 0 || month < 1 || month > 12 || day < 1) {
    return false;
  }
  const int max_day = kMonthDays[month - 1] + (month == 2 && is_leap_year(year) ? 1 : 0);
  return day <= max_day;
}

[[nodiscard]] bool identity_is_zero(const ArchiveContentIdentity &identity) noexcept {
  return identity == ArchiveContentIdentity{};
}

[[nodiscard]] bool parameter_less(const ResearchParameter &lhs,
                                  const ResearchParameter &rhs) noexcept {
  return std::tie(lhs.scope, lhs.key) < std::tie(rhs.scope, rhs.key);
}

[[nodiscard]] bool dependency_less(const ResearchDependency &lhs,
                                   const ResearchDependency &rhs) noexcept {
  return std::tie(lhs.role, lhs.kind, lhs.key, lhs.dependency_id, lhs.date_lo, lhs.date_hi) <
         std::tie(rhs.role, rhs.kind, rhs.key, rhs.dependency_id, rhs.date_lo, rhs.date_hi);
}

[[nodiscard]] bool same_parameter_key(const ResearchParameter &lhs,
                                      const ResearchParameter &rhs) noexcept {
  return lhs.scope == rhs.scope && lhs.key == rhs.key;
}

[[nodiscard]] bool same_dependency_key(const ResearchDependency &lhs,
                                       const ResearchDependency &rhs) noexcept {
  return lhs.role == rhs.role && lhs.kind == rhs.kind && lhs.key == rhs.key &&
         lhs.dependency_id == rhs.dependency_id && lhs.date_lo == rhs.date_lo &&
         lhs.date_hi == rhs.date_hi;
}

[[nodiscard]] bool artifact_less(const ResearchArtifactInfo &lhs,
                                 const ResearchArtifactInfo &rhs) noexcept {
  return lhs.artifact_id < rhs.artifact_id;
}

[[nodiscard]] bool head_less(const ResearchHead &lhs, const ResearchHead &rhs) noexcept {
  return std::tie(lhs.kind, lhs.logical_id) < std::tie(rhs.kind, rhs.logical_id);
}

[[nodiscard]] bool same_head_key(const ResearchHead &lhs, const ResearchHead &rhs) noexcept {
  return lhs.kind == rhs.kind && lhs.logical_id == rhs.logical_id;
}

[[nodiscard]] std::size_t column_size(const RaColumnData &column) noexcept {
  switch (column.dtype) {
  case RaDType::F64:
    return column.f64.size();
  case RaDType::I64:
    return column.i64.size();
  case RaDType::U32:
  case RaDType::DictStr:
    return column.u32.size();
  case RaDType::U8Enum:
    return column.u8.size();
  }
  return 0u;
}

[[nodiscard]] const RaColumnData *find_column(const RaSectionData &section,
                                              std::string_view name) noexcept {
  const auto it = std::find_if(section.columns.begin(), section.columns.end(),
                               [name](const auto &entry) { return entry.first == name; });
  return it != section.columns.end() ? &it->second : nullptr;
}

[[nodiscard]] Status validate_parameter(const ResearchParameter &parameter) {
  if (parameter.scope.empty() || parameter.scope.size() > kMaxLogicalIdBytes ||
      parameter.key.empty() || parameter.key.size() > kMaxLogicalIdBytes ||
      !valid_value_kind(parameter.kind) || parameter.text_value.size() > kMaxParameterTextBytes) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid parameter metadata");
  }
  switch (parameter.kind) {
  case ResearchValueKind::F64:
    if (!std::isfinite(parameter.f64_value) || parameter.i64_value != 0 ||
        parameter.u32_value != 0u || !parameter.text_value.empty()) {
      return Err(ErrorCode::InvalidArgument, "research_db: ambiguous F64 parameter");
    }
    break;
  case ResearchValueKind::I64:
    if (parameter.f64_value != 0.0 || parameter.u32_value != 0u || !parameter.text_value.empty()) {
      return Err(ErrorCode::InvalidArgument, "research_db: ambiguous I64 parameter");
    }
    break;
  case ResearchValueKind::U32:
    if (parameter.f64_value != 0.0 || parameter.i64_value != 0 || !parameter.text_value.empty()) {
      return Err(ErrorCode::InvalidArgument, "research_db: ambiguous U32 parameter");
    }
    break;
  case ResearchValueKind::Text:
    if (parameter.f64_value != 0.0 || parameter.i64_value != 0 || parameter.u32_value != 0u) {
      return Err(ErrorCode::InvalidArgument, "research_db: ambiguous text parameter");
    }
    break;
  }
  return Ok();
}

[[nodiscard]] Status validate_dependency(const ResearchDependency &dependency) {
  if (dependency.key.empty() || dependency.key.size() > kMaxDependencyTextBytes ||
      dependency.dependency_id.size() > kMaxDependencyTextBytes ||
      dependency.date_lo.size() > 10u || dependency.date_hi.size() > 10u ||
      !valid_iso_date(dependency.date_lo) || !valid_iso_date(dependency.date_hi) ||
      (!dependency.date_lo.empty() && !dependency.date_hi.empty() &&
       dependency.date_lo > dependency.date_hi)) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid dependency");
  }
  return Ok();
}

[[nodiscard]] Status validate_section_data(const RaSectionData &section) {
  if (section.name.empty() || section.name.size() > sizeof(RaSectionDescriptor{}.name) ||
      section.columns.empty() || section.columns.size() > kMaxRequestItems ||
      section.n_rows > (1ULL << 48u) ||
      static_cast<std::uint8_t>(section.kind) >
          static_cast<std::uint8_t>(RaSectionKind::SubTable)) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid payload section");
  }
  for (std::size_t i = 0; i < section.columns.size(); ++i) {
    const auto &[name, column] = section.columns[i];
    if (name.empty() || name.size() > sizeof(RaColumnDescriptor{}.name) ||
        static_cast<std::uint8_t>(column.dtype) > static_cast<std::uint8_t>(RaDType::DictStr) ||
        column_size(column) != section.n_rows) {
      return Err(ErrorCode::InvalidArgument, "research_db: invalid payload column");
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (section.columns[j].first == name) {
        return Err(ErrorCode::InvalidArgument, "research_db: duplicate payload column");
      }
    }
    if (column.dtype == RaDType::DictStr) {
      for (const std::uint32_t code : column.u32) {
        if (code >= column.strings.size()) {
          return Err(ErrorCode::InvalidArgument, "research_db: dictionary code out of range");
        }
      }
    } else if (column.dtype == RaDType::U8Enum) {
      for (const std::uint8_t code : column.u8) {
        if (code >= column.strings.size()) {
          return Err(ErrorCode::InvalidArgument, "research_db: enum code out of range");
        }
      }
    } else if (column.dtype == RaDType::F64 &&
               std::any_of(column.f64.begin(), column.f64.end(),
                           [](double value) { return !std::isfinite(value); })) {
      return Err(ErrorCode::InvalidArgument, "research_db: non-finite payload value");
    }
  }
  return Ok();
}

[[nodiscard]] Status validate_signal_data(const ResearchPublishRequest &request,
                                          const RaSectionData &section) {
  const RaColumnData *event_ts = find_column(section, "event_ts_ns");
  const RaColumnData *available_ts = find_column(section, "available_ts_ns");
  const RaColumnData *uids = find_column(section, "uid");
  const RaColumnData *symbols = find_column(section, "symbol");
  const RaColumnData *values = find_column(section, "value");
  const RaColumnData *status = find_column(section, "status");
  if (section.kind != RaSectionKind::TimeSeries || event_ts == nullptr || available_ts == nullptr ||
      uids == nullptr || symbols == nullptr || values == nullptr || status == nullptr ||
      event_ts->dtype != RaDType::I64 || available_ts->dtype != RaDType::I64 ||
      uids->dtype != RaDType::U32 || symbols->dtype != RaDType::DictStr ||
      values->dtype != RaDType::F64 || status->dtype != RaDType::U32 ||
      section.n_rows != request.row_count || section.n_rows == 0u) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid signal_values schema");
  }
  for (std::size_t i = 0; i < event_ts->i64.size(); ++i) {
    if (event_ts->i64[i] <= 0 || available_ts->i64[i] < event_ts->i64[i] || uids->u32[i] == 0u ||
        symbols->strings[symbols->u32[i]].empty() ||
        (status->u32[i] == 0u && !std::isfinite(values->f64[i]))) {
      return Err(ErrorCode::InvalidArgument, "research_db: invalid signal row");
    }
    if (i != 0u && std::tie(event_ts->i64[i - 1u], uids->u32[i - 1u]) >=
                       std::tie(event_ts->i64[i], uids->u32[i])) {
      return Err(ErrorCode::InvalidArgument, "research_db: signal rows must be sorted and unique");
    }
  }
  if (request.first_ts_ns != event_ts->i64.front() || request.last_ts_ns != event_ts->i64.back()) {
    return Err(ErrorCode::InvalidArgument,
               "research_db: signal timestamp envelope disagrees with rows");
  }
  return Ok();
}

[[nodiscard]] Result<std::vector<RaSectionData>>
canonical_sections(const ResearchPublishRequest &request) {
  struct CanonicalSectionStorage {
    std::shared_ptr<const void> source_storage;
    std::vector<std::vector<double>> f64;
  };

  if (request.sections.size() > kMaxRequestItems) {
    return Err(ErrorCode::InvalidArgument, "research_db: too many payload sections");
  }
  std::vector<RaSectionData> sections = request.sections;
  for (RaSectionData &section : sections) {
    ATX_TRY_VOID(validate_section_data(section));
    auto storage = std::make_shared<CanonicalSectionStorage>();
    storage->source_storage = section.storage;
    storage->f64.reserve(static_cast<std::size_t>(
        std::count_if(section.columns.begin(), section.columns.end(),
                      [](const auto &entry) { return entry.second.dtype == RaDType::F64; })));
    for (auto &[name, column] : section.columns) {
      static_cast<void>(name);
      if (column.dtype != RaDType::F64) {
        continue;
      }
      storage->f64.emplace_back(column.f64.begin(), column.f64.end());
      for (double &value : storage->f64.back()) {
        if (value == 0.0) {
          value = 0.0;
        }
      }
      column.f64 = std::span<const double>(storage->f64.back());
    }
    if (!storage->f64.empty()) {
      section.storage = std::move(storage);
    }
    if (section.name == kArtifactMetaSection || section.name == kParametersSection ||
        section.name == kDependenciesSection) {
      return Err(ErrorCode::InvalidArgument, "research_db: reserved payload section name");
    }
    std::sort(section.columns.begin(), section.columns.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
  }
  std::sort(sections.begin(), sections.end(),
            [](const RaSectionData &lhs, const RaSectionData &rhs) { return lhs.name < rhs.name; });
  for (std::size_t i = 1; i < sections.size(); ++i) {
    if (sections[i - 1u].name == sections[i].name) {
      return Err(ErrorCode::InvalidArgument, "research_db: duplicate payload section");
    }
  }
  return Ok(std::move(sections));
}

[[nodiscard]] Result<std::vector<ResearchParameter>>
canonical_parameters(std::span<const ResearchParameter> input) {
  if (input.size() > kMaxRequestItems) {
    return Err(ErrorCode::InvalidArgument, "research_db: too many parameters");
  }
  std::vector<ResearchParameter> parameters(input.begin(), input.end());
  for (ResearchParameter &parameter : parameters) {
    ATX_TRY_VOID(validate_parameter(parameter));
    if (parameter.f64_value == 0.0) {
      parameter.f64_value = 0.0;
    }
  }
  std::sort(parameters.begin(), parameters.end(), parameter_less);
  for (std::size_t i = 1; i < parameters.size(); ++i) {
    if (same_parameter_key(parameters[i - 1u], parameters[i])) {
      return Err(ErrorCode::InvalidArgument, "research_db: duplicate parameter key");
    }
  }
  return Ok(std::move(parameters));
}

[[nodiscard]] Result<std::vector<ResearchDependency>>
canonical_dependencies(std::span<const ResearchDependency> input) {
  if (input.size() > kMaxRequestItems) {
    return Err(ErrorCode::InvalidArgument, "research_db: too many dependencies");
  }
  std::vector<ResearchDependency> dependencies(input.begin(), input.end());
  for (const ResearchDependency &dependency : dependencies) {
    ATX_TRY_VOID(validate_dependency(dependency));
  }
  std::sort(dependencies.begin(), dependencies.end(), dependency_less);
  for (std::size_t i = 1; i < dependencies.size(); ++i) {
    if (same_dependency_key(dependencies[i - 1u], dependencies[i])) {
      return Err(ErrorCode::InvalidArgument, "research_db: duplicate dependency key");
    }
  }
  return Ok(std::move(dependencies));
}

[[nodiscard]] Status validate_request_envelope(const ResearchPublishRequest &request) {
  if (!valid_kind(request.kind) || !valid_logical_id(request.logical_id) ||
      request.payload_format_version == 0u || request.payload_schema_salt == 0u ||
      (!request.expected_head_id.empty() && !valid_sha256(request.expected_head_id))) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid publish envelope");
  }
  if ((request.row_count == 0u && (request.first_ts_ns != 0 || request.last_ts_ns != 0)) ||
      (request.row_count != 0u &&
       (request.first_ts_ns <= 0 || request.last_ts_ns < request.first_ts_ns))) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid row timestamp envelope");
  }
  return Ok();
}

class CanonicalBytes {
public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

  void u32(std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) {
      u8(static_cast<std::uint8_t>(value >> (8u * i)));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned i = 0; i < 8u; ++i) {
      u8(static_cast<std::uint8_t>(value >> (8u * i)));
    }
  }

  void i64(std::int64_t value) { u64(i64_bits(value)); }

  void f64(double value) { u64(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value)); }

  void text(std::string_view value) {
    u64(value.size());
    for (const char ch : value) {
      u8(static_cast<std::uint8_t>(ch));
    }
  }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
  std::vector<std::byte> bytes_;
};

void append_parameter(CanonicalBytes &out, const ResearchParameter &parameter) {
  out.text(parameter.scope);
  out.text(parameter.key);
  out.u8(static_cast<std::uint8_t>(parameter.kind));
  out.f64(parameter.f64_value);
  out.i64(parameter.i64_value);
  out.u32(parameter.u32_value);
  out.text(parameter.text_value);
}

void append_dependency(CanonicalBytes &out, const ResearchDependency &dependency) {
  out.u32(dependency.role);
  out.u32(dependency.kind);
  out.text(dependency.key);
  out.text(dependency.dependency_id);
  out.text(dependency.date_lo);
  out.text(dependency.date_hi);
  out.u64(dependency.archive_identity.file_size);
  out.u64(dependency.archive_identity.created_ts_ns);
  out.u32(dependency.archive_identity.header_crc32c);
  out.u32(dependency.archive_identity.metadata_crc32c);
}

void append_column(CanonicalBytes &out, const std::pair<std::string, RaColumnData> &entry) {
  const RaColumnData &column = entry.second;
  out.text(entry.first);
  out.u8(static_cast<std::uint8_t>(column.dtype));
  switch (column.dtype) {
  case RaDType::F64:
    for (const double value : column.f64) {
      out.f64(value);
    }
    break;
  case RaDType::I64:
    for (const std::int64_t value : column.i64) {
      out.i64(value);
    }
    break;
  case RaDType::U32:
  case RaDType::DictStr:
    for (const std::uint32_t value : column.u32) {
      out.u32(value);
    }
    break;
  case RaDType::U8Enum:
    for (const std::uint8_t value : column.u8) {
      out.u8(value);
    }
    break;
  }
  out.u64(column.strings.size());
  for (const std::string &value : column.strings) {
    out.text(value);
  }
}

void append_section(CanonicalBytes &out, const RaSectionData &section) {
  out.text(section.name);
  out.u8(static_cast<std::uint8_t>(section.kind));
  out.u64(section.n_rows);
  out.u64(section.columns.size());
  for (const auto &column : section.columns) {
    append_column(out, column);
  }
}

[[nodiscard]] Result<std::string> sha256_parameters(ResearchArtifactKind kind,
                                                    std::string_view logical_id,
                                                    std::uint32_t payload_format_version,
                                                    std::uint64_t payload_schema_salt,
                                                    std::span<const ResearchParameter> parameters) {
  CanonicalBytes bytes;
  bytes.text("atx.research.spec.v1");
  bytes.u64(kResearchDbSchemaSalt);
  bytes.u8(static_cast<std::uint8_t>(kind));
  bytes.text(logical_id);
  bytes.u32(payload_format_version);
  bytes.u64(payload_schema_salt);
  bytes.u64(parameters.size());
  for (const ResearchParameter &parameter : parameters) {
    append_parameter(bytes, parameter);
  }
  return atx::core::sha256_hex(bytes.bytes());
}

[[nodiscard]] Result<std::string>
sha256_dependencies(std::span<const ResearchDependency> dependencies) {
  CanonicalBytes bytes;
  bytes.text("atx.research.dependencies.v1");
  bytes.u64(kResearchDbSchemaSalt);
  bytes.u64(dependencies.size());
  for (const ResearchDependency &dependency : dependencies) {
    append_dependency(bytes, dependency);
  }
  return atx::core::sha256_hex(bytes.bytes());
}

struct CanonicalRequest {
  std::vector<ResearchParameter> parameters;
  std::vector<ResearchDependency> dependencies;
  std::vector<RaSectionData> sections;
  std::string specification_sha256;
  std::string dependency_sha256;
  std::string artifact_id;
};

[[nodiscard]] Result<CanonicalRequest> canonical_request(const ResearchPublishRequest &request) {
  ATX_TRY_VOID(validate_request_envelope(request));
  ATX_TRY(std::vector<ResearchParameter> parameters, canonical_parameters(request.parameters));
  ATX_TRY(std::vector<ResearchDependency> dependencies,
          canonical_dependencies(request.dependencies));
  ATX_TRY(std::vector<RaSectionData> sections, canonical_sections(request));
  if (request.kind == ResearchArtifactKind::SignalSegment) {
    const auto it =
        std::find_if(sections.begin(), sections.end(), [](const RaSectionData &section) {
          return section.name == kSignalValuesSection;
        });
    if (it == sections.end()) {
      return Err(ErrorCode::InvalidArgument, "research_db: SignalSegment requires signal_values");
    }
    ATX_TRY_VOID(validate_signal_data(request, *it));
  }
  ATX_TRY(std::string specification_sha256,
          sha256_parameters(request.kind, request.logical_id, request.payload_format_version,
                            request.payload_schema_salt, parameters));
  ATX_TRY(std::string dependency_sha256, sha256_dependencies(dependencies));

  CanonicalBytes bytes;
  bytes.text("atx.research.artifact.v1");
  bytes.u64(kResearchDbSchemaSalt);
  bytes.text(specification_sha256);
  bytes.text(dependency_sha256);
  bytes.text(request.expected_head_id);
  bytes.i64(request.first_ts_ns);
  bytes.i64(request.last_ts_ns);
  bytes.u64(request.row_count);
  bytes.u64(sections.size());
  for (const RaSectionData &section : sections) {
    append_section(bytes, section);
  }
  ATX_TRY(std::string artifact_id, atx::core::sha256_hex(bytes.bytes()));
  return Ok(CanonicalRequest{std::move(parameters), std::move(dependencies), std::move(sections),
                             std::move(specification_sha256), std::move(dependency_sha256),
                             std::move(artifact_id)});
}

[[nodiscard]] std::string kind_prefix(ResearchArtifactKind kind) {
  switch (kind) {
  case ResearchArtifactKind::StrategyDefinition:
    return "strategy";
  case ResearchArtifactKind::SignalSegment:
    return "signal";
  case ResearchArtifactKind::Candidate:
    return "candidate";
  case ResearchArtifactKind::Trial:
    return "trial";
  case ResearchArtifactKind::RiskScenarioSegment:
    return "risk";
  case ResearchArtifactKind::ExecutionIntent:
    return "intent";
  }
  return "invalid";
}

[[nodiscard]] std::string object_filename(ResearchArtifactKind kind, std::string_view artifact_id) {
  return kind_prefix(kind) + "-" + std::string(artifact_id) +
         std::string(kResearchDbObjectExtension);
}

[[nodiscard]] bool safe_object_filename(std::string_view filename, ResearchArtifactKind kind,
                                        std::string_view artifact_id) {
  return filename == object_filename(kind, artifact_id);
}

[[nodiscard]] Result<std::uint64_t> artifact_u64(std::string_view artifact_id) {
  if (!valid_sha256(artifact_id)) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid artifact ID");
  }
  std::uint64_t value = 0u;
  for (std::size_t i = 0; i < 16u; ++i) {
    const char ch = artifact_id[i];
    const std::uint64_t nibble = ch <= '9' ? static_cast<std::uint64_t>(ch - '0')
                                           : static_cast<std::uint64_t>(10 + ch - 'a');
    value = (value << 4u) | nibble;
  }
  return Ok(value == 0u ? 1u : value);
}

[[nodiscard]] std::int64_t deterministic_created_ts(std::string_view artifact_id,
                                                    std::int64_t last_ts_ns) {
  if (last_ts_ns > 0) {
    return last_ts_ns;
  }
  auto parsed = artifact_u64(artifact_id);
  const std::uint64_t value = parsed ? *parsed : 1u;
  return static_cast<std::int64_t>((value & 0x7FFFFFFFFFFFFFFFULL) | 1u);
}

struct SectionArena {
  std::vector<std::vector<double>> f64;
  std::vector<std::vector<std::int64_t>> i64;
  std::vector<std::vector<std::uint32_t>> u32;
  std::vector<std::vector<std::string>> strings;
};

[[nodiscard]] RaSectionData make_section(std::string name, RaSectionKind kind, std::uint64_t n_rows,
                                         const std::shared_ptr<SectionArena> &arena) {
  RaSectionData section;
  section.name = std::move(name);
  section.kind = kind;
  section.n_rows = n_rows;
  section.storage = arena;
  return section;
}

void add_f64(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
             std::vector<double> values) {
  arena->f64.push_back(std::move(values));
  section.columns.emplace_back(std::move(name),
                               RaColumnData::of_f64(std::span<const double>(arena->f64.back())));
}

void add_i64(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
             std::vector<std::int64_t> values) {
  arena->i64.push_back(std::move(values));
  section.columns.emplace_back(
      std::move(name), RaColumnData::of_i64(std::span<const std::int64_t>(arena->i64.back())));
}

void add_u32(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
             std::vector<std::uint32_t> values) {
  arena->u32.push_back(std::move(values));
  section.columns.emplace_back(
      std::move(name), RaColumnData::of_u32(std::span<const std::uint32_t>(arena->u32.back())));
}

void add_dict(RaSectionData &section, const std::shared_ptr<SectionArena> &arena, std::string name,
              std::vector<std::string> values) {
  std::vector<std::uint32_t> codes(values.size());
  std::iota(codes.begin(), codes.end(), std::uint32_t{0});
  arena->u32.push_back(std::move(codes));
  arena->strings.push_back(std::move(values));
  section.columns.emplace_back(
      std::move(name), RaColumnData::of_dict(std::span<const std::uint32_t>(arena->u32.back()),
                                             std::span<const std::string>(arena->strings.back())));
}

struct ExpectedColumn {
  std::string_view name;
  RaDType dtype;
};

[[nodiscard]] Status validate_columns(const RaSectionView &section,
                                      std::span<const ExpectedColumn> expected) {
  const auto columns = section.columns();
  if (columns.size() != expected.size()) {
    return Err(ErrorCode::ParseError, "research_db: custom section column count mismatch");
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const RaColumnDescriptor &column = columns[i];
    if (column.name_len > sizeof(column.name) ||
        std::string_view(column.name, column.name_len) != expected[i].name ||
        column.dtype != expected[i].dtype) {
      return Err(ErrorCode::ParseError, "research_db: custom section schema mismatch");
    }
  }
  return Ok();
}

[[nodiscard]] Result<RaSectionView> required_section(const RunArchive &archive,
                                                     std::string_view name, RaSectionKind kind) {
  auto section = archive.section(name);
  if (!section || section->kind() != kind) {
    return Err(ErrorCode::ParseError,
               "research_db: missing or invalid section " + std::string(name));
  }
  return section;
}

constexpr ExpectedColumn kMetaColumns[] = {
    {"format_version", RaDType::U32}, {"generation", RaDType::I64},
    {"schema_salt", RaDType::I64},    {"created_ts_ns", RaDType::I64},
    {"updated_ts_ns", RaDType::I64},
};

constexpr ExpectedColumn kArtifactIndexColumns[] = {
    {"artifact_id", RaDType::DictStr},
    {"kind", RaDType::U32},
    {"logical_id", RaDType::DictStr},
    {"revision", RaDType::U32},
    {"parent_id", RaDType::DictStr},
    {"specification_sha256", RaDType::DictStr},
    {"dependency_sha256", RaDType::DictStr},
    {"first_ts_ns", RaDType::I64},
    {"last_ts_ns", RaDType::I64},
    {"row_count", RaDType::I64},
    {"filename", RaDType::DictStr},
    {"file_size", RaDType::I64},
    {"file_created_ts_ns", RaDType::I64},
    {"header_crc32c", RaDType::U32},
    {"metadata_crc32c", RaDType::U32},
};

constexpr ExpectedColumn kHeadIndexColumns[] = {
    {"kind", RaDType::U32},
    {"logical_id", RaDType::DictStr},
    {"artifact_id", RaDType::DictStr},
    {"revision", RaDType::U32},
};

constexpr ExpectedColumn kArtifactMetaColumns[] = {
    {"format_version", RaDType::U32},
    {"schema_salt", RaDType::I64},
    {"payload_format_version", RaDType::U32},
    {"payload_schema_salt", RaDType::I64},
    {"artifact_id", RaDType::DictStr},
    {"kind", RaDType::U32},
    {"logical_id", RaDType::DictStr},
    {"revision", RaDType::U32},
    {"parent_id", RaDType::DictStr},
    {"specification_sha256", RaDType::DictStr},
    {"dependency_sha256", RaDType::DictStr},
    {"first_ts_ns", RaDType::I64},
    {"last_ts_ns", RaDType::I64},
    {"row_count", RaDType::I64},
};

constexpr ExpectedColumn kParameterColumns[] = {
    {"scope", RaDType::DictStr},      {"key", RaDType::DictStr},   {"value_kind", RaDType::U32},
    {"f64_value", RaDType::F64},      {"i64_value", RaDType::I64}, {"u32_value", RaDType::U32},
    {"text_value", RaDType::DictStr},
};

constexpr ExpectedColumn kDependencyColumns[] = {
    {"ordinal", RaDType::U32},
    {"role", RaDType::U32},
    {"kind", RaDType::U32},
    {"key", RaDType::DictStr},
    {"dependency_id", RaDType::DictStr},
    {"date_lo", RaDType::DictStr},
    {"date_hi", RaDType::DictStr},
    {"file_size", RaDType::I64},
    {"created_ts_ns", RaDType::I64},
    {"header_crc32c", RaDType::U32},
    {"metadata_crc32c", RaDType::U32},
};

} // namespace

ResearchParameter ResearchParameter::f64(std::string scope, std::string key, double value) {
  ResearchParameter parameter;
  parameter.scope = std::move(scope);
  parameter.key = std::move(key);
  parameter.kind = ResearchValueKind::F64;
  parameter.f64_value = value;
  return parameter;
}

ResearchParameter ResearchParameter::i64(std::string scope, std::string key, std::int64_t value) {
  ResearchParameter parameter;
  parameter.scope = std::move(scope);
  parameter.key = std::move(key);
  parameter.kind = ResearchValueKind::I64;
  parameter.i64_value = value;
  return parameter;
}

ResearchParameter ResearchParameter::u32(std::string scope, std::string key, std::uint32_t value) {
  ResearchParameter parameter;
  parameter.scope = std::move(scope);
  parameter.key = std::move(key);
  parameter.kind = ResearchValueKind::U32;
  parameter.u32_value = value;
  return parameter;
}

ResearchParameter ResearchParameter::text(std::string scope, std::string key, std::string value) {
  ResearchParameter parameter;
  parameter.scope = std::move(scope);
  parameter.key = std::move(key);
  parameter.kind = ResearchValueKind::Text;
  parameter.text_value = std::move(value);
  return parameter;
}

Result<std::string> research_artifact_identity(const ResearchPublishRequest &request) {
  ATX_TRY(CanonicalRequest canonical, canonical_request(request));
  return Ok(std::move(canonical.artifact_id));
}

Result<std::vector<std::uint64_t>>
research_signal_rows_available_as_of(const RaSectionView &signal_values,
                                     std::int64_t decision_ts_ns) {
  if (decision_ts_ns <= 0 || signal_values.name() != kSignalValuesSection ||
      signal_values.kind() != RaSectionKind::TimeSeries) {
    return Err(ErrorCode::InvalidArgument, "research_db: invalid point-in-time signal query");
  }
  const auto event_ts = signal_values.i64_col("event_ts_ns");
  const auto available_ts = signal_values.i64_col("available_ts_ns");
  const auto uids = signal_values.u32_col("uid");
  const RaDictColumn symbols = signal_values.dict_col("symbol");
  const auto values = signal_values.f64_col("value");
  const auto status = signal_values.u32_col("status");
  const std::size_t rows = static_cast<std::size_t>(signal_values.n_rows());
  if (event_ts.size() != rows || available_ts.size() != rows || uids.size() != rows ||
      symbols.size() != rows || values.size() != rows || status.size() != rows) {
    return Err(ErrorCode::ParseError, "research_db: invalid signal_values schema");
  }

  std::vector<std::uint64_t> available;
  available.reserve(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    if (event_ts[i] <= 0 || available_ts[i] < event_ts[i] || uids[i] == 0u ||
        symbols.at(i).empty() || (status[i] == 0u && !std::isfinite(values[i]))) {
      return Err(ErrorCode::ParseError, "research_db: invalid persisted signal row");
    }
    if (i != 0u && std::tie(event_ts[i - 1u], uids[i - 1u]) >= std::tie(event_ts[i], uids[i])) {
      return Err(ErrorCode::ParseError,
                 "research_db: persisted signal rows are not sorted and unique");
    }
    if (available_ts[i] <= decision_ts_ns) {
      available.push_back(static_cast<std::uint64_t>(i));
    }
  }
  return Ok(std::move(available));
}

struct ResearchDb::Snapshot {
  std::uint64_t generation{0};
  std::int64_t created_ts_ns{0};
  std::int64_t updated_ts_ns{0};
  std::vector<ResearchArtifactInfo> artifacts;
  std::vector<ResearchHead> heads;
};

namespace {

[[nodiscard]] RaSectionData build_meta_section(std::uint64_t generation, std::int64_t created_ts_ns,
                                               std::int64_t updated_ts_ns) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kMetaSection), RaSectionKind::ScalarKV, 1u, arena);
  add_u32(section, arena, "format_version", {kResearchDbFormatVersion});
  add_i64(section, arena, "generation", {u64_bits(generation)});
  add_i64(section, arena, "schema_salt", {u64_bits(kResearchDbSchemaSalt)});
  add_i64(section, arena, "created_ts_ns", {created_ts_ns});
  add_i64(section, arena, "updated_ts_ns", {updated_ts_ns});
  return section;
}

[[nodiscard]] RaSectionData
build_artifact_index_section(std::span<const ResearchArtifactInfo> artifacts) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section = make_section(std::string(kArtifactIndexSection), RaSectionKind::SubTable,
                                       artifacts.size(), arena);
  std::vector<std::string> artifact_ids;
  std::vector<std::uint32_t> kinds;
  std::vector<std::string> logical_ids;
  std::vector<std::uint32_t> revisions;
  std::vector<std::string> parent_ids;
  std::vector<std::string> specifications;
  std::vector<std::string> dependencies;
  std::vector<std::int64_t> first_ts;
  std::vector<std::int64_t> last_ts;
  std::vector<std::int64_t> row_counts;
  std::vector<std::string> filenames;
  std::vector<std::int64_t> file_sizes;
  std::vector<std::int64_t> file_created;
  std::vector<std::uint32_t> header_crcs;
  std::vector<std::uint32_t> metadata_crcs;
  for (const ResearchArtifactInfo &artifact : artifacts) {
    artifact_ids.push_back(artifact.artifact_id);
    kinds.push_back(static_cast<std::uint32_t>(artifact.kind));
    logical_ids.push_back(artifact.logical_id);
    revisions.push_back(artifact.revision);
    parent_ids.push_back(artifact.parent_artifact_id);
    specifications.push_back(artifact.specification_sha256);
    dependencies.push_back(artifact.dependency_sha256);
    first_ts.push_back(artifact.first_ts_ns);
    last_ts.push_back(artifact.last_ts_ns);
    row_counts.push_back(u64_bits(artifact.row_count));
    filenames.push_back(artifact.filename);
    file_sizes.push_back(u64_bits(artifact.archive_identity.file_size));
    file_created.push_back(u64_bits(artifact.archive_identity.created_ts_ns));
    header_crcs.push_back(artifact.archive_identity.header_crc32c);
    metadata_crcs.push_back(artifact.archive_identity.metadata_crc32c);
  }
  add_dict(section, arena, "artifact_id", std::move(artifact_ids));
  add_u32(section, arena, "kind", std::move(kinds));
  add_dict(section, arena, "logical_id", std::move(logical_ids));
  add_u32(section, arena, "revision", std::move(revisions));
  add_dict(section, arena, "parent_id", std::move(parent_ids));
  add_dict(section, arena, "specification_sha256", std::move(specifications));
  add_dict(section, arena, "dependency_sha256", std::move(dependencies));
  add_i64(section, arena, "first_ts_ns", std::move(first_ts));
  add_i64(section, arena, "last_ts_ns", std::move(last_ts));
  add_i64(section, arena, "row_count", std::move(row_counts));
  add_dict(section, arena, "filename", std::move(filenames));
  add_i64(section, arena, "file_size", std::move(file_sizes));
  add_i64(section, arena, "file_created_ts_ns", std::move(file_created));
  add_u32(section, arena, "header_crc32c", std::move(header_crcs));
  add_u32(section, arena, "metadata_crc32c", std::move(metadata_crcs));
  return section;
}

[[nodiscard]] RaSectionData build_head_index_section(std::span<const ResearchHead> heads) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kHeadIndexSection), RaSectionKind::SubTable, heads.size(), arena);
  std::vector<std::uint32_t> kinds;
  std::vector<std::string> logical_ids;
  std::vector<std::string> artifact_ids;
  std::vector<std::uint32_t> revisions;
  for (const ResearchHead &head : heads) {
    kinds.push_back(static_cast<std::uint32_t>(head.kind));
    logical_ids.push_back(head.logical_id);
    artifact_ids.push_back(head.artifact_id);
    revisions.push_back(head.revision);
  }
  add_u32(section, arena, "kind", std::move(kinds));
  add_dict(section, arena, "logical_id", std::move(logical_ids));
  add_dict(section, arena, "artifact_id", std::move(artifact_ids));
  add_u32(section, arena, "revision", std::move(revisions));
  return section;
}

[[nodiscard]] std::vector<RaSectionData>
build_manifest_sections(std::uint64_t generation, std::int64_t created_ts_ns,
                        std::int64_t updated_ts_ns, std::span<const ResearchArtifactInfo> artifacts,
                        std::span<const ResearchHead> heads) {
  std::vector<RaSectionData> sections;
  sections.reserve(3u);
  sections.push_back(build_meta_section(generation, created_ts_ns, updated_ts_ns));
  sections.push_back(build_artifact_index_section(artifacts));
  sections.push_back(build_head_index_section(heads));
  return sections;
}

[[nodiscard]] Status write_manifest_file(std::string_view path, std::uint64_t generation,
                                         std::int64_t created_ts_ns, std::int64_t updated_ts_ns,
                                         std::span<const ResearchArtifactInfo> artifacts,
                                         std::span<const ResearchHead> heads) {
  const std::vector<RaSectionData> sections =
      build_manifest_sections(generation, created_ts_ns, updated_ts_ns, artifacts, heads);
  std::uint64_t identity = kResearchDbSchemaSalt;
  identity ^= generation + 0x9e3779b97f4a7c15ULL + (identity << 6u) + (identity >> 2u);
  identity ^= static_cast<std::uint64_t>(artifacts.size()) + (identity << 6u) + (identity >> 2u);
  identity ^= static_cast<std::uint64_t>(heads.size()) + (identity << 6u) + (identity >> 2u);
  if (identity == 0u) {
    identity = 1u;
  }
  return write_run_archive_file(path, sections, updated_ts_ns, identity);
}

[[nodiscard]] RaSectionData build_artifact_meta_section(const ResearchArtifactInfo &info,
                                                        const ResearchPublishRequest &request) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section =
      make_section(std::string(kArtifactMetaSection), RaSectionKind::ScalarKV, 1u, arena);
  add_u32(section, arena, "format_version", {kResearchDbFormatVersion});
  add_i64(section, arena, "schema_salt", {u64_bits(kResearchDbSchemaSalt)});
  add_u32(section, arena, "payload_format_version", {request.payload_format_version});
  add_i64(section, arena, "payload_schema_salt", {u64_bits(request.payload_schema_salt)});
  add_dict(section, arena, "artifact_id", {info.artifact_id});
  add_u32(section, arena, "kind", {static_cast<std::uint32_t>(info.kind)});
  add_dict(section, arena, "logical_id", {info.logical_id});
  add_u32(section, arena, "revision", {info.revision});
  add_dict(section, arena, "parent_id", {info.parent_artifact_id});
  add_dict(section, arena, "specification_sha256", {info.specification_sha256});
  add_dict(section, arena, "dependency_sha256", {info.dependency_sha256});
  add_i64(section, arena, "first_ts_ns", {info.first_ts_ns});
  add_i64(section, arena, "last_ts_ns", {info.last_ts_ns});
  add_i64(section, arena, "row_count", {u64_bits(info.row_count)});
  return section;
}

[[nodiscard]] RaSectionData
build_parameters_section(std::span<const ResearchParameter> parameters) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section = make_section(std::string(kParametersSection), RaSectionKind::SubTable,
                                       parameters.size(), arena);
  std::vector<std::string> scopes;
  std::vector<std::string> keys;
  std::vector<std::uint32_t> kinds;
  std::vector<double> f64_values;
  std::vector<std::int64_t> i64_values;
  std::vector<std::uint32_t> u32_values;
  std::vector<std::string> text_values;
  for (const ResearchParameter &parameter : parameters) {
    scopes.push_back(parameter.scope);
    keys.push_back(parameter.key);
    kinds.push_back(static_cast<std::uint32_t>(parameter.kind));
    f64_values.push_back(parameter.f64_value);
    i64_values.push_back(parameter.i64_value);
    u32_values.push_back(parameter.u32_value);
    text_values.push_back(parameter.text_value);
  }
  add_dict(section, arena, "scope", std::move(scopes));
  add_dict(section, arena, "key", std::move(keys));
  add_u32(section, arena, "value_kind", std::move(kinds));
  add_f64(section, arena, "f64_value", std::move(f64_values));
  add_i64(section, arena, "i64_value", std::move(i64_values));
  add_u32(section, arena, "u32_value", std::move(u32_values));
  add_dict(section, arena, "text_value", std::move(text_values));
  return section;
}

[[nodiscard]] RaSectionData
build_dependencies_section(std::span<const ResearchDependency> dependencies) {
  auto arena = std::make_shared<SectionArena>();
  RaSectionData section = make_section(std::string(kDependenciesSection), RaSectionKind::SubTable,
                                       dependencies.size(), arena);
  std::vector<std::uint32_t> ordinals;
  std::vector<std::uint32_t> roles;
  std::vector<std::uint32_t> kinds;
  std::vector<std::string> keys;
  std::vector<std::string> dependency_ids;
  std::vector<std::string> date_lo;
  std::vector<std::string> date_hi;
  std::vector<std::int64_t> file_sizes;
  std::vector<std::int64_t> created_ts;
  std::vector<std::uint32_t> header_crcs;
  std::vector<std::uint32_t> metadata_crcs;
  for (std::size_t i = 0; i < dependencies.size(); ++i) {
    const ResearchDependency &dependency = dependencies[i];
    ordinals.push_back(static_cast<std::uint32_t>(i));
    roles.push_back(dependency.role);
    kinds.push_back(dependency.kind);
    keys.push_back(dependency.key);
    dependency_ids.push_back(dependency.dependency_id);
    date_lo.push_back(dependency.date_lo);
    date_hi.push_back(dependency.date_hi);
    file_sizes.push_back(u64_bits(dependency.archive_identity.file_size));
    created_ts.push_back(u64_bits(dependency.archive_identity.created_ts_ns));
    header_crcs.push_back(dependency.archive_identity.header_crc32c);
    metadata_crcs.push_back(dependency.archive_identity.metadata_crc32c);
  }
  add_u32(section, arena, "ordinal", std::move(ordinals));
  add_u32(section, arena, "role", std::move(roles));
  add_u32(section, arena, "kind", std::move(kinds));
  add_dict(section, arena, "key", std::move(keys));
  add_dict(section, arena, "dependency_id", std::move(dependency_ids));
  add_dict(section, arena, "date_lo", std::move(date_lo));
  add_dict(section, arena, "date_hi", std::move(date_hi));
  add_i64(section, arena, "file_size", std::move(file_sizes));
  add_i64(section, arena, "created_ts_ns", std::move(created_ts));
  add_u32(section, arena, "header_crc32c", std::move(header_crcs));
  add_u32(section, arena, "metadata_crc32c", std::move(metadata_crcs));
  return section;
}

[[nodiscard]] std::vector<RaSectionData>
build_object_sections(const ResearchArtifactInfo &info, const ResearchPublishRequest &request,
                      const CanonicalRequest &canonical) {
  std::vector<RaSectionData> sections;
  sections.reserve(3u + canonical.sections.size());
  sections.push_back(build_artifact_meta_section(info, request));
  sections.push_back(build_parameters_section(canonical.parameters));
  sections.push_back(build_dependencies_section(canonical.dependencies));
  sections.insert(sections.end(), canonical.sections.begin(), canonical.sections.end());
  return sections;
}

[[nodiscard]] Result<std::vector<ResearchArtifactInfo>>
decode_artifact_index(const RaSectionView &section) {
  ATX_TRY_VOID(validate_columns(section, kArtifactIndexColumns));
  const std::size_t count = static_cast<std::size_t>(section.n_rows());
  const RaDictColumn artifact_ids = section.dict_col("artifact_id");
  const auto kinds = section.u32_col("kind");
  const RaDictColumn logical_ids = section.dict_col("logical_id");
  const auto revisions = section.u32_col("revision");
  const RaDictColumn parent_ids = section.dict_col("parent_id");
  const RaDictColumn specifications = section.dict_col("specification_sha256");
  const RaDictColumn dependencies = section.dict_col("dependency_sha256");
  const auto first_ts = section.i64_col("first_ts_ns");
  const auto last_ts = section.i64_col("last_ts_ns");
  const auto row_counts = section.i64_col("row_count");
  const RaDictColumn filenames = section.dict_col("filename");
  const auto file_sizes = section.i64_col("file_size");
  const auto file_created = section.i64_col("file_created_ts_ns");
  const auto header_crcs = section.u32_col("header_crc32c");
  const auto metadata_crcs = section.u32_col("metadata_crc32c");
  std::vector<ResearchArtifactInfo> output;
  output.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    ResearchArtifactInfo info;
    info.artifact_id = artifact_ids.at(i);
    info.kind = static_cast<ResearchArtifactKind>(kinds[i]);
    info.logical_id = logical_ids.at(i);
    info.revision = revisions[i];
    info.parent_artifact_id = parent_ids.at(i);
    info.specification_sha256 = specifications.at(i);
    info.dependency_sha256 = dependencies.at(i);
    info.first_ts_ns = first_ts[i];
    info.last_ts_ns = last_ts[i];
    info.row_count = i64_bits(row_counts[i]);
    info.filename = filenames.at(i);
    info.archive_identity = ArchiveContentIdentity{
        i64_bits(file_sizes[i]), i64_bits(file_created[i]), header_crcs[i], metadata_crcs[i]};
    if (!valid_sha256(info.artifact_id) || !valid_kind(info.kind) ||
        !valid_logical_id(info.logical_id) || info.revision == 0u ||
        (!info.parent_artifact_id.empty() && !valid_sha256(info.parent_artifact_id)) ||
        !valid_sha256(info.specification_sha256) || !valid_sha256(info.dependency_sha256) ||
        !safe_object_filename(info.filename, info.kind, info.artifact_id) ||
        identity_is_zero(info.archive_identity) ||
        (info.row_count == 0u && (info.first_ts_ns != 0 || info.last_ts_ns != 0)) ||
        (info.row_count != 0u && (info.first_ts_ns <= 0 || info.last_ts_ns < info.first_ts_ns))) {
      return Err(ErrorCode::ParseError, "research_db: invalid artifact index record");
    }
    output.push_back(std::move(info));
  }
  if (!std::is_sorted(output.begin(), output.end(), artifact_less)) {
    return Err(ErrorCode::ParseError, "research_db: artifact index is not sorted");
  }
  for (std::size_t i = 1; i < output.size(); ++i) {
    if (output[i - 1u].artifact_id == output[i].artifact_id) {
      return Err(ErrorCode::ParseError, "research_db: duplicate artifact ID");
    }
  }
  return Ok(std::move(output));
}

[[nodiscard]] Result<std::vector<ResearchHead>>
decode_head_index(const RaSectionView &section, std::span<const ResearchArtifactInfo> artifacts) {
  ATX_TRY_VOID(validate_columns(section, kHeadIndexColumns));
  const std::size_t count = static_cast<std::size_t>(section.n_rows());
  const auto kinds = section.u32_col("kind");
  const RaDictColumn logical_ids = section.dict_col("logical_id");
  const RaDictColumn artifact_ids = section.dict_col("artifact_id");
  const auto revisions = section.u32_col("revision");
  std::vector<ResearchHead> output;
  output.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    ResearchHead head{static_cast<ResearchArtifactKind>(kinds[i]), std::string(logical_ids.at(i)),
                      std::string(artifact_ids.at(i)), revisions[i]};
    const auto artifact =
        std::lower_bound(artifacts.begin(), artifacts.end(), head.artifact_id,
                         [](const ResearchArtifactInfo &value, std::string_view id) {
                           return value.artifact_id < id;
                         });
    if (!valid_kind(head.kind) || !valid_logical_id(head.logical_id) ||
        !valid_sha256(head.artifact_id) || head.revision == 0u || artifact == artifacts.end() ||
        artifact->artifact_id != head.artifact_id || artifact->kind != head.kind ||
        artifact->logical_id != head.logical_id || artifact->revision != head.revision) {
      return Err(ErrorCode::ParseError, "research_db: invalid head index record");
    }
    output.push_back(std::move(head));
  }
  if (!std::is_sorted(output.begin(), output.end(), head_less)) {
    return Err(ErrorCode::ParseError, "research_db: head index is not sorted");
  }
  for (std::size_t i = 1; i < output.size(); ++i) {
    if (same_head_key(output[i - 1u], output[i])) {
      return Err(ErrorCode::ParseError, "research_db: duplicate logical head");
    }
  }
  return Ok(std::move(output));
}

[[nodiscard]] Status validate_artifact_meta(const RaSectionView &section,
                                            const ResearchArtifactInfo &info) {
  ATX_TRY_VOID(validate_columns(section, kArtifactMetaColumns));
  if (section.n_rows() != 1u) {
    return Err(ErrorCode::ParseError, "research_db: invalid artifact metadata row count");
  }
  const auto format = section.u32_col("format_version");
  const auto salt = section.i64_col("schema_salt");
  const auto payload_format = section.u32_col("payload_format_version");
  const auto payload_salt = section.i64_col("payload_schema_salt");
  const RaDictColumn artifact_id = section.dict_col("artifact_id");
  const auto kind = section.u32_col("kind");
  const RaDictColumn logical_id = section.dict_col("logical_id");
  const auto revision = section.u32_col("revision");
  const RaDictColumn parent_id = section.dict_col("parent_id");
  const RaDictColumn specification = section.dict_col("specification_sha256");
  const RaDictColumn dependency = section.dict_col("dependency_sha256");
  const auto first_ts = section.i64_col("first_ts_ns");
  const auto last_ts = section.i64_col("last_ts_ns");
  const auto row_count = section.i64_col("row_count");
  if (format[0] != kResearchDbFormatVersion || i64_bits(salt[0]) != kResearchDbSchemaSalt ||
      payload_format[0] == 0u || i64_bits(payload_salt[0]) == 0u ||
      artifact_id.at(0) != info.artifact_id || kind[0] != static_cast<std::uint32_t>(info.kind) ||
      logical_id.at(0) != info.logical_id || revision[0] != info.revision ||
      parent_id.at(0) != info.parent_artifact_id ||
      specification.at(0) != info.specification_sha256 ||
      dependency.at(0) != info.dependency_sha256 || first_ts[0] != info.first_ts_ns ||
      last_ts[0] != info.last_ts_ns || i64_bits(row_count[0]) != info.row_count) {
    return Err(ErrorCode::ParseError, "research_db: artifact metadata/index mismatch");
  }
  return Ok();
}

struct OpenedObject {
  std::shared_ptr<const RunArchive> archive;
};

[[nodiscard]] Result<OpenedObject> open_object(const std::string &path,
                                               const ResearchArtifactInfo &info) {
  ATX_TRY(RunArchive opened, RunArchive::open_mapped(path));
  ATX_TRY_VOID(opened.validate_all());
  if (opened.identity() != info.archive_identity) {
    return Err(ErrorCode::ParseError, "research_db: object identity disagrees with manifest");
  }
  ATX_TRY(std::uint64_t expected_run_identity, artifact_u64(info.artifact_id));
  if (opened.header().run_identity_hash != expected_run_identity || opened.count() < 3u) {
    return Err(ErrorCode::ParseError, "research_db: invalid object envelope");
  }
  ATX_TRY(RaSectionView meta,
          required_section(opened, kArtifactMetaSection, RaSectionKind::ScalarKV));
  ATX_TRY_VOID(validate_artifact_meta(meta, info));
  ATX_TRY(RaSectionView parameters,
          required_section(opened, kParametersSection, RaSectionKind::SubTable));
  ATX_TRY_VOID(validate_columns(parameters, kParameterColumns));
  ATX_TRY(RaSectionView dependencies,
          required_section(opened, kDependenciesSection, RaSectionKind::SubTable));
  ATX_TRY_VOID(validate_columns(dependencies, kDependencyColumns));
  auto archive = std::make_shared<RunArchive>(std::move(opened));
  return Ok(OpenedObject{std::shared_ptr<const RunArchive>(std::move(archive))});
}

[[nodiscard]] Result<std::vector<ResearchDependency>>
decode_dependencies(const RaSectionView &section) {
  ATX_TRY_VOID(validate_columns(section, kDependencyColumns));
  const std::size_t count = static_cast<std::size_t>(section.n_rows());
  const auto ordinals = section.u32_col("ordinal");
  const auto roles = section.u32_col("role");
  const auto kinds = section.u32_col("kind");
  const RaDictColumn keys = section.dict_col("key");
  const RaDictColumn dependency_ids = section.dict_col("dependency_id");
  const RaDictColumn date_lo = section.dict_col("date_lo");
  const RaDictColumn date_hi = section.dict_col("date_hi");
  const auto file_sizes = section.i64_col("file_size");
  const auto created_ts = section.i64_col("created_ts_ns");
  const auto header_crcs = section.u32_col("header_crc32c");
  const auto metadata_crcs = section.u32_col("metadata_crc32c");
  std::vector<ResearchDependency> output;
  output.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (ordinals[i] != i) {
      return Err(ErrorCode::ParseError, "research_db: dependency ordinal mismatch");
    }
    ResearchDependency dependency;
    dependency.role = roles[i];
    dependency.kind = kinds[i];
    dependency.key = keys.at(i);
    dependency.dependency_id = dependency_ids.at(i);
    dependency.date_lo = date_lo.at(i);
    dependency.date_hi = date_hi.at(i);
    dependency.archive_identity = ArchiveContentIdentity{
        i64_bits(file_sizes[i]), i64_bits(created_ts[i]), header_crcs[i], metadata_crcs[i]};
    const Status valid = validate_dependency(dependency);
    if (!valid) {
      return Err(ErrorCode::ParseError, valid.error().message());
    }
    output.push_back(std::move(dependency));
  }
  if (!std::is_sorted(output.begin(), output.end(), dependency_less)) {
    return Err(ErrorCode::ParseError, "research_db: dependencies are not canonical");
  }
  return Ok(std::move(output));
}

[[nodiscard]] Result<ArchiveContentIdentity>
validate_object_for_publish(const std::string &path, const ResearchArtifactInfo &info) {
  ATX_TRY(RunArchive archive, RunArchive::open_mapped(path));
  ATX_TRY_VOID(archive.validate_all());
  ATX_TRY(std::uint64_t expected_run_identity, artifact_u64(info.artifact_id));
  if (archive.header().run_identity_hash != expected_run_identity || archive.count() < 3u) {
    return Err(ErrorCode::ParseError, "research_db: invalid published object envelope");
  }
  ATX_TRY(RaSectionView meta,
          required_section(archive, kArtifactMetaSection, RaSectionKind::ScalarKV));
  ATX_TRY_VOID(validate_artifact_meta(meta, info));
  ATX_TRY(RaSectionView parameters,
          required_section(archive, kParametersSection, RaSectionKind::SubTable));
  ATX_TRY_VOID(validate_columns(parameters, kParameterColumns));
  ATX_TRY(RaSectionView dependencies,
          required_section(archive, kDependenciesSection, RaSectionKind::SubTable));
  ATX_TRY_VOID(validate_columns(dependencies, kDependencyColumns));
  return Ok(archive.identity());
}

} // namespace

Result<ResearchDb> ResearchDb::create(std::string_view backtest_root) {
  if (backtest_root.empty()) {
    return Err(ErrorCode::InvalidArgument, "ResearchDb::create: empty backtest root");
  }
  const std::filesystem::path backtest_path{std::string(backtest_root)};
  const std::filesystem::path research_path = backtest_path / std::string(kResearchDbDirectory);
  const std::filesystem::path manifest = research_path / std::string(kResearchDbManifestName);
  std::error_code ec;
  const bool manifest_exists = std::filesystem::exists(manifest, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "ResearchDb::create: cannot inspect manifest");
  }
  if (manifest_exists) {
    return Err(ErrorCode::AlreadyExists, "ResearchDb::create: research manifest already exists");
  }
  std::filesystem::create_directories(research_path / std::string(kResearchDbObjectDirectory), ec);
  if (ec) {
    return Err(ErrorCode::IoError, "ResearchDb::create: cannot create research directories");
  }
  const std::int64_t now = wall_clock_ns();
  const std::vector<ResearchArtifactInfo> artifacts;
  const std::vector<ResearchHead> heads;
  const Status written = write_manifest_file(manifest.string(), 1u, now, now, artifacts, heads);
  if (!written) {
    return tl::unexpected<Error>(written.error());
  }
  return open(backtest_root);
}

Result<ResearchDb> ResearchDb::open(std::string_view backtest_root) {
  if (backtest_root.empty()) {
    return Err(ErrorCode::InvalidArgument, "ResearchDb::open: empty backtest root");
  }
  const std::filesystem::path backtest_path{std::string(backtest_root)};
  const std::filesystem::path research_path = backtest_path / std::string(kResearchDbDirectory);
  const std::filesystem::path manifest = research_path / std::string(kResearchDbManifestName);
  std::error_code ec;
  const bool exists = std::filesystem::exists(manifest, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "ResearchDb::open: cannot inspect manifest");
  }
  if (!exists) {
    return Err(ErrorCode::NotFound, "ResearchDb::open: research manifest not found");
  }
  ATX_TRY(std::shared_ptr<const Snapshot> snapshot, read_manifest(manifest.string()));
  ResearchDb db;
  db.backtest_root_ = backtest_path.string();
  db.research_root_ = research_path.string();
  db.mu_ = std::make_unique<std::mutex>();
  db.snapshot_ = std::move(snapshot);
  return Ok(std::move(db));
}

std::string ResearchDb::manifest_path() const {
  return (std::filesystem::path(research_root_) / std::string(kResearchDbManifestName)).string();
}

std::string ResearchDb::object_path(std::string_view filename) const {
  return (std::filesystem::path(research_root_) / std::string(kResearchDbObjectDirectory) /
          std::string(filename))
      .string();
}

std::uint64_t ResearchDb::generation() const {
  const std::lock_guard lock(*mu_);
  return snapshot_->generation;
}

Status ResearchDb::refresh() {
  const std::lock_guard lock(*mu_);
  auto replacement = read_manifest(manifest_path());
  if (!replacement) {
    return tl::unexpected<Error>(std::move(replacement).error());
  }
  if ((*replacement)->generation < snapshot_->generation) {
    return Err(ErrorCode::ParseError, "ResearchDb::refresh: manifest generation rolled back");
  }
  if ((*replacement)->generation > snapshot_->generation) {
    snapshot_ = std::move(*replacement);
  }
  return Ok();
}

std::vector<ResearchArtifactInfo> ResearchDb::artifacts() const {
  const std::lock_guard lock(*mu_);
  return snapshot_->artifacts;
}

std::vector<ResearchHead> ResearchDb::heads() const {
  const std::lock_guard lock(*mu_);
  return snapshot_->heads;
}

Result<ResearchArtifactInfo> ResearchDb::find_artifact(std::string_view artifact_id) const {
  if (!valid_sha256(artifact_id)) {
    return Err(ErrorCode::InvalidArgument, "ResearchDb::find_artifact: invalid artifact ID");
  }
  const std::lock_guard lock(*mu_);
  const auto it =
      std::lower_bound(snapshot_->artifacts.begin(), snapshot_->artifacts.end(), artifact_id,
                       [](const ResearchArtifactInfo &value, std::string_view id) {
                         return value.artifact_id < id;
                       });
  if (it == snapshot_->artifacts.end() || it->artifact_id != artifact_id) {
    return Err(ErrorCode::NotFound, "ResearchDb::find_artifact: artifact not found");
  }
  return Ok(*it);
}

Result<ResearchHead> ResearchDb::find_head(ResearchArtifactKind kind,
                                           std::string_view logical_id) const {
  if (!valid_kind(kind) || !valid_logical_id(logical_id)) {
    return Err(ErrorCode::InvalidArgument, "ResearchDb::find_head: invalid key");
  }
  ResearchHead key;
  key.kind = kind;
  key.logical_id = logical_id;
  const std::lock_guard lock(*mu_);
  const auto it =
      std::lower_bound(snapshot_->heads.begin(), snapshot_->heads.end(), key, head_less);
  if (it == snapshot_->heads.end() || !same_head_key(*it, key)) {
    return Err(ErrorCode::NotFound, "ResearchDb::find_head: head not found");
  }
  return Ok(*it);
}

Status ResearchDb::persist_locked(std::vector<ResearchArtifactInfo> artifacts,
                                  std::vector<ResearchHead> heads) {
  if (snapshot_->generation == std::numeric_limits<std::uint64_t>::max()) {
    return Err(ErrorCode::InvalidArgument, "ResearchDb: manifest generation exhausted");
  }
  std::sort(artifacts.begin(), artifacts.end(), artifact_less);
  std::sort(heads.begin(), heads.end(), head_less);
  const std::uint64_t next_generation = snapshot_->generation + 1u;
  const std::int64_t updated_ts_ns = std::max(wall_clock_ns(), snapshot_->updated_ts_ns);
  const Status written = write_manifest_file(
      manifest_path(), next_generation, snapshot_->created_ts_ns, updated_ts_ns, artifacts, heads);
  if (!written) {
    return written;
  }
  auto replacement = read_manifest(manifest_path());
  if (!replacement || (*replacement)->generation != next_generation) {
    return Err(ErrorCode::ParseError, "ResearchDb: published manifest failed validation");
  }
  snapshot_ = std::move(*replacement);
  return Ok();
}

Result<ResearchArtifactInfo> ResearchDb::publish(const ResearchPublishRequest &request) {
  ATX_TRY(CanonicalRequest canonical, canonical_request(request));

  const std::lock_guard lock(*mu_);
  auto replacement = read_manifest(manifest_path());
  if (!replacement) {
    return tl::unexpected<Error>(std::move(replacement).error());
  }
  if ((*replacement)->generation < snapshot_->generation) {
    return Err(ErrorCode::ParseError, "ResearchDb::publish: manifest generation rolled back");
  }
  if ((*replacement)->generation > snapshot_->generation) {
    snapshot_ = std::move(*replacement);
  }

  ResearchHead head_key;
  head_key.kind = request.kind;
  head_key.logical_id = request.logical_id;
  const auto current =
      std::lower_bound(snapshot_->heads.begin(), snapshot_->heads.end(), head_key, head_less);
  const bool has_current = current != snapshot_->heads.end() && same_head_key(*current, head_key);
  const std::string current_id = has_current ? current->artifact_id : std::string{};
  if (has_current && current_id == canonical.artifact_id) {
    const auto existing = std::lower_bound(
        snapshot_->artifacts.begin(), snapshot_->artifacts.end(), canonical.artifact_id,
        [](const ResearchArtifactInfo &value, std::string_view id) {
          return value.artifact_id < id;
        });
    if (existing == snapshot_->artifacts.end() || existing->artifact_id != canonical.artifact_id) {
      return Err(ErrorCode::ParseError, "ResearchDb::publish: head artifact is not indexed");
    }
    return Ok(*existing);
  }
  if (current_id != request.expected_head_id) {
    return Err(ErrorCode::Unavailable, "ResearchDb::publish: expected head is stale");
  }
  if (has_current && current->revision == std::numeric_limits<std::uint32_t>::max()) {
    return Err(ErrorCode::InvalidArgument, "ResearchDb::publish: logical revision exhausted");
  }

  ResearchArtifactInfo info;
  info.artifact_id = canonical.artifact_id;
  info.kind = request.kind;
  info.logical_id = request.logical_id;
  info.revision = has_current ? current->revision + 1u : 1u;
  info.parent_artifact_id = request.expected_head_id;
  info.specification_sha256 = canonical.specification_sha256;
  info.dependency_sha256 = canonical.dependency_sha256;
  info.first_ts_ns = request.first_ts_ns;
  info.last_ts_ns = request.last_ts_ns;
  info.row_count = request.row_count;
  info.filename = object_filename(info.kind, info.artifact_id);

  const std::string path = object_path(info.filename);
  std::error_code ec;
  const bool object_exists = std::filesystem::exists(path, ec);
  if (ec) {
    return Err(ErrorCode::IoError, "ResearchDb::publish: cannot inspect object path");
  }
  if (!object_exists) {
    const std::vector<RaSectionData> sections = build_object_sections(info, request, canonical);
    ATX_TRY(std::uint64_t run_identity, artifact_u64(info.artifact_id));
    const Status written = write_run_archive_file(
        path, sections, deterministic_created_ts(info.artifact_id, info.last_ts_ns), run_identity);
    if (!written) {
      return tl::unexpected<Error>(written.error());
    }
  }
  ATX_TRY(ArchiveContentIdentity object_identity, validate_object_for_publish(path, info));
  info.archive_identity = object_identity;

  const auto duplicate =
      std::lower_bound(snapshot_->artifacts.begin(), snapshot_->artifacts.end(), info.artifact_id,
                       [](const ResearchArtifactInfo &value, std::string_view id) {
                         return value.artifact_id < id;
                       });
  if (duplicate != snapshot_->artifacts.end() && duplicate->artifact_id == info.artifact_id) {
    return Err(ErrorCode::AlreadyExists, "ResearchDb::publish: artifact ID is already indexed");
  }

  std::vector<ResearchArtifactInfo> artifacts = snapshot_->artifacts;
  artifacts.push_back(info);
  std::vector<ResearchHead> heads = snapshot_->heads;
  if (has_current) {
    const std::size_t head_index = static_cast<std::size_t>(current - snapshot_->heads.begin());
    heads[head_index] = ResearchHead{info.kind, info.logical_id, info.artifact_id, info.revision};
  } else {
    heads.push_back(ResearchHead{info.kind, info.logical_id, info.artifact_id, info.revision});
  }
  ATX_TRY_VOID(persist_locked(std::move(artifacts), std::move(heads)));
  return Ok(std::move(info));
}

Result<MappedResearchSection> ResearchDb::map_section(std::string_view artifact_id,
                                                      std::string_view section_name) const {
  if (section_name.empty()) {
    return Err(ErrorCode::InvalidArgument, "ResearchDb::map_section: empty section name");
  }
  ATX_TRY(ResearchArtifactInfo info, find_artifact(artifact_id));
  ATX_TRY(OpenedObject opened, open_object(object_path(info.filename), info));
  auto section = opened.archive->section(section_name);
  if (!section) {
    return Err(ErrorCode::NotFound, "ResearchDb::map_section: section not found");
  }
  MappedResearchSection mapped;
  mapped.archive = std::move(opened.archive);
  mapped.view = std::move(*section);
  return Ok(std::move(mapped));
}

Result<std::vector<ResearchDependency>>
ResearchDb::load_dependencies(std::string_view artifact_id) const {
  ATX_TRY(ResearchArtifactInfo info, find_artifact(artifact_id));
  ATX_TRY(OpenedObject opened, open_object(object_path(info.filename), info));
  ATX_TRY(RaSectionView dependencies,
          required_section(*opened.archive, kDependenciesSection, RaSectionKind::SubTable));
  return decode_dependencies(dependencies);
}

Result<std::shared_ptr<const ResearchDb::Snapshot>>
ResearchDb::read_manifest(const std::string &path) {
  ATX_TRY(RunArchive archive, RunArchive::open_file(path));
  ATX_TRY_VOID(archive.validate_all());
  if (archive.count() != 3u) {
    return Err(ErrorCode::ParseError, "research_db: manifest section count mismatch");
  }
  ATX_TRY(RaSectionView meta, required_section(archive, kMetaSection, RaSectionKind::ScalarKV));
  ATX_TRY(RaSectionView artifact_index,
          required_section(archive, kArtifactIndexSection, RaSectionKind::SubTable));
  ATX_TRY(RaSectionView head_index,
          required_section(archive, kHeadIndexSection, RaSectionKind::SubTable));
  ATX_TRY_VOID(validate_columns(meta, kMetaColumns));
  if (meta.n_rows() != 1u) {
    return Err(ErrorCode::ParseError, "research_db: invalid manifest metadata row count");
  }
  const auto format = meta.u32_col("format_version");
  const auto generation = meta.i64_col("generation");
  const auto schema_salt = meta.i64_col("schema_salt");
  const auto created_ts = meta.i64_col("created_ts_ns");
  const auto updated_ts = meta.i64_col("updated_ts_ns");
  if (format[0] != kResearchDbFormatVersion || i64_bits(schema_salt[0]) != kResearchDbSchemaSalt ||
      i64_bits(generation[0]) == 0u || created_ts[0] <= 0 || updated_ts[0] < created_ts[0]) {
    return Err(ErrorCode::ParseError, "research_db: unsupported manifest metadata");
  }

  ATX_TRY(std::vector<ResearchArtifactInfo> artifacts, decode_artifact_index(artifact_index));
  ATX_TRY(std::vector<ResearchHead> heads, decode_head_index(head_index, artifacts));

  for (const ResearchArtifactInfo &artifact : artifacts) {
    if ((artifact.revision == 1u) != artifact.parent_artifact_id.empty()) {
      return Err(ErrorCode::ParseError, "research_db: invalid artifact revision root");
    }
    if (!artifact.parent_artifact_id.empty()) {
      const auto parent =
          std::lower_bound(artifacts.begin(), artifacts.end(), artifact.parent_artifact_id,
                           [](const ResearchArtifactInfo &value, std::string_view id) {
                             return value.artifact_id < id;
                           });
      if (parent == artifacts.end() || parent->artifact_id != artifact.parent_artifact_id ||
          parent->kind != artifact.kind || parent->logical_id != artifact.logical_id ||
          parent->revision + 1u != artifact.revision) {
        return Err(ErrorCode::ParseError, "research_db: invalid artifact parent chain");
      }
    }
    ResearchHead head_key;
    head_key.kind = artifact.kind;
    head_key.logical_id = artifact.logical_id;
    const auto head = std::lower_bound(heads.begin(), heads.end(), head_key, head_less);
    if (head == heads.end() || !same_head_key(*head, head_key) ||
        head->revision < artifact.revision) {
      return Err(ErrorCode::ParseError, "research_db: artifact has no current logical head");
    }
  }

  auto snapshot = std::make_shared<Snapshot>();
  snapshot->generation = i64_bits(generation[0]);
  snapshot->created_ts_ns = created_ts[0];
  snapshot->updated_ts_ns = updated_ts[0];
  snapshot->artifacts = std::move(artifacts);
  snapshot->heads = std::move(heads);
  return Ok(std::shared_ptr<const Snapshot>(std::move(snapshot)));
}

} // namespace atx::vol

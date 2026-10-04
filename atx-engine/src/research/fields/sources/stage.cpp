#include "atx/engine/research/fields/sources/stage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/fields/clock.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr u64 kStageManifestLimit = 64ULL << 20;

[[nodiscard]] core::Error refused(const StageContract &contract, std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     std::string(contract.key) + " stage: " + std::move(message));
}

[[nodiscard]] std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

[[nodiscard]] std::string string_member(const Json &j, const char *key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// The manifest's rule names `rule`: it is that string, or a list holding it (a stage built under
// several rules, e.g. the indexes stage's russell-proxy-v1 and sp500-proxy-v1).
[[nodiscard]] bool names_rule(const Json &m, std::string_view rule) {
  const auto it = m.find("rule");
  if (it == m.end()) {
    return false;
  }
  if (it->is_string()) {
    return it->get<std::string>() == rule;
  }
  return it->is_array() && std::any_of(it->begin(), it->end(), [rule](const Json &r) {
           return r.is_string() && r.get<std::string>() == rule;
         });
}

// The declared rule keys of the contract against the manifest's (Python pin_stage).
[[nodiscard]] core::Status check_rules(const Json &m, const StageContract &c) {
  if (!c.clock_rule.empty() && string_member(m, "clock_rule") != c.clock_rule) {
    return core::Err(
        refused(c, "clock_rule differs from the declared " + std::string(c.clock_rule)));
  }
  if (!c.rule.empty() && !names_rule(m, c.rule)) {
    return core::Err(refused(c, "rule does not name the declared " + std::string(c.rule)));
  }
  if (c.staleness_days) {
    const auto it = m.find("staleness_days");
    if (it == m.end() || !it->is_number_integer() || it->get<i64>() != *c.staleness_days) {
      return core::Err(refused(c, "staleness_days differs from the declared " +
                                      std::to_string(*c.staleness_days)));
    }
  }
  return core::Ok();
}

} // namespace

std::vector<i64> previous_session_marks(std::span<const i64> days) {
  std::vector<i64> out(days.size(), kBeforeAllNs);
  for (usize t = 1; t < days.size(); ++t) {
    out[t] = days[t - 1] * kDayNs + kSessionMarkNs;
  }
  return out;
}

std::string iso_instant(i64 ns) {
  constexpr i64 kHourNs = 3'600'000'000'000LL;
  constexpr i64 kMinuteNs = 60'000'000'000LL;
  constexpr i64 kSecondNs = 1'000'000'000LL;
  i64 day = ns / kDayNs;
  i64 rem = ns % kDayNs;
  if (rem < 0) {
    rem += kDayNs;
    --day;
  }
  std::array<char, 40> clock_text{};
  const int written =
      std::snprintf(clock_text.data(), clock_text.size(), "T%02lld:%02lld:%02lld.%09lldZ",
                    static_cast<long long>(rem / kHourNs),
                    static_cast<long long>((rem % kHourNs) / kMinuteNs),
                    static_cast<long long>((rem % kMinuteNs) / kSecondNs),
                    static_cast<long long>(rem % kSecondNs));
  if (written <= 0) {
    return iso_date(day);
  }
  return iso_date(day) + std::string(clock_text.data());
}

core::Result<PinnedStage> PinnedStage::open(const StageRequest &request,
                                            const StageContract &contract) {
  try {
    const auto path = request.dir / "manifest.json";
    ATX_TRY(const auto blob, read_bounded(path, kStageManifestLimit));
    ATX_TRY(const auto digest, core::sha256_hex(std::string_view(blob)));
    if (digest != lower(request.manifest_sha256)) {
      return core::Err(refused(contract, "manifest SHA-256 does not match the pin"));
    }
    PinnedStage out;
    out.dir_ = request.dir;
    out.contract_ = contract;
    out.manifest_ = Json::parse(blob, nullptr, false);
    const Json &m = out.manifest_;
    if (m.is_discarded() || !m.is_object() || string_member(m, "schema") != contract.schema ||
        string_member(m, "status") != "complete") {
      return core::Err(refused(contract, "manifest is not a complete " +
                                             std::string(contract.schema) + " manifest"));
    }
    ATX_TRY_VOID(check_rules(m, contract));
    const auto files = m.find("files");
    if (files == m.end() || !files->is_object()) {
      return core::Err(refused(contract, "manifest lists no files"));
    }
    out.manifest_source_ = SourceRecord{record_path(path), static_cast<u64>(blob.size()), digest};
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(refused(contract, e.what()));
  }
}

bool PinnedStage::lists(std::string_view rel) const {
  const auto files = manifest_.find("files");
  return files != manifest_.end() && files->is_object() && files->contains(std::string(rel));
}

core::Result<std::filesystem::path> PinnedStage::verified(std::string_view rel, FileStamp &stamp) {
  const Json &files = manifest_.at("files");
  const auto entry = files.find(std::string(rel));
  if (entry == files.end() || !entry->is_object()) {
    return core::Err(refused(contract_, "manifest does not list " + std::string(rel)));
  }
  const auto bytes = entry->find("bytes");
  const std::string pinned = lower(string_member(*entry, "sha256"));
  if (bytes == entry->end() || !bytes->is_number_unsigned() || pinned.empty()) {
    return core::Err(refused(contract_, "manifest entry of " + std::string(rel) +
                                            " lacks bytes or sha256"));
  }
  const std::filesystem::path path = dir_ / std::filesystem::path(std::string(rel));
  ATX_TRY(stamp, file_stamp(path));
  ATX_TRY(const auto digest, digest_file(path));
  ATX_TRY(const auto after, file_stamp(path));
  if (stamp.bytes != bytes->get<u64>() || digest.bytes != stamp.bytes || digest.sha256 != pinned ||
      !(after == stamp)) {
    return core::Err(refused(contract_, std::string(rel) + " does not match its manifest entry"));
  }
  SourceRecord record{record_path(path), digest.bytes, digest.sha256};
  const bool seen = std::any_of(read_.begin(), read_.end(), [&record](const SourceRecord &r) {
    return r.path == record.path;
  });
  if (!seen) {
    read_.push_back(std::move(record));
  }
  return core::Ok(path);
}

core::Result<core::io::ParquetTable>
PinnedStage::table(std::string_view rel, std::span<const std::string_view> columns) {
  FileStamp stamp;
  ATX_TRY(const auto path, verified(rel, stamp));
  ATX_TRY(auto decoded, core::io::read_parquet(path.string(), columns));
  ATX_TRY(const auto after, file_stamp(path));
  if (!(after == stamp)) {
    return core::Err(core::ErrorCode::IoError, std::string(contract_.key) + " stage: " +
                                                   std::string(rel) + " changed while it was read");
  }
  return core::Ok(std::move(decoded));
}

core::Status PinnedStage::for_each_group(std::string_view rel,
                                         std::span<const std::string_view> columns,
                                         const GroupVisitor &visit) {
  FileStamp stamp;
  ATX_TRY(const auto path, verified(rel, stamp));
  {
    ATX_TRY(auto scan, core::io::LazyParquet::scan(path.string()));
    std::vector<std::string> names;
    names.reserve(columns.size());
    for (const std::string_view c : columns) {
      names.emplace_back(c);
    }
    scan.select(std::move(names));
    ATX_TRY(auto stream, scan.stream());
    const auto groups = scan.num_row_groups();
    for (i64 g = 0; g <= groups; ++g) { // at most one group per row group, then the end
      ATX_TRY(auto group, stream.next());
      if (!group) {
        break;
      }
      ATX_TRY_VOID(visit(*group));
    }
  }
  ATX_TRY(const auto after, file_stamp(path));
  if (!(after == stamp)) {
    return core::Err(core::ErrorCode::IoError, std::string(contract_.key) + " stage: " +
                                                   std::string(rel) + " changed while it was read");
  }
  return core::Ok();
}

std::vector<SourceRecord> PinnedStage::sources() const {
  std::vector<SourceRecord> files = read_;
  std::sort(files.begin(), files.end(),
            [](const SourceRecord &a, const SourceRecord &b) { return a.path < b.path; });
  std::vector<SourceRecord> out;
  out.reserve(files.size() + 1);
  out.push_back(manifest_source_);
  out.insert(out.end(), files.begin(), files.end());
  return out;
}

nlohmann::json PinnedStage::check() const {
  const auto member = [this](const char *key) -> Json {
    const auto it = manifest_.find(key);
    return it == manifest_.end() ? Json(nullptr) : *it;
  };
  Json git_head = nullptr;
  const auto code = manifest_.find("code");
  if (code != manifest_.end() && code->is_object() && code->contains("git_head")) {
    git_head = code->at("git_head");
  }
  return Json{{"manifest_sha256", manifest_source_.sha256},
              {"schema", member("schema")},
              {"clock_rule", member("clock_rule")},
              {"staleness_days", member("staleness_days")},
              {"rule", member("rule")},
              {"code_git_head", std::move(git_head)},
              {"files_read", read_.size()}};
}

} // namespace atx::engine::research::fields

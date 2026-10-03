#include "atx/engine/research/fields/role_axes.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;

constexpr u64 kManifestLimit = 1ULL << 20;
constexpr u64 kAxisLimit = 1ULL << 30;
constexpr std::string_view kRoleSchema = "atx.recent-research-role/v1";

static_assert(std::endian::native == std::endian::little, "role payloads are little-endian");

[[nodiscard]] core::Error invalid(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "research role: " + std::move(message));
}

[[nodiscard]] std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

// A non-negative integer member of `j`, or `fallback` when absent; nullopt when present but not
// one.
[[nodiscard]] std::optional<i64> integer_or(const Json &j, const char *key, i64 fallback) {
  const auto it = j.find(key);
  if (it == j.end()) {
    return fallback;
  }
  if (!it->is_number_integer()) {
    return std::nullopt;
  }
  return it->get<i64>();
}

// The receipts of the manifest's "files" object (entries without an integer bytes and a string
// sha256 are skipped: a builder that needs one gets NotFound from RoleAxes::receipt).
[[nodiscard]] std::vector<std::pair<std::string, FileReceipt>> receipts_of(const Json &files) {
  std::vector<std::pair<std::string, FileReceipt>> out;
  for (auto it = files.begin(); it != files.end(); ++it) {
    const Json &entry = it.value();
    if (!entry.is_object()) {
      continue;
    }
    const auto bytes = entry.find("bytes");
    const auto sha = entry.find("sha256");
    if (bytes == entry.end() || sha == entry.end() || !bytes->is_number_unsigned() ||
        !sha->is_string()) {
      continue;
    }
    out.emplace_back(it.key(), FileReceipt{bytes->get<u64>(), sha->get<std::string>()});
  }
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  return out;
}

// One axis file: its bytes must equal the receipt's size and SHA-256.
[[nodiscard]] core::Result<std::string>
axis_bytes(const std::filesystem::path &directory,
           const std::vector<std::pair<std::string, FileReceipt>> &receipts,
           std::string_view name) {
  const auto it = std::find_if(receipts.begin(), receipts.end(),
                               [&](const auto &r) { return r.first == name; });
  if (it == receipts.end()) {
    return core::Err(invalid("manifest lists no " + std::string(name)));
  }
  ATX_TRY(auto bytes, read_bounded(directory / std::string(name), kAxisLimit));
  ATX_TRY(auto digest, core::sha256_hex(std::string_view(bytes)));
  if (static_cast<u64>(bytes.size()) != it->second.bytes || digest != it->second.sha256) {
    return core::Err(invalid(std::string(name) + " bytes do not match the role manifest"));
  }
  return core::Ok(std::move(bytes));
}

template <class T> [[nodiscard]] std::vector<T> as_vector(const std::string &bytes) {
  std::vector<T> out(bytes.size() / sizeof(T));
  if (!out.empty()) {
    std::memcpy(out.data(), bytes.data(), out.size() * sizeof(T));
  }
  return out;
}

} // namespace

core::Result<RoleAxes> RoleAxes::load(const std::filesystem::path &directory,
                                      std::string_view manifest_sha256) {
  try {
    ATX_TRY(const auto text, read_bounded(directory / "manifest.json", kManifestLimit));
    RoleAxes out;
    out.directory_ = directory;
    ATX_TRY(out.manifest_sha256_, core::sha256_hex(std::string_view(text)));
    if (out.manifest_sha256_ != lower(manifest_sha256)) {
      return core::Err(invalid("manifest SHA-256 does not match the pin"));
    }
    const Json m = Json::parse(text, nullptr, false);
    if (m.is_discarded() || !m.is_object()) {
      return core::Err(invalid("manifest is not a JSON object"));
    }
    if (m.value("schema", std::string{}) != kRoleSchema ||
        m.value("status", std::string{}) != "complete") {
      return core::Err(invalid("not a complete atx.recent-research-role/v1 payload"));
    }
    if (m.value("instrument_namespace", std::string{}) != "spiderrock.securityID") {
      return core::Err(invalid("instrument namespace is not spiderrock.securityID"));
    }
    const auto dates = integer_or(m, "dates", -1);
    const auto names = integer_or(m, "instruments", -1);
    if (!dates || !names || *dates <= 0 || *names <= 0) {
      return core::Err(invalid("dates and instruments must be positive integers"));
    }
    const auto files = m.find("files");
    if (files == m.end() || !files->is_object()) {
      return core::Err(invalid("manifest has no files object"));
    }
    out.receipts_ = receipts_of(*files);
    ATX_TRY(const auto sessions_bytes, axis_bytes(directory, out.receipts_, "sessions.i64"));
    ATX_TRY(const auto ids_bytes, axis_bytes(directory, out.receipts_, "ids.u64"));
    ATX_TRY(const auto member_bytes, axis_bytes(directory, out.receipts_, "member.u8"));
    const auto d = static_cast<usize>(*dates);
    const auto n = static_cast<usize>(*names);
    if (sessions_bytes.size() != d * sizeof(i64) || ids_bytes.size() != n * sizeof(u64) ||
        member_bytes.size() != d * n) {
      return core::Err(invalid("axes disagree with the manifest shape"));
    }
    const auto sessions = as_vector<i64>(sessions_bytes);
    const auto ids = as_vector<u64>(ids_bytes);
    for (usize t = 0; t < d; ++t) {
      if (sessions[t] % kDayNs != 0 || (t > 0 && sessions[t] <= sessions[t - 1])) {
        return core::Err(invalid("sessions are not strictly increasing midnight labels"));
      }
    }
    constexpr u64 kIdLimit = 1ULL << 63;
    for (usize j = 0; j < n; ++j) {
      if (ids[j] == 0 || ids[j] >= kIdLimit || (j > 0 && ids[j] <= ids[j - 1])) {
        return core::Err(invalid("ids are not strictly increasing positive i64 securityIDs"));
      }
    }
    out.days_.reserve(d);
    out.years_.reserve(d);
    for (const i64 session : sessions) {
      out.days_.push_back(session / kDayNs);
      out.years_.push_back(year_of(session / kDayNs));
    }
    if (is_sealed_day(out.days_.back())) {
      return core::Err(core::ErrorCode::InvalidArgument, seal_refusal("role contains a session"));
    }
    out.ids_.reserve(n);
    for (const u64 id : ids) {
      out.ids_.push_back(static_cast<i64>(id));
    }
    out.member_ = as_vector<u8>(member_bytes);
    const auto first_scored = integer_or(m, "score_begin", 0);
    const auto score_stop = integer_or(m, "score_end", static_cast<i64>(d));
    if (!first_scored || !score_stop) {
      return core::Err(invalid("score_begin and score_end must be integers"));
    }
    out.score_begin_ = *first_scored;
    out.score_end_ = *score_stop;
    const auto source = m.find("source_sha256");
    if (source != m.end() && source->is_string()) {
      out.source_sha256_ = source->get<std::string>();
    }
    return core::Ok(std::move(out));
  } catch (const std::exception &e) {
    return core::Err(invalid(e.what()));
  }
}

std::span<const u8> RoleAxes::member_row(usize t) const noexcept {
  const usize n = ids_.size();
  return std::span<const u8>(member_).subspan(t * n, n);
}

std::optional<usize> RoleAxes::column_of(i64 security_id) const noexcept {
  const auto it = std::lower_bound(ids_.begin(), ids_.end(), security_id);
  if (it == ids_.end() || *it != security_id) {
    return std::nullopt;
  }
  return static_cast<usize>(it - ids_.begin());
}

core::Result<FileReceipt> RoleAxes::receipt(std::string_view file) const {
  const auto it =
      std::lower_bound(receipts_.begin(), receipts_.end(), file,
                       [](const auto &r, std::string_view key) { return r.first < key; });
  if (it == receipts_.end() || it->first != file) {
    return core::Err(core::ErrorCode::NotFound,
                     "research role: manifest lists no " + std::string(file));
  }
  return core::Ok(it->second);
}

} // namespace atx::engine::research::fields

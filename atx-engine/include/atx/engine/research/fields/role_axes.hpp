#pragma once

// atx::engine::research::fields -- the pinned research role every field builder aligns to (slice 1
// of docs/plans/2026-10-02-platform-core-migration.md). Mirrors prepare_research_fields.py's Role:
// an atx.recent-research-role/v1 directory read by the SHA-256 of its manifest.json, its session
// and id axes and its decision membership. Nothing is re-projected: the role's own session labels
// and securityID axis are the only axes of every field written on it.
//
// load() refuses (Err) before returning a role:
//   * a manifest whose SHA-256 differs from the pin (compared lower-case), or larger than 1 MiB;
//   * a schema other than atx.recent-research-role/v1, a status other than complete, a namespace
//     other than spiderrock.securityID;
//   * sessions.i64, ids.u64 or member.u8 whose bytes or SHA-256 differ from the manifest's receipt;
//   * an empty axis, axes that disagree with the manifest's dates / instruments, sessions that are
//     not strictly increasing midnight labels, ids that are not strictly increasing positive values
//     below 2^63;
//   * the reader-side seal: a session on or after research_window.hpp's seal.
// score_begin / score_end default to 0 / dates as in the Python (no further contract is imposed on
// them). Cold path; the role is immutable after load and safe to read from several threads.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

// One input of a field as its manifest entry records it: {path, bytes, sha256 (lower-case hex)}.
struct SourceRecord {
  std::string path;
  u64 bytes{};
  std::string sha256;
};

// The role manifest's record of one payload file ("files": {name: {bytes, sha256}}).
struct FileReceipt {
  u64 bytes{};
  std::string sha256;
};

class RoleAxes {
public:
  [[nodiscard]] static core::Result<RoleAxes> load(const std::filesystem::path &directory,
                                                   std::string_view manifest_sha256);

  [[nodiscard]] usize dates() const noexcept { return days_.size(); }
  [[nodiscard]] usize instruments() const noexcept { return ids_.size(); }
  // Session days (calendar days since 1970-01-01), ids (securityID), calendar year of each session.
  [[nodiscard]] std::span<const i64> days() const noexcept { return days_; }
  [[nodiscard]] std::span<const i64> ids() const noexcept { return ids_; }
  [[nodiscard]] std::span<const i64> years() const noexcept { return years_; }
  // member.u8 row t (instruments() values); precondition t < dates().
  [[nodiscard]] std::span<const u8> member_row(usize t) const noexcept;
  [[nodiscard]] i64 score_begin() const noexcept { return score_begin_; }
  [[nodiscard]] i64 score_end() const noexcept { return score_end_; }
  [[nodiscard]] const std::string &manifest_sha256() const noexcept { return manifest_sha256_; }
  [[nodiscard]] const std::optional<std::string> &source_sha256() const noexcept {
    return source_sha256_;
  }
  [[nodiscard]] const std::filesystem::path &directory() const noexcept { return directory_; }
  // The column of `security_id` on the id axis, nullopt when the id is not on it.
  [[nodiscard]] std::optional<usize> column_of(i64 security_id) const noexcept;
  // The manifest's receipt of payload `file`; Err(NotFound) when the manifest lists no such file.
  [[nodiscard]] core::Result<FileReceipt> receipt(std::string_view file) const;

private:
  RoleAxes() = default;

  std::filesystem::path directory_;
  std::string manifest_sha256_;
  std::optional<std::string> source_sha256_;
  std::vector<i64> days_;
  std::vector<i64> ids_;
  std::vector<i64> years_;
  std::vector<u8> member_;
  i64 score_begin_{};
  i64 score_end_{};
  std::vector<std::pair<std::string, FileReceipt>> receipts_; // ascending by name
};

} // namespace atx::engine::research::fields

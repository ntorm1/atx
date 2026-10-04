#pragma once

// atx::engine::research::fields -- a pinned atx-db alpha-panel stage (P9 lane AL-SIG, ruling P18:
// the source half of K-P9-2 for the 13F and Russell-proxy readers; research_fields_holdings.py
// pin_stage / Stage). A stage directory is read by the SHA-256 of its manifest.json, which must be
// a complete manifest of the declared schema carrying the declared rules, and whose "files" object
// pins every file read ({rel: {bytes, sha256}}).
//
// A file is decoded only after its size and SHA-256 (streamed) equal the manifest entry, from the
// same path, and its (size, write time) must not move while it is hashed and decoded; otherwise
// Err and the build stops. Every file read is recorded once (sources(), check()). A stage is read
// by one builder at a time (not thread-safe); it holds no file open between calls.
//
// The visibility rule of every stage source (research_fields_holdings.py VISIBILITY_RULE): a row is
// usable at role session t iff its available_at < date(t-1) 22:00 UTC, t-1 the previous role
// session; session 0 sees nothing. previous_session_marks() gives that mark per session.

#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/io/parquet.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/file_io.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

// An instant (ns since 1970-01-01 UTC) no mark reaches, and one below every mark.
inline constexpr i64 kNeverNs = std::numeric_limits<i64>::max();
inline constexpr i64 kBeforeAllNs = std::numeric_limits<i64>::min();
// The session mark: date 22:00 UTC (the role close clock).
inline constexpr i64 kSessionMarkNs = 22LL * 3'600'000'000'000LL;

inline constexpr std::string_view kStageVisibilityRule =
    "a source row is usable at role session t iff available_at < date(t-1) 22:00 UTC, t-1 = the "
    "previous role session (v7 data request section 0.1); session 0 -> NaN";

// Per session t of `days` (calendar days, ascending): the mark of session t-1 (date 22:00 UTC, ns);
// kBeforeAllNs for session 0.
[[nodiscard]] std::vector<i64> previous_session_marks(std::span<const i64> days);

// "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ" of an instant (ns since 1970-01-01 UTC).
[[nodiscard]] std::string iso_instant(i64 ns);

// The declared contract of one stage: its key (named in refusals), its schema and the rule keys
// its manifest must carry. An empty clock_rule or rule and an absent staleness_days are not
// checked. The manifest's rule must equal `rule` or be a list holding it (a stage built under
// several rules). The views name static text (the readers' inline constexpr contracts).
struct StageContract {
  std::string_view key;
  std::string_view schema;
  std::string_view clock_rule;
  std::optional<i64> staleness_days;
  std::string_view rule;
};

class PinnedStage {
public:
  // Err(InvalidArgument) for a manifest whose SHA-256 is not the request's pin (compared
  // lower-case), larger than 64 MiB, not a complete manifest of the contract's schema, carrying a
  // declared rule other than the contract's, or listing no files object; Err(IoError) when it
  // cannot be read.
  [[nodiscard]] static core::Result<PinnedStage> open(const StageRequest &request,
                                                      const StageContract &contract);

  // True when the manifest lists `rel` (a '/'-separated path below the stage directory).
  [[nodiscard]] bool lists(std::string_view rel) const;

  // The verified file decoded with `columns` only (every name must exist in the file).
  [[nodiscard]] core::Result<core::io::ParquetTable>
  table(std::string_view rel, std::span<const std::string_view> columns);

  // The verified file decoded with `columns` one row group at a time: `visit` sees each group in
  // file order (its Err stops the read and is returned). Bounded memory for the large holdings
  // parts.
  using GroupVisitor = std::function<core::Status(const core::io::ParquetTable &)>;
  [[nodiscard]] core::Status for_each_group(std::string_view rel,
                                            std::span<const std::string_view> columns,
                                            const GroupVisitor &visit);

  [[nodiscard]] const SourceRecord &manifest_source() const noexcept { return manifest_source_; }
  [[nodiscard]] const nlohmann::json &manifest() const noexcept { return manifest_; }
  [[nodiscard]] const StageContract &contract() const noexcept { return contract_; }
  // The manifest's record, then every file read, ascending by path.
  [[nodiscard]] std::vector<SourceRecord> sources() const;
  // {manifest_sha256, schema, clock_rule, staleness_days, rule, code_git_head, files_read} (the
  // Python Stage.check() block).
  [[nodiscard]] nlohmann::json check() const;

private:
  PinnedStage() = default;
  // The path of `rel` once its bytes equal the manifest entry, and its stamp at that time.
  [[nodiscard]] core::Result<std::filesystem::path> verified(std::string_view rel,
                                                             FileStamp &stamp);

  std::filesystem::path dir_;
  StageContract contract_{};
  nlohmann::json manifest_;
  SourceRecord manifest_source_;
  std::vector<SourceRecord> read_;
};

} // namespace atx::engine::research::fields

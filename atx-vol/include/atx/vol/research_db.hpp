#pragma once

// Durable companion catalog for quantitative-strategy research artifacts.
//
// ResearchDb deliberately lives beside, rather than inside, BacktestDb v1:
//
//   <backtest-root>/
//     manifest.atxbtdb
//     partitions/
//     research/
//       manifest.atxqrdb
//       objects/<kind>-<sha256>.atxrun
//
// This preserves the exact BacktestDb v1 manifest contract. Research objects
// are immutable, content-addressed RunArchive files; a writer publishes and
// validates an object before atomically advancing the research manifest. An
// expected-head compare-and-swap prevents a stale research worker from moving a
// logical head. Across processes the store is single-publisher/many-reader;
// independent workers may compute payloads, but one publisher serializes the
// manifest updates. Readers call refresh() to observe a newer generation.
//
// This module only persists research and implementation intent. In particular,
// an ExecutionIntent is inert data and this API has no order transport.

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/research/run_archive.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

inline constexpr std::string_view kResearchDbDirectory = "research";
inline constexpr std::string_view kResearchDbManifestName = "manifest.atxqrdb";
inline constexpr std::string_view kResearchDbObjectDirectory = "objects";
inline constexpr std::string_view kResearchDbObjectExtension = ".atxrun";
inline constexpr std::uint64_t kResearchDbSchemaSalt =
    0x4154585152440001ULL; // "ATXQRD", companion-store revision 1

enum class ResearchArtifactKind : std::uint8_t {
  StrategyDefinition = 0,
  SignalSegment = 1,
  Candidate = 2,
  Trial = 3,
  RiskScenarioSegment = 4,
  ExecutionIntent = 5,
};

enum class ResearchValueKind : std::uint8_t {
  F64 = 0,
  I64 = 1,
  U32 = 2,
  Text = 3,
};

// One canonical strategy/candidate parameter. Exactly the field selected by
// kind may be nonzero/nonempty; publish rejects ambiguous union states. Scope
// and key form the unique canonical ordering key.
struct ResearchParameter {
  std::string scope;
  std::string key;
  ResearchValueKind kind{ResearchValueKind::F64};
  double f64_value{0.0};
  std::int64_t i64_value{0};
  std::uint32_t u32_value{0};
  std::string text_value;

  [[nodiscard]] static ResearchParameter f64(std::string scope, std::string key, double value);
  [[nodiscard]] static ResearchParameter i64(std::string scope, std::string key,
                                             std::int64_t value);
  [[nodiscard]] static ResearchParameter u32(std::string scope, std::string key,
                                             std::uint32_t value);
  [[nodiscard]] static ResearchParameter text(std::string scope, std::string key,
                                              std::string value);

  [[nodiscard]] bool operator==(const ResearchParameter &) const = default;
};

// A dependency is either another durable semantic object (dependency_id) or an
// external archive/source attestation (archive_identity), and may carry both.
// date_lo/date_hi are inclusive ISO dates when present. The tuple
// (role, kind, key, dependency_id, date bounds) must be unique within an object.
struct ResearchDependency {
  std::uint32_t role{0};
  std::uint32_t kind{0};
  std::string key;
  std::string dependency_id;
  std::string date_lo;
  std::string date_hi;
  ArchiveContentIdentity archive_identity{};

  [[nodiscard]] bool operator==(const ResearchDependency &) const = default;
};

struct ResearchArtifactInfo {
  std::string artifact_id; // lowercase SHA-256, exactly 64 hexadecimal bytes
  ResearchArtifactKind kind{ResearchArtifactKind::StrategyDefinition};
  std::string logical_id;
  std::uint32_t revision{0};
  std::string parent_artifact_id;
  std::string specification_sha256;
  std::string dependency_sha256;
  std::int64_t first_ts_ns{0};
  std::int64_t last_ts_ns{0};
  std::uint64_t row_count{0};
  std::string filename;
  ArchiveContentIdentity archive_identity{};

  [[nodiscard]] bool operator==(const ResearchArtifactInfo &) const = default;
};

struct ResearchHead {
  ResearchArtifactKind kind{ResearchArtifactKind::StrategyDefinition};
  std::string logical_id;
  std::string artifact_id;
  std::uint32_t revision{0};

  [[nodiscard]] bool operator==(const ResearchHead &) const = default;
};

// Value-semantic publish description. RaSectionData is non-owning: every
// column span must remain valid until publish()/research_artifact_identity()
// returns. `expected_head_id == ""` means "this logical head must not exist";
// an update supplies the exact current head ID. The expected head becomes the
// immutable object's parent. Replaying an identical already-published request
// is an idempotent no-op.
//
// `sections` contains typed RunArchive sections, never opaque byte blobs.
// artifact_meta, parameters and dependencies are reserved and synthesized by
// the store. Caller section and column order does not affect identity or bytes:
// both are canonicalized by name before publication. All F64 payload values
// must be finite; signed zero is normalized to positive zero in parameters and
// payloads before hashing and persistence.
struct ResearchPublishRequest {
  ResearchArtifactKind kind{ResearchArtifactKind::StrategyDefinition};
  std::string logical_id;
  std::string expected_head_id;
  std::uint32_t payload_format_version{1};
  std::uint64_t payload_schema_salt{0};
  std::int64_t first_ts_ns{0};
  std::int64_t last_ts_ns{0};
  std::uint64_t row_count{0};
  std::vector<ResearchParameter> parameters;
  std::vector<ResearchDependency> dependencies;
  std::vector<RaSectionData> sections;
};

// A mapped section that co-owns the RunArchive backing its zero-copy view.
struct MappedResearchSection {
  std::shared_ptr<const RunArchive> archive;
  RaSectionView view;

  [[nodiscard]] const RaSectionView *operator->() const noexcept { return &view; }
  [[nodiscard]] const RaSectionView &operator*() const noexcept { return view; }
};

// Canonical SHA-256 over the request's semantic fields, sorted parameters,
// sorted dependencies, and name-canonicalized typed section contents.
// expected_head_id participates as the immutable parent. Wall-clock time and
// display/catalog ordering and the sign bit of zero do not. Returns
// InvalidArgument for any request the store would refuse, including non-finite
// F64 parameters or payload values.
[[nodiscard]] Result<std::string> research_artifact_identity(const ResearchPublishRequest &request);

// Validate a signal_values section and return the row ordinals observable at
// decision_ts_ns. Required columns are event_ts_ns(I64),
// available_ts_ns(I64), uid(U32), symbol(DictStr), value(F64), status(U32).
// Rows must be sorted by (event_ts_ns, uid), unique, and available_ts>=event_ts.
[[nodiscard]] Result<std::vector<std::uint64_t>>
research_signal_rows_available_as_of(const RaSectionView &signal_values,
                                     std::int64_t decision_ts_ns);

class ResearchDb {
public:
  // Create <backtest_root>/research and its objects directory, then publish an
  // empty generation-1 manifest. AlreadyExists when that manifest exists.
  [[nodiscard]] static Result<ResearchDb> create(std::string_view backtest_root);

  // Open <backtest_root>/research/manifest.atxqrdb and fully validate its
  // framing, CRCs, schema, ordering, heads, and artifact metadata.
  [[nodiscard]] static Result<ResearchDb> open(std::string_view backtest_root);

  ResearchDb(ResearchDb &&) noexcept = default;
  ResearchDb &operator=(ResearchDb &&) noexcept = default;
  ResearchDb(const ResearchDb &) = delete;
  ResearchDb &operator=(const ResearchDb &) = delete;

  [[nodiscard]] const std::string &backtest_root() const noexcept { return backtest_root_; }
  [[nodiscard]] const std::string &root() const noexcept { return research_root_; }
  [[nodiscard]] std::uint64_t generation() const;

  // Re-read the manifest and adopt a strictly newer generation. A rollback is
  // rejected fail-closed.
  [[nodiscard]] Status refresh();

  [[nodiscard]] std::vector<ResearchArtifactInfo> artifacts() const;
  [[nodiscard]] std::vector<ResearchHead> heads() const;
  [[nodiscard]] Result<ResearchArtifactInfo> find_artifact(std::string_view artifact_id) const;
  [[nodiscard]] Result<ResearchHead> find_head(ResearchArtifactKind kind,
                                               std::string_view logical_id) const;

  // Publish one immutable content-addressed object, validate it, then
  // atomically move the logical head in generation+1 of the manifest.
  [[nodiscard]] Result<ResearchArtifactInfo> publish(const ResearchPublishRequest &request);

  // Open and fully CRC-validate the indexed object, validate its artifact_meta
  // envelope against the manifest, then return a zero-copy mapped section.
  [[nodiscard]] Result<MappedResearchSection> map_section(std::string_view artifact_id,
                                                          std::string_view section_name) const;

  // Decode the indexed object's canonical dependency table.
  [[nodiscard]] Result<std::vector<ResearchDependency>>
  load_dependencies(std::string_view artifact_id) const;

private:
  struct Snapshot;

  ResearchDb() = default;
  [[nodiscard]] static Result<std::shared_ptr<const Snapshot>>
  read_manifest(const std::string &path);
  [[nodiscard]] Status persist_locked(std::vector<ResearchArtifactInfo> artifacts,
                                      std::vector<ResearchHead> heads);
  [[nodiscard]] std::string manifest_path() const;
  [[nodiscard]] std::string object_path(std::string_view filename) const;

  std::string backtest_root_;
  std::string research_root_;
  mutable std::unique_ptr<std::mutex> mu_;
  std::shared_ptr<const Snapshot> snapshot_;
};

} // namespace atx::vol

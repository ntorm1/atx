#pragma once

// atx::engine::research::fields -- the reuse decision of a registry build (migration slice 3, keyed
// on the producer identity of contract K-P9-3): a payload of a prior fields directory is copied
// instead of rebuilt when its entry proves the same producer and the same inputs.
//
// The prior DIR/manifest.json is read once and refused (Err: the build stops) unless it is a
// complete atx.research-role-fields/v1 manifest, matches the request's manifest_sha256 pin when one
// is given, is bound to this build's role (manifest, sessions, ids and member SHA-256) and carries
// this build's research seal. A present seal block whose exclusive_end is not research_window.hpp's
// seal is refused; an absent seal block is recorded ("seal": "absent") and does not refuse (ruling
// P13: refusing an absent seal waits for wave 2).
//
// Per planned field, in plan order, the first failing rule recomputes it and records the reason:
//   1. the prior manifest has an entry and a files pin for <name>.f64;
//   2. the entry's layout is this role's: file <name>.f64, dtype <f8, date-major, shape
//      [dates, instruments], sha256 equal to the pin, pin bytes = dates * instruments * 8;
//   3. its producer is an engine producer whose {exe_sha256, git_sha, build_type} equals this
//      executable's, and this executable's exe_sha256 is known. A legacy Python producer (a block
//      without "kind") is never reused here: the Python builder's own reuse owns those entries;
//   4. its formula_sha256 equals the plan's spec fingerprint;
//   5. its sources equal, in order and by (bytes, sha256), the plan's input files re-hashed now.
// A field that passes is copied to output_dir/<name>.f64 (created exclusively); a copy that does
// not hash to the pin is Err (a corrupt prior directory stops the build). Its manifest entry is the
// prior entry unchanged, so its producer still names the build that wrote the payload, plus
//   "reused_from": {"dir", "manifest_sha256", "payload_sha256", "mode": "copy"}.
// No ported kind requires another field, so no dependency closure is applied; a kind whose field
// requires another must add one here.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/research/fields/build_spec.hpp"
#include "atx/engine/research/fields/file_io.hpp"
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

inline constexpr std::string_view kSealMatch = "match";
inline constexpr std::string_view kSealAbsent = "absent";

struct ReusedField {
  std::string name;
  nlohmann::json entry;         // the manifest entry: the prior entry plus reused_from
  nlohmann::json source_checks; // the prior manifest's source_checks[name]; null when absent
  FileDigest payload;           // the copied payload's size and SHA-256
};

struct ReuseOutcome {
  std::vector<ReusedField> reused; // plan order
  // The manifest's "reuse" block: {dir, manifest_sha256, seal, reused[], recomputed{name: reason}}.
  nlohmann::json record;

  [[nodiscard]] const ReusedField *find(std::string_view name) const noexcept;
};

// Decides reuse for `plans` against `request` and copies every reused payload into `output_dir`.
// Err(InvalidArgument) for a refused prior manifest or a corrupt prior payload, Err(IoError) when a
// prior file cannot be read or a copy cannot be written.
[[nodiscard]] core::Result<ReuseOutcome> reuse_payloads(const ReuseRequest &request,
                                                        const RoleAxes &role,
                                                        std::span<const FieldPlan> plans,
                                                        const ProducerIdentity &producer,
                                                        const std::filesystem::path &output_dir);

} // namespace atx::engine::research::fields

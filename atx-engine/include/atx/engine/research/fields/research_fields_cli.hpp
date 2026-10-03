#pragma once

// atx::engine::research::fields -- the `atx-research-fields` executable's verb (platform core
// migration slice 3): a JSON spec in, field payloads plus one JSON receipt out.
//
//   atx-research-fields build --spec SPEC.json --receipt RECEIPT.json
//
// SPEC (atx.research-fields-spec/v1):
//   {"schema", "role": {"dir", "manifest_sha256"}, "output_dir", "fields": [name, ...],
//    "finra": dir (required with si_shares / si_dtc)}
// Every field is written to output_dir/<name>.f64, created exclusively (an existing file is never
// replaced). After the last field closes, RECEIPT (atx.research-fields-receipt/v1) is created
// exclusively: one entry per field with exactly the blocks the Python builder records for it --
// file, bytes, sha256, formula_sha256, sources, coverage (member quantiles included; for the FINRA
// fields the vintage_risk block), source_checks and extra (FINRA) -- plus the engine identity, the
// spec's SHA-256 and the research window. A run that fails leaves no receipt: the receipt is the
// publish-last marker of a complete build. prepare_research_fields.py --engine-fields reads it and
// writes the manifest entries from it, byte for byte the Python path's.
//
// Exit codes: 0 built; 2 usage or spec error; 1 a build error (message on `err`).

#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::engine::research::fields {

inline constexpr std::string_view kSpecSchema = "atx.research-fields-spec/v1";
inline constexpr std::string_view kReceiptSchema = "atx.research-fields-receipt/v1";

struct BuildSpec {
  std::filesystem::path role_dir;
  std::string role_manifest_sha256;
  std::filesystem::path output_dir;
  std::vector<std::string> fields; // distinct names the engine builds, in build order
  std::optional<std::filesystem::path> finra;
};

// The field names this engine builds.
[[nodiscard]] std::span<const std::string_view> engine_field_names() noexcept;

// Err(InvalidArgument) on a malformed spec, an unknown or repeated field, or a missing input.
[[nodiscard]] core::Result<BuildSpec> parse_build_spec(std::string_view json_text);

// Builds every field of `spec` and returns the receipt's JSON text (not yet written).
[[nodiscard]] core::Result<std::string> build_fields(const BuildSpec &spec,
                                                     std::string_view spec_sha256);

// The verb: argv as main() receives it.
[[nodiscard]] int research_fields_main(std::span<const std::string_view> args, std::ostream &out,
                                       std::ostream &err);

} // namespace atx::engine::research::fields

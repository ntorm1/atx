#pragma once

// atx::engine::research::fields -- the publish-last manifest of a registry build (migration slice
// 3; contract K-P9-3): an atx.research-role-fields/v1 document over output_dir in the layout the
// Python builder writes and the fields consumers read (role binding, seal, entries, files pins,
// source_checks), carrying the engine's identity instead of the Python builder's code identity.
//
// Entry of a field this run built, from its receipt entry and its kind's spec: name, file, dtype
// "<f8", layout "date-major", shape, units, clock, staleness, source_columns, caveats,
// point_in_time, non_pit_aspects, definition / formula_id / min_history when the spec declares
// them, formula_sha256, sources, coverage, sha256, the kind's extra keys (FINRA: vintage_safe_from)
// and
//   "producer": {"kind": "engine", "exe_sha256", "git_sha", "build_type", "receipt_sha256"}
// A reused field's entry is the prior entry with reused_from (reuse.hpp). Entries follow the plan
// order (registration order). Top level: schema, status, role, instrument_namespace, seal,
// cell_rule, coverage_basis, visibility_mark, non_point_in_time_fields, fields, files,
// source_checks, engine {name, exe_sha256, git_sha, build_type}, receipt_sha256, spec_sha256,
// registry {path, sha256}, research_window, historical_vintage_verified, common_stock_verified, and
// reuse when the spec asked for reuse. The text is JSON with sorted keys, a two-space indent, ASCII
// escapes and a final newline. It is not claimed equal to a Python manifest (migration plan §3.2):
// what the engine claims is its payloads and their coverage.
//
// research_fields_main writes the receipt first and publishes this manifest last
// (file_io.hpp publish_exclusive): its presence marks a complete registry build.

#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/research/fields/field_registry.hpp"
#include "atx/engine/research/fields/producer.hpp"
#include "atx/engine/research/fields/registry.hpp"
#include "atx/engine/research/fields/reuse.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

inline constexpr std::string_view kFieldsManifestSchema = "atx.research-role-fields/v1";
inline constexpr std::string_view kEngineName = "atx-research-fields";

struct ManifestInputs {
  const RoleAxes &role;
  std::span<const FieldPlan> plans; // manifest order
  const nlohmann::json &built;      // receipt entries of the fields this run built
  const ReuseOutcome *reuse;        // nullptr when the spec asked for no reuse
  const ProducerIdentity &producer;
  std::string_view receipt_sha256;
  std::string_view spec_sha256;
  const FieldRegistry &registry;
};

// Err(InvalidArgument) when a plan has neither a receipt entry nor a reused entry.
[[nodiscard]] core::Result<nlohmann::json> engine_manifest(const ManifestInputs &in);

// The manifest's bytes (see above).
[[nodiscard]] core::Result<std::string> manifest_text(const nlohmann::json &manifest);

} // namespace atx::engine::research::fields

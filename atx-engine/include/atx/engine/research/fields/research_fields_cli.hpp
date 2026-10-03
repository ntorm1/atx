#pragma once

// atx::engine::research::fields -- the `atx-research-fields` executable's verb (platform core
// migration slice 3): a JSON spec in, field payloads plus one JSON receipt out, and with a field
// registry also a publish-last fields manifest.
//
//   atx-research-fields build --spec SPEC.json --receipt RECEIPT.json [--registry REGISTRY.json]
//
// Without --registry (the engine path of prepare_research_fields_engine.py; unchanged contract):
// SPEC is atx.research-fields-spec/v1
//   {"schema", "role": {"dir", "manifest_sha256"}, "output_dir", "fields": [name, ...],
//    "finra": dir (required with si_shares / si_dtc)}
// and each field name names its builder kind (registry.hpp). Every field is written to
// output_dir/<name>.f64, created exclusively (an existing file is never replaced). After the last
// field closes, RECEIPT (atx.research-fields-receipt/v1) is created exclusively: one entry per
// field with exactly the blocks the Python builder records for it -- file, bytes, sha256,
// formula_sha256, sources, coverage (member quantiles included; for the FINRA fields the
// vintage_risk block), source_checks (sealed rows counted under rows_sealed_dropped) and extra
// (FINRA) -- plus the engine identity {name, fields, exe_sha256, git_sha, build_type} (K-P9-3), the
// spec's SHA-256 and the research window. A run that fails leaves no receipt: here the receipt is the publish-last
// marker of a complete build. prepare_research_fields_engine.py reads it, writes the manifest
// entries from it and stamps each engine entry's producer block from its engine identity.
//
// With --registry (contract K-P9-1, schema atx.field-registry/v1): SPEC is
// atx.research-fields-spec/v2, the v1 keys plus an optional "reuse": {"dir", "manifest_sha256"?};
// every field must be a registry row of kind engine (registry.hpp plan_fields) and the build runs
// in registration order. Reusable payloads of the prior directory are copied first (reuse.hpp), the
// rest are built, the receipt (as above, plus "registry" {path, sha256} and "reused" [names]) is
// created exclusively, and output_dir/manifest.json is published last (manifest.hpp) with the
// producer block of K-P9-3 in every entry this run built.
//
// Exit codes: 0 built; 2 usage, registry or spec error; 1 a build error (message on `err`).

#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/build_spec.hpp"

namespace atx::engine::research::fields {

struct FieldRegistry;
struct ProducerIdentity;

inline constexpr std::string_view kSpecSchema = "atx.research-fields-spec/v1";
inline constexpr std::string_view kRegistrySpecSchema = "atx.research-fields-spec/v2";
inline constexpr std::string_view kReceiptSchema = "atx.research-fields-receipt/v1";

// The field names this engine builds without a registry (the builder kind ids).
[[nodiscard]] std::span<const std::string_view> engine_field_names() noexcept;

// Without a registry: an atx.research-fields-spec/v1 spec whose names name builder kinds. With
// one: an atx.research-fields-spec/v2 spec whose names are engine rows of `*registry`.
// Err(InvalidArgument) on a malformed spec, an unknown or repeated field, a refused registry row or
// a missing input.
[[nodiscard]] core::Result<BuildSpec> parse_build_spec(std::string_view json_text,
                                                       const FieldRegistry *registry = nullptr);

// Builds every field of a v1 `spec` and returns the receipt's JSON text (not yet written).
[[nodiscard]] core::Result<std::string> build_fields(const BuildSpec &spec,
                                                     std::string_view spec_sha256,
                                                     const ProducerIdentity &producer);

// A registry build, not yet written: the receipt text and the manifest text whose engine entries
// carry the receipt text's SHA-256.
struct RegistryBuild {
  std::string receipt;
  std::string manifest;
  usize built{};
  usize reused{};
};

// Reuses, then builds, every field of a v2 `spec` through `registry`.
[[nodiscard]] core::Result<RegistryBuild> build_registry_fields(const BuildSpec &spec,
                                                                const FieldRegistry &registry,
                                                                std::string_view spec_sha256,
                                                                const ProducerIdentity &producer);

// Creates the receipt exclusively, then publishes output_dir/manifest.json last.
[[nodiscard]] core::Status write_registry_build(const RegistryBuild &build,
                                                const std::filesystem::path &receipt_path,
                                                const std::filesystem::path &output_dir);

// The verb: argv as main() receives it.
[[nodiscard]] int research_fields_main(std::span<const std::string_view> args, std::ostream &out,
                                       std::ostream &err);

} // namespace atx::engine::research::fields

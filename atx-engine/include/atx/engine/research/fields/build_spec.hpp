#pragma once

// atx::engine::research::fields -- one build request of the `atx-research-fields` verb as its spec
// declares it (research_fields_cli.hpp): the pinned role, the output directory, the fields in build
// order, the input roots the builder kinds read (registry.hpp) and, for a registry build, the prior
// fields directory the build may reuse payloads from (reuse.hpp).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace atx::engine::research::fields {

// A prior fields directory (DIR/manifest.json); manifest_sha256, when given, pins its manifest.
struct ReuseRequest {
  std::filesystem::path dir;
  std::optional<std::string> manifest_sha256;
};

struct BuildSpec {
  std::filesystem::path role_dir;
  std::string role_manifest_sha256;
  std::filesystem::path output_dir;
  std::vector<std::string> fields; // distinct names the engine builds, in build order
  std::optional<std::filesystem::path> finra;
  std::optional<ReuseRequest> reuse; // registry builds only (atx.research-fields-spec/v2)
};

} // namespace atx::engine::research::fields

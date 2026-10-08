#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "atx/server/paper.hpp"

namespace atx::server {

/// Operator-registered immutable TRAIN dataset; clients submit its ID and SHA, never paths.
struct ResearchDataset {
  std::string id;
  std::string name;
  std::filesystem::path manifest_path;
  std::string manifest_sha256;
};

struct ResearchOptions {
  std::filesystem::path repo_root;
  std::vector<std::filesystem::path> artifact_roots;
  std::size_t max_input_bytes{32U * 1024U * 1024U};
  std::size_t max_result_points{2000};
  std::size_t max_csv_rows{100'000};
  std::filesystem::path output_root{};
  std::uint64_t max_working_bytes{512ULL * 1024ULL * 1024ULL};
  std::vector<ResearchDataset> datasets{};
  std::string runtime_build_sha256{};
};

/// Immutable research adapter. Concurrent executions own all intermediate state.
/// Imported evidence must remain immutable during a read; configure trusted artifact roots.
class ResearchBackend {
public:
  /// Empty artifact roots still permit synthetic and separately registered dataset execution.
  /// @throws std::invalid_argument for invalid limits or non-directory artifact roots.
  explicit ResearchBackend(ResearchOptions options);

  /// @return A JSON object containing supported strategies and their evidence modes.
  [[nodiscard]] std::string strategies_json() const;

  /// Compile and type-check an expression against a pinned dataset without reading market rows.
  /// Empty dataset ID selects demonstration fields; a registered ID requires its exact SHA.
  /// @throws std::invalid_argument for invalid syntax, fields, limits or stale dataset revisions.
  [[nodiscard]] std::string validate_expression_json(std::string_view strategy_dsl,
      std::string_view dataset_id = {}, std::string_view dataset_sha256 = {}) const;

  /// Executes import_nav, synthetic_demo, real_nav, real_dsl or real_portfolio.
  /// @param progress Optional callback, invoked without shared locks; exceptions propagate.
  /// @return Canonical JSON, with unavailable evidence represented by null or empty arrays.
  /// @throws std::invalid_argument for malformed requests, forbidden paths or invalid evidence.
  /// @throws std::runtime_error for cancellation, I/O failure or engine failure.
  [[nodiscard]] std::string execute(
      std::string_view request_json, std::stop_token stop,
      const std::function<void(double, std::string_view)>& progress) const;

  /// Pure historical TRAIN simulation of a new, bounded window after approved evidence.
  /// Returns complete owned ledger inputs or throws without publishing partial state.
  [[nodiscard]] PaperReplayResult replay_paper(const PaperReplayInput& input,
      std::stop_token stop,
      const std::function<void(double, std::string_view)>& progress) const;

private:
  ResearchOptions options_;
};

} // namespace atx::server

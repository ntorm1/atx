#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <span>
#include <string>
#include <vector>

#include "atx/engine/alpha/panel.hpp"
#include "research_detail.hpp"

namespace atx::engine::book { struct ReplayResult; }

namespace atx::server::research_detail {

struct DatasetRecipe {
  std::string dsl;
  std::chrono::sys_days start, end;
  std::size_t instrument_limit{}, lookback{}, cadence{}, delay{};
  double capital{}, trade_bps{}, borrow_bps{};
};

struct DatasetPanel {
  atx::engine::alpha::Panel panel;
  std::vector<std::int64_t> sessions;
  std::vector<std::uint64_t> instruments;
  std::size_t score_begin{};
  Json provenance;
};

enum class DatasetAdmissionProfile { Research, Paper };

/// Validate trusted registrations and freeze canonical locations. Does not read market rows.
void validate_datasets(ResearchOptions& options);
[[nodiscard]] Json dataset_catalog(const ResearchOptions& options);
[[nodiscard]] std::vector<std::string> dataset_fields();
[[nodiscard]] DatasetRecipe make_dataset_recipe(const Json& request);
[[nodiscard]] atx::engine::alpha::Panel make_dataset_score_panel(const DatasetPanel& data);
void append_dataset_accounting(Evidence& evidence,
    const atx::engine::book::ReplayResult& replay, const atx::engine::alpha::Panel& panel,
    std::span<const std::int64_t> sessions, const DatasetPanel& data,
    const ResearchOptions& options, std::stop_token stop);

/// Reads only pinned monthly TRAIN files beneath the registered manifest directory.
/// Owns the materialized columns and axes; all SQL and filesystem paths are server generated.
[[nodiscard]] DatasetPanel load_dataset(const ResearchDataset& dataset,
    const DatasetRecipe& recipe, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress,
    DatasetAdmissionProfile profile = DatasetAdmissionProfile::Research);

/// Returns no partial evidence on failure/cancellation. Research costs and missing-mark
/// liquidations remain explicitly declared assumptions, with actual accounting diagnostics.
[[nodiscard]] Evidence replay_dataset(const DatasetPanel& data, const DatasetRecipe& recipe,
    const atx::engine::alpha::SignalSet& signal, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress);

[[nodiscard]] Evidence real_dsl(const Json& request, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress);
} // namespace atx::server::research_detail

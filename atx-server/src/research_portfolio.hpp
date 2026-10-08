#pragma once

#include "research_dataset.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/book/replay.hpp"

namespace atx::server::research_detail {

struct PortfolioComponent {
  std::string expression, effective_expression, sha;
  double weight{};
  atx::engine::alpha::Program program;
  std::uint64_t work_per_cell{};
};

struct PortfolioRecipe {
  DatasetRecipe data;
  std::vector<PortfolioComponent> components;
  double gross{}, name_cap{}, turnover{};
  std::size_t decay{}, max_slots{};
};

struct PortfolioSizingSnapshot {
  std::vector<double> old_units, new_units;
};

/// Native full accounting and exact sizing snapshots; paper callers retain all rows.
struct PortfolioReplayOutput {
  atx::engine::book::ReplayPolicyResult replay;
  Json diagnostics;
  std::vector<PortfolioSizingSnapshot> sizing;
  Json provenance;
};

[[nodiscard]] PortfolioRecipe make_portfolio_recipe(const Json& request);
[[nodiscard]] Json admit_portfolio(const DatasetPanel& data, const PortfolioRecipe& recipe,
    const ResearchOptions& options);
[[nodiscard]] PortfolioReplayOutput replay_portfolio_owned(const DatasetPanel& data,
    const PortfolioRecipe& recipe, const atx::engine::alpha::SignalSet& signal,
    const ResearchOptions& options, std::stop_token stop, const Progress& progress,
    bool retain_sizing);

[[nodiscard]] atx::engine::alpha::SignalSet blend_portfolio_signals(
    const DatasetPanel& data, const PortfolioRecipe& recipe, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress);
[[nodiscard]] Evidence replay_portfolio(const DatasetPanel& data, const PortfolioRecipe& recipe,
    const atx::engine::alpha::SignalSet& signal, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress);

/// Versioned manual expression blend, constrained at actual delayed allocation time.
/// This is bounded signal projection with identity geometry, not calibrated alpha/risk.
[[nodiscard]] Evidence real_portfolio(const Json& request, const ResearchOptions& options,
    std::stop_token stop, const Progress& progress);

} // namespace atx::server::research_detail

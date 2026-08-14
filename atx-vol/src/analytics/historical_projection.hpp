#pragma once

// Corpus-scale projection of relative option positions onto historical surface
// snapshots. Each scenario materializes its own absolute strike/expiry contract;
// this is relative-template historical risk, not repricing one fixed listed contract.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/analytics/contract_projection.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

struct RelativeOptionPosition {
  OptionProjectionSpec option{};
  double quantity{0.0};
  [[nodiscard]] bool operator==(const RelativeOptionPosition &) const = default;
};

struct HistoricalProjectionScenario {
  std::int64_t ts_ns{0};
  const SurfaceSet *surfaces{nullptr};
};

struct HistoricalProjectionFrame {
  std::int64_t ts_ns{0};
  double value{0.0};
  double delta{0.0};
  double gamma{0.0};
  double vega{0.0};
  double theta{0.0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint64_t definition_fingerprint{0};
  [[nodiscard]] bool operator==(const HistoricalProjectionFrame &) const = default;
};

struct HistoricalProjectionConfig {
  bool analytic_greeks{true};
  double delta_tolerance{1.0e-7};
  unsigned n_threads{0};
};

struct ProjectedHistoricalVar {
  double confidence{0.0};
  double reference_value{0.0};
  double value_at_risk{0.0};
  double expected_shortfall{0.0};
  std::size_t n_scenarios{0};
};

class PreparedHistoricalProjection {
public:
  [[nodiscard]] static Result<PreparedHistoricalProjection>
  create(std::span<const RelativeOptionPosition> positions);

  [[nodiscard]] std::size_t n_positions() const noexcept { return positions_.size(); }
  [[nodiscard]] std::uint64_t fingerprint() const noexcept { return fingerprint_; }

  // `leg_output` is scenario-major and must contain scenarios.size()*n_positions()
  // slots. The evaluator allocates no result storage; a parallel call may create
  // its bounded worker vector. `execution` routes every mark/Greek/delta-solve
  // residual this call makes; Configured (default) preserves this engine's
  // historical behavior of inheriting each surface's own prepared query tier.
  // VaR-path callers (dispersion_book_var and friends) pass ColdReference
  // explicitly, matching VarEvaluationConfig's cold-by-default marks.
  [[nodiscard]] Status evaluate_into(std::span<const HistoricalProjectionScenario> scenarios,
                                     std::span<HistoricalProjectionFrame> frames,
                                     std::span<ProjectedOption> leg_output,
                                     const HistoricalProjectionConfig &config = {},
                                     QueryExecution execution = QueryExecution::Configured) const;

private:
  std::vector<RelativeOptionPosition> positions_{};
  PreparedOptionProjection projection_{};
  std::uint64_t fingerprint_{0};
};

// Deterministic nearest-rank loss quantile over successful scenario frames.
// Loss = reference_value - scenario.value; expected shortfall averages the
// inclusive tail beginning at the VaR rank.
[[nodiscard]] Result<ProjectedHistoricalVar>
projected_historical_var(std::span<const HistoricalProjectionFrame> frames, double reference_value,
                         double confidence);

} // namespace atx::vol

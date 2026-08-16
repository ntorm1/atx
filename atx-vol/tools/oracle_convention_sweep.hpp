#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "oracle_cohort_reader.hpp"
#include "oracle_conventions.hpp"

namespace atx::vol::oracle {

struct FloorMetric {
  std::string metric_id;
  double value = 0.0;
  std::int64_t count = 0;
  std::string unit;
};

struct CandidatePriceMetric {
  std::string candidate_id;
  double smoke_price_mae_ticks = 0.0;
  std::int64_t smoke_count = 0;
  double tune_sample_price_mae_ticks = 0.0;
  std::int64_t tune_sample_count = 0;
};

struct ConventionSweepResult {
  ConventionMap winner;
  std::vector<FloorMetric> metrics;
  std::vector<FloorMetric> baseline_metrics;
  std::vector<CandidatePriceMetric> candidate_prices;
  std::int64_t smoke_rows = 0;
  std::int64_t tune_rows = 0;
  std::int64_t rows_priced = 0;
  std::int64_t engine_errors = 0;
  double diagnostic_wall_seconds = 0.0;
  double diagnostic_rows_per_second = 0.0;
};

// Closed Stage 3 search: all input candidates on smoke, then the best two on a
// deterministic tune sample, followed by full smoke+tune baseline/winner Greek
// attribution. Ties always break on the stable candidate ID.
[[nodiscard]] Result<ConventionSweepResult> run_convention_sweep(std::span<const OracleRow> smoke,
                                                                 std::span<const OracleRow> tune);

[[nodiscard]] std::string convention_map_json(const ConventionMap &map);
[[nodiscard]] std::string convention_sweep_json(const ConventionSweepResult &result,
                                                std::string_view git_sha);

struct ScaleObservation {
  double raw = 0.0;
  double oracle = 0.0;
};

// Test seam for the deterministic independent-unit attribution used by all
// nine Greeks. Returns the winning signed scale.
[[nodiscard]] double select_relative_scale(std::span<const ScaleObservation> observations,
                                           std::span<const double> signed_scales) noexcept;

} // namespace atx::vol::oracle

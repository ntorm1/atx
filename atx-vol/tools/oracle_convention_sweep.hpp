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
  // Rows behind `value`: every row both arms priced.
  std::int64_t count = 0;
  // Rows the scale SELECTION ran on. For the nine relative Greeks this excludes
  // |oracle| < kGreekAbsFloor, where the relative objective's denominator floor
  // would otherwise reward the smallest candidate scale regardless of
  // correctness. Reported separately so the exclusion stays auditable.
  std::int64_t selection_count = 0;
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
// deterministic tune sample ALONE (smoke decides only the 8-way cut, so its
// evidence is never counted twice), followed by full smoke+tune baseline/winner
// Greek attribution. Every observation is committed to the candidate and the
// baseline accumulator together or to neither, so the two floors always
// describe one row population.
[[nodiscard]] Result<ConventionSweepResult> run_convention_sweep(std::span<const OracleRow> smoke,
                                                                 std::span<const OracleRow> tune);

[[nodiscard]] std::string convention_map_json(const ConventionMap &map);
[[nodiscard]] std::string convention_sweep_json(const ConventionSweepResult &result,
                                                std::string_view git_sha);

// Mean absolute (or mean relative) error over the rows it admitted. Non-finite
// observations are skipped rather than poisoning the mean.
struct Accumulator {
  double sum = 0.0;
  std::int64_t count = 0;

  void absolute(double model, double oracle) noexcept;
  void relative(double model, double oracle) noexcept;
  [[nodiscard]] double mean() const noexcept;
};

// Full-coverage reported floor plus the floor-filtered population the scale
// selection actually ran on.
struct FloorAccumulators {
  Accumulator report;
  Accumulator selection;
};

// One (Greek source, signed scale) hypothesis in a bounded independent search.
struct ScaleCandidate {
  GreekSource source{};
  double scale = 1.0;
  FloorAccumulators error;
};

// The production per-Greek pick: lowest mean relative error on the SELECTION
// population, ties broken on the stable candidate identity (source ID, then the
// signed scale numerically). Returns the winning index; `candidates` must be
// non-empty.
[[nodiscard]] std::size_t best_scale(std::span<const ScaleCandidate> candidates) noexcept;

} // namespace atx::vol::oracle

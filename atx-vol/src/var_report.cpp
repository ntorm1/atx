#include "atx/vol/var_report.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <unordered_map>
#include <utility>

#include "atx/core/error.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

// True iff a scenario-major leg-frame span of `leg_frames_size` elements is
// exactly what `n_scenarios` scenarios of `n_legs` legs each would produce,
// without overflowing the multiplication.
[[nodiscard]] bool leg_block_sizes_consistent(std::size_t n_scenarios, std::size_t n_legs,
                                              std::size_t leg_frames_size) noexcept {
  if (n_legs == 0u) {
    return leg_frames_size == 0u;
  }
  if (n_scenarios > std::numeric_limits<std::size_t>::max() / n_legs) {
    return false;
  }
  return leg_frames_size == n_scenarios * n_legs;
}

} // namespace

Status write_var_scenario_tsv(std::ostream &out, const HistoricalVarResult &result,
                              const VarExclusionSummary &exclusions,
                              std::span<const VarReferenceLeg> reference_legs) {
  if (result.frames.size() != result.base_dates.size() ||
      result.frames.size() != result.shifted_dates.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VaR scenario TSV: frame/date-array cardinality mismatch");
  }
  const bool have_legs = !result.leg_frames.empty();
  if (have_legs) {
    if (!leg_block_sizes_consistent(result.frames.size(), result.n_legs,
                                    result.leg_frames.size())) {
      return Err(ErrorCode::InvalidArgument, "VaR scenario TSV: leg-frame cardinality mismatch");
    }
    if (reference_legs.size() != result.n_legs) {
      return Err(ErrorCode::InvalidArgument,
                 "VaR scenario TSV: reference_legs must have exactly result.n_legs entries "
                 "when result.leg_frames is populated");
    }
  }

  out << "base_date\tshifted_date\tbase_value\tshifted_value\tpnl\tcumulative_pnl\t"
         "dollar_delta\tn_positions\tsource_option_lots\tcoverage_excluded_option_lots\t"
         "delta_boundary_excluded_option_lots\treplay_excluded_option_lots\tstock_hedges\t"
         "max_abs_leg_index\tmax_abs_leg_underlier\t"
         "max_abs_leg_reference_units\tmax_abs_leg_reference_delta\t"
         "max_abs_leg_target_dollar_delta\tmax_abs_leg_log_moneyness\t"
         "max_abs_leg_scenario_units\tmax_abs_leg_base_delta\tmax_abs_leg_base_mark\t"
         "max_abs_leg_shifted_mark\tmax_abs_leg_pnl\n";
  out << std::setprecision(17);
  double cumulative_pnl = 0.0;
  for (std::size_t index = 0u; index < result.frames.size(); ++index) {
    const VarScenarioFrame &frame = result.frames[index];
    cumulative_pnl += frame.pnl;
    out << result.base_dates[index] << '\t' << result.shifted_dates[index] << '\t'
        << frame.base_value << '\t' << frame.shifted_value << '\t' << frame.pnl << '\t'
        << cumulative_pnl << '\t' << frame.dollar_delta << '\t' << frame.n_ok << '\t'
        << exclusions.source_option_lots << '\t' << exclusions.coverage_excluded_option_lots << '\t'
        << exclusions.delta_boundary_excluded_option_lots << '\t'
        << exclusions.replay_excluded_option_lots << '\t' << exclusions.stock_hedges << '\t';
    if (have_legs) {
      const std::span<const VarLegFrame> scenario_legs =
          std::span<const VarLegFrame>{result.leg_frames}.subspan(index * result.n_legs,
                                                                  result.n_legs);
      const auto largest = std::max_element(scenario_legs.begin(), scenario_legs.end(),
                                            [](const VarLegFrame &left, const VarLegFrame &right) {
                                              return std::fabs(left.pnl) < std::fabs(right.pnl);
                                            });
      const std::size_t largest_index = static_cast<std::size_t>(largest - scenario_legs.begin());
      const VarReferenceLeg &largest_reference = reference_legs[largest_index];
      out << largest_index << '\t' << largest_reference.underlier << '\t'
          << largest_reference.reference_units << '\t' << largest_reference.reference_delta << '\t'
          << largest_reference.target_dollar_delta << '\t' << largest_reference.log_moneyness
          << '\t' << largest->units << '\t' << largest->base_delta << '\t' << largest->base_mark
          << '\t' << largest->shifted_mark << '\t' << largest->pnl;
    } else {
      // 11 empty max_abs_leg_* columns -> 10 tab separators between them (the
      // 11th trailing tab, separating stock_hedges from the first of these
      // columns, was already written above).
      for (int tab = 0; tab < 10; ++tab) {
        out << '\t';
      }
    }
    out << '\n';
  }
  if (!out) {
    return Err(ErrorCode::IoError, "VaR scenario TSV: write failed");
  }
  return Ok();
}

Result<std::vector<VarUnderlierAttribution>>
attribute_by_underlier(const HistoricalVarResult &result,
                       std::span<const VarReferenceLeg> reference_legs) {
  if (result.leg_frames.empty()) {
    if (result.frames.empty()) {
      return Ok(std::vector<VarUnderlierAttribution>{});
    }
    return Err(ErrorCode::InvalidArgument,
               "VaR underlier attribution requires VarRunConfig::retain_leg_frames");
  }
  if (!leg_block_sizes_consistent(result.frames.size(), result.n_legs, result.leg_frames.size())) {
    return Err(ErrorCode::InvalidArgument,
               "VaR underlier attribution: leg-frame cardinality mismatch");
  }
  if (reference_legs.size() != result.n_legs) {
    return Err(ErrorCode::InvalidArgument,
               "VaR underlier attribution: reference_legs must have exactly result.n_legs entries");
  }

  struct Accumulator {
    double total_pnl{0.0};
    double worst_pnl{0.0};
    std::int64_t worst_base_ts_ns{0};
    bool has_worst{false};
  };

  // A plain unordered_map's iteration order is not reproducible across runs;
  // `discovery_order` records first-appearance order (scenario-major,
  // position-ascending -- deterministic given the same inputs) so the
  // pre-sort row order, and therefore stable_sort's tie-breaking, is
  // reproducible too.
  std::unordered_map<std::string, Accumulator> by_underlier;
  std::vector<std::string> discovery_order;
  for (std::size_t scenario = 0u; scenario < result.frames.size(); ++scenario) {
    const std::int64_t base_ts_ns = result.frames[scenario].base_ts_ns;
    const std::size_t offset = scenario * result.n_legs;
    for (std::size_t position = 0u; position < result.n_legs; ++position) {
      const VarLegFrame &leg = result.leg_frames[offset + position];
      if (leg.status != VarLegStatus::Ok) {
        continue;
      }
      const std::string &underlier = reference_legs[position].underlier;
      auto [entry, inserted] = by_underlier.try_emplace(underlier);
      if (inserted) {
        discovery_order.push_back(underlier);
      }
      Accumulator &accumulator = entry->second;
      accumulator.total_pnl += leg.pnl;
      if (!accumulator.has_worst || leg.pnl < accumulator.worst_pnl) {
        accumulator.worst_pnl = leg.pnl;
        accumulator.worst_base_ts_ns = base_ts_ns;
        accumulator.has_worst = true;
      }
    }
  }

  std::vector<VarUnderlierAttribution> rows;
  rows.reserve(discovery_order.size());
  for (const std::string &underlier : discovery_order) {
    const Accumulator &accumulator = by_underlier.at(underlier);
    rows.push_back(VarUnderlierAttribution{underlier, accumulator.total_pnl, accumulator.worst_pnl,
                                           accumulator.worst_base_ts_ns});
  }
  std::stable_sort(rows.begin(), rows.end(),
                   [](const VarUnderlierAttribution &left, const VarUnderlierAttribution &right) {
                     return left.total_pnl < right.total_pnl;
                   });
  return Ok(std::move(rows));
}

} // namespace atx::vol

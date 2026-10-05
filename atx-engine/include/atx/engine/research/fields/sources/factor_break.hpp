#pragma once

// atx::engine::research::fields -- rule factor-break-v1 (ported from prepare_research_fields.py
// factor_breaks and atx-impl/tools/repair_role_factor_breaks.py, same parameters and step
// classification): one implementation for the vendor panel, run once per build.
//
// Inputs are vendor observations on one date axis, rows x lines date-major: the cumulReturnFactor
// f (f64) and the raw close (f32), NaN where the row is not an observation. A step is a pair of
// consecutive observations p < t of a line at most kFbMaxGapDays calendar days apart; with
// s = ln(f_t / f_p), r = ln(raw_t / raw_p) (each ln a separate log, as numpy's
// log(f_t) - log(f_p)), the step is a jump cell when |s| > 0.01 and |r + s| > |r| + 0.01; a mass
// session has >= 50 jump cells. On each mass session b (ascending), every line observed before b
// and at or after b has one crossing step (p = its last observation before b, t = its first at or
// after b) and, unless |s| <= 1e-9, it is kept_gap (t - p > 10 calendar days), kept_split_follow
// (s < 0 and r >= max(|s| / 2, ln 1.25), or s > 0 and -r >= max(|s| / 2, ln 1.25)),
// kept_distribution (0 < s < ln 1.25), else repaired with k = f_t / f_p. A step crossing several
// mass sessions is listed once, under the first. Every decision reads rows <= t only, so the rule
// is point in time.
//
// Repair (research_fields_price.source_panel): in step order, every row >= t of the line's factor
// is divided by k. A span (lo, hi] containing a kept_gap step of the line cannot be bridged by the
// chained factor (gap_crosses).

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

inline constexpr std::string_view kFactorBreakRule = "factor-break-v1";
inline constexpr f64 kFbCellStep = 0.01;
inline constexpr f64 kFbCellExcess = 0.01;
inline constexpr i64 kFbMassMinCells = 50;
inline constexpr f64 kFbNoise = 1e-9;
inline constexpr f64 kFbSplitRatio = 1.25;
// Python math.log(1.25) (repr 0.22314355131420976), written out so no libm call can move it.
inline constexpr f64 kFbLogSplitRatio = 0.22314355131420976;
inline constexpr i64 kFbMaxGapDays = 10;

enum class FactorBreakAction : u8 {
  Repaired = 1,
  KeptGap = 2,
  KeptSplitFollow = 3,
  KeptDistribution = 4,
};

struct FactorBreakStep {
  usize line{};
  usize row{}; // t, the step's end row on the axis
  i64 day{};   // the axis day of t
  f64 k{};     // f_t / f_p
  FactorBreakAction action{FactorBreakAction::Repaired};
};

struct MassSession {
  usize row{};
  i64 jump_cells{};
  i64 crossing_steps{};
  // Crossing steps first listed here, by action: repaired, kept_gap, kept_split_follow,
  // kept_distribution.
  std::array<i64, 4> by_action{};
};

struct FactorBreaks {
  std::vector<i64> jump;              // jump cells per row
  std::vector<MassSession> mass;      // ascending row
  std::vector<FactorBreakStep> steps; // mass session order, then line order
  i64 max_non_mass{};                 // the largest jump count below the mass threshold
  std::optional<usize> max_non_mass_row;
};

// The rule over `factor` / `raw_close` (rows x lines, date-major; rows = days.size()).
// Preconditions (asserted): both spans hold days.size() * lines values.
[[nodiscard]] FactorBreaks factor_breaks_v1(std::span<const f64> factor,
                                            std::span<const f32> raw_close,
                                            std::span<const i64> days, usize lines);

// The kept_gap steps of every line, for span queries.
class KeptGaps {
public:
  KeptGaps() = default;
  KeptGaps(const FactorBreaks &breaks, usize lines);

  // True when line `line` has a kept_gap step whose end row lies in (min(lo, hi), max(lo, hi)]
  // (the Python Gaps.crosses: the cumulative kept_gap count differs between the two rows).
  [[nodiscard]] bool crosses(usize line, usize lo, usize hi) const noexcept;
  [[nodiscard]] usize count() const noexcept { return count_; }

private:
  std::vector<std::vector<usize>> rows_; // per line, ascending end rows
  usize count_{};
};

} // namespace atx::engine::research::fields

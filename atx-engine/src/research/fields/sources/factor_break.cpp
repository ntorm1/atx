#include "atx/engine/research/fields/sources/factor_break.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace atx::engine::research::fields {
namespace {

constexpr usize kNone = std::numeric_limits<usize>::max();

// A step p -> t is a jump cell: |s| > 0.01 and |r + s| > |r| + 0.01, each ln a separate log.
[[nodiscard]] bool is_jump(f64 f, f64 f_prev, f64 raw, f64 raw_prev) noexcept {
  const f64 s = std::log(f) - std::log(f_prev);
  const f64 r = std::log(raw) - std::log(raw_prev);
  return std::fabs(s) > kFbCellStep && std::fabs(r + s) > std::fabs(r) + kFbCellExcess;
}

// Jump cells per row: steps between consecutive observations at most kFbMaxGapDays apart.
[[nodiscard]] std::vector<i64> jump_counts(std::span<const f64> factor,
                                           std::span<const f32> raw_close,
                                           std::span<const i64> days, usize lines) {
  const usize rows = days.size();
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  std::vector<i64> jump(rows, 0);
  std::vector<usize> last(lines, kNone);
  std::vector<f64> last_factor(lines, nan);
  std::vector<f64> last_raw(lines, nan);
  for (usize e = 0; e < rows; ++e) {
    i64 count = 0;
    for (usize j = 0; j < lines; ++j) {
      const f64 f = factor[e * lines + j];
      if (!std::isfinite(f)) {
        continue;
      }
      const auto raw = static_cast<f64>(raw_close[e * lines + j]);
      const bool step = last[j] != kNone && days[e] - days[last[j]] <= kFbMaxGapDays;
      if (step && is_jump(f, last_factor[j], raw, last_raw[j])) {
        ++count;
      }
      last[j] = e;
      last_factor[j] = f;
      last_raw[j] = raw;
    }
    jump[e] = count;
  }
  return jump;
}

// The class of a crossing step (nullopt: |s| at the noise level, not listed).
[[nodiscard]] std::optional<FactorBreakAction> classify(f64 s, f64 r, i64 gap_days) noexcept {
  if (std::fabs(s) <= kFbNoise) {
    return std::nullopt;
  }
  if (gap_days > kFbMaxGapDays) {
    return FactorBreakAction::KeptGap;
  }
  const f64 half = std::fabs(s) / 2.0;
  const f64 bound = std::max(half, kFbLogSplitRatio); // both finite: observations are positive
  if ((s < 0.0 && r >= bound) || (s > 0.0 && -r >= bound)) {
    return FactorBreakAction::KeptSplitFollow;
  }
  if (s > 0.0 && s < kFbLogSplitRatio) {
    return FactorBreakAction::KeptDistribution;
  }
  return FactorBreakAction::Repaired;
}

[[nodiscard]] usize action_slot(FactorBreakAction action) noexcept {
  return static_cast<usize>(static_cast<u8>(action) - 1U);
}

struct Axis {
  std::span<const f64> factor;
  std::span<const f32> raw;
  std::span<const i64> days;
  usize lines{};

  [[nodiscard]] bool observed(usize row, usize line) const noexcept {
    return std::isfinite(factor[row * lines + line]);
  }
  // The line's last observation before row b, kNone when none.
  [[nodiscard]] usize last_before(usize line, usize b) const noexcept {
    for (usize e = b; e > 0; --e) {
      if (observed(e - 1, line)) {
        return e - 1;
      }
    }
    return kNone;
  }
  // The line's first observation at or after row b, kNone when none.
  [[nodiscard]] usize first_from(usize line, usize b) const noexcept {
    for (usize e = b; e < days.size(); ++e) {
      if (observed(e, line)) {
        return e;
      }
    }
    return kNone;
  }
};

// The crossing steps of mass session b first listed here (`listed`: per line, the end row of the
// line's last listed step; a step crossing an earlier mass session has the same end row).
void cross_mass_session(const Axis &axis, usize b, std::vector<usize> &listed, MassSession &mass,
                        std::vector<FactorBreakStep> &steps) {
  for (usize j = 0; j < axis.lines; ++j) {
    const usize p = axis.last_before(j, b);
    const usize t = p == kNone ? kNone : axis.first_from(j, b);
    if (t == kNone || listed[j] == t) {
      continue;
    }
    const f64 fp = axis.factor[p * axis.lines + j];
    const f64 ft = axis.factor[t * axis.lines + j];
    const auto rp = static_cast<f64>(axis.raw[p * axis.lines + j]);
    const auto rt = static_cast<f64>(axis.raw[t * axis.lines + j]);
    const f64 s = std::log(ft) - std::log(fp);
    const f64 r = std::log(rt) - std::log(rp);
    const auto action = classify(s, r, axis.days[t] - axis.days[p]);
    if (!action) {
      continue;
    }
    listed[j] = t;
    steps.push_back(FactorBreakStep{j, t, axis.days[t], ft / fp, *action});
    ++mass.crossing_steps;
    ++mass.by_action[action_slot(*action)];
  }
}

} // namespace

FactorBreaks factor_breaks_v1(std::span<const f64> factor, std::span<const f32> raw_close,
                              std::span<const i64> days, usize lines) {
  assert(factor.size() == days.size() * lines && raw_close.size() == factor.size());
  FactorBreaks out;
  out.jump = jump_counts(factor, raw_close, days, lines);
  const Axis axis{factor, raw_close, days, lines};
  std::vector<usize> listed(lines, kNone);
  for (usize b = 0; b < out.jump.size(); ++b) {
    if (out.jump[b] < kFbMassMinCells) {
      continue;
    }
    MassSession mass;
    mass.row = b;
    mass.jump_cells = out.jump[b];
    cross_mass_session(axis, b, listed, mass, out.steps);
    out.mass.push_back(mass);
  }
  // numpy argmax of the jump counts below the threshold: the first largest (row 0 when all zero).
  usize top = 0;
  i64 quiet_max = 0;
  for (usize e = 0; e < out.jump.size(); ++e) {
    const i64 quiet = out.jump[e] < kFbMassMinCells ? out.jump[e] : 0;
    if (quiet > quiet_max) {
      quiet_max = quiet;
      top = e;
    }
  }
  out.max_non_mass = quiet_max;
  if (quiet_max > 0) {
    out.max_non_mass_row = top;
  }
  return out;
}

KeptGaps::KeptGaps(const FactorBreaks &breaks, usize lines) : rows_(lines) {
  for (const FactorBreakStep &s : breaks.steps) {
    if (s.action == FactorBreakAction::KeptGap && s.line < lines) {
      rows_[s.line].push_back(s.row);
      ++count_;
    }
  }
  for (auto &r : rows_) {
    std::sort(r.begin(), r.end());
  }
}

bool KeptGaps::crosses(usize line, usize lo, usize hi) const noexcept {
  if (line >= rows_.size()) {
    return false;
  }
  const usize a = std::min(lo, hi);
  const usize b = std::max(lo, hi);
  const auto &r = rows_[line];
  const auto it = std::upper_bound(r.begin(), r.end(), a); // the first end row > a
  return it != r.end() && *it <= b;
}

} // namespace atx::engine::research::fields

#pragma once

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "fitting/calendar_walk_work.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace atx::vol::detail {

// Borrowed immutable arrays. Owners must outlive the complete local walk.
// Nodes come from a certified slice (finite, strictly increasing strikes).
struct PriceSliceView {
  std::span<const AmericanPriceNode> nodes;
  std::span<const std::uint8_t> withheld;
  bool enabled = true;
};

struct PriceSlicePoint {
  double call = 0., put = 0.;
  unsigned served = 0; // bit1 call, bit2 put; exact support rule of the API
  bool available = false;
};

class PriceSliceCursor {
 public:
  explicit PriceSliceCursor(PriceSliceView view, CalendarWalkWork *work = nullptr) noexcept
      : view_(view), work_(work) {}

  [[nodiscard]] PriceSlicePoint at(double strike) noexcept {
    const auto nodes = view_.nodes;
    if (!view_.enabled || nodes.empty() || !std::isfinite(strike) ||
        strike < nodes.front().strike || strike > nodes.back().strike || strike < last_) {
      return {};
    }
    last_ = strike;
    while (nodes[right_].strike < strike) {
      ++right_;
      if (work_) ++work_->node_advances;
    }
    const auto &right = nodes[right_];
    if (right.strike == strike || right_ == 0u) {
      return {right.call, right.put, support(right_), true};
    }
    const auto &left = nodes[right_ - 1u];
    const double fraction = (strike - left.strike) / (right.strike - left.strike);
    return {std::lerp(left.call, right.call, fraction),
            std::lerp(left.put, right.put, fraction),
            support(right_ - 1u) & support(right_), true};
  }

 private:
  [[nodiscard]] unsigned support(std::size_t index) const noexcept {
    if (view_.withheld.empty()) return 3u;
    if (view_.withheld.size() != view_.nodes.size()) return 0u;
    return ~static_cast<unsigned>(view_.withheld[index]) & 3u;
  }
  PriceSliceView view_;
  CalendarWalkWork *work_;
  std::size_t right_ = 0;
  double last_ = -std::numeric_limits<double>::infinity();
};

struct PriceSlicePairPoint {
  double strike = 0., allowance_strike = 0.;
  bool midpoint = false;
  PriceSlicePoint early, late;
};

class PriceSlicePairWalk {
 public:
  PriceSlicePairWalk(PriceSliceView early, PriceSliceView late, bool midpoints,
                    CalendarWalkWork *work = nullptr) noexcept
      : first_(early.nodes), second_(late.nodes), early_(early, work), late_(late, work),
        work_(work), midpoints_(midpoints) {
    if (work_) ++work_->attempted_pairs;
    if (first_.empty() || second_.empty()) return;
    lo_ = std::max(first_.front().strike, second_.front().strike);
    hi_ = std::min(first_.back().strike, second_.back().strike);
    overlap_ = hi_ >= lo_;
    if (!overlap_) return;
    if (work_) ++work_->overlapping_pairs;
    current_ = next_knot();
    following_ = next_knot();
  }

  [[nodiscard]] bool overlaps() const noexcept { return overlap_; }

  [[nodiscard]] std::optional<PriceSlicePairPoint> next() noexcept {
    if (!current_) return {};
    const bool midpoint = pending_midpoint_;
    const double strike = midpoint ? 0.5 * (*current_ + *following_) : *current_;
    const double allowance_strike = midpoint ? *following_ : *current_;
    // Preserve midpoint rounding, including overflow. An unavailable midpoint
    // does not advance either interpolation cursor past the next finite knot.
    PriceSlicePairPoint point{strike, allowance_strike, midpoint,
                              early_.at(strike), late_.at(strike)};
    if (work_) {
      if (midpoint) ++work_->midpoints;
      else ++work_->knots;
    }
    if (!midpoint && midpoints_ && following_) {
      pending_midpoint_ = true;
    } else {
      pending_midpoint_ = false;
      current_ = following_;
      following_ = next_knot();
    }
    return point;
  }

 private:
  [[nodiscard]] std::optional<double> next_knot() noexcept {
    while (i_ < first_.size() || j_ < second_.size()) {
      const double strike = j_ == second_.size() ||
          (i_ < first_.size() && first_[i_].strike <= second_[j_].strike)
          ? first_[i_++].strike : second_[j_++].strike;
      if (i_ < first_.size() && first_[i_].strike == strike) ++i_;
      if (j_ < second_.size() && second_[j_].strike == strike) ++j_;
      if (strike > hi_) { i_ = first_.size(); j_ = second_.size(); return {}; }
      if (strike >= lo_) return strike;
    }
    return {};
  }
  std::span<const AmericanPriceNode> first_, second_;
  PriceSliceCursor early_, late_;
  CalendarWalkWork *work_;
  std::size_t i_ = 0, j_ = 0;
  double lo_ = 0., hi_ = 0.;
  bool midpoints_ = false, overlap_ = false, pending_midpoint_ = false;
  std::optional<double> current_, following_;
};

} // namespace atx::vol::detail

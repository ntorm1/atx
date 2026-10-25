#pragma once

#include <chrono>
#include <cstdint>

namespace atx::vol::detail {

struct CalendarWalkWork {
  std::uint64_t attempted_pairs = 0, overlapping_pairs = 0;
  std::uint64_t knots = 0, midpoints = 0, side_comparisons = 0;
  std::uint64_t node_advances = 0, wall_ns = 0;
};

// Optional attribution only: a null sink reads no clock.
class CalendarWalkTimer {
 public:
  explicit CalendarWalkTimer(CalendarWalkWork *work) noexcept : work_(work) {
    if (work_) start_ = Clock::now();
  }
  ~CalendarWalkTimer() {
    if (work_) {
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now() - start_).count();
      if (ns > 0) work_->wall_ns += static_cast<std::uint64_t>(ns);
    }
  }
  CalendarWalkTimer(const CalendarWalkTimer &) = delete;
  CalendarWalkTimer &operator=(const CalendarWalkTimer &) = delete;

 private:
  using Clock = std::chrono::steady_clock;
  CalendarWalkWork *work_;
  Clock::time_point start_{};
};

} // namespace atx::vol::detail

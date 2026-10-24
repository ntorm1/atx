#pragma once

// NeumaierSum — the compensated summation every risk-engine total uses.
//
// The idea is the one `NeumaierSum` in api/analytics/served_analytics.hpp ships;
// it is restated here (ten lines) rather than including that heavy header. Every
// total in api/risk/* is a serial Neumaier sum in INPUT order on the calling thread,
// so a total is a pure function of its inputs and never of the thread count.

#include <cmath>

namespace atx::vol::risk_detail {

class NeumaierSum {
public:
  void add(double x) noexcept {
    const double t = sum_ + x;
    // The branch keeps the compensation exact whichever operand is larger
    // (Neumaier's improvement over Kahan).
    if (std::fabs(sum_) >= std::fabs(x)) {
      comp_ += (sum_ - t) + x;
    } else {
      comp_ += (x - t) + sum_;
    }
    sum_ = t;
  }
  [[nodiscard]] double value() const noexcept { return sum_ + comp_; }

private:
  double sum_{0.0};
  double comp_{0.0};
};

} // namespace atx::vol::risk_detail

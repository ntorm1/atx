#pragma once

#include <cmath>

namespace atx::vol {

[[nodiscard]] inline double interpolate_positive_log(double lo, double hi,
                                                     double alpha) noexcept {
  return std::exp(std::log(lo) + alpha * (std::log(hi) - std::log(lo)));
}

// Recover the unique continuous effective yield that makes a market-implied
// forward and the pricing carry describe the same term. Callers validate their
// positive spot/forward/time invariants at construction/query boundaries.
[[nodiscard]] inline double coherent_q_eff(double spot, double forward, double T,
                                           double rate) noexcept {
  return rate - std::log(forward / spot) / T;
}

} // namespace atx::vol

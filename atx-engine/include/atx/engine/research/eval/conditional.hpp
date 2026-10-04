#pragma once

// atx::engine::research::eval -- inference on winners for the acceptance rule (platform P9 B2,
// K-P9-5 `conditional`; G-B7, lit section 5.2 (a); Andrews, Kitagawa & McCloskey 2024 QJE,
// "Inference on Winners"). PRINTED ONLY: no rule reads it (PM7-34 stays).
//
// A cell is accepted when its paired estimate X = dSR exceeds the threshold c (the house rule
// dSR > 0 is c = 0, a one-sided test at alpha .5). Given acceptance, X ~ N(mu, se^2)
// truncated to X > c, so the raw estimate is biased upwards (the winner's curse): under the
// null mu = 0 an accepted cell's expected dSR is se phi(c/se) / Q(c/se) (.80 se at c = 0).
// The conditional CDF F_mu(x) = P(X <= x | X > c) = 1 - Q((x - mu)/se) / Q((c - mu)/se) is
// decreasing in mu, so for an accepted x:
//
//   median_unbiased   mu with F_mu(x) = 1/2 (AKM's median-unbiased estimator)
//   ci_lo, ci_hi      mu with F_mu(x) = 1 - alpha/2 and alpha/2: the equal-tailed conditional
//                     confidence interval at level 1 - alpha
//   null_bias         se phi(c/se) / Q(c/se): the expected accepted estimate under mu = 0
//
// The cumulative gain of an accepted lineage sums the selected cells' median-unbiased
// estimates beside their raw sum and their null biases (cells treated as independent
// selections, each on its own threshold; a cell that was not selected is listed, not summed).
// Q is the normal upper tail: 0.5 erfc(z / sqrt 2) for z < 5, the Laplace continued fraction
// for the Mills ratio beyond, so the ratio stays exact deep in the tail. Roots by bisection
// (200 halvings); a bound outside [c - 2^40 x 10 se, x + 40 se] is reported as clamped.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::eval {

struct ConditionalEstimate {
  f64 raw{};
  f64 se{};
  f64 threshold{};
  f64 alpha{};
  bool selected{};                   // raw > threshold
  std::optional<f64> median_unbiased; // empty unless selected (and found)
  std::optional<f64> ci_lo;
  std::optional<f64> ci_hi;
  f64 null_bias{};
  bool clamped{};
};

// InvalidArgument when se is not a positive finite number, raw or threshold is not finite,
// or alpha is outside (0, 1).
[[nodiscard]] core::Result<ConditionalEstimate> conditional_estimate(f64 raw, f64 se,
                                                                     f64 threshold, f64 alpha);

struct LineageCell {
  std::string name;
  f64 raw{};
  f64 se{};
  f64 threshold{};
};

struct CumulativeConditional {
  usize cells{};                   // selected cells summed
  f64 raw_sum{};
  std::optional<f64> adjusted_sum; // empty when a selected cell has no estimate (clamped)
  f64 null_bias_sum{};
  std::vector<ConditionalEstimate> rows; // every input cell, input order
};

[[nodiscard]] core::Result<CumulativeConditional>
cumulative_conditional(std::span<const LineageCell> cells, f64 alpha);

// log Q(z), Q(z) = P(N(0, 1) > z).
[[nodiscard]] f64 log_upper_tail(f64 z) noexcept;

} // namespace atx::engine::research::eval

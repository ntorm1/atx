#pragma once

// atx::engine::eval — Minimum Track Record Length (MinTRL).
//
// Bailey & López de Prado (2012), "The Sharpe Ratio Efficient Frontier",
// Journal of Risk 15(2), eq. (13): the number of observations needed before
// PSR(SR*) reaches a confidence level, i.e. before we can reject
// H0: true SR <= SR* at that confidence given the observed SR and moments:
//
//   MinTRL = 1 + (1 − γ3·SR + ((γ4 − 1)/4)·SR²) · (Z_conf / (SR − SR*))²
//
// with γ4 the RAW kurtosis. This file takes EXCESS kurtosis κ (γ4 = κ + 3,
// so (γ4 − 1)/4 = (κ + 2)/4), matching deflated_sharpe.hpp / stats_ext.hpp.
// All Sharpes are PER-PERIOD; the result is a count of periods, not years.
// Pure, allocation-free; the inverse of probabilistic_sharpe in T.

#include <cmath>  // std::sqrt
#include <limits> // std::numeric_limits

#include "atx/core/types.hpp"            // atx::f64
#include "atx/engine/eval/stats_ext.hpp" // norm_ppf

namespace atx::engine::eval {

// Returns NaN (documented degenerate cases) when SR <= SR* (no finite record
// suffices), confidence is not in (0, 1), or the moment term is <= 0.
[[nodiscard]] inline atx::f64 min_track_record_length(atx::f64 sr, atx::f64 sr_star,
                                                      atx::f64 skew, atx::f64 exkurt,
                                                      atx::f64 confidence) noexcept {
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  if (!(sr > sr_star) || !(confidence > 0.0) || !(confidence < 1.0)) {
    return nan;
  }
  const atx::f64 var_term = 1.0 - skew * sr + ((exkurt + 2.0) / 4.0) * sr * sr;
  if (!(var_term > 0.0)) {
    return nan;
  }
  const atx::f64 z = norm_ppf(confidence) / (sr - sr_star);
  return 1.0 + var_term * z * z;
}

} // namespace atx::engine::eval

#pragma once

// Backtest validation statistics for a historical VaR breach sequence:
// Kupiec's unconditional-coverage (POF) test and Christoffersen's
// conditional-coverage test (POF + a first-order Markov independence test).
//
// Before this header, atx-vol emitted a single nearest-rank VaR/ES estimate
// (var.hpp's historical_var_statistics) and nothing that scored whether the
// realized breach rate is statistically consistent with the model's stated
// confidence, or whether breaches cluster in time instead of arriving
// independently (feature-gaps.md finding 4). Both tests below reduce to a
// chi-square likelihood-ratio statistic; this header evaluates the
// closed-form survival functions directly (erfc for 1 dof, exp(-LR/2) for 2
// dof) rather than depending on a general chi-square CDF implementation.
//
// Degenerate-case policy (documented once here, shared by both tests): every
// log-likelihood term below is of the form `count * log(count / denominator)`
// for some observed transition/breach count. Whenever `count == 0` the term
// is defined as exactly 0.0 without evaluating `log(0)` -- the standard
// `0 * ln(0) = 0` entropy convention -- and the code takes that shortcut
// *before* touching the corresponding ratio, so no term ever divides by a
// zero denominator either (a zero count's denominator is only ever queried
// by that same zero-count term). This uniformly covers every degenerate
// input named in the task brief -- no breaches, all breaches, and
// n01 + n11 == 0 (every breach observation is un-preceded, e.g. a single
// breach at position 0) -- without separate branches per case. The only
// input this cannot absorb is a breach_sequence with fewer than two
// observations (zero transitions to fit a Markov chain to at all), which
// christoffersen() rejects with an explicit Err. A likelihood-ratio can also
// land a few ULPs below its analytic minimum of 0.0 at an exact-fit point
// (both hypotheses' log-likelihoods equal) purely from floating-point
// summation order; both kupiec_pof and christoffersen clamp LR to
// [0, +inf) before handing it to a survival function, since a negative
// argument would otherwise NaN out of erfc/exp's domain.

#include <cstddef>
#include <span>

#include "atx/vol/types.hpp"

namespace atx::vol {

struct KupiecResult {
  double lr_pof{0.0};
  double p_value{0.0};
  std::size_t n_obs{0};
  std::size_t n_breaches{0};

  [[nodiscard]] bool operator==(const KupiecResult &) const = default;
};

// Kupiec (1995) proportion-of-failures test. Null hypothesis: the true
// breach probability equals `1 - var_confidence`.
//   LR_pof = -2 ln[ (1-p)^(n-x) p^x ] + 2 ln[ (1-x/n)^(n-x) (x/n)^x ]
// where n = n_obs, x = n_breaches, p = 1 - var_confidence. LR_pof is
// chi-square distributed with 1 degree of freedom under the null; its
// survival function has the closed form p_value = erfc(sqrt(LR_pof / 2)).
// n_obs must be > 0, n_breaches must be in [0, n_obs], and var_confidence
// must be finite and in (0, 1) (so p is bounded away from the {0, 1}
// boundary and every log(1-p)/log(p) term below is finite).
[[nodiscard]] Result<KupiecResult> kupiec_pof(std::size_t n_obs, std::size_t n_breaches,
                                              double var_confidence);

struct ChristoffersenResult {
  double lr_independence{0.0};
  double p_independence{0.0};
  double lr_conditional_coverage{0.0};
  double p_conditional_coverage{0.0};

  [[nodiscard]] bool operator==(const ChristoffersenResult &) const = default;
};

// Christoffersen (1998) independence and conditional-coverage tests. From the
// 9-transition counts (n00, n01, n10, n11) of consecutive breach_sequence
// pairs (n_ij = number of transitions from state i to state j, breach =
// true = state 1):
//   LR_ind = -2 ln[(1-pi)^(n00+n10) pi^(n01+n11)]
//            + 2 ln[(1-pi01)^n00 pi01^n01 (1-pi11)^n10 pi11^n11]
//   pi01 = n01/(n00+n01), pi11 = n11/(n10+n11), pi = (n01+n11)/(n00+n01+n10+n11)
// LR_ind is chi-square 1 dof under the null of no first-order dependence
// (survival p_independence = erfc(sqrt(LR_ind / 2))). Conditional coverage
// jointly tests correct unconditional coverage and independence:
//   LR_cc = LR_pof + LR_ind          (LR_pof from kupiec_pof(n_obs, n_breaches, var_confidence))
// LR_cc is chi-square 2 dof (survival p_conditional_coverage = exp(-LR_cc/2)).
// breach_sequence must have at least 2 observations (>= 1 transition);
// var_confidence is validated exactly as in kupiec_pof, whose error this
// function propagates unchanged.
[[nodiscard]] Result<ChristoffersenResult> christoffersen(std::span<const bool> breach_sequence,
                                                          double var_confidence);

} // namespace atx::vol

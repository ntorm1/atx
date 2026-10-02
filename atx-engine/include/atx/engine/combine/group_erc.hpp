#pragma once

// atx::engine::combine -- equal risk contribution shares of a few groups (platform v8 X lane
// XCOMB; the composition rule theme-erc-v1 in atx-impl reads it).
//
// group_erc_shares: given the g x g covariance C of the groups' return series (row-major,
// exactly symmetric, positive diagonal), the shares s (s_t > 0, sum 1) whose risk contributions
// c_t = s_t (C s)_t are equal (Maillard, Roncalli and Teiletche 2010, "The properties of equally
// weighted risk contribution portfolios", J. Portfolio Management 36(4)). Solved as Spinu's
// (2013) strictly convex problem min_x 1/2 x'Cx - (1/g) sum_t ln x_t on x > 0, whose stationarity
// x_t (C x)_t = 1/g is the equal-contribution condition, by cyclical coordinate descent
// (Griveau-Billion, Richard and Roncalli 2013):
//   start   x_t = 1 / sqrt(C_tt) (the inverse-volatility point);
//   sweep   for t ascending: beta_t = sum_{u != t} C_tu x_u (u ascending), then x_t = the
//           positive root of C_tt x^2 + beta_t x - b = 0, b = 1/g, written without cancellation:
//           x_t = 2b / (beta_t + sqrt(D)) when beta_t >= 0, else (-beta_t + sqrt(D)) / (2 C_tt),
//           D = beta_t^2 + 4 C_tt b;
//   end     exactly `sweeps` sweeps (no early exit: the sequence is a fixed function of the
//           inputs, so a port that keeps the order reproduces it bit for bit), then s = x / sum x
//           (sum in group order).
// The result also reports each c_t (the inner sum in group order) and the dispersion
// max_t |c_t / mean(c) - 1|, the caller's convergence test: CCD converges to the unique solution
// for a positive definite C, and the dispersion is ~1e-14 once it has.
//
// Header-only as group_shrink.hpp: small loops over at most a few dozen groups, no engine
// library object (the IC TUs that call it are /O2 while the engine library is /Od in Debug).

#include <algorithm> // std::any_of, std::max
#include <cmath>     // std::abs, std::isfinite, std::sqrt
#include <span>      // std::span
#include <utility>   // std::move
#include <vector>    // std::vector

#include "atx/core/error.hpp" // Result, Err, Ok
#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::combine {

// group_erc_shares output, per group in input order.
struct GroupErcShares {
  std::vector<atx::f64> share;        // s_t > 0, sum 1
  std::vector<atx::f64> contribution; // c_t = s_t (C s)_t
  atx::f64 dispersion = 0.0;          // max_t |c_t / mean(c) - 1|
};

// Equal risk contribution shares (see the header comment). Refuses (InvalidArgument): groups
// == 0, a covariance that is not groups x groups, a non-finite entry, an entry that differs from
// its transpose, a diagonal entry that is not > 0, sweeps == 0, or an iterate, share or
// contribution that is not finite and positive (an indefinite C).
[[nodiscard]] inline atx::core::Result<GroupErcShares>
group_erc_shares(std::span<const atx::f64> covariance, atx::usize groups, atx::usize sweeps) {
  namespace co = atx::core;
  const auto non_finite = [](atx::f64 v) { return !std::isfinite(v); };
  if (groups == 0U || covariance.size() != groups * groups || sweeps == 0U ||
      std::any_of(covariance.begin(), covariance.end(), non_finite))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "group erc: shapes, a non-finite covariance entry or no sweep");
  for (atx::usize t = 0; t < groups; ++t) {
    if (!(covariance[t * groups + t] > 0.0))
      return co::Err(co::ErrorCode::InvalidArgument, "group erc: a variance that is not > 0");
    for (atx::usize u = t + 1; u < groups; ++u)
      if (covariance[t * groups + u] != covariance[u * groups + t])
        return co::Err(co::ErrorCode::InvalidArgument, "group erc: the covariance is not symmetric");
  }
  const atx::f64 budget = 1.0 / static_cast<atx::f64>(groups);
  std::vector<atx::f64> x(groups);
  for (atx::usize t = 0; t < groups; ++t) x[t] = 1.0 / std::sqrt(covariance[t * groups + t]);
  for (atx::usize sweep = 0; sweep < sweeps; ++sweep) {
    for (atx::usize t = 0; t < groups; ++t) {
      atx::f64 beta = 0.0;
      for (atx::usize u = 0; u < groups; ++u)
        if (u != t) beta += covariance[t * groups + u] * x[u];
      const atx::f64 diag = covariance[t * groups + t];
      const atx::f64 root = std::sqrt(beta * beta + 4.0 * diag * budget);
      x[t] = beta >= 0.0 ? 2.0 * budget / (beta + root) : (-beta + root) / (2.0 * diag);
    }
  }
  atx::f64 total = 0.0;
  for (const atx::f64 v : x) total += v;
  const auto not_positive = [](atx::f64 v) { return !std::isfinite(v) || !(v > 0.0); };
  if (!std::isfinite(total) || !(total > 0.0) || std::any_of(x.begin(), x.end(), not_positive))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "group erc: the iterates are not finite and positive (an indefinite covariance)");
  GroupErcShares out;
  out.share.resize(groups);
  out.contribution.resize(groups);
  for (atx::usize t = 0; t < groups; ++t) out.share[t] = x[t] / total;
  atx::f64 sum = 0.0;
  for (atx::usize t = 0; t < groups; ++t) {
    atx::f64 marginal = 0.0;
    for (atx::usize u = 0; u < groups; ++u) marginal += covariance[t * groups + u] * out.share[u];
    out.contribution[t] = out.share[t] * marginal;
    sum += out.contribution[t];
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(groups);
  if (!std::isfinite(mean) || !(mean > 0.0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "group erc: the risk contributions are not positive (an indefinite covariance)");
  for (const atx::f64 c : out.contribution)
    out.dispersion = std::max(out.dispersion, std::abs(c / mean - 1.0));
  return co::Ok(std::move(out));
}

} // namespace atx::engine::combine

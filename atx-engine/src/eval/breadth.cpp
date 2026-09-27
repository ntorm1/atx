#include "atx/engine/eval/breadth.hpp"

#include <cmath> // std::sqrt, std::isfinite
#include <algorithm>
#include <limits>

#include "atx/core/error.hpp"            // Result, EigResult (via decompose.hpp)
#include "atx/core/linalg/decompose.hpp" // symmetric_eig, EigResult
#include "atx/core/macro.hpp"            // ATX_ASSERT, ATX_WARN

namespace atx::engine::eval {

namespace {
// Both reductions contain nonnegative, scaled terms. Compensation keeps the
// participation ratio accurate when a large covariance has many small entries.
void add_compensated(atx::f64 x, atx::f64& sum, atx::f64& correction) {
  const auto next = sum + x;
  correction += sum >= x ? (sum - next) + x : (x - next) + sum;
  sum = next;
}

atx::f64 psd_trace_breadth(const atx::core::linalg::MatX& cov) {
  if (cov.rows() != cov.cols() || cov.rows() == 0) {
    ATX_WARN("effective_breadth: covariance must be square and nonempty");
    return 0.0;
  }
  atx::f64 scale = 0.0;
  for (Eigen::Index j = 0; j < cov.cols(); ++j)
    for (Eigen::Index i = 0; i < cov.rows(); ++i) {
      const auto value = cov(i, j);
      if (!std::isfinite(value) || (i == j && value < 0.0)) {
        ATX_WARN("effective_breadth: invalid covariance entry");
        return 0.0;
      }
      scale = std::max(scale, std::abs(value));
    }
  if (scale == 0.0) return 0.0;

  atx::f64 trace = 0.0, trace_correction = 0.0;
  atx::f64 frobenius = 0.0, frobenius_correction = 0.0;
  for (Eigen::Index j = 0; j < cov.cols(); ++j) {
    add_compensated(cov(j, j) / scale, trace, trace_correction);
    for (Eigen::Index i = 0; i < cov.rows(); ++i) {
      const auto value = cov(i, j) / scale;
      // Scale before differencing: extreme finite covariances need not overflow.
      if (std::abs(value - cov(j, i) / scale) > 64.0 * std::numeric_limits<atx::f64>::epsilon()) {
        ATX_WARN("effective_breadth: covariance must be symmetric");
        return 0.0;
      }
      add_compensated(value * value, frobenius, frobenius_correction);
    }
  }
  const auto ratio = (trace + trace_correction) / std::sqrt(frobenius + frobenius_correction);
  return ratio * ratio;
}
} // namespace

atx::f64 effective_breadth(const atx::core::linalg::MatX &cov, BreadthRule rule) {
  if (rule == BreadthRule::PsdTraceV2) return psd_trace_breadth(cov);
  ATX_CHECK(rule == BreadthRule::LegacyClippedEigenV1);
  // PRECONDITION: a covariance/correlation matrix is square and non-empty. The
  // participation ratio is undefined for a 0×0 spectrum; fail fast in debug.
  ATX_ASSERT(cov.rows() == cov.cols());
  ATX_ASSERT(cov.rows() >= 1);

  // Eigenvalues via the atx-core symmetric eigensolver (ascending). A genuine
  // covariance is symmetric and the solver converges; should it Err (a non-
  // symmetric input, or a non-convergence the SelfAdjointEigenSolver does not
  // normally hit), we cannot return a meaningful breadth. The public signature is
  // f64, so we surface the failure as N_eff = 0 with a logged reason rather than
  // dereferencing an error state — a degenerate, conservative "no measurable
  // breadth" that never poisons the report with UB or a silent wrong number.
  const auto eig = atx::core::linalg::symmetric_eig(cov);
  if (!eig.has_value()) {
    ATX_WARN("effective_breadth: symmetric_eig failed ({}); reporting N_eff = 0",
             eig.error().message());
    return 0.0;
  }
  const atx::core::linalg::VecX &lambda = eig->values;

  // Σλ and Σλ², order-fixed in the solver's ascending order so the result is
  // byte-identical run-to-run. Each eigenvalue is clamped to max(λ, 0): a PSD
  // covariance has λ ≥ 0 in exact arithmetic, but a near-singular input (e.g. a
  // rank-1 identical-bets covariance) can produce a tiny NEGATIVE λ in finite
  // precision. A negative variance is physically zero; clamping keeps both sums
  // honest (a squared negative would otherwise inflate Σλ²).
  atx::f64 sum_l = 0.0;
  atx::f64 sum_l2 = 0.0;
  for (Eigen::Index i = 0; i < lambda.size(); ++i) {
    const atx::f64 li = lambda[i] > 0.0 ? lambda[i] : 0.0;
    sum_l += li;
    sum_l2 += li * li;
  }

  // A zero matrix (all λ clamped to 0) has no variance and hence no bet to count:
  // documented as N_eff = 0, which also guards the 0/0 that would otherwise be a
  // NaN. sum_l == 0 ⇒ sum_l2 == 0 (every term is the square of a clamped value),
  // so this is the only division-by-zero path.
  if (sum_l == 0.0) {
    return 0.0;
  }
  return (sum_l * sum_l) / sum_l2;
}

BreadthResult breadth_decomposition(const atx::core::linalg::MatX &cov, atx::f64 ic, BreadthRule rule) {
  // PRECONDITION: the IC is a finite skill scalar. A NaN/inf IC would propagate
  // straight into IR; fail closed in debug rather than emit a non-finite IR.
  ATX_ASSERT(std::isfinite(ic));

  const atx::f64 n_eff = effective_breadth(cov, rule);
  // Fundamental Law of Active Management: IR = IC · √breadth. √n_eff is well-
  // defined (n_eff ≥ 0 by construction — a ratio of a square over a sum of
  // squares, or the documented 0).
  const atx::f64 ir = ic * std::sqrt(n_eff);
  return BreadthResult{n_eff, ic, ir, rule};
}

} // namespace atx::engine::eval

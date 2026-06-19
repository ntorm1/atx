#include "atx/engine/eval/breadth.hpp"

#include <cmath> // std::sqrt, std::isfinite

#include "atx/core/error.hpp"            // Result, EigResult (via decompose.hpp)
#include "atx/core/linalg/decompose.hpp" // symmetric_eig, EigResult
#include "atx/core/macro.hpp"            // ATX_ASSERT, ATX_WARN

namespace atx::engine::eval {

atx::f64 effective_breadth(const atx::core::linalg::MatX &cov) {
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

BreadthResult breadth_decomposition(const atx::core::linalg::MatX &cov, atx::f64 ic) {
  // PRECONDITION: the IC is a finite skill scalar. A NaN/inf IC would propagate
  // straight into IR; fail closed in debug rather than emit a non-finite IR.
  ATX_ASSERT(std::isfinite(ic));

  const atx::f64 n_eff = effective_breadth(cov);
  // Fundamental Law of Active Management: IR = IC · √breadth. √n_eff is well-
  // defined (n_eff ≥ 0 by construction — a ratio of a square over a sum of
  // squares, or the documented 0).
  const atx::f64 ir = ic * std::sqrt(n_eff);
  return BreadthResult{n_eff, ic, ir};
}

} // namespace atx::engine::eval

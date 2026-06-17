// atx::engine::learn — scaffold + atx-core L7 link smoke-test (S5-0).
//
// Two jobs:
//   1. Prove that `include/atx/engine/learn/fwd.hpp` compiles cleanly under the
//      /W4 /permissive- /WX warnings gate and that the atx-engine-tests binary
//      links with the new learn namespace in scope.
//   2. RETIRE THE EIGEN-LINK RISK (§0.8) BEFORE any model unit: prove that the
//      Eigen-backed atx-core L7 primitives this layer consumes — ridge
//      regression, PCA, and the deterministic PRNG — actually INCLUDE, COMPILE
//      and LINK from the engine test target. If `ridge`/`pca` fail to LINK (as
//      opposed to a missing header), the transitive Eigen link is broken and S5
//      stops here for guidance.
//
// AS-BUILT API NOTE (the plan's S5-0 sketch guessed the paths/namespaces):
//   - Headers live under atx/core/linalg/, not atx/core/ flat:
//       "atx/core/linalg/regression.hpp"  (ridge / ols / wls)
//       "atx/core/linalg/pca.hpp"         (pca / transform)
//       "atx/core/random.hpp"             (Xoshiro256pp)
//   - ridge, pca, as_matrix, as_vector all live in namespace atx::core::linalg
//     (NOT atx::core::regression / atx::core::pca).
//   - ridge(X, y, lambda) -> Result<OlsResult>{ .beta, .r2, .residuals }.
//   - pca(X, k)          -> Result<PcaResult>{ .mean, .components,
//                                              .explained_variance,
//                                              .explained_ratio }.
//   - Result<T> is tl::expected; success is .has_value(), value via ->.
//   - as_matrix(span<const double>, rows, cols) / as_vector(span<const double>)
//     return zero-copy Eigen Maps; we materialize them into MatX/VecX so they
//     bind to ridge/pca's `const MatX&` parameters (a Map is not a MatrixXd).
//   - Xoshiro256pp{seed}.next_u64() is the next-u64 draw.

#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"     // as_matrix, as_vector, MatX, VecX
#include "atx/core/linalg/pca.hpp"        // pca, PcaResult
#include "atx/core/linalg/regression.hpp" // ridge, OlsResult
#include "atx/core/random.hpp"            // Xoshiro256pp
#include "atx/core/types.hpp"             // atx::f64

#include "atx/engine/learn/fwd.hpp" // the layer doc block + forward decls

namespace atxtest_learn_scaffold_test {

using atx::f64;

// LearnScaffold — caught by `ctest -R LearnScaffold`.

// §0.8 link smoke-test: ridge must INCLUDE, COMPILE and LINK from the engine
// test target, and recover beta on a trivial over-determined system. The POINT
// is the LINK; the numeric recovery is secondary.
TEST(LearnScaffold, AtxCoreRidge_TwoByTwo_Solves) {
  // y = 2*x1 + 0*x2. Column-major 3x2: col0 = {1,2,3}, col1 = {0,0,0}.
  const std::vector<f64> xdata{1.0, 2.0, 3.0, 0.0, 0.0, 0.0};
  const std::vector<f64> ydata{2.0, 4.0, 6.0};
  const atx::core::linalg::MatX X = atx::core::linalg::as_matrix(xdata, 3, 2);
  const atx::core::linalg::VecX y = atx::core::linalg::as_vector(ydata);
  const auto r = atx::core::linalg::ridge(X, y, 1e-6);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->beta[0], 2.0, 1e-3);
}

// §0.8 pca link: PCA must INCLUDE and LINK; a rank-1 matrix yields one factor
// that explains ~all the variance.
TEST(LearnScaffold, AtxCorePca_Includable_FitsRankOne) {
  // 3x2 column-major: col0 = {1,2,3}, col1 = 2*col0 = {2,4,6} => rank-1.
  const std::vector<f64> xdata{1.0, 2.0, 3.0, 2.0, 4.0, 6.0};
  const atx::core::linalg::MatX X = atx::core::linalg::as_matrix(xdata, 3, 2);
  const auto p = atx::core::linalg::pca(X, 1);
  ASSERT_TRUE(p.has_value());
  EXPECT_GT(p->explained_ratio[0], 0.99); // one factor explains ~all variance
}

// M1 RNG determinism precondition: two generators with the same seed produce
// identical sequences (the contract every fitted model relies on).
TEST(LearnScaffold, Xoshiro_SameSeed_SameSequence) {
  atx::core::Xoshiro256pp a{42};
  atx::core::Xoshiro256pp b{42};
  EXPECT_EQ(a.next_u64(), b.next_u64());
}


}  // namespace atxtest_learn_scaffold_test

// combine_hrp_test.cpp — Lane 5: HRP / NCO.
//
//   * A hand-worked 3-asset López de Prado example (linkage, quasi-diag, bisection).
//   * Diagonal Σ ⇒ HRP == inverse-variance portfolio (the paper's limiting case).
//   * Quasi-diagonalization groups interleaved correlated blocks contiguously.
//   * NCO with k=1, k=N and block-matched k reproduces the global min-variance
//     portfolio exactly (Σ block-diagonal).
//   * HrpCombiner orients a negative-edge alpha short, Σ|w| = 1.
//
// Suite: CombineHrp

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/hrp.hpp"

namespace atx_test_l5_combine_hrp {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
namespace cb = atx::engine::combine;

MatX from_sd_corr(const std::vector<f64> &sd, const MatX &corr) {
  const auto n = static_cast<Eigen::Index>(sd.size());
  MatX c(n, n);
  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      c(i, j) = sd[static_cast<usize>(i)] * sd[static_cast<usize>(j)] * corr(i, j);
    }
  }
  return c;
}

TEST(CombineHrp, HandWorkedThreeAssetExample) {
  MatX corr = MatX::Identity(3, 3);
  corr(0, 1) = corr(1, 0) = 0.8;
  const MatX cov = from_sd_corr({0.1, 0.2, 0.15}, corr);
  const auto link = cb::single_linkage(cb::hrp_detail::distance_of_distances(cov));
  ASSERT_EQ(link.size(), 2U);
  EXPECT_EQ(link[0].a, 0U);
  EXPECT_EQ(link[0].b, 1U);
  EXPECT_NEAR(link[0].dist, std::sqrt(0.2), 1e-12); // ‖d_0 − d_1‖ = √(2·0.1)
  EXPECT_EQ(link[1].a, 2U);
  EXPECT_EQ(link[1].b, 3U);
  EXPECT_EQ(cb::quasi_diag(link, 3), (std::vector<usize>{2U, 0U, 1U}));
  const auto w = cb::hrp_weights(cov);
  ASSERT_TRUE(w.has_value());
  // Bisection [2] | [0,1]: V_L = .0225, V_R = IVP var of {0,1} = .01312 → α = .36833…
  EXPECT_NEAR((*w)[2], 0.3683323975294778, 1e-14);
  EXPECT_NEAR((*w)[0], 0.5053340819764178, 1e-14);
  EXPECT_NEAR((*w)[1], 0.12633352049410446, 1e-14);
}

TEST(CombineHrp, DiagonalCovarianceIsInverseVariance) {
  const std::vector<f64> var{0.04, 0.01, 0.09, 0.02, 0.05, 0.03, 0.07};
  MatX cov = MatX::Zero(7, 7);
  f64 s = 0.0;
  for (usize i = 0U; i < var.size(); ++i) {
    cov(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = var[i];
    s += 1.0 / var[i];
  }
  const auto w = cb::hrp_weights(cov);
  ASSERT_TRUE(w.has_value());
  for (usize i = 0U; i < var.size(); ++i) {
    EXPECT_NEAR((*w)[i], (1.0 / var[i]) / s, 1e-14);
  }
}

MatX interleaved_blocks() {
  // Assets {0,2,4} correlated 0.7, {1,3,5} correlated 0.5, 0.1 across.
  MatX corr(6, 6);
  for (Eigen::Index i = 0; i < 6; ++i) {
    for (Eigen::Index j = 0; j < 6; ++j) {
      corr(i, j) = (i == j) ? 1.0 : ((i % 2) == (j % 2) ? ((i % 2 == 0) ? 0.7 : 0.5) : 0.1);
    }
  }
  return from_sd_corr({0.1, 0.2, 0.15, 0.25, 0.12, 0.18}, corr);
}

TEST(CombineHrp, QuasiDiagGroupsCorrelatedBlocksAndWeightsArePositive) {
  const MatX cov = interleaved_blocks();
  const auto order = cb::quasi_diag(cb::single_linkage(cb::hrp_detail::distance_of_distances(cov)), 6);
  ASSERT_EQ(order.size(), 6U);
  const bool first_even = (order[0] % 2U) == 0U;
  for (usize i = 0U; i < 3U; ++i) {
    EXPECT_EQ((order[i] % 2U) == 0U, first_even);
    EXPECT_EQ((order[i + 3U] % 2U) == 0U, !first_even);
  }
  const auto w = cb::hrp_weights(cov);
  ASSERT_TRUE(w.has_value());
  f64 sum = 0.0;
  for (const f64 x : *w) {
    EXPECT_GT(x, 0.0);
    sum += x;
  }
  EXPECT_NEAR(sum, 1.0, 1e-14);
  const auto labels =
      cb::cut_tree(cb::single_linkage(cb::hrp_detail::distance_of_distances(cov)), 6, 2);
  for (usize i = 0U; i < 6U; ++i) {
    EXPECT_EQ(labels[i], i % 2U);
  }
}

TEST(CombineHrp, NcoReproducesMinVarianceOnBlockDiagonal) {
  MatX corr = MatX::Identity(5, 5);
  corr(0, 1) = corr(1, 0) = 0.6;
  corr(2, 3) = corr(3, 2) = 0.4;
  corr(2, 4) = corr(4, 2) = 0.3;
  corr(3, 4) = corr(4, 3) = 0.5;
  const MatX cov = from_sd_corr({0.1, 0.2, 0.15, 0.25, 0.12}, corr);
  const VecX mv = cov.llt().solve(VecX::Ones(5));
  const VecX mv_n = mv / mv.sum();
  for (const usize k : {1U, 2U, 5U}) {
    const auto w = cb::nco_weights(cov, {}, k);
    ASSERT_TRUE(w.has_value()) << k;
    for (Eigen::Index i = 0; i < 5; ++i) {
      EXPECT_NEAR((*w)[static_cast<usize>(i)], mv_n[i], 1e-12) << "k=" << k;
    }
  }
  // Max-Sharpe variant with k=1 is the tangency portfolio.
  const std::vector<f64> mu{0.01, 0.02, 0.015, 0.03, 0.005};
  const auto wt = cb::nco_weights(cov, mu, 1);
  ASSERT_TRUE(wt.has_value());
  const VecX tan = cov.llt().solve(Eigen::Map<const VecX>(mu.data(), 5));
  for (Eigen::Index i = 0; i < 5; ++i) {
    EXPECT_NEAR((*wt)[static_cast<usize>(i)], tan[i] / tan.sum(), 1e-12);
  }
  EXPECT_FALSE(cb::nco_weights(cov, std::vector<f64>{1.0}, 2).has_value());
}

TEST(CombineHrp, HrpCombinerOrientsNegativeEdgeShort) {
  constexpr usize kT = 50;
  constexpr usize kN = 40;
  auto st = cb::SignalStore::create(kT, kN);
  ASSERT_TRUE(st.has_value());
  std::uint64_t s = 9U;
  const auto next = [&s]() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) / 9007199254740992.0) * 2.0 - 1.0;
  };
  std::vector<f64> a(kT * kN);
  std::vector<f64> b(kT * kN);
  std::vector<f64> fwd(kT * kN);
  for (usize c = 0U; c < fwd.size(); ++c) {
    a[c] = next();
    b[c] = next();
    fwd[c] = 0.3 * a[c] - 0.3 * b[c] + next();
  }
  ASSERT_TRUE(st->add_signal(a).has_value());
  ASSERT_TRUE(st->add_signal(b).has_value());
  ASSERT_TRUE(st->set_forward_returns(fwd).has_value());
  const auto r = cb::HrpCombiner{}.fit(*st, {0, kT});
  ASSERT_TRUE(r.has_value()) << r.error().message();
  EXPECT_GT(r->w[0], 0.0);
  EXPECT_LT(r->w[1], 0.0);
  EXPECT_NEAR(std::abs(r->w[0]) + std::abs(r->w[1]), 1.0, 1e-14);
}

TEST(CombineHrp, RejectsInvalidCovariance) {
  EXPECT_FALSE(cb::hrp_weights(MatX::Zero(0, 0)).has_value());
  EXPECT_FALSE(cb::hrp_weights(MatX::Zero(2, 3)).has_value());
  MatX bad = MatX::Identity(2, 2);
  bad(1, 1) = 0.0;
  EXPECT_FALSE(cb::hrp_weights(bad).has_value());
}

} // namespace atx_test_l5_combine_hrp

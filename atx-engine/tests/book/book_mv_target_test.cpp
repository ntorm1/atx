// book_mv_target_test.cpp -- P9 lane COV-MV: the kernel of mv-aim-v1 (atx::engine::book,
// mv_target.hpp), w = P V^-1 P alpha / sum |.|, V = X F X' + D, alpha = sigma x score.
//
//   the closed form on a diagonal model (F = [0], exactly singular), the push-through solve
//   against a dense V^-1 (40 names, 6 factors) and on an exactly rank-deficient F (Ruling
//   COV-13 (c): finite and equal to the dense solve, F never inverted), the gross
//   normalisation, a positive scale on alpha washing out, NaN as no opinion (bit for bit the
//   model without those names), dollar neutrality, the scratch bound, the refusals and the
//   print helpers (factor_exposure, factor_eigen, low_eigen_share; factor_eigen on storage kept
//   across calls is a fresh symmetric_eig bit for bit, Ruling COVMV-S S-1).
//
// Suite: MvTarget

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/linalg/decompose.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/mv_target.hpp"
#include "atx/engine/book/risk_target.hpp"

namespace atx_test_p9_book_mv_target {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
namespace eb = atx::engine::book;
namespace co = atx::core;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A factor model held by value; view() lends it to the kernel.
struct Model {
  usize groups{};
  usize styles{};
  std::vector<u32> group;
  std::vector<f64> exposures, covariance, specific;
  [[nodiscard]] eb::FactorRiskView view() const {
    return eb::FactorRiskView{groups, styles, group, exposures, covariance, specific};
  }
  [[nodiscard]] usize k() const { return 1U + groups + styles; }
};

// A deterministic uniform [lo, hi) stream (LCG; the same draws on every platform).
struct Draws {
  u64 state;
  f64 next(f64 lo, f64 hi) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (hi - lo) * static_cast<f64>(state >> 11U) * 0x1.0p-53;
  }
};

// n names, 3 groups (one name in none), 2 styles: K = 6. D ~ 1e-4, F = A A' x 1e-4 + a small
// diagonal (positive definite), so the forecast is of the store's order of magnitude.
Model small_model(usize n, u64 seed) {
  Model m;
  m.groups = 3U;
  m.styles = 2U;
  Draws rng{seed};
  for (usize i = 0U; i < n; ++i) {
    m.group.push_back(i % 7U == 6U ? 0U : static_cast<u32>(1U + i % 3U));
    m.exposures.push_back(rng.next(-1.5, 1.5));
    m.exposures.push_back(rng.next(-1.5, 1.5));
    m.specific.push_back(rng.next(0.5e-4, 2e-4));
  }
  const usize k = m.k();
  std::vector<f64> a(k * k);
  for (f64& x : a) x = rng.next(-1.0, 1.0);
  m.covariance.assign(k * k, 0.0);
  for (usize r = 0U; r < k; ++r)
    for (usize c = 0U; c < k; ++c) {
      f64 v = r == c ? 1e-6 : 0.0;
      for (usize l = 0U; l < k; ++l) v += 1e-4 * a[r * k + l] * a[c * k + l];
      m.covariance[r * k + c] = v;
    }
  return m;
}
std::vector<f64> scores(usize n, u64 seed) {
  Draws rng{seed};
  std::vector<f64> s(n);
  for (f64& x : s) x = rng.next(-1.0, 1.0);
  return s;
}
std::vector<f64> sqrt_of(const std::vector<f64>& d) {
  std::vector<f64> out;
  for (const f64 x : d) out.push_back(std::sqrt(x));
  return out;
}

// The reference: V = X F X' + D formed densely (n x n), V^-1 by Eigen's LU, P on both sides,
// gross 1. Active names = finite score; 0 elsewhere.
std::vector<f64> dense_target(const Model& m, const std::vector<f64>& score,
                              const std::vector<f64>& sigma) {
  std::vector<usize> active;
  for (usize i = 0U; i < score.size(); ++i)
    if (!std::isnan(score[i])) active.push_back(i);
  const auto na = static_cast<Eigen::Index>(active.size());
  const usize k = m.k();
  Eigen::MatrixXd x = Eigen::MatrixXd::Zero(na, static_cast<Eigen::Index>(k));
  Eigen::VectorXd alpha(na);
  for (Eigen::Index j = 0; j < na; ++j) {
    const usize i = active[static_cast<usize>(j)];
    x(j, 0) = 1.0;
    if (m.group[i] != 0U) x(j, static_cast<Eigen::Index>(m.group[i])) = 1.0;
    for (usize c = 0U; c < m.styles; ++c)
      x(j, static_cast<Eigen::Index>(1U + m.groups + c)) = m.exposures[i * m.styles + c];
    alpha(j) = sigma[i] * score[i];
  }
  Eigen::MatrixXd f(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(k));
  for (usize r = 0U; r < k; ++r)
    for (usize c = 0U; c < k; ++c)
      f(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c)) = m.covariance[r * k + c];
  Eigen::MatrixXd v = x * f * x.transpose();
  for (Eigen::Index j = 0; j < na; ++j) v(j, j) += m.specific[active[static_cast<usize>(j)]];
  alpha.array() -= alpha.mean();
  Eigen::VectorXd w = v.partialPivLu().solve(alpha);
  w.array() -= w.mean();
  w /= w.cwiseAbs().sum();
  std::vector<f64> out(score.size(), 0.0);
  for (Eigen::Index j = 0; j < na; ++j) out[active[static_cast<usize>(j)]] = w(j);
  return out;
}

co::Result<usize> run(const Model& m, const std::vector<f64>& score,
                      const std::vector<f64>& sigma, std::vector<f64>& out) {
  eb::MvAimScratch scratch;
  out.assign(score.size(), kNaN);
  return eb::mv_aim(m.view(), score, sigma, out, scratch);
}

// No factor variance (K = 1, F = [0]: exactly singular), V = diag(sigma^2): w is P(z / sigma)
// at gross 1. Dyadic sigma and a zero-mean alpha make every step exact: alpha = (.5, -.25,
// -.25), z / sigma = (2, -4, -16), mean -6, P: (8, 2, -10), gross 20.
TEST(MvTarget, ThreeNamesDiagonalClosedForm) {
  Model m;
  m.group = {0U, 0U, 0U};
  m.covariance = {0.0};
  m.specific = {0.25, 0.0625, 0.015625};
  const std::vector<f64> sigma{0.5, 0.25, 0.125}, score{1.0, -1.0, -2.0};
  std::vector<f64> out;
  const auto active = run(m, score, sigma, out);
  ASSERT_TRUE(active) << active.error().to_string();
  EXPECT_EQ(*active, 3U);
  EXPECT_EQ(out[0], 8.0 / 20.0);
  EXPECT_EQ(out[1], 2.0 / 20.0);
  EXPECT_EQ(out[2], -10.0 / 20.0);
}

// The push-through form equals a dense V^-1 on 40 names and K = 6 (1 + 3 groups + 2 styles),
// D ~ 1e-4 and F ~ 1e-4 (kappa(V) of order 40): 1e-12 per element at gross 1.
TEST(MvTarget, EqualsDenseInverseOnSmallN) {
  const Model m = small_model(40U, 11U);
  const auto score = scores(40U, 12U);
  const auto sigma = sqrt_of(m.specific);
  std::vector<f64> out;
  const auto active = run(m, score, sigma, out);
  ASSERT_TRUE(active) << active.error().to_string();
  EXPECT_EQ(*active, 40U);
  const auto dense = dense_target(m, score, sigma);
  f64 worst = 0.0;
  for (usize i = 0U; i < out.size(); ++i) worst = std::max(worst, std::abs(out[i] - dense[i]));
  EXPECT_LE(worst, 1e-12);
  // The tilt is not the identity: the target is not the gross-1 desired score.
  f64 moved = 0.0, gross = 0.0;
  for (const f64 s : score) gross += std::abs(s);
  for (usize i = 0U; i < out.size(); ++i) moved += std::abs(out[i] - score[i] / gross);
  EXPECT_GT(moved, 1e-3);
}

// Ruling COV-13 (c): F = v v' with dyadic v and a zero row and column (rank 1 of 6, exactly
// singular; a form that inverted F would fail or blow up). The target is finite and equal to the
// dense solve, whose V = X F X' + D stays positive definite.
TEST(MvTarget, RankDeficientFEqualsDenseSolve) {
  Model m = small_model(40U, 21U);
  const std::vector<f64> v{0.0078125, -0.00390625, 0.0, 0.001953125, 0.005859375, -0.0009765625};
  const usize k = m.k();
  for (usize r = 0U; r < k; ++r)
    for (usize c = 0U; c < k; ++c) m.covariance[r * k + c] = v[r] * v[c];
  ASSERT_EQ(m.covariance[2U * k + 2U], 0.0); // the zero row / column
  const auto score = scores(40U, 22U);
  const auto sigma = sqrt_of(m.specific);
  std::vector<f64> out;
  const auto active = run(m, score, sigma, out);
  ASSERT_TRUE(active) << active.error().to_string();
  const auto dense = dense_target(m, score, sigma);
  for (usize i = 0U; i < out.size(); ++i) {
    ASSERT_TRUE(std::isfinite(out[i])) << i;
    EXPECT_NEAR(out[i], dense[i], 1e-12) << i;
  }
  // F = 0 (rank 0) as well: V = D, the target is P(P alpha / D) at gross 1.
  m.covariance.assign(k * k, 0.0);
  ASSERT_TRUE(run(m, score, sigma, out));
  const auto diagonal = dense_target(m, score, sigma);
  for (usize i = 0U; i < out.size(); ++i) EXPECT_NEAR(out[i], diagonal[i], 1e-12) << i;
}

TEST(MvTarget, GrossNormalised) {
  const Model m = small_model(25U, 31U);
  auto score = scores(25U, 32U);
  score[3] = kNaN;
  std::vector<f64> out;
  ASSERT_TRUE(run(m, score, sqrt_of(m.specific), out));
  f64 gross = 0.0;
  for (const f64 x : out) gross += std::abs(x);
  EXPECT_NEAR(gross, 1.0, 1e-14);
  EXPECT_EQ(out[3], 0.0);
}

// A positive scalar on alpha (IC, 1 / sqrt(h), the score's SD) washes out: score x c for c in
// {1e-3, 1, 1e3} gives the same target within 1e-14 relative.
TEST(MvTarget, PositiveScalarWashesOut) {
  const Model m = small_model(30U, 41U);
  const auto score = scores(30U, 42U);
  const auto sigma = sqrt_of(m.specific);
  std::vector<f64> base;
  ASSERT_TRUE(run(m, score, sigma, base));
  f64 largest = 0.0;
  for (const f64 x : base) largest = std::max(largest, std::abs(x));
  for (const f64 c : {1e-3, 1.0, 1e3}) {
    std::vector<f64> scaled = score;
    for (f64& x : scaled) x *= c;
    std::vector<f64> out;
    ASSERT_TRUE(run(m, scaled, sigma, out)) << c;
    for (usize i = 0U; i < out.size(); ++i)
      EXPECT_NEAR(out[i], base[i], 1e-14 * largest) << c << ' ' << i;
  }
}

// A NaN score is no opinion: weight 0, and every other weight is bit for bit the kernel's on the
// model without those names.
TEST(MvTarget, NanAlphaIsNoOpinion) {
  const Model m = small_model(20U, 51U);
  auto score = scores(20U, 52U);
  const auto sigma = sqrt_of(m.specific);
  const std::vector<usize> dropped{0U, 7U, 19U};
  for (const usize i : dropped) score[i] = kNaN;
  std::vector<f64> out;
  const auto active = run(m, score, sigma, out);
  ASSERT_TRUE(active) << active.error().to_string();
  EXPECT_EQ(*active, 17U);
  Model kept;
  kept.groups = m.groups;
  kept.styles = m.styles;
  kept.covariance = m.covariance;
  std::vector<f64> kept_score, kept_sigma;
  std::vector<usize> index;
  for (usize i = 0U; i < 20U; ++i) {
    if (std::isnan(score[i])) continue;
    kept.group.push_back(m.group[i]);
    kept.exposures.push_back(m.exposures[i * 2U]);
    kept.exposures.push_back(m.exposures[i * 2U + 1U]);
    kept.specific.push_back(m.specific[i]);
    kept_score.push_back(score[i]);
    kept_sigma.push_back(sigma[i]);
    index.push_back(i);
  }
  std::vector<f64> compact;
  ASSERT_TRUE(run(kept, kept_score, kept_sigma, compact));
  for (const usize i : dropped) EXPECT_EQ(out[i], 0.0) << i;
  for (usize j = 0U; j < index.size(); ++j)
    EXPECT_EQ(std::bit_cast<u64>(out[index[j]]), std::bit_cast<u64>(compact[j])) << index[j];
}

TEST(MvTarget, DollarNeutralOnThePricedSet) {
  const Model m = small_model(35U, 61U);
  auto score = scores(35U, 62U);
  for (f64& x : score) x += 0.5; // a net-long desired target
  std::vector<f64> out;
  ASSERT_TRUE(run(m, score, sqrt_of(m.specific), out));
  f64 net = 0.0;
  for (const f64 x : out) net += x;
  EXPECT_NEAR(net, 0.0, 1e-14);
}

// The scratch stays within mv_aim_working_bytes (exact reservations, no growth factor), a
// smaller second call does not grow it, and the bound saturates instead of wrapping.
TEST(MvTarget, WorkingBytesBound) {
  const Model m = small_model(40U, 71U);
  const auto score = scores(40U, 72U);
  const auto sigma = sqrt_of(m.specific);
  eb::MvAimScratch scratch;
  std::vector<f64> out(40U);
  ASSERT_TRUE(eb::mv_aim(m.view(), score, sigma, out, scratch));
  const u64 bound = eb::mv_aim_working_bytes(40U, m.groups, m.styles);
  EXPECT_GT(scratch.bytes(), 0U);
  EXPECT_LE(scratch.bytes(), bound);
  const u64 after_first = scratch.bytes();
  const Model smaller = small_model(10U, 73U);
  std::vector<f64> small_out(10U);
  ASSERT_TRUE(eb::mv_aim(smaller.view(), scores(10U, 74U), sqrt_of(smaller.specific), small_out,
                         scratch));
  EXPECT_EQ(scratch.bytes(), after_first);
  constexpr usize big = std::numeric_limits<usize>::max();
  EXPECT_EQ(eb::mv_aim_working_bytes(big, big, big), std::numeric_limits<u64>::max());
  EXPECT_EQ(eb::mv_aim_working_bytes(16U, 0U, 0U),
            16U * (sizeof(usize) + 2U * sizeof(f64)) + 2U * (sizeof(usize) + sizeof(f64)) +
                4U * sizeof(f64));
}

// Every refusal names the rule; the output is untouched on a refusal.
TEST(MvTarget, RefusalsAndUnavailable) {
  const Model good = small_model(12U, 81U);
  const auto score = scores(12U, 82U);
  const auto sigma = sqrt_of(good.specific);
  const auto code = [&](const Model& m, const std::vector<f64>& s, const std::vector<f64>& sg) {
    eb::MvAimScratch scratch;
    std::vector<f64> out(s.size(), 7.0);
    const auto r = eb::mv_aim(m.view(), s, sg, out, scratch);
    EXPECT_FALSE(r);
    for (const f64 x : out) EXPECT_EQ(x, 7.0);
    if (!r) EXPECT_NE(r.error().message().find("mv-aim-v1"), std::string::npos);
    return r ? co::ErrorCode::Internal : r.error().code();
  };
  const auto bad = co::ErrorCode::InvalidArgument;
  // Sizes.
  EXPECT_EQ(code(good, std::vector<f64>(score.begin(), score.end() - 1), sigma), bad);
  EXPECT_EQ(code(good, score, std::vector<f64>(sigma.begin(), sigma.end() - 1)), bad);
  {
    Model m = good;
    m.covariance.pop_back();
    EXPECT_EQ(code(m, score, sigma), bad);
  }
  {
    Model m = good; // K x K would wrap: groups near the top of usize (R2b class)
    m.groups = std::numeric_limits<usize>::max() - 2U;
    EXPECT_EQ(code(m, score, sigma), bad);
  }
  {
    Model m = good;
    m.group[4] = 4U; // above groups = 3
    EXPECT_EQ(code(m, score, sigma), bad);
  }
  {
    Model m = good;
    m.exposures[5] = kNaN;
    EXPECT_EQ(code(m, score, sigma), bad);
  }
  {
    Model m = good;
    m.covariance[7] = std::numeric_limits<f64>::infinity();
    EXPECT_EQ(code(m, score, sigma), bad);
  }
  {
    Model m = good;
    m.specific[2] = 0.0; // an active name without a positive specific variance
    EXPECT_EQ(code(m, score, sigma), bad);
  }
  {
    auto s = sigma;
    s[1] = -0.01;
    EXPECT_EQ(code(good, score, s), bad);
  }
  {
    auto s = score;
    s[1] = std::numeric_limits<f64>::infinity();
    EXPECT_EQ(code(good, s, sigma), bad);
  }
  {
    // F not positive semi-definite: K = 1, G = sum 1 / D = 4, F = -1 / 4: I + F G = 0.
    Model m;
    m.group = {0U, 0U};
    m.covariance = {-0.25};
    m.specific = {0.5, 0.5};
    EXPECT_EQ(code(m, {1.0, -1.0}, {1.0, 1.0}), bad);
  }
  // Unavailable: fewer than two active names, a flat P alpha.
  {
    std::vector<f64> s(12U, kNaN);
    s[3] = 1.0;
    EXPECT_EQ(code(good, s, sigma), co::ErrorCode::Unavailable);
  }
  {
    const std::vector<f64> unit(12U, 1.0); // alpha = .25 everywhere: P alpha = 0
    EXPECT_EQ(code(good, std::vector<f64>(12U, 0.25), unit), co::ErrorCode::Unavailable);
  }
}

TEST(MvTarget, FactorExposureIsXTransposeW) {
  const Model m = small_model(9U, 91U);
  const auto w = scores(9U, 92U);
  std::vector<f64> e(m.k());
  ASSERT_TRUE(eb::factor_exposure(m.view(), w, e));
  std::vector<f64> expected(m.k(), 0.0);
  for (usize i = 0U; i < 9U; ++i) {
    expected[0] += w[i];
    if (m.group[i] != 0U) expected[m.group[i]] += w[i];
    expected[4] += m.exposures[i * 2U] * w[i];
    expected[5] += m.exposures[i * 2U + 1U] * w[i];
  }
  for (usize c = 0U; c < m.k(); ++c) EXPECT_NEAR(e[c], expected[c], 1e-15) << c;
  std::vector<f64> short_e(m.k() - 1U);
  EXPECT_FALSE(eb::factor_exposure(m.view(), w, short_e));
}

// The forecast block excludes a factor with F_jj = 0 (no forecast); ranks ascend (00 = the
// smallest); on a diagonal F the share of e'Fe on the low ranks is exact.
TEST(MvTarget, LowEigenShare) {
  const usize k = 4U;
  std::vector<f64> f(k * k, 0.0);
  f[1U * k + 1U] = 9e-4;
  f[2U * k + 2U] = 1e-4;
  f[3U * k + 3U] = 4e-4;
  eb::FactorEigen eig;
  ASSERT_TRUE(eb::factor_eigen(f, k, eig));
  ASSERT_EQ(eig.factors, (std::vector<usize>{1U, 2U, 3U}));
  ASSERT_EQ(eig.values.size(), 3U);
  EXPECT_NEAR(eig.values[0], 1e-4, 1e-18);
  EXPECT_NEAR(eig.values[1], 4e-4, 1e-18);
  EXPECT_NEAR(eig.values[2], 9e-4, 1e-18);
  const std::vector<f64> e{5.0, 1.0, 1.0, 1.0}; // factor 0 has no forecast: never read
  EXPECT_NEAR(eb::low_eigen_share(eig, e, 1U), 1e-4 / 1.4e-3, 1e-12);
  EXPECT_NEAR(eb::low_eigen_share(eig, e, 2U), 5e-4 / 1.4e-3, 1e-12);
  EXPECT_NEAR(eb::low_eigen_share(eig, e, 3U), 1.0, 1e-12);
  const std::vector<f64> zero(k, 0.0), short_e{1.0, 1.0};
  EXPECT_TRUE(std::isnan(eb::low_eigen_share(eig, zero, 3U)));
  EXPECT_TRUE(std::isnan(eb::low_eigen_share(eig, short_e, 3U))); // e shorter than a factor
  // A non-square or non-finite covariance is refused; an empty block leaves the result empty.
  EXPECT_FALSE(eb::factor_eigen(std::vector<f64>(15U, 0.0), k, eig));
  std::vector<f64> bad = f;
  bad[5] = kNaN;
  EXPECT_FALSE(eb::factor_eigen(bad, k, eig));
  ASSERT_TRUE(eb::factor_eigen(std::vector<f64>(k * k, 0.0), k, eig));
  EXPECT_TRUE(eig.factors.empty());
  EXPECT_TRUE(std::isnan(eb::low_eigen_share(eig, e, 3U)));
}

// Ruling COVMV-S S-1: one FactorEigen keeps its block and Eigen's solver across calls. Each call on
// that storage (the same 62-factor block again, a smaller block, a larger one, a single factor,
// a call after a refusal) gives the bits a fresh atx::core::linalg::symmetric_eig of the same
// block gives; an asymmetric block is refused as symmetric_eig refuses it.
TEST(MvTarget, FactorEigenOnKeptStorageIsAFreshSolve) {
  namespace la = atx::core::linalg;
  constexpr usize k = 62U;
  Draws rng{29U};
  std::vector<f64> a(k * k);
  for (f64& x : a) x = rng.next(-1.0, 1.0);
  std::vector<f64> full(k * k, 0.0);
  for (usize r = 0U; r < k; ++r)
    for (usize c = 0U; c < k; ++c) {
      f64 v = r == c ? 1e-6 : 0.0;
      for (usize l = 0U; l < k; ++l) v += 1e-4 * a[r * k + l] * a[c * k + l];
      full[r * k + c] = v;
    }
  // F with every factor j outside `keep` unforecast (a zero row and column).
  const auto masked = [&](auto keep) {
    std::vector<f64> f = full;
    for (usize j = 0U; j < k; ++j)
      if (!keep(j))
        for (usize l = 0U; l < k; ++l) f[j * k + l] = f[l * k + j] = 0.0;
    return f;
  };
  const std::vector<std::vector<f64>> cases{full,
                                            full,
                                            masked([](usize j) { return j % 2U == 0U; }),
                                            full,
                                            masked([](usize j) { return j == 7U; }),
                                            masked([](usize j) { return j < 40U; }),
                                            full};
  const std::vector<usize> sizes{k, k, 31U, k, 1U, 40U, k};
  std::vector<f64> skew = full;
  skew[0U * k + 5U] += 1e-3;
  const auto index = [](usize x) { return static_cast<Eigen::Index>(x); };
  eb::FactorEigen eig;
  for (usize c = 0U; c < cases.size(); ++c) {
    const auto& f = cases[c];
    ASSERT_TRUE(eb::factor_eigen(f, k, eig)) << c;
    const usize size = eig.factors.size();
    ASSERT_EQ(size, sizes[c]) << c;
    la::MatX block(index(size), index(size));
    for (usize p = 0U; p < size; ++p)
      for (usize q = 0U; q < size; ++q)
        block(index(p), index(q)) = f[eig.factors[p] * k + eig.factors[q]];
    const auto fresh = la::symmetric_eig(block);
    ASSERT_TRUE(fresh) << c;
    ASSERT_EQ(eig.values.size(), size) << c;
    ASSERT_EQ(eig.vectors.size(), size * size) << c;
    const auto same = [](f64 x, f64 y) { return std::bit_cast<u64>(x) == std::bit_cast<u64>(y); };
    usize differ = 0U;
    for (usize r = 0U; r < size; ++r) {
      if (!same(eig.values[r], fresh->values(index(r)))) ++differ;
      for (usize p = 0U; p < size; ++p)
        if (!same(eig.vectors[r * size + p], fresh->vectors(index(p), index(r)))) ++differ;
    }
    EXPECT_EQ(differ, 0U) << c;
    if (c == 3U) { // a refusal on the kept storage; the next call is a fresh solve again
      const auto refused = eb::factor_eigen(skew, k, eig);
      ASSERT_FALSE(refused);
      EXPECT_EQ(refused.error().code(), co::ErrorCode::InvalidArgument);
      EXPECT_NE(refused.error().message().find("symmetric"), std::string::npos);
      EXPECT_TRUE(eig.factors.empty());
      EXPECT_TRUE(eig.values.empty());
    }
  }
  la::MatX skewed(index(k), index(k));
  for (usize p = 0U; p < k; ++p)
    for (usize q = 0U; q < k; ++q) skewed(index(p), index(q)) = skew[p * k + q];
  EXPECT_FALSE(la::symmetric_eig(skewed));
}

} // namespace atx_test_p9_book_mv_target

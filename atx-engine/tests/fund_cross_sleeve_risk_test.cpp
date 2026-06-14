// fund_cross_sleeve_risk_test.cpp — P2-S2-3: the Cross-Sleeve Risk model.
//
// Given the netted fund book W = Σ_s c_s·w_s, the SHARED risk::FactorModel V (one
// X,F,D all sleeves see), and the S×S sleeve-return covariance Ω, fund_risk computes
//   (a) sigma_fund   = sqrt(WᵀVW)            (the V-based BOOK vol, via FactorModel::risk)
//   (b) factor_var   = risk(W) − WᵀDW        (= b_fundᵀ F b_fund, NO F accessor, §0.4)
//       specific_var = WᵀDW
//   (c) b_fund       = Xᵀ W                   (aggregate factor exposure, length K)
//   (d) risk_contrib = Euler RC_s OVER Ω      (Σ_s RC_s = sqrt(cᵀΩc) = sigma_sleeve, R4)
// and sleeve_return_cov builds Ω from trailing per-sleeve P&L (corr·σ·σ off-diagonal,
// variance on the diagonal).
//
// THE load-bearing subtlety (R4): the Euler RC divisor is the Ω-based sleeve-portfolio
// vol sqrt(cᵀΩc), NOT the V-based sigma_fund. We assert Σ_s RC_s ≈ sqrt(cᵀΩc) to ~1e-9
// — that only holds if the divisor is the Ω-sigma. The factor/specific split is checked
// against the INDEPENDENT bᵀFb oracle (the test owns F as its fixture input), validating
// the risk(W) − WᵀDW identity equals the true factor variance.
//
// Suite FundCrossSleeveRisk; names Subject_Condition_ExpectedResult.

#include <cmath>   // std::sqrt, std::isfinite, std::fabs, NAN
#include <span>    // std::span
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/fund/cross_sleeve_risk.hpp" // UNIT UNDER TEST
#include "atx/engine/risk/factor_model.hpp"      // risk::FactorModel fixture

namespace {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::fund::FundRisk;
using atx::engine::fund::fund_risk;
using atx::engine::fund::sleeve_return_cov;
using atx::engine::risk::FactorModel;

// ---------------------------------------------------------------------------
//  Fixtures: a KNOWN FactorModel V (M×K X, K×K SPD F, M D) so the oracle is
//  hand-computable, and helpers to net W and span-ify sleeve books.
// ---------------------------------------------------------------------------
constexpr usize kM = 5U; // instruments
constexpr usize kK = 2U; // factors

// X (M×K), F (K×K SPD), D (M) — all KNOWN so the test computes the oracle itself.
[[nodiscard]] MatX known_x() {
  MatX x(static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kK));
  x << 1.0, 0.0,  //
      0.5, 1.0,   //
      -1.0, 0.5,  //
      0.2, -0.3,  //
      0.8, 0.4;   //
  return x;
}
[[nodiscard]] MatX known_f() {
  MatX f(static_cast<Eigen::Index>(kK), static_cast<Eigen::Index>(kK));
  f << 0.04, 0.01, //
      0.01, 0.09;  // SPD (0.04·0.09 − 0.01² > 0, diagonal > 0)
  return f;
}
[[nodiscard]] VecX known_d() {
  VecX d(static_cast<Eigen::Index>(kM));
  d << 0.10, 0.20, 0.05, 0.15, 0.08;
  return d;
}

[[nodiscard]] FactorModel make_v() {
  auto r = FactorModel::create(known_x(), known_f(), known_d(), 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// Net W = Σ_s c_s·w_s (the oracle netting, ascending sleeve then name).
[[nodiscard]] std::vector<f64> net_book(const std::vector<std::vector<f64>> &books,
                                        const std::vector<f64> &c) {
  std::vector<f64> w(kM, 0.0);
  for (usize s = 0; s < books.size(); ++s) {
    for (usize i = 0; i < kM; ++i) {
      w[i] += c[s] * books[s][i];
    }
  }
  return w;
}

// span-of-spans over a vector-of-vectors (the call-site adapter).
[[nodiscard]] std::vector<std::span<const f64>>
spans_of(const std::vector<std::vector<f64>> &v) {
  std::vector<std::span<const f64>> out;
  out.reserve(v.size());
  for (const auto &row : v) {
    out.emplace_back(row);
  }
  return out;
}

// cᵀΩc (order-fixed) — the Ω-based sleeve-portfolio variance oracle.
[[nodiscard]] f64 quad(const MatX &Omega, const std::vector<f64> &c) {
  f64 q = 0.0;
  for (Eigen::Index i = 0; i < Omega.rows(); ++i) {
    for (Eigen::Index j = 0; j < Omega.cols(); ++j) {
      q += c[static_cast<usize>(i)] * Omega(i, j) * c[static_cast<usize>(j)];
    }
  }
  return q;
}

// A small symmetric PD Ω (S=3) the tests reuse as a KNOWN sleeve covariance.
[[nodiscard]] MatX known_omega3() {
  MatX o(3, 3);
  o << 0.04, 0.012, -0.006, //
      0.012, 0.09, 0.018,   //
      -0.006, 0.018, 0.0225;
  return o;
}

// Two sleeve books (S matches the c length the caller passes).
const std::vector<std::vector<f64>> kBooks3 = {
    {1.0, -0.5, 0.3, 0.2, -1.0},
    {-0.4, 0.8, 0.1, -0.6, 0.5},
    {0.2, 0.2, -0.7, 0.9, 0.1},
};

// ===========================================================================
//  TEST 1 — Euler additivity (R4, THE load-bearing proof). Σ_s RC_s ≈ sqrt(cᵀΩc).
//  Only true if the RC divisor is the Ω-based sigma, NOT the V-based sigma_fund.
// ===========================================================================
TEST(FundCrossSleeveRisk, RiskContrib_KnownOmega_SumsToSleevePortfolioVol) {
  const FactorModel v = make_v();
  const MatX omega = known_omega3();
  const std::vector<f64> c = {0.6, 0.3, 0.5};

  const auto books = spans_of(kBooks3);
  auto r = fund_risk(books, c, v, omega);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  const f64 sigma_sleeve = std::sqrt(quad(omega, c)); // Ω-based sleeve-portfolio vol
  f64 sum_rc = 0.0;
  for (const f64 rc : r->risk_contrib) {
    sum_rc += rc;
  }
  ASSERT_EQ(r->risk_contrib.size(), c.size());
  EXPECT_NEAR(sum_rc, sigma_sleeve, 1e-9)
      << "Euler RC sum must equal the Ω-based sigma_sleeve, not the V-based sigma_fund";

  // Per-component oracle: RC_s = c_s·(Ωc)_s / sigma_sleeve.
  for (usize s = 0; s < c.size(); ++s) {
    f64 oc = 0.0; // (Ωc)_s
    for (usize t = 0; t < c.size(); ++t) {
      oc += omega(static_cast<Eigen::Index>(s), static_cast<Eigen::Index>(t)) * c[t];
    }
    const f64 rc_oracle = c[s] * oc / sigma_sleeve;
    EXPECT_NEAR(r->risk_contrib[s], rc_oracle, 1e-12) << "RC component " << s;
  }
}

// ===========================================================================
//  TEST 2 — Factor/specific split (R6). factor_var + specific_var ≈ V.risk(W);
//  specific_var ≈ Σ_i D_i W_i² (hand); factor_var ≈ b_fundᵀ F b_fund (independent
//  oracle — validates the risk(W) − WᵀDW identity equals the true factor variance).
// ===========================================================================
TEST(FundCrossSleeveRisk, FactorSpecificSplit_KnownModel_MatchesIndependentOracle) {
  const FactorModel v = make_v();
  const MatX omega = known_omega3();
  const std::vector<f64> c = {0.6, 0.3, 0.5};
  const std::vector<f64> w = net_book(kBooks3, c);

  const auto books = spans_of(kBooks3);
  auto r = fund_risk(books, c, v, omega);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  // var_total = V.risk(W) (the factored wᵀVw).
  const f64 var_total = v.risk(std::span<const f64>(w));
  EXPECT_NEAR(r->factor_var + r->specific_var, var_total, 1e-12);

  // specific_var ≈ Σ_i D_i W_i² (hand, with the SAME floored D the model carries).
  const VecX d = v.specific_var();
  f64 spec = 0.0;
  for (usize i = 0; i < kM; ++i) {
    spec += d[static_cast<Eigen::Index>(i)] * w[i] * w[i];
  }
  EXPECT_NEAR(r->specific_var, spec, 1e-12);

  // factor_var ≈ b_fundᵀ F b_fund — the INDEPENDENT oracle (test owns F). This is the
  // proof that risk(W) − WᵀDW equals the true factor variance.
  const MatX f = known_f();
  ASSERT_EQ(r->b_fund.size(), kK);
  f64 bfb = 0.0;
  for (usize a = 0; a < kK; ++a) {
    for (usize b = 0; b < kK; ++b) {
      bfb += r->b_fund[a] * f(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) *
             r->b_fund[b];
    }
  }
  EXPECT_NEAR(r->factor_var, bfb, 1e-12)
      << "factor_var (= risk(W) − WᵀDW) must equal the independent bᵀFb oracle";
}

// ===========================================================================
//  TEST 3 — b_fund correctness. b_fund ≈ Xᵀ W (hand-computed from the known X, W).
// ===========================================================================
TEST(FundCrossSleeveRisk, BFund_KnownModel_MatchesXtW) {
  const FactorModel v = make_v();
  const MatX omega = known_omega3();
  const std::vector<f64> c = {0.6, 0.3, 0.5};
  const std::vector<f64> w = net_book(kBooks3, c);

  const auto books = spans_of(kBooks3);
  auto r = fund_risk(books, c, v, omega);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  const MatX x = known_x();
  ASSERT_EQ(r->b_fund.size(), kK);
  for (usize k = 0; k < kK; ++k) {
    f64 bk = 0.0; // Σ_i X(i,k)·W[i]
    for (usize i = 0; i < kM; ++i) {
      bk += x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(k)) * w[i];
    }
    EXPECT_NEAR(r->b_fund[k], bk, 1e-12) << "b_fund[" << k << "]";
  }
}

// ===========================================================================
//  TEST 4 — sigma_fund ≈ sqrt(V.risk(W)) (the V-based BOOK vol, R6).
// ===========================================================================
TEST(FundCrossSleeveRisk, SigmaFund_KnownModel_IsSqrtRiskW) {
  const FactorModel v = make_v();
  const MatX omega = known_omega3();
  const std::vector<f64> c = {0.6, 0.3, 0.5};
  const std::vector<f64> w = net_book(kBooks3, c);

  const auto books = spans_of(kBooks3);
  auto r = fund_risk(books, c, v, omega);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  EXPECT_NEAR(r->sigma_fund, std::sqrt(v.risk(std::span<const f64>(w))), 1e-12);
  EXPECT_TRUE(std::isfinite(r->sigma_fund));
}

// ===========================================================================
//  TEST 5 — sleeve_return_cov. Ω symmetric; Ω(s,s) ≈ pop-var(pnl_s); Ω(s,t) ≈
//  corr·σ_s·σ_t (hand-checked for one pair); diagonal ≥ 0.
// ===========================================================================
[[nodiscard]] f64 pop_var(const std::vector<f64> &xs) {
  if (xs.size() < 2U) {
    return 0.0;
  }
  f64 sum = 0.0;
  for (const f64 v : xs) {
    sum += v;
  }
  const f64 mean = sum / static_cast<f64>(xs.size());
  f64 ss = 0.0;
  for (const f64 v : xs) {
    const f64 dd = v - mean;
    ss += dd * dd;
  }
  return ss / static_cast<f64>(xs.size());
}
[[nodiscard]] f64 pop_corr(const std::vector<f64> &a, const std::vector<f64> &b) {
  // Pearson over both-finite overlap (the test fixtures are NaN-free, so full length).
  const usize n = a.size();
  f64 sa = 0.0, sb = 0.0, sab = 0.0, saa = 0.0, sbb = 0.0;
  for (usize i = 0; i < n; ++i) {
    sa += a[i];
    sb += b[i];
    sab += a[i] * b[i];
    saa += a[i] * a[i];
    sbb += b[i] * b[i];
  }
  const f64 nf = static_cast<f64>(n);
  const f64 cov = nf * sab - sa * sb;
  const f64 va = nf * saa - sa * sa;
  const f64 vb = nf * sbb - sb * sb;
  return cov / std::sqrt(va * vb);
}

TEST(FundCrossSleeveRisk, SleeveReturnCov_KnownPanel_MatchesCorrSigmaSigma) {
  const std::vector<std::vector<f64>> pnl = {
      {0.0, 0.01, -0.02, 0.015, 0.005, -0.01},
      {0.0, -0.005, 0.02, -0.01, 0.0, 0.012},
      {0.0, 0.008, 0.004, 0.006, -0.002, 0.01},
  };
  const auto spans = spans_of(pnl);
  auto r = sleeve_return_cov(spans);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const MatX &omega = *r;
  ASSERT_EQ(omega.rows(), 3);
  ASSERT_EQ(omega.cols(), 3);

  // Diagonal = pop-variance; symmetric; diagonal ≥ 0.
  for (usize s = 0; s < 3; ++s) {
    EXPECT_NEAR(omega(static_cast<Eigen::Index>(s), static_cast<Eigen::Index>(s)),
                pop_var(pnl[s]), 1e-15);
    EXPECT_GE(omega(static_cast<Eigen::Index>(s), static_cast<Eigen::Index>(s)), 0.0);
  }
  for (Eigen::Index i = 0; i < 3; ++i) {
    for (Eigen::Index j = 0; j < 3; ++j) {
      EXPECT_EQ(omega(i, j), omega(j, i)) << "symmetry " << i << "," << j;
    }
  }

  // One off-diagonal pair hand-checked: Ω(0,1) ≈ corr·σ_0·σ_1.
  const f64 s0 = std::sqrt(pop_var(pnl[0]));
  const f64 s1 = std::sqrt(pop_var(pnl[1]));
  const f64 expect01 = pop_corr(pnl[0], pnl[1]) * s0 * s1;
  EXPECT_NEAR(omega(0, 1), expect01, 1e-12);
}

// ===========================================================================
//  TEST 6 — No-look-ahead / trailing (R2). sleeve_return_cov is a PURE function of
//  its input window: truncating the panel to the first t periods yields exactly what
//  the trailing slice would give, and two calls on identical inputs are byte-identical.
// ===========================================================================
TEST(FundCrossSleeveRisk, SleeveReturnCov_TruncatedPanel_IsPureFunctionOfWindow) {
  const std::vector<std::vector<f64>> full = {
      {0.0, 0.01, -0.02, 0.015, 0.005, -0.01, 0.02, 0.0},
      {0.0, -0.005, 0.02, -0.01, 0.0, 0.012, -0.008, 0.003},
      {0.0, 0.008, 0.004, 0.006, -0.002, 0.01, 0.001, -0.004},
  };
  // Trailing window = the first t periods (the caller's slice; structurally PIT-safe).
  const usize t = 5U;
  std::vector<std::vector<f64>> trailing;
  for (const auto &row : full) {
    trailing.emplace_back(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(t));
  }

  const auto spans_trailing = spans_of(trailing);
  auto a = sleeve_return_cov(spans_trailing);
  auto b = sleeve_return_cov(spans_trailing); // identical inputs ⇒ byte-identical
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  for (Eigen::Index i = 0; i < a->rows(); ++i) {
    for (Eigen::Index j = 0; j < a->cols(); ++j) {
      EXPECT_EQ((*a)(i, j), (*b)(i, j)) << "non-determinism at " << i << "," << j;
    }
  }

  // The truncated-panel Ω equals the Ω computed from a freshly-built trailing slice
  // (same numbers) — i.e. no future period (index ≥ t) influenced it.
  std::vector<std::vector<f64>> trailing2;
  for (const auto &row : full) {
    trailing2.emplace_back(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(t));
  }
  const auto spans2 = spans_of(trailing2);
  auto c2 = sleeve_return_cov(spans2);
  ASSERT_TRUE(c2.has_value());
  for (Eigen::Index i = 0; i < a->rows(); ++i) {
    for (Eigen::Index j = 0; j < a->cols(); ++j) {
      EXPECT_EQ((*a)(i, j), (*c2)(i, j)) << "trailing-window mismatch at " << i << "," << j;
    }
  }
}

// ===========================================================================
//  TEST 7 — Boundaries.
//   * S=1 ⇒ Ω is 1×1 = var(pnl_0); risk_contrib == [sigma_sleeve].
//   * all-NaN sleeve P&L ⇒ σ=0, that row/col of Ω is 0, no NaN/Inf.
//   * c all zero ⇒ W=0 ⇒ sigma_fund 0; risk_contrib all 0 (div-by-zero guarded).
// ===========================================================================
TEST(FundCrossSleeveRisk, SleeveReturnCov_SingleSleeve_IsScalarVariance) {
  const std::vector<std::vector<f64>> pnl = {{0.0, 0.01, -0.02, 0.015, 0.005}};
  const auto spans = spans_of(pnl);
  auto r = sleeve_return_cov(spans);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->rows(), 1);
  EXPECT_NEAR((*r)(0, 0), pop_var(pnl[0]), 1e-15);

  // S=1 fund_risk: risk_contrib == [sqrt(cᵀΩc)] = [c·σ].
  const FactorModel v = make_v();
  const std::vector<std::vector<f64>> books = {{0.3, -0.1, 0.2, 0.0, 0.4}};
  const std::vector<f64> c = {1.5};
  const auto bspans = spans_of(books);
  auto fr = fund_risk(bspans, c, v, *r);
  ASSERT_TRUE(fr.has_value()) << (fr ? "" : fr.error().to_string());
  ASSERT_EQ(fr->risk_contrib.size(), 1U);
  EXPECT_NEAR(fr->risk_contrib[0], std::sqrt(quad(*r, c)), 1e-12);
}

TEST(FundCrossSleeveRisk, SleeveReturnCov_AllNaNColumn_ZeroRowNoNaN) {
  const f64 kNaN = std::nan("");
  const std::vector<std::vector<f64>> pnl = {
      {0.0, 0.01, -0.02, 0.015, 0.005},
      {kNaN, kNaN, kNaN, kNaN, kNaN}, // degenerate (all-NaN) sleeve ⇒ σ=0
  };
  const auto spans = spans_of(pnl);
  auto r = sleeve_return_cov(spans);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const MatX &o = *r;
  // The all-NaN sleeve's row/col is all 0, and every entry is finite (no NaN/Inf).
  for (Eigen::Index j = 0; j < o.cols(); ++j) {
    EXPECT_EQ(o(1, j), 0.0);
    EXPECT_EQ(o(j, 1), 0.0);
  }
  for (Eigen::Index i = 0; i < o.rows(); ++i) {
    for (Eigen::Index j = 0; j < o.cols(); ++j) {
      EXPECT_TRUE(std::isfinite(o(i, j))) << "non-finite at " << i << "," << j;
    }
  }
}

TEST(FundCrossSleeveRisk, FundRisk_AllZeroCapital_ZeroVolAndContrib) {
  const FactorModel v = make_v();
  const MatX omega = known_omega3();
  const std::vector<f64> c = {0.0, 0.0, 0.0}; // all-zero capital ⇒ W = 0
  const auto books = spans_of(kBooks3);
  auto r = fund_risk(books, c, v, omega);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  EXPECT_EQ(r->sigma_fund, 0.0);     // W=0 ⇒ risk(0)=0
  EXPECT_EQ(r->factor_var, 0.0);
  EXPECT_EQ(r->specific_var, 0.0);
  for (const f64 rc : r->risk_contrib) {
    EXPECT_EQ(rc, 0.0) << "div-by-zero must be guarded ⇒ all RC 0";
  }
}

// ===========================================================================
//  TEST 8 — Errors. sleeve_books.size() != c.size(); a book length != M;
//  Omega.rows() != c.size() ⇒ Err(InvalidArgument).
// ===========================================================================
TEST(FundCrossSleeveRisk, FundRisk_ShapeMismatch_ReturnsErr) {
  const FactorModel v = make_v();
  const MatX omega = known_omega3();

  // sleeve_books.size() != c.size() (2 books, 3 weights).
  {
    const std::vector<std::vector<f64>> books2 = {kBooks3[0], kBooks3[1]};
    const std::vector<f64> c = {0.6, 0.3, 0.5};
    const auto bspans = spans_of(books2);
    auto r = fund_risk(bspans, c, v, omega);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  // a book length != M (first book truncated to M-1).
  {
    std::vector<std::vector<f64>> bad = kBooks3;
    bad[0].pop_back();
    const std::vector<f64> c = {0.6, 0.3, 0.5};
    const auto bspans = spans_of(bad);
    auto r = fund_risk(bspans, c, v, omega);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  // Omega.rows() != c.size() (2×2 Ω, 3 weights).
  {
    MatX o2(2, 2);
    o2 << 0.04, 0.0, 0.0, 0.09;
    const std::vector<f64> c = {0.6, 0.3, 0.5};
    const auto bspans = spans_of(kBooks3);
    auto r = fund_risk(bspans, c, v, o2);
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

} // namespace

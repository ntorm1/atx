// risk_optimizer_test.cpp — P4-9: turnover-penalized risk-aware PortfolioOptimizer.
//
// PortfolioOptimizer solves
//   maximize_w  αᵀw − λ·wᵀVw − κ·‖w − w_prev‖₁
//   subject to  Σ w = 0 (dollar-neutral),  Σ|w| ≤ L (gross),  |w_i| ≤ cap (per-name)
// where α is the combined mega-alpha (length-M), V is the FACTORED risk::FactorModel
// (applied via apply_inverse / risk — never materialized M×M), and the inequality
// terms + the L1 turnover penalty are handled by a DETERMINISTIC fixed-iteration
// projected/proximal loop (cfg.max_iters, no convergence early-exit, §3.2).
//
// Coverage (the 8 contract pins + boundaries):
//   1. λ=κ=0, caps off ⇒ recovers gross_normalize(demean(α)) to 1e-9 (THE regression
//      check; incl. a NaN-hole variant where NaN cells get 0 and are excluded).
//   2. κ=0 ⇒ pure risk-aware mean-variance book (no turnover term).
//   3. Raising λ shrinks gross exposure to high-variance names.
//   4. κ>0 reduces turnover vs w_prev relative to κ=0.
//   5. Name cap never exceeded: max_i |w_i| ≤ name_cap (+1e-9).
//   6. Σw=0 and Σ|w| ≤ L hold (within 1e-9).
//   7. Determinism: same inputs ⇒ bitwise-identical weights across re-runs.
//   8. Apply path uses the factored V (validated indirectly: it solves with a real
//      FactorModel and never builds a dense V).
//   Boundaries: w_prev = w* (no trade), single-name degenerate (all-zero),
//   cap below equal-weight (all pinned), empty w_prev (turnover from flat).

#include <cmath>   // std::isnan, std::isfinite, std::fabs
#include <cstdint> // fixed-width
#include <limits>  // std::numeric_limits (NaN-hole / finite checks)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include <bit> // std::bit_cast (wired-path byte-pin)

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include "atx/engine/risk/reference_data.hpp"

namespace atxtest_risk_optimizer_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::CapacityRef;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorModel;
using atx::engine::risk::GrossNet;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::ParticipationCap;
using atx::engine::risk::PortfolioOptimizer;
using atx::engine::risk::PositionCap;

// Construct a FactorModel from raw buffers (copies). Asserts success.
[[nodiscard]] FactorModel make_model(const MatX &x, const MatX &f, const VecX &d, usize fb = 0U,
                                     usize fe = 10U) {
  auto r = FactorModel::create(x, f, d, fb, fe);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// A simple "near-identity" risk model: K=1, tiny exposures, unit-ish specific
// variances. V ≈ diag(d) + small rank-1, so V⁻¹ ≈ I direction-wise. Used where we
// want a benign V that does not dominate the alpha direction.
[[nodiscard]] FactorModel benign_model(usize m) {
  MatX x = MatX::Zero(static_cast<Eigen::Index>(m), 1);
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    x(i, 0) = 0.0; // zero factor exposure ⇒ V == diag(d), purely specific
  }
  MatX f(1, 1);
  f << 0.01;
  VecX d = VecX::Constant(static_cast<Eigen::Index>(m), 1.0); // unit specific var
  return make_model(x, f, d);
}

// L1 norm of a weight vector.
[[nodiscard]] f64 l1(const std::vector<f64> &w) {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += std::fabs(x);
  }
  return s;
}

// Σw (signed) of a weight vector.
[[nodiscard]] f64 sum(const std::vector<f64> &w) {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += x;
  }
  return s;
}

// max_i |w_i|.
[[nodiscard]] f64 max_abs(const std::vector<f64> &w) {
  f64 m = 0.0;
  for (const f64 x : w) {
    m = std::fabs(x) > m ? std::fabs(x) : m;
  }
  return m;
}

// Σ|w − w_prev| (turnover). w_prev empty ⇒ measured from flat (0).
[[nodiscard]] f64 turnover(const std::vector<f64> &w, const std::vector<f64> &w_prev) {
  f64 s = 0.0;
  for (usize i = 0; i < w.size(); ++i) {
    const f64 p = i < w_prev.size() ? w_prev[i] : 0.0;
    s += std::fabs(w[i] - p);
  }
  return s;
}

// The WeightPolicy tail oracle: demean(α) with NaN→0 exclusion, then gross-normalize
// so Σ|w| == L. This is the EXACT target of pin #1 (the λ=κ=0, caps-off limit).
[[nodiscard]] std::vector<f64> weight_policy_book(std::span<const f64> alpha, f64 leverage) {
  const usize n = alpha.size();
  std::vector<f64> w(n, 0.0);
  // Mean over the live (non-NaN) cells only.
  f64 s = 0.0;
  usize live = 0U;
  for (usize i = 0; i < n; ++i) {
    if (!std::isnan(alpha[i])) {
      s += alpha[i];
      ++live;
    }
  }
  if (live == 0U) {
    return w;
  }
  const f64 mean = s / static_cast<f64>(live);
  for (usize i = 0; i < n; ++i) {
    w[i] = std::isnan(alpha[i]) ? 0.0 : alpha[i] - mean;
  }
  // gross-normalize to leverage.
  f64 g = 0.0;
  for (const f64 x : w) {
    g += std::fabs(x);
  }
  if (g == 0.0) {
    return w;
  }
  const f64 scale = leverage / g;
  for (f64 &x : w) {
    x *= scale;
  }
  return w;
}

// ===========================================================================
//  PIN #1 — λ=κ=0, caps off ⇒ recovers gross_normalize(demean(α)) (1e-9)
// ===========================================================================
TEST(RiskOptimizer, LambdaKappaZeroRecoversWeightPolicyBook) {
  const std::vector<f64> alpha = {2.0, -1.0, 0.5, 3.0, -0.5};
  const FactorModel v = benign_model(alpha.size());

  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.turnover_penalty = 0.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0; // ≥ L ⇒ never binds
  cfg.dollar_neutral = true;
  PortfolioOptimizer opt{cfg};

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  const std::vector<f64> w = *r;
  const std::vector<f64> want = weight_policy_book(std::span<const f64>(alpha), cfg.gross_leverage);

  ASSERT_EQ(w.size(), want.size());
  for (usize i = 0; i < w.size(); ++i) {
    EXPECT_NEAR(w[i], want[i], 1e-9) << "i=" << i;
  }
  EXPECT_NEAR(sum(w), 0.0, 1e-9);
  EXPECT_NEAR(l1(w), cfg.gross_leverage, 1e-9);
}

// Pin #1 with a different leverage to confirm the gross scale tracks L.
TEST(RiskOptimizer, LambdaKappaZeroRecoversWeightPolicyBookLeverageTwo) {
  const std::vector<f64> alpha = {1.0, 2.0, -3.0, 0.0, 4.0, -1.5};
  const FactorModel v = benign_model(alpha.size());

  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.gross_leverage = 2.0;
  cfg.name_cap = 10.0;
  PortfolioOptimizer opt{cfg};

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value());
  const std::vector<f64> w = *r;
  const std::vector<f64> want = weight_policy_book(std::span<const f64>(alpha), 2.0);
  for (usize i = 0; i < w.size(); ++i) {
    EXPECT_NEAR(w[i], want[i], 1e-9) << "i=" << i;
  }
  EXPECT_NEAR(l1(w), 2.0, 1e-9);
}

// Pin #1, NaN-hole variant: a NaN alpha cell gets 0 weight and is EXCLUDED from the
// demean / Σ reductions (mirrors WeightPolicy's NaN→0 exclusion).
TEST(RiskOptimizer, LambdaKappaZeroRecoversWeightPolicyBookWithNaNHoles) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<f64> alpha = {2.0, nan, 0.5, 3.0, nan, -1.0};
  const FactorModel v = benign_model(alpha.size());

  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  PortfolioOptimizer opt{cfg};

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value());
  const std::vector<f64> w = *r;
  const std::vector<f64> want = weight_policy_book(std::span<const f64>(alpha), 1.0);

  ASSERT_EQ(w.size(), want.size());
  for (usize i = 0; i < w.size(); ++i) {
    EXPECT_NEAR(w[i], want[i], 1e-9) << "i=" << i;
  }
  // NaN cells are exactly zero.
  EXPECT_EQ(w[1], 0.0);
  EXPECT_EQ(w[4], 0.0);
  EXPECT_NEAR(sum(w), 0.0, 1e-9);
  EXPECT_NEAR(l1(w), 1.0, 1e-9);
}

// ===========================================================================
//  PIN #2 — κ=0 ⇒ pure risk-aware mean-variance book (no turnover term)
// ===========================================================================
TEST(RiskOptimizer, KappaZeroIsTurnoverFree) {
  // With κ=0 the result must NOT depend on w_prev: solving with two different
  // previous books yields the SAME weights (the turnover term is inert).
  const std::vector<f64> alpha = {1.0, -2.0, 0.5, 1.5, -1.0};
  MatX x(5, 1);
  x << 1.0, 0.5, -1.0, 0.2, 0.8;
  MatX f(1, 1);
  f << 0.04;
  VecX d(5);
  d << 0.10, 0.20, 0.05, 0.15, 0.12;
  const FactorModel v = make_model(x, f, d);

  OptimizerConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.turnover_penalty = 0.0; // κ=0
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  PortfolioOptimizer opt{cfg};

  const std::vector<f64> prev_a = {0.5, -0.5, 0.0, 0.0, 0.0};
  const std::vector<f64> prev_b = {-0.2, 0.2, -0.4, 0.4, 0.0};
  auto ra = opt.solve(std::span<const f64>(alpha), v, std::span<const f64>(prev_a));
  auto rb = opt.solve(std::span<const f64>(alpha), v, std::span<const f64>(prev_b));
  ASSERT_TRUE(ra.has_value());
  ASSERT_TRUE(rb.has_value());
  for (usize i = 0; i < alpha.size(); ++i) {
    EXPECT_EQ((*ra)[i], (*rb)[i]) << "i=" << i; // κ=0 ⇒ w_prev-independent (bitwise)
  }
}

// ===========================================================================
//  PIN #3 — raising λ shrinks gross exposure to high-variance names
// ===========================================================================
TEST(RiskOptimizer, RaisingLambdaShrinksHighVarianceNames) {
  // Two names with equal-magnitude alpha but very different specific variance.
  // As λ rises the high-variance name's |weight| must fall relative to the
  // low-variance name's (mean-variance penalizes its risk).
  const std::vector<f64> alpha = {1.0, -1.0, 1.0, -1.0};
  MatX x = MatX::Zero(4, 1); // zero exposure ⇒ V = diag(d), risk is purely specific
  MatX fm(1, 1);
  fm << 0.01;
  VecX d(4);
  d << 0.05, 0.05, 1.00, 1.00; // names 2,3 are HIGH variance
  const FactorModel v = make_model(x, fm, d);

  OptimizerConfig lo;
  lo.risk_aversion = 0.0;
  lo.gross_leverage = 1.0;
  lo.name_cap = 10.0;
  OptimizerConfig hi = lo;
  hi.risk_aversion = 50.0;

  auto r_lo = PortfolioOptimizer{lo}.solve(std::span<const f64>(alpha), v, {});
  auto r_hi = PortfolioOptimizer{hi}.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r_lo.has_value());
  ASSERT_TRUE(r_hi.has_value());
  const std::vector<f64> &wl = *r_lo;
  const std::vector<f64> &wh = *r_hi;

  // gross on the HIGH-variance pair (indices 2,3) shrinks under high λ.
  const f64 hi_gross_lo = std::fabs(wl[2]) + std::fabs(wl[3]);
  const f64 hi_gross_hi = std::fabs(wh[2]) + std::fabs(wh[3]);
  EXPECT_LT(hi_gross_hi, hi_gross_lo - 1e-6) << "high-λ must shrink high-variance gross";
  // and the LOW-variance pair (0,1) gains relative share.
  const f64 lo_gross_lo = std::fabs(wl[0]) + std::fabs(wl[1]);
  const f64 lo_gross_hi = std::fabs(wh[0]) + std::fabs(wh[1]);
  EXPECT_GT(lo_gross_hi, lo_gross_lo - 1e-9);
}

// ===========================================================================
//  PIN #4 — κ>0 reduces turnover vs w_prev relative to κ=0
// ===========================================================================
TEST(RiskOptimizer, KappaReducesTurnover) {
  const std::vector<f64> alpha = {2.0, -1.0, 0.5, 1.5, -2.0, 0.8};
  MatX x = MatX::Zero(6, 1);
  MatX f(1, 1);
  f << 0.01;
  VecX d = VecX::Constant(6, 1.0);
  const FactorModel v = make_model(x, f, d);
  // A previous book deliberately different from the alpha-optimal direction.
  const std::vector<f64> w_prev = {-0.3, 0.3, -0.2, 0.1, 0.2, -0.1};

  OptimizerConfig base;
  base.risk_aversion = 1.0;
  base.gross_leverage = 1.0;
  base.name_cap = 10.0;
  OptimizerConfig with_k = base;
  with_k.turnover_penalty = 0.5;

  auto r0 = PortfolioOptimizer{base}.solve(std::span<const f64>(alpha), v,
                                           std::span<const f64>(w_prev));
  auto rk = PortfolioOptimizer{with_k}.solve(std::span<const f64>(alpha), v,
                                             std::span<const f64>(w_prev));
  ASSERT_TRUE(r0.has_value());
  ASSERT_TRUE(rk.has_value());
  const f64 t0 = turnover(*r0, w_prev);
  const f64 tk = turnover(*rk, w_prev);
  EXPECT_LT(tk, t0 - 1e-9) << "κ>0 must reduce turnover vs κ=0";
}

// ===========================================================================
//  PIN #5 — name cap never exceeded (feasible cap)
// ===========================================================================
TEST(RiskOptimizer, NameCapNeverExceeded) {
  const std::vector<f64> alpha = {5.0, -4.0, 3.0, -2.0, 1.0, -0.5};
  const FactorModel v = benign_model(alpha.size());

  OptimizerConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.25; // feasible: 0.25 * 6 = 1.5 ≥ L = 1.0
  PortfolioOptimizer opt{cfg};

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value());
  EXPECT_LE(max_abs(*r), cfg.name_cap + 1e-9);
}

// ===========================================================================
//  PIN #6 — Σw=0 and Σ|w| ≤ L hold
// ===========================================================================
TEST(RiskOptimizer, DollarNeutralAndGrossLeverageInvariants) {
  const std::vector<f64> alpha = {1.3, -0.7, 2.1, -1.4, 0.6, -0.9, 0.2};
  MatX x(7, 2);
  x << 1.0, 0.0, 0.5, 1.0, -1.0, 0.5, 0.2, -0.7, 0.9, 0.3, -0.4, 0.8, 0.1, -0.2;
  MatX f(2, 2);
  f << 0.05, 0.01, 0.01, 0.03;
  VecX d(7);
  d << 0.10, 0.15, 0.08, 0.20, 0.12, 0.18, 0.09;
  const FactorModel v = make_model(x, f, d);

  OptimizerConfig cfg;
  cfg.risk_aversion = 2.0;
  cfg.turnover_penalty = 0.1;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.5;
  cfg.dollar_neutral = true;
  PortfolioOptimizer opt{cfg};

  const std::vector<f64> w_prev = {0.1, -0.1, 0.2, -0.2, 0.0, 0.1, -0.1};
  auto r = opt.solve(std::span<const f64>(alpha), v, std::span<const f64>(w_prev));
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(sum(*r), 0.0, 1e-9);
  EXPECT_LE(l1(*r), cfg.gross_leverage + 1e-9);
  EXPECT_LE(max_abs(*r), cfg.name_cap + 1e-9);
}

// ===========================================================================
//  PIN #7 — determinism: re-run is bitwise-identical
// ===========================================================================
TEST(RiskOptimizer, DeterministicAcrossReruns) {
  const std::vector<f64> alpha = {1.0, -2.0, 0.5, 1.5, -1.0, 0.7, -0.3, 2.0};
  MatX x = MatX::Zero(8, 1);
  for (Eigen::Index i = 0; i < 8; ++i) {
    x(i, 0) = 0.1 * static_cast<f64>(i) - 0.4;
  }
  MatX f(1, 1);
  f << 0.04;
  VecX d = VecX::Constant(8, 0.1);
  const FactorModel v = make_model(x, f, d);

  OptimizerConfig cfg;
  cfg.risk_aversion = 1.5;
  cfg.turnover_penalty = 0.3;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.4;
  PortfolioOptimizer opt{cfg};

  const std::vector<f64> w_prev = {0.1, -0.1, 0.0, 0.2, -0.2, 0.1, 0.0, -0.1};
  auto r1 = opt.solve(std::span<const f64>(alpha), v, std::span<const f64>(w_prev));
  auto r2 = opt.solve(std::span<const f64>(alpha), v, std::span<const f64>(w_prev));
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  ASSERT_EQ(r1->size(), r2->size());
  for (usize i = 0; i < r1->size(); ++i) {
    EXPECT_EQ((*r1)[i], (*r2)[i]) << "i=" << i; // bitwise
  }
}

// ===========================================================================
//  Dimension-mismatch errors
// ===========================================================================
TEST(RiskOptimizer, ErrsOnAlphaDimMismatch) {
  const std::vector<f64> alpha = {1.0, 2.0, 3.0}; // length 3
  const FactorModel v = benign_model(4U);          // M = 4
  PortfolioOptimizer opt;
  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  EXPECT_FALSE(r.has_value());
}

TEST(RiskOptimizer, ErrsOnWPrevDimMismatch) {
  const std::vector<f64> alpha = {1.0, 2.0, 3.0, 4.0};
  const FactorModel v = benign_model(4U);
  const std::vector<f64> w_prev = {0.1, -0.1}; // non-empty but wrong length
  PortfolioOptimizer opt;
  auto r = opt.solve(std::span<const f64>(alpha), v, std::span<const f64>(w_prev));
  EXPECT_FALSE(r.has_value());
}

// ===========================================================================
//  BOUNDARY — w_prev = w* (the optimum) ⇒ no trade (κ term contributes 0)
// ===========================================================================
TEST(RiskOptimizer, WPrevAtOptimumNoTrade) {
  const std::vector<f64> alpha = {1.0, -2.0, 0.5, 1.5, -1.0};
  MatX x = MatX::Zero(5, 1);
  MatX f(1, 1);
  f << 0.01;
  VecX d = VecX::Constant(5, 0.5);
  const FactorModel v = make_model(x, f, d);

  OptimizerConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  PortfolioOptimizer opt{cfg};

  // First solve with κ=0 to find w* (no turnover term).
  auto r_star = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r_star.has_value());
  const std::vector<f64> w_star = *r_star;

  // Now turn on κ and pass w_prev = w*. Since we are already at the optimum, the
  // turnover term contributes 0 and the result is unchanged vs the κ=0 book.
  OptimizerConfig cfg_k = cfg;
  cfg_k.turnover_penalty = 0.5;
  auto r_k = PortfolioOptimizer{cfg_k}.solve(std::span<const f64>(alpha), v,
                                             std::span<const f64>(w_star));
  ASSERT_TRUE(r_k.has_value());
  for (usize i = 0; i < w_star.size(); ++i) {
    EXPECT_NEAR((*r_k)[i], w_star[i], 1e-9) << "i=" << i; // no trade away from w*
  }
}

// ===========================================================================
//  BOUNDARY — single name + dollar_neutral ⇒ all-zero (demean of one elt is 0)
// ===========================================================================
TEST(RiskOptimizer, SingleNameDollarNeutralIsZero) {
  const std::vector<f64> alpha = {3.0};
  const FactorModel v = benign_model(1U);

  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.dollar_neutral = true;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  PortfolioOptimizer opt{cfg};

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 1U);
  EXPECT_NEAR((*r)[0], 0.0, 1e-12);
}

// ===========================================================================
//  BOUNDARY — cap below equal-weight ⇒ all names pinned at the cap
// ===========================================================================
TEST(RiskOptimizer, CapBelowEqualWeightAllPinned) {
  // 4 names, L=1.0, equal-weight |w| would be 0.25. A cap of 0.20 < 0.25 is
  // INFEASIBLE for the full gross, so every name pins at the cap.
  const std::vector<f64> alpha = {1.0, -1.0, 1.0, -1.0};
  const FactorModel v = benign_model(4U);

  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.20;
  PortfolioOptimizer opt{cfg};

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value());
  for (const f64 w : *r) {
    EXPECT_NEAR(std::fabs(w), cfg.name_cap, 1e-9); // every name pinned at the cap
  }
  EXPECT_NEAR(sum(*r), 0.0, 1e-9);
}

// ===========================================================================
//  BOUNDARY — empty w_prev ⇒ turnover measured from flat (no crash, κ acts)
// ===========================================================================
TEST(RiskOptimizer, EmptyWPrevTreatedAsFlat) {
  const std::vector<f64> alpha = {2.0, -1.0, 0.5, 1.5, -2.0};
  const FactorModel v = benign_model(alpha.size());

  OptimizerConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.turnover_penalty = 0.2;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  PortfolioOptimizer opt{cfg};

  // Empty w_prev must be treated as a flat (all-zero) previous book of length M.
  auto r_empty = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r_empty.has_value());
  const std::vector<f64> flat(alpha.size(), 0.0);
  auto r_flat = opt.solve(std::span<const f64>(alpha), v, std::span<const f64>(flat));
  ASSERT_TRUE(r_flat.has_value());
  for (usize i = 0; i < alpha.size(); ++i) {
    EXPECT_EQ((*r_empty)[i], (*r_flat)[i]) << "i=" << i; // empty == flat
  }
}

// ===========================================================================
//  All outputs are finite (no NaN/inf leaks through the loop).
// ===========================================================================
TEST(RiskOptimizer, OutputsAreFinite) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<f64> alpha = {2.0, nan, 0.5, 3.0, -1.0};
  MatX x = MatX::Zero(5, 1);
  MatX f(1, 1);
  f << 0.04;
  VecX d = VecX::Constant(5, 0.3);
  const FactorModel v = make_model(x, f, d);

  OptimizerConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.turnover_penalty = 0.2;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.5;
  PortfolioOptimizer opt{cfg};

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value());
  for (const f64 w : *r) {
    EXPECT_TRUE(std::isfinite(w));
  }
}


// ===========================================================================
//  S8.4 — wired-path dispatch (G-PIN). PortfolioOptimizer::solve gains a minimal-
//  vs-augmented dispatch: a MINIMAL constraint set (GrossNet [+ PositionCap]) keeps
//  the as-built projected/proximal fast path BYTE-IDENTICAL; any extra constraint
//  routes through the ConstrainedQpSolver. These tests re-prove the relevant pins on
//  the newly-wired path: attaching a minimal ConstraintSet is bit-for-bit the same
//  book as the no-constraints fast path (so pins #1-#4 + NaN + empty-w_prev, which
//  the fast path already passes, are preserved through the dispatch).
// ===========================================================================

// Helper: solve with an attached minimal ConstraintSet (GrossNet + PositionCap)
// matching cfg, and assert the book is BYTE-IDENTICAL to the un-attached fast path.
void expect_minimal_dispatch_byte_identical(const OptimizerConfig& cfg,
                                            std::span<const f64> alpha, const FactorModel& v,
                                            std::span<const f64> w_prev) {
  // Fast path: no ConstraintSet attached (the as-built behavior).
  PortfolioOptimizer fast{cfg};
  auto rf = fast.solve(alpha, v, w_prev);
  ASSERT_TRUE(rf.has_value()) << (rf ? "" : rf.error().to_string());

  // Wired minimal path: a ConstraintSet carrying ONLY GrossNet + PositionCap.
  PortfolioOptimizer wired{cfg};
  ConstraintSet cs;
  cs.gross = GrossNet{cfg.gross_leverage, cfg.dollar_neutral};
  cs.pos = PositionCap{cfg.name_cap};
  wired.constraints = cs;
  auto rw = wired.solve(alpha, v, w_prev);
  ASSERT_TRUE(rw.has_value()) << (rw ? "" : rw.error().to_string());

  ASSERT_EQ(rf->size(), rw->size());
  for (usize i = 0; i < rf->size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>((*rf)[i]), std::bit_cast<std::uint64_t>((*rw)[i]))
        << "BYTE DIVERGENCE at name " << i;
  }
}

// Pin #1 (λ=κ=0) on the wired minimal path.
TEST(RiskOptimizer, WiredMinimalDispatch_Pin1_ByteIdentical) {
  const std::vector<f64> alpha = {2.0, -1.0, 0.5, 3.0, -0.5};
  const FactorModel v = benign_model(alpha.size());
  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.turnover_penalty = 0.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  expect_minimal_dispatch_byte_identical(cfg, std::span<const f64>(alpha), v, {});
}

// Pin (NaN-hole) on the wired minimal path.
TEST(RiskOptimizer, WiredMinimalDispatch_NaNHole_ByteIdentical) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<f64> alpha = {2.0, nan, 0.5, 3.0, nan, -1.0};
  const FactorModel v = benign_model(alpha.size());
  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  expect_minimal_dispatch_byte_identical(cfg, std::span<const f64>(alpha), v, {});
}

// Pins #2/#4 (κ>0) + empty-w_prev on the wired minimal path: a real risk-aware,
// turnover-penalized, cap-binding solve with a w_prev still reduces bit-for-bit.
TEST(RiskOptimizer, WiredMinimalDispatch_RiskAwareTurnover_ByteIdentical) {
  const std::vector<f64> alpha = {1.3, -0.7, 2.1, -1.4, 0.6, -0.9, 0.2};
  MatX x(7, 2);
  x << 1.0, 0.0, 0.5, 1.0, -1.0, 0.5, 0.2, -0.7, 0.9, 0.3, -0.4, 0.8, 0.1, -0.2;
  MatX f(2, 2);
  f << 0.05, 0.01, 0.01, 0.03;
  VecX d(7);
  d << 0.10, 0.15, 0.08, 0.20, 0.12, 0.18, 0.09;
  const FactorModel v = make_model(x, f, d);
  OptimizerConfig cfg;
  cfg.risk_aversion = 2.0;
  cfg.turnover_penalty = 0.1;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 0.5;
  const std::vector<f64> w_prev = {0.1, -0.1, 0.2, -0.2, 0.0, 0.1, -0.1};
  expect_minimal_dispatch_byte_identical(cfg, std::span<const f64>(alpha), v,
                                         std::span<const f64>(w_prev));
  // empty-w_prev boundary also reduces.
  expect_minimal_dispatch_byte_identical(cfg, std::span<const f64>(alpha), v, {});
}

// Pin #3 (λ>0) on the wired minimal path.
TEST(RiskOptimizer, WiredMinimalDispatch_Pin3LambdaTilt_ByteIdentical) {
  const std::vector<f64> alpha = {1.0, -1.0, 1.0, -1.0};
  MatX x = MatX::Zero(4, 1);
  MatX fm(1, 1);
  fm << 0.01;
  VecX d(4);
  d << 0.05, 0.05, 1.00, 1.00;
  const FactorModel v = make_model(x, fm, d);
  OptimizerConfig cfg;
  cfg.risk_aversion = 50.0;
  cfg.gross_leverage = 1.0;
  cfg.name_cap = 10.0;
  expect_minimal_dispatch_byte_identical(cfg, std::span<const f64>(alpha), v, {});
}

// Fix round 1 / Finding #1: a MINIMAL ConstraintSet whose gross_leverage AND
// name_cap DIFFER from cfg must be HONORED (mirrors the MultiHorizon translation),
// not silently discarded in favor of cfg. The set's tighter gross + name_cap must
// bind the returned book; cfg's looser values must NOT govern. (Before the fix the
// minimal dispatch fed cfg straight to solve_fast and the set was ignored — RED.)
TEST(RiskOptimizer, WiredMinimalDispatch_HonorsAttachedSetOverCfg) {
  const std::vector<f64> alpha = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -1.8, 0.9};
  const FactorModel v = benign_model(alpha.size());

  // cfg is DELIBERATELY loose: a big gross and an effectively-uncapped name_cap.
  OptimizerConfig cfg;
  cfg.risk_aversion = 0.0;
  cfg.turnover_penalty = 0.0;
  cfg.gross_leverage = 4.0;  // loose gross
  cfg.name_cap = 10.0;       // effectively uncapped
  cfg.dollar_neutral = true;

  // The attached MINIMAL set tightens BOTH knobs well below cfg's.
  const f64 set_gross = 1.0;
  const f64 set_cap = 0.2; // < set_gross/n-style binder ⇒ must clip several names
  PortfolioOptimizer opt{cfg};
  ConstraintSet cs;
  cs.gross = GrossNet{set_gross, true};
  cs.pos = PositionCap{set_cap};
  opt.constraints = cs;

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());

  // The book must honor the SET, not cfg: gross == set_gross (not cfg's 4.0) and
  // every name capped at set_cap (not cfg's 10.0). With cfg ignored as before, the
  // gross would be 4.0 and no name would be clipped at 0.2 — these EXPECTs fail RED.
  EXPECT_NEAR(l1(*r), set_gross, 1e-9) << "gross must match the attached set, not cfg";
  for (usize i = 0; i < r->size(); ++i) {
    EXPECT_LE(std::fabs((*r)[i]), set_cap + 1e-9)
        << "name " << i << " must respect the set's name_cap, not cfg's";
  }
  // And it must equal what an explicit effective-config fast solve produces (the
  // MultiHorizon translation): gross/cap/neutral from the set.
  OptimizerConfig eff = cfg;
  eff.gross_leverage = set_gross;
  eff.name_cap = set_cap;
  eff.dollar_neutral = true;
  PortfolioOptimizer ref_opt{eff};
  auto rr = ref_opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(rr.has_value()) << (rr ? "" : rr.error().to_string());
  ASSERT_EQ(r->size(), rr->size());
  for (usize i = 0; i < r->size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>((*r)[i]), std::bit_cast<std::uint64_t>((*rr)[i]))
        << "honored set must match the effective-config fast solve at name " << i;
  }
}

// ===========================================================================
//  S8.4 — augmented dispatch through the single-period driver (G-CONSTRAINT). An
//  attached ParticipationCap routes to the ConstrainedQpSolver and the %ADV box is
//  satisfied at the returned book within the QP feas_tol.
// ===========================================================================
TEST(RiskOptimizer, WiredAugmentedDispatch_ParticipationCapSatisfied) {
  const std::vector<f64> alpha = {2.0, -1.0, 0.5, 1.5, -2.0, 0.8};
  const usize m = alpha.size();
  const FactorModel v = benign_model(m);

  // A participation cap that genuinely binds (small ADV ⇒ small box).
  const std::vector<f64> adv(m, 4000.0);
  const std::vector<f64> shares(m, 1e12); // ownership never binds
  const std::vector<f64> price(m, 10.0);
  const f64 nav = 1.0e6, H = 1.0, rho = 0.05;
  const f64 cap = rho * H * 4000.0 * 10.0 / nav; // == 0.002 per name

  OptimizerConfig cfg;
  cfg.risk_aversion = 0.5;
  cfg.gross_leverage = 1.0;
  cfg.dollar_neutral = true;
  PortfolioOptimizer opt{cfg};
  ConstraintSet cs;
  cs.gross = GrossNet{1.0, true};
  cs.part = ParticipationCap{rho};
  opt.constraints = cs;
  opt.ref = CapacityRef{std::span<const f64>(adv), std::span<const f64>(shares),
                        std::span<const f64>(price), nav, H};
  opt.qp.iters = 1500U; // headroom for the aux-split to clear feas_tol

  auto r = opt.solve(std::span<const f64>(alpha), v, {});
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  for (usize i = 0; i < m; ++i) {
    EXPECT_LE(std::fabs((*r)[i]), cap + opt.qp.feas_tol) << "name " << i;
  }
}

}  // namespace atxtest_risk_optimizer_test

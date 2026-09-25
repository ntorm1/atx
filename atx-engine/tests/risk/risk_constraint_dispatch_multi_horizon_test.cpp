// RiskConstraintDispatchMH_* — Lane 6: every ConstraintSet descriptor, set ALONE on top of
// the minimal GrossNet set, either MOVES the MultiHorizonOptimizer book (and is honored)
// or fails closed with a typed Err. A descriptor that is silently dropped by the dispatch
// (minimal vs augmented path) would leave the book unchanged — the failure this pins.

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"

namespace atx_test_l6_optim_dispatch_mh {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

constexpr usize kM = 6;
const std::vector<f64> kAlpha{0.6, -0.4, 0.3, -0.5, 0.2, -0.2};
const std::vector<usize> kGroups{0U, 0U, 1U, 1U, 2U, 2U};
const std::vector<f64> kBeta{1.4, 0.6, 1.2, 0.8, 1.0, 0.9};

risk::FactorModel model6() {
  la::MatX x(static_cast<Eigen::Index>(kM), 2);
  x << 1.0, 0.3, -0.4, 0.8, 0.7, -0.6, -0.9, 0.2, 0.5, 0.9, -0.2, -0.7;
  la::MatX f(2, 2);
  f << 0.1, 0.01, 0.01, 0.05;
  la::VecX d = la::VecX::Constant(static_cast<Eigen::Index>(kM), 0.5);
  auto r = risk::FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(r.has_value());
  return std::move(*r);
}

atx::core::Result<std::vector<f64>> run_book(const risk::ConstraintSet &cs,
                                             const risk::FactorModel &v) {
  risk::MultiHorizonConfig cfg;
  cfg.risk_aversion = 0.5;
  cfg.constraints = cs;
  cfg.qp.rho = 10.0;
  cfg.qp.iters = 1500U;
  const risk::MultiHorizonOptimizer opt{cfg};
  auto r = opt.run(
      risk::RebalanceSchedule{{0U}},
      [](usize) {
        risk::HorizonSources s;
        s.pairs.emplace_back(std::span<const f64>{kAlpha}, risk::SignalHorizon::identity());
        return s;
      },
      [&v](usize) -> const risk::FactorModel & { return v; },
      atx::engine::book::CostInputs{0.0, 0.0, 1.0});
  if (!r) {
    return atx::core::Err(r.error());
  }
  return atx::core::Ok(std::move(r->books.at(0)));
}

risk::ConstraintSet minimal() {
  risk::ConstraintSet cs;
  cs.gross.gross_leverage = 1.0;
  cs.gross.dollar_neutral = true;
  return cs;
}

f64 max_diff(std::span<const f64> a, std::span<const f64> b) {
  f64 d = 0.0;
  for (usize i = 0; i < a.size(); ++i) {
    d = std::max(d, std::fabs(a[i] - b[i]));
  }
  return d;
}

f64 dot(std::span<const f64> a, std::span<const f64> b) {
  f64 s = 0.0;
  for (usize i = 0; i < a.size(); ++i) {
    s += a[i] * b[i];
  }
  return s;
}

f64 factor_exposure(const risk::FactorModel &v, std::span<const f64> w, Eigen::Index k) {
  f64 s = 0.0;
  for (usize i = 0; i < w.size(); ++i) {
    s += v.exposures()(static_cast<Eigen::Index>(i), k) * w[i];
  }
  return s;
}

f64 max_group_net(std::span<const f64> w) {
  std::vector<f64> net(3U, 0.0);
  for (usize i = 0; i < w.size(); ++i) {
    net[kGroups[i]] += w[i];
  }
  f64 m = 0.0;
  for (const f64 x : net) {
    m = std::max(m, std::fabs(x));
  }
  return m;
}

// sqrt(wᵀ V w) with V = X F Xᵀ + D: the norm every risk cone bounds (the cone rows are
// [L_Fᵀ Xᵀ w ; sqrt(D)∘w], whose 2-norm is exactly this).
f64 risk_sigma(const risk::FactorModel &v, std::span<const f64> w) {
  return std::sqrt(v.risk(w));
}

// ‖Xᵀw‖₂: the robust-alpha cone's argument when Ω_f is the identity.
f64 factor_norm(const risk::FactorModel &v, std::span<const f64> w) {
  f64 s = 0.0;
  for (Eigen::Index k = 0; k < v.exposures().cols(); ++k) {
    const f64 e = factor_exposure(v, w, k);
    s += e * e;
  }
  return std::sqrt(s);
}

// The cone tolerance: the solver gates each cone at feas_tol = 1e-6 in cone-row units.
constexpr f64 kConeTol = 1e-5;

class RiskConstraintDispatchMH : public ::testing::Test {
protected:
  void SetUp() override {
    auto b = run_book(minimal(), v_);
    ASSERT_TRUE(b) << b.error().message();
    base_ = std::move(*b);
  }
  // The descriptor must move the book by more than the solver noise floor.
  void expect_moved(const std::vector<f64> &w) const { EXPECT_GT(max_diff(w, base_), 1e-4); }

  risk::FactorModel v_ = model6();
  std::vector<f64> base_;
};

TEST_F(RiskConstraintDispatchMH, BaselineIsDollarNeutralWithinGross) {
  f64 net = 0.0;
  f64 gross = 0.0;
  for (const f64 w : base_) {
    net += w;
    gross += std::fabs(w);
  }
  EXPECT_NEAR(net, 0.0, 1e-6);
  EXPECT_LE(gross, 1.0 + 1e-6);
  EXPECT_GT(gross, 0.05); // a non-trivial book, so every tightening below can bind
}

TEST_F(RiskConstraintDispatchMH, GrossLeverageAloneMovesAndHolds) {
  auto cs = minimal();
  cs.gross.gross_leverage = 0.25;
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  f64 g = 0.0;
  for (const f64 x : *w) {
    g += std::fabs(x);
  }
  EXPECT_LE(g, 0.25 + 1e-6);
}

TEST_F(RiskConstraintDispatchMH, PositionCapAloneMovesAndHolds) {
  f64 biggest = 0.0;
  for (const f64 x : base_) {
    biggest = std::max(biggest, std::fabs(x));
  }
  auto cs = minimal();
  cs.pos = risk::PositionCap{0.5 * biggest};
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  for (const f64 x : *w) {
    EXPECT_LE(std::fabs(x), 0.5 * biggest + 1e-6);
  }
}

TEST_F(RiskConstraintDispatchMH, FactorExposureAloneMovesAndHolds) {
  const f64 e0 = factor_exposure(v_, base_, 0);
  ASSERT_GT(std::fabs(e0), 1e-3);
  const f64 bound = 0.25 * std::fabs(e0);
  auto cs = minimal();
  cs.fexp = risk::FactorExposure{{0U}, {bound}};
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  EXPECT_LE(std::fabs(factor_exposure(v_, *w, 0)), bound + 1e-6);
}

TEST_F(RiskConstraintDispatchMH, GroupCapAloneMovesAndHolds) {
  const f64 g0 = max_group_net(base_);
  ASSERT_GT(g0, 1e-3);
  auto cs = minimal();
  risk::GroupCap gc;
  gc.group_id = std::span<const usize>(kGroups);
  gc.cap = std::vector<f64>(3U, 0.3 * g0);
  cs.grp = std::move(gc);
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  EXPECT_LE(max_group_net(*w), 0.3 * g0 + 1e-6);
}

TEST_F(RiskConstraintDispatchMH, BetaNeutralAloneMovesAndHolds) {
  const f64 b0 = dot(kBeta, base_);
  ASSERT_GT(std::fabs(b0), 1e-3);
  auto cs = minimal();
  cs.beta = risk::BetaNeutral{std::span<const f64>(kBeta), 0.2 * std::fabs(b0)};
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  EXPECT_LE(std::fabs(dot(kBeta, *w)), 0.2 * std::fabs(b0) + 1e-6);
}

TEST_F(RiskConstraintDispatchMH, TurnoverBudgetAloneMovesAndHolds) {
  auto cs = minimal();
  cs.turn = risk::TurnoverBudget{0.1}; // from the zero book: Σ|w| ≤ 0.1
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  f64 t = 0.0;
  for (const f64 x : *w) {
    t += std::fabs(x);
  }
  EXPECT_LE(t, 0.1 + 1e-6);
}

TEST_F(RiskConstraintDispatchMH, SectorNetBudgetAloneMovesAndHolds) {
  const f64 g0 = max_group_net(base_);
  ASSERT_GT(g0, 1e-3);
  auto cs = minimal();
  cs.sector = risk::SectorRiskBudget{std::span<const usize>(kGroups),
                                     std::vector<f64>(3U, 0.3 * g0)};
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  EXPECT_LE(max_group_net(*w), 0.3 * g0 + 1e-6);
}

TEST_F(RiskConstraintDispatchMH, SectorSocBudgetAloneMoves) {
  auto cs = minimal();
  risk::SectorRiskBudget sb;
  sb.sector_id = std::span<const usize>(kGroups);
  sb.soc = true;
  sb.sigma = std::vector<f64>(3U, 0.02);
  cs.sector = std::move(sb);
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  // Each sector's realized risk sqrt((m_g∘w)ᵀ V (m_g∘w)) respects its sigma budget, and the
  // budget binds for at least one sector (it is what moved the book).
  f64 worst = 0.0;
  for (usize g = 0; g < 3U; ++g) {
    std::vector<f64> masked(kM, 0.0);
    for (usize i = 0; i < kM; ++i) {
      masked[i] = (kGroups[i] == g) ? (*w)[i] : 0.0;
    }
    const f64 sg = risk_sigma(v_, masked);
    EXPECT_LE(sg, 0.02 + kConeTol) << "sector " << g;
    worst = std::max(worst, sg);
  }
  EXPECT_GT(worst, 0.02 - kConeTol);
}

TEST_F(RiskConstraintDispatchMH, TrackingErrorAloneMovesTheBook) {
  auto cs = minimal();
  cs.track = risk::TrackingError{{}, 0.05};
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  // Empty benchmark ⇒ tracking error == total risk sqrt(wᵀVw): within the 0.05 budget, and
  // binding (the unconstrained baseline carries more risk than the budget).
  ASSERT_GT(risk_sigma(v_, base_), 0.05);
  EXPECT_LE(risk_sigma(v_, *w), 0.05 + kConeTol);
  EXPECT_GT(risk_sigma(v_, *w), 0.05 - kConeTol);
}

TEST_F(RiskConstraintDispatchMH, RobustAlphaAloneMovesTheBook) {
  auto cs = minimal();
  cs.robust = risk::RobustAlpha{0.3};
  const auto w = run_book(cs, v_);
  ASSERT_TRUE(w) << w.error().message();
  expect_moved(*w);
  // The robust cone penalizes κ‖Ω_f^{1/2} Xᵀw‖₂ (Ω_f = I here). Against the NOMINAL solve of
  // the same augmented QP (κ = 0 keeps the augmented dispatch but emits no cone) the penalized
  // optimum must carry strictly less of that norm: for w₀ = argmin f and w₁ = argmin f + κg,
  // f(w₀) ≤ f(w₁) and f(w₁) + κg(w₁) ≤ f(w₀) + κg(w₀) give g(w₁) ≤ g(w₀). (The minimal-set
  // baseline base_ is solved by PortfolioOptimizer, a different algorithm, so it is not the
  // reference here.)
  auto nominal = minimal();
  nominal.robust = risk::RobustAlpha{0.0};
  const auto w0 = run_book(nominal, v_);
  ASSERT_TRUE(w0) << w0.error().message();
  EXPECT_LT(factor_norm(v_, *w), factor_norm(v_, *w0) - 1e-6);
}

TEST_F(RiskConstraintDispatchMH, CapacityDescriptorsAloneFailClosedTyped) {
  auto part = minimal();
  part.part = risk::ParticipationCap{0.1};
  auto own = minimal();
  own.own = risk::OwnershipCap{0.1};
  for (const risk::ConstraintSet &cs : {part, own}) {
    const auto w = run_book(cs, v_);
    ASSERT_FALSE(w);
    EXPECT_EQ(w.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

} // namespace atx_test_l6_optim_dispatch_mh

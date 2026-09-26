// Postimplementation R3 conformance: independent scalar oracle, executable limits,
// legacy recipe compatibility and the actual PortfolioOptimizer/CostSurface bridge.
#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/cost/optimizer_cost_terms.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace atx_test_r3_factor_cost {
namespace risk = atx::engine::risk;
namespace cost = atx::engine::cost;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;
constexpr auto v2 = risk::CostedSolveRule::FactorProxV2;
constexpr f64 inf = std::numeric_limits<f64>::infinity();

risk::FactorModel model(usize count) {
  const auto n = static_cast<Eigen::Index>(count);
  auto out = risk::FactorModel::create(la::MatX::Zero(n, 1), la::MatX::Identity(1, 1),
                                       la::VecX::Ones(n), 0, 1);
  EXPECT_TRUE(out);
  return std::move(*out);
}
risk::MaterializedConstraints box(usize count) {
  risk::MaterializedConstraints out;
  const auto n = static_cast<Eigen::Index>(count);
  out.A = la::MatX::Identity(n, n);
  out.l = la::VecX::Constant(n, -1.0);
  out.u = -out.l;
  return out;
}
risk::ConstrainedQpSolver solver() {
  risk::ConstrainedQpSolver out;
  out.cfg.iters = 2000;
  out.cfg.feas_tol = 1e-8;
  return out;
}
// Independent monotone right-derivative bisection for 0.5*w^2+q*w+
// k*abs(w-prev)+c*abs(w-prev)^1.5+b*max(0,-w), over [-1,1].
f64 scalar_book(f64 q, f64 previous, f64 kappa, f64 impact, f64 borrow) {
  f64 lo = -1, hi = 1;
  for (usize j = 0; j < 160; ++j) {
    const auto mid = lo + (hi - lo) * 0.5;
    const auto delta = mid - previous;
    const auto sign = delta < 0 ? -1.0 : 1.0;
    const auto derivative = mid + q + sign * kappa +
                            1.5 * impact * sign * std::sqrt(std::abs(delta)) -
                            (mid < 0 ? borrow : 0.0);
    if (derivative >= 0)
      hi = mid;
    else
      lo = mid;
  }
  return lo + (hi - lo) * 0.5;
}

TEST(RiskCostFactorV2, ProxMatchesIndependentDerivativeOracleAndExtremeScales) {
  for (const auto v : std::array{-2.0, -0.07, 0.0, 0.03, 1.5})
    for (const auto k : std::array{0.0, 0.04})
      for (const auto c : std::array{0.0, 0.3, 2.0}) {
        const f64 rho = 1.7;
        auto out = risk::prox_trade_cost(v, k, c, rho);
        ASSERT_TRUE(out);
        f64 lo = 0, hi = std::abs(v);
        if (rho * hi <= k)
          hi = 0;
        for (usize it = 0; it < 150; ++it) {
          const auto mid = lo + (hi - lo) * 0.5;
          if (rho * (mid - std::abs(v)) + k + 1.5 * c * std::sqrt(mid) >= 0)
            hi = mid;
          else
            lo = mid;
        }
        EXPECT_NEAR(*out, std::copysign(lo + (hi - lo) * 0.5, v), 2e-14);
      }
  auto subnormal = risk::prox_trade_cost(1e308, 0.0, 1e308, 1e-155);
  ASSERT_TRUE(subnormal);
  EXPECT_GT(*subnormal, 0.0);
  EXPECT_NEAR(*subnormal, (1.0 / 1.5) * (1.0 / 1.5) * 1e-310,
              32 * std::numeric_limits<f64>::denorm_min());
  EXPECT_FALSE(risk::prox_trade_cost(1.0, 0.1, 0.1, 0.0));
  EXPECT_FALSE(risk::prox_trade_cost(inf, 0.1, 0.1, 1.0));
}

TEST(RiskCostFactorV2, HeterogeneousCostsMatchScalarOracleAndExplicitLegacy) {
  const auto v = model(3);
  const auto c = box(3);
  const std::array q{-0.5, 0.5, -0.11}, previous{0.2, -0.2, 0.1};
  const std::array kappa{0.03, 0.06, 0.05}, impact{0.15, 0.07, 0.1}, borrow{0.01, 0.08, 0.02};
  risk::TradeCostTerms terms{kappa, impact, borrow, {}, previous};
  const auto out = risk::solve_with_costs(solver(), {v, 0.5, q, c}, terms, nullptr, nullptr, v2);
  ASSERT_TRUE(out) << out.error().message();
  EXPECT_TRUE(out->cert.factor_space);
  EXPECT_FALSE(out->cert.polished);
  for (usize i = 0; i < 3; ++i)
    EXPECT_NEAR(out->book[i], scalar_book(q[i], previous[i], kappa[i], impact[i], borrow[i]), 2e-8);
  EXPECT_EQ(std::bit_cast<atx::u64>(out->book[2]), std::bit_cast<atx::u64>(previous[2]));
  auto legacy_solver = solver();
  legacy_solver.cfg.iters = 6000;
  const auto legacy = risk::solve_with_costs(legacy_solver, {v, 0.5, q, c}, terms, nullptr, nullptr,
                                             risk::CostedSolveRule::AugmentedConeV1);
  ASSERT_TRUE(legacy) << legacy.error().message();
  for (usize i = 0; i < 3; ++i)
    EXPECT_NEAR(out->book[i], legacy->book[i], 3e-5);
  const auto defaults = risk::solve_with_costs(legacy_solver, {v, 0.5, q, c}, terms);
  ASSERT_TRUE(defaults);
  for (usize i = 0; i < 3; ++i)
    EXPECT_EQ(std::bit_cast<atx::u64>(defaults->book[i]), std::bit_cast<atx::u64>(legacy->book[i]));
}

TEST(RiskCostFactorV2, HardPinsCapsAndContradictoryLocateAreNotDropped) {
  const auto v = model(4);
  auto c = box(4);
  const std::array q{-1.0, 1.0, -1.0, 1.0}, previous{0.2, -0.2, 0.1, -0.1};
  const std::array cap{0.0, -0.01, 0.03, inf};
  const std::array<atx::u8, 4> locked{0, 0, 0, 1};
  risk::TradeCostTerms terms{{}, {}, {}, {}, previous, locked, cap};
  auto out = risk::solve_with_costs(solver(), {v, 0.5, q, c}, terms, nullptr, nullptr, v2);
  ASSERT_TRUE(out) << out.error().message();
  for (auto i : std::array<usize, 3>{0, 1, 3})
    EXPECT_EQ(std::bit_cast<atx::u64>(out->book[i]), std::bit_cast<atx::u64>(previous[i]));
  EXPECT_NEAR(out->book[2], 0.13, 2e-8);
  EXPECT_LE(out->book[2], previous[2] + cap[2]);
  EXPECT_FALSE(risk::solve_with_costs(solver(), {v, 0.5, q, c}, terms)); // V1 must refuse limits
  const std::array locate{inf, 0.1, inf, inf};
  terms.locate_cap = locate;
  EXPECT_FALSE(risk::solve_with_costs(solver(), {v, 0.5, q, c}, terms, nullptr, nullptr, v2));
}

TEST(RiskCostFactorV2, EconomicBudgetsAndFailedConvergenceRemainHardErrors) {
  const auto v = model(2);
  auto c = box(2);
  const std::array q{-1.0, 0.8}, previous{0.0, 0.0}, impact{0.1, 0.3};
  c.gross_l1_budget = 0.2;
  c.has_turnover = true;
  c.turnover_budget = 0.15;
  c.turnover_ref.assign(previous.begin(), previous.end());
  risk::TradeCostTerms terms{{}, impact, {}, {}, previous};
  auto out = risk::solve_with_costs(solver(), {v, 0.5, q, c}, terms, nullptr, nullptr, v2);
  ASSERT_TRUE(out) << out.error().message();
  EXPECT_LE(std::abs(out->book[0]) + std::abs(out->book[1]), 0.15 + 1e-8);
  EXPECT_TRUE(c.check_relative_feasible(out->book, 1e-8, 0.0));
  auto short_solver = solver();
  short_solver.cfg.iters = 1;
  EXPECT_FALSE(risk::solve_with_costs(short_solver, {v, 0.5, q, c}, terms, nullptr, nullptr, v2));
  c.storage.max_solver_bytes = 1;
  EXPECT_FALSE(risk::solve_with_costs(solver(), {v, 0.5, q, c}, terms, nullptr, nullptr, v2));
}

TEST(RiskCostFactorV2, RejectsMalformedStandaloneImpactReferenceBeforeCompilation) {
  const auto v=model(2);
  auto c=box(2);
  const std::array q{-0.2,0.2},previous{0.0,0.0};
  risk::TradeCostTerms terms{{},{},{},{},previous};
  c.impact.active=true; c.impact.coeff={0.1,0.1};
  c.turnover_ref={0.0}; // No has_turnover/penalty guard covers this reference.
  EXPECT_FALSE(risk::solve_with_costs(solver(),{v,0.5,q,c},terms,nullptr,nullptr,v2));
  c.turnover_ref={0.0,std::numeric_limits<f64>::quiet_NaN()};
  EXPECT_FALSE(risk::solve_with_costs(solver(),{v,0.5,q,c},terms,nullptr,nullptr,v2));
  c.turnover_ref.clear(); // Explicit empty reference continues to mean flat.
  const auto valid=risk::solve_with_costs(solver(),{v,0.5,q,c},terms,nullptr,nullptr,v2);
  ASSERT_TRUE(valid) << valid.error().message();
}

cost::CostSurface surface(bool second_available = true) {
  cost::CostSurfaceRecipe recipe;
  recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
  recipe.max_participation = 0.02;
  std::array<cost::CostSurfaceRow, 2> rows{};
  for (usize i = 0; i < rows.size(); ++i) {
    rows[i].instrument_id = 100 + i;
    rows[i].state = cost::CostInputState::Available;
    rows[i].available_at_ns = 999;
    rows[i].adv_dollars = 1'000'000;
    rows[i].daily_vol = 0.02;
    rows[i].full_spread = 0.002;
    rows[i].borrow_state = cost::CostInputState::Available;
    rows[i].borrow_available_at_ns = 999;
    rows[i].borrow_annual_fraction = 0.04;
  }
  if (!second_available)
    rows[1].state = cost::CostInputState::Unavailable;
  auto result = cost::CostSurface::create(
      recipe, {1000, std::string(64, 'a'), "synthetic-prior-raw-ADV", "modeled-not-locates"}, rows);
  EXPECT_TRUE(result);
  return std::move(*result);
}

TEST(RiskCostFactorV2, PortfolioConsumerBindsCostsUnitsAxisAndActualHeldBook) {
  const auto v = model(2);
  const auto snapshot = surface(false);
  const std::array alpha{0.8, std::numeric_limits<f64>::quiet_NaN()}, previous{0.0, -0.1};
  const std::array<atx::u64, 2> ids{100, 101};
  risk::PortfolioOptimizer opt;
  opt.cfg.dollar_neutral = false;
  opt.cfg.risk_aversion = 0.5;
  opt.qp = solver().cfg;
  const cost::SurfaceSolvePolicy policy{};
  const auto out = opt.solve_surface(alpha, v, previous, snapshot, ids, 1000, 1'000'000, policy);
  ASSERT_TRUE(out) << out.error().message();
  EXPECT_EQ(out->rule, v2);
  EXPECT_EQ(out->surface_recipe_sha256, snapshot.recipe_sha256());
  EXPECT_EQ(out->surface_snapshot_sha256, snapshot.snapshot_sha256());
  EXPECT_EQ(out->unavailable_liquidity_pins, 1U);
  EXPECT_FALSE(out->locate_limits_supplied);
  EXPECT_EQ(std::bit_cast<atx::u64>(out->solve.book[1]), std::bit_cast<atx::u64>(previous[1]));
  EXPECT_NEAR(out->solve.book[0], 0.02, 2e-8);
  f64 execution = 0;
  for (usize i = 0; i < 2; ++i) {
    const auto quote =
        snapshot.quote_weight_change(i, 1000, out->solve.book[i] - previous[i], 1'000'000);
    ASSERT_TRUE(quote.dollars.priced());
    execution += quote.cost_return;
  }
  EXPECT_NEAR(out->costs.linear + out->costs.impact, execution, 1e-12);
  EXPECT_NEAR(out->costs.borrow, 0.1 * 0.04 / 365.0, 1e-12);
  const std::array<atx::u64, 2> swapped{101, 100};
  EXPECT_FALSE(opt.solve_surface(alpha, v, previous, snapshot, swapped, 1000, 1'000'000, policy));
  EXPECT_FALSE(opt.solve_surface(alpha, v, previous, snapshot, ids, 1001, 1'000'000, policy));
  EXPECT_FALSE(opt.solve_surface(alpha, v, std::span<const f64>{previous}.first(1), snapshot, ids,
                                 1000, 1'000'000, policy));
}

TEST(RiskCostFactorV2, SnapshotAvailabilityAndBorrowPolicyAreExplicit) {
  const auto v = model(2);
  const auto snapshot = surface();
  auto rows = std::vector<cost::CostSurfaceRow>(snapshot.rows().begin(), snapshot.rows().end());
  cost::CostSurfaceRecipe recipe;
  recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
  const cost::CostSurfaceIdentity identity{1000, std::string(64, 'a'), "synthetic-prior-raw-ADV",
                                           "modeled-not-locates"};
  rows[0].available_at_ns = 1000;
  EXPECT_FALSE(cost::CostSurface::create(recipe, identity, rows));
  rows[0].available_at_ns = 999;
  rows[0].borrow_state = cost::CostInputState::Unavailable;
  auto unknown = cost::CostSurface::create(recipe, identity, rows);
  ASSERT_TRUE(unknown);
  risk::PortfolioOptimizer opt;
  opt.cfg.dollar_neutral = false;
  opt.qp = solver().cfg;
  const std::array alpha{0.2, -0.2}, previous{0.0, 0.0};
  const std::array<atx::u64, 2> ids{100, 101};
  cost::SurfaceSolvePolicy policy;
  EXPECT_FALSE(opt.solve_surface(alpha, v, previous, *unknown, ids, 1000, 1'000'000, policy));
  policy.borrow = cost::SurfaceBorrowPolicy::DisabledExplicitV1;
  const auto out = opt.solve_surface(alpha, v, previous, *unknown, ids, 1000, 1'000'000, policy);
  ASSERT_TRUE(out) << out.error().message();
  EXPECT_DOUBLE_EQ(out->costs.borrow, 0.0);
  EXPECT_EQ(out->policy.borrow, cost::SurfaceBorrowPolicy::DisabledExplicitV1);
}
} // namespace atx_test_r3_factor_cost

// risk_multi_horizon_test.cpp — P2-S1-4: the constrained multi-horizon optimizer.
//
// MultiHorizonOptimizer is the INTEGRATIVE CAPSTONE of S1: it mirrors the as-built
// S7 MultiPeriodOptimizer schedule walk, but replaces the inner single-period solve
// with the S1 stack — build a horizon trajectory (S1-3 forecast_trajectory), collapse
// it to a Gârleanu-Pedersen AIM alpha (the horizon AVERAGE, D9), then solve toward the
// aim with EITHER the as-built PortfolioOptimizer (minimal-constraint dispatch, R7) or
// the S1-1/S1-2 constrained QP (augmented constraints). The first move is executed and
// w_prev threads from the realized book, exactly as the S7 driver does.
//
// Coverage (the S1-4 contract):
//   1. THE BOUNDARY PIN (R7) — H=1 + identity source + minimal constraints + matching
//      trade_rate ⇒ the book schedule is BYTE-IDENTICAL to MultiPeriodOptimizer over
//      the same schedule/alpha/model/cost (full step AND a partial trade_rate; with a
//      NaN α cell to prove NaN handling reduces identically). The load-bearing test.
//   2. gp_aim (R8) — the horizon-average; identity/H=1 single-source ⇒ aim == α_t
//      bit-identical; a persistent source earns MORE aim weight than a fast one.
//   3. Augmented path (R3) — a FactorExposure (and beta) constraint is satisfied at
//      every period's book within feas_tol.
//   4. Determinism (R1) — two runs ⇒ byte-identical books + turnover + cost_bps.
//   5. Look-ahead (R2) — books.size()==periods.size(); w_prev threads from the
//      realized book (period s feeds period s+1's turnover).
//   6. Boundaries/errors — trade_rate ∉ (0,1] ⇒ Err(InvalidArgument); stacked_mpc ⇒
//      Err(NotImplemented); empty schedule ⇒ empty Ok; multi-source multi-horizon book
//      is finite; single-name universe.

#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::isnan, std::isfinite
#include <cstdint> // std::uint64_t
#include <functional>
#include <limits> // std::numeric_limits
#include <span>
#include <utility> // std::move, std::pair
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"
#include "atx/engine/risk/multi_period.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::book::CostInputs;
using atx::engine::risk::BetaNeutral;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorExposure;
using atx::engine::risk::FactorModel;
using atx::engine::risk::HorizonForecast;
using atx::engine::risk::HorizonSources;
using atx::engine::risk::MultiHorizonConfig;
using atx::engine::risk::MultiHorizonOptimizer;
using atx::engine::risk::MultiPeriodConfig;
using atx::engine::risk::MultiPeriodOptimizer;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PositionCap;
using atx::engine::risk::RebalanceSchedule;
using atx::engine::risk::SignalHorizon;

constexpr usize kM = 8U; // instruments
constexpr usize kK = 2U; // factors

// A small benign FactorModel: M=8 instruments, K=2 factors. Mirrors the S7 test's
// make_model EXACTLY so the boundary pin compares like-for-like.
[[nodiscard]] FactorModel make_model() {
  MatX x(static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kK));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(kM); ++i) {
    x(i, 0) = 0.1 * static_cast<f64>(i) - 0.35; // small spread
    x(i, 1) = 0.05 * static_cast<f64>(i % 3) - 0.05;
  }
  MatX f = MatX::Identity(static_cast<Eigen::Index>(kK), static_cast<Eigen::Index>(kK)); // SPD
  VecX d = VecX::Constant(static_cast<Eigen::Index>(kM), 0.2);                           // > 0
  auto r = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// Σ|a − b| with empty b ⇒ Σ|a| (mirrors the driver's l1_diff for the test asserts).
[[nodiscard]] f64 l1_diff(std::span<const f64> a, std::span<const f64> b) {
  f64 s = 0.0;
  for (usize i = 0; i < a.size(); ++i) {
    const f64 bi = i < b.size() ? b[i] : 0.0;
    s += std::fabs(a[i] - bi);
  }
  return s;
}

[[nodiscard]] f64 l1_norm(const std::vector<f64> &w) {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += std::fabs(x);
  }
  return s;
}

// The shared default OptimizerConfig (mirrors the S7 test's default_oc).
[[nodiscard]] OptimizerConfig default_oc() {
  OptimizerConfig oc;
  oc.risk_aversion = 1.0;
  oc.turnover_penalty = 0.0;
  oc.gross_leverage = 1.0;
  oc.name_cap = 1.0;
  oc.dollar_neutral = true;
  oc.max_iters = 64U;
  return oc;
}

// A minimal MultiHorizonConfig matching default_oc's knobs (the dispatch path). H=1
// + identity source ⇒ aim == α_t, so this reduces to MultiPeriodOptimizer.
[[nodiscard]] MultiHorizonConfig minimal_mh_cfg(f64 trade_rate) {
  MultiHorizonConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.constraints.gross.gross_leverage = 1.0;
  cfg.constraints.gross.dollar_neutral = true;
  cfg.constraints.pos = PositionCap{1.0};
  cfg.horizon = 1U;
  cfg.trade_rate = trade_rate;
  cfg.stacked_mpc = false;
  cfg.prox_max_iters = 64U;
  cfg.capacity_bound_gross = true;
  return cfg;
}

// The matching MultiPeriodConfig (the oracle).
[[nodiscard]] MultiPeriodConfig mp_cfg(f64 trade_rate) {
  MultiPeriodConfig cfg;
  cfg.single = default_oc();
  cfg.trade_rate = trade_rate;
  cfg.capacity_bound_gross = true;
  return cfg;
}

// Constant alphas (length M) — the SAME values the S7 oracle is fed.
const std::vector<f64> kAlpha = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -2.0, 0.8};
std::vector<f64> kAlphaNeg = [] {
  std::vector<f64> v = kAlpha;
  for (f64 &x : v) {
    x = -x;
  }
  return v;
}();
// A NaN-holed alpha to prove the NaN reduction is identical across both drivers.
std::vector<f64> kAlphaNaN = [] {
  std::vector<f64> v = kAlpha;
  v[2] = std::numeric_limits<f64>::quiet_NaN();
  v[5] = std::numeric_limits<f64>::quiet_NaN();
  return v;
}();

[[nodiscard]] f64 sum(const std::vector<f64> &v) {
  f64 s = 0.0;
  for (const f64 x : v) {
    s += x;
  }
  return s;
}

// FNV-1a fold over the bit patterns of every book double (the determinism digest).
[[nodiscard]] std::uint64_t digest(const std::vector<std::vector<f64>> &books) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const auto &book : books) {
    for (const f64 v : book) {
      const std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
      for (int byte = 0; byte < 8; ++byte) {
        h ^= (bits >> (byte * 8)) & 0xFFULL;
        h *= 1099511628211ULL;
      }
    }
  }
  return h;
}

// One identity HorizonSources from a single α span (the boundary-pin source builder).
[[nodiscard]] HorizonSources identity_source(std::span<const f64> a) {
  HorizonSources hs;
  hs.pairs.emplace_back(a, SignalHorizon::identity());
  return hs;
}

// ===========================================================================
//  TEST 1 — THE BOUNDARY PIN (R7). H=1 + identity source + minimal constraints
//  ⇒ the MultiHorizon book schedule is BYTE-IDENTICAL to MultiPeriodOptimizer.
//  This is the single most important test of the sprint.
// ===========================================================================
TEST(RiskMultiHorizon, BoundaryPinByteIdenticalToMultiPeriodFullStep) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U, 2U}};
  // Non-trivial cost: κ>0 (prox), round-trip charge, finite capacity.
  const CostInputs cost{/*kappa=*/0.25, /*round_trip_cost_bps=*/7.5, /*capacity_gross=*/1e9};

  // The SAME per-period α both drivers see (period 0 = kAlpha, 1 = neg, 2 = a NaN-holed
  // alpha so the NaN reduction is exercised in the pin).
  const auto alpha_for = [&](usize s) -> const std::vector<f64> & {
    if (s == 0U) {
      return kAlpha;
    }
    if (s == 1U) {
      return kAlphaNeg;
    }
    return kAlphaNaN;
  };

  // Oracle: MultiPeriodOptimizer over the raw α.
  MultiPeriodOptimizer mp{mp_cfg(1.0)};
  auto oracle = mp.run(
      sched, [&](usize s) { return std::span<const f64>(alpha_for(s)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().to_string());

  // Under test: MultiHorizonOptimizer with one identity source carrying the SAME α.
  MultiHorizonOptimizer mh{minimal_mh_cfg(1.0)};
  auto got = mh.run(
      sched, [&](usize s) { return identity_source(std::span<const f64>(alpha_for(s))); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());

  ASSERT_EQ(got->books.size(), oracle->books.size());
  for (usize s = 0; s < oracle->books.size(); ++s) {
    ASSERT_EQ(got->books[s].size(), oracle->books[s].size()) << "period " << s;
    for (usize i = 0; i < oracle->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(got->books[s][i]),
                std::bit_cast<std::uint64_t>(oracle->books[s][i]))
          << "BYTE DIVERGENCE at period " << s << " name " << i;
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->turnover[s]),
              std::bit_cast<std::uint64_t>(oracle->turnover[s]))
        << "turnover diverged at period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->cost_bps[s]),
              std::bit_cast<std::uint64_t>(oracle->cost_bps[s]))
        << "cost_bps diverged at period " << s;
  }
}

// The same pin at a PARTIAL trade_rate — the GP blend must match the oracle's blend
// byte-for-byte too (the rate != 1.0 algebraic path is shared verbatim).
TEST(RiskMultiHorizon, BoundaryPinByteIdenticalToMultiPeriodPartialStep) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U, 2U}};
  const CostInputs cost{0.25, 7.5, 1e9};
  const auto alpha_for = [&](usize s) -> const std::vector<f64> & {
    return s % 2U == 0U ? kAlpha : kAlphaNeg;
  };

  MultiPeriodOptimizer mp{mp_cfg(0.3)};
  auto oracle = mp.run(
      sched, [&](usize s) { return std::span<const f64>(alpha_for(s)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().to_string());

  MultiHorizonOptimizer mh{minimal_mh_cfg(0.3)};
  auto got = mh.run(
      sched, [&](usize s) { return identity_source(std::span<const f64>(alpha_for(s))); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());

  EXPECT_EQ(digest(got->books), digest(oracle->books));
  ASSERT_EQ(got->turnover.size(), oracle->turnover.size());
  for (usize s = 0; s < oracle->turnover.size(); ++s) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->turnover[s]),
              std::bit_cast<std::uint64_t>(oracle->turnover[s]))
        << "turnover period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->cost_bps[s]),
              std::bit_cast<std::uint64_t>(oracle->cost_bps[s]))
        << "cost_bps period " << s;
  }
}

// The capacity-clip path of the pin (finite capacity_gross < gross_leverage): the
// MultiHorizon dispatch must clip gross identically to the S7 driver.
TEST(RiskMultiHorizon, BoundaryPinByteIdenticalUnderCapacityClip) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U}};
  const CostInputs cost{0.0, 0.0, /*capacity_gross=*/0.5}; // clips gross 1.0 → 0.5
  const auto alpha_for = [&](usize s) -> const std::vector<f64> & {
    return s == 0U ? kAlpha : kAlphaNeg;
  };

  MultiPeriodOptimizer mp{mp_cfg(1.0)};
  auto oracle = mp.run(
      sched, [&](usize s) { return std::span<const f64>(alpha_for(s)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(oracle.has_value());

  MultiHorizonOptimizer mh{minimal_mh_cfg(1.0)};
  auto got = mh.run(
      sched, [&](usize s) { return identity_source(std::span<const f64>(alpha_for(s))); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value());

  EXPECT_EQ(digest(got->books), digest(oracle->books));
  for (const auto &book : got->books) {
    EXPECT_LE(l1_norm(book), 0.5 + 1e-9); // gross actually clipped
  }
}

// ===========================================================================
//  TEST 2 — gp_aim (R8): the horizon AVERAGE; identity single-source ⇒ aim == α_t
//  bit-identical; a persistent source earns more aim weight than a fast one.
// ===========================================================================
TEST(RiskMultiHorizon, GpAimIsHorizonAverage) {
  // Hand-built (H+1)=3 × M=4 trajectory; aim[i] must be the column average.
  HorizonForecast traj;
  traj.H = 2U;
  traj.alpha = {{1.0, 2.0, -1.0, 0.0}, {0.5, 1.0, -0.5, 0.0}, {0.25, 0.5, -0.25, 0.0}};
  const std::vector<f64> aim = MultiHorizonOptimizer::gp_aim(traj, 4U);
  ASSERT_EQ(aim.size(), 4U);
  for (usize i = 0; i < 4U; ++i) {
    const f64 expect = (traj.alpha[0][i] + traj.alpha[1][i] + traj.alpha[2][i]) / 3.0;
    EXPECT_NEAR(aim[i], expect, 1e-12) << "name " << i;
  }
}

TEST(RiskMultiHorizon, GpAimIdentitySingleSourceEqualsAlphaTBitIdentical) {
  // H=1, ONE identity source ⇒ the trajectory is constant == α_t at both rows, so the
  // average is EXACTLY α_t. Bit-identity is the load-bearing property (the boundary pin
  // relies on aim == α_t to inherit PortfolioOptimizer's scale-invariant result).
  const std::vector<f64> a = {0.3, -0.7, 1.1, -0.4};
  HorizonForecast traj;
  traj.H = 1U;
  traj.alpha = {a, a};
  const std::vector<f64> aim = MultiHorizonOptimizer::gp_aim(traj, 4U);
  ASSERT_EQ(aim.size(), 4U);
  for (usize i = 0; i < 4U; ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(aim[i]), std::bit_cast<std::uint64_t>(a[i]))
        << "name " << i;
  }
}

TEST(RiskMultiHorizon, GpAimPreservesAllNaNName) {
  HorizonForecast traj;
  traj.H = 1U;
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  traj.alpha = {{1.0, nan}, {0.5, nan}};
  const std::vector<f64> aim = MultiHorizonOptimizer::gp_aim(traj, 2U);
  EXPECT_NEAR(aim[0], 0.75, 1e-12);
  EXPECT_TRUE(std::isnan(aim[1])); // all-horizon-NaN name stays NaN
}

TEST(RiskMultiHorizon, GpAimWeightsPersistentSourceMore) {
  // A 2-source fixture run through forecast_trajectory: a SLOW-decay source and a
  // FAST-decay source, each with an opinion on a DIFFERENT name. The GP property: the
  // persistent name's aim retains MORE of its α_t (decay barely shrinks it over the
  // horizon) than the fast name's aim (which decays toward 0 across the horizon).
  const usize M = 2U;
  const usize H = 6U;
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<f64> slow_src = {1.0, nan}; // opinion on name 0 only
  const std::vector<f64> fast_src = {nan, 1.0}; // opinion on name 1 only
  const SignalHorizon slow{100.0};              // long halflife ⇒ ~no decay
  const SignalHorizon fast{0.5};                // short halflife ⇒ fast decay
  std::vector<std::pair<std::span<const f64>, SignalHorizon>> sources = {
      {std::span<const f64>(slow_src), slow}, {std::span<const f64>(fast_src), fast}};
  auto traj = atx::engine::risk::forecast_trajectory(
      std::span<const std::pair<std::span<const f64>, SignalHorizon>>(sources), M, H);
  ASSERT_TRUE(traj.has_value()) << (traj ? "" : traj.error().to_string());
  const std::vector<f64> aim = MultiHorizonOptimizer::gp_aim(*traj, M);
  // Both started at α_t = 1.0; the persistent name keeps a larger horizon-average.
  EXPECT_GT(aim[0], aim[1]) << "persistent source must earn more aim weight";
  EXPECT_NEAR(aim[0], 1.0, 0.05); // ~no decay
  EXPECT_LT(aim[1], 0.5);         // heavy decay shrinks the average
}

// ===========================================================================
//  TEST 3 — Augmented path (R3): a FactorExposure + BetaNeutral constraint is
//  satisfied at every period's book within the QP feas_tol.
// ===========================================================================
TEST(RiskMultiHorizon, AugmentedConstraintsSatisfiedEveryPeriod) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U}};
  const CostInputs cost{0.0, 0.0, 1e9};

  MultiHorizonConfig cfg;
  cfg.risk_aversion = 0.5;
  cfg.constraints.gross.gross_leverage = 1.0;
  cfg.constraints.gross.dollar_neutral = true;
  cfg.constraints.pos = PositionCap{0.4};
  cfg.constraints.fexp = FactorExposure{{0U, 1U}, {0.15, 0.15}}; // |Xᵀw| ≤ 0.15
  // Beta-neutral on a benign beta vector.
  static const std::vector<f64> beta = {1.0, 0.8, 1.2, 0.9, 1.1, 0.7, 1.3, 1.0};
  cfg.constraints.beta = BetaNeutral{std::span<const f64>(beta), 0.10};
  cfg.horizon = 1U;
  cfg.trade_rate = 1.0;
  cfg.qp.iters = 1200U; // headroom for the aux-split + extra rows to clear feas_tol
  cfg.qp.kkt_iters = 100U;

  MultiHorizonOptimizer mh{cfg};
  auto got = mh.run(
      sched,
      [&](usize s) { return identity_source(std::span<const f64>(s == 0U ? kAlpha : kAlphaNeg)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->books.size(), 2U);

  // Re-materialize the constraints and check A w ∈ [l, u] at every realized book.
  const MatX &X = v.exposures();
  const f64 tol = cfg.qp.feas_tol;
  std::vector<f64> w_prev;
  for (usize s = 0; s < got->books.size(); ++s) {
    const std::vector<f64> &w = got->books[s];
    auto mc = cfg.constraints.materialize(X, std::span<const f64>(w_prev), kM);
    ASSERT_TRUE(mc.has_value()) << (mc ? "" : mc.error().to_string());
    VecX wv(static_cast<Eigen::Index>(kM));
    for (usize i = 0; i < kM; ++i) {
      wv[static_cast<Eigen::Index>(i)] = w[i];
    }
    const VecX ax = mc->A * wv;
    for (Eigen::Index r = 0; r < mc->A.rows(); ++r) {
      EXPECT_LE(ax[r], mc->u[r] + tol) << "period " << s << " row " << r << " upper";
      EXPECT_GE(ax[r], mc->l[r] - tol) << "period " << s << " row " << r << " lower";
    }
    w_prev = w;
  }
}

// ===========================================================================
//  TEST 4 — Determinism (R1): two runs ⇒ byte-identical books/turnover/cost.
// ===========================================================================
TEST(RiskMultiHorizon, TwoRunsByteIdentical) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U, 2U}};
  const CostInputs cost{0.25, 7.5, 1e9};
  MultiHorizonOptimizer mh{minimal_mh_cfg(0.7)};
  const auto sources_at = [&](usize s) {
    return identity_source(std::span<const f64>(s % 2U == 0U ? kAlpha : kAlphaNeg));
  };
  const auto model_at = [&](usize) -> const FactorModel & { return v; };

  auto a = mh.run(sched, sources_at, model_at, cost);
  auto b = mh.run(sched, sources_at, model_at, cost);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(digest(a->books), digest(b->books));
  ASSERT_EQ(a->turnover.size(), b->turnover.size());
  for (usize s = 0; s < a->turnover.size(); ++s) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->turnover[s]),
              std::bit_cast<std::uint64_t>(b->turnover[s]));
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->cost_bps[s]),
              std::bit_cast<std::uint64_t>(b->cost_bps[s]));
  }
}

// ===========================================================================
//  TEST 5 — Look-ahead / first-move execution (R2): one book per period, w_prev
//  threads from the realized book (period s's book keys period s+1's turnover).
// ===========================================================================
TEST(RiskMultiHorizon, FirstMoveExecutionThreadsRealizedBook) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U, 2U}};
  const CostInputs cost{0.0, 0.0, 1e9};
  MultiHorizonOptimizer mh{minimal_mh_cfg(1.0)};
  auto got = mh.run(
      sched,
      [&](usize s) {
        return identity_source(std::span<const f64>(s % 2U == 0U ? kAlpha : kAlphaNeg));
      },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->books.size(), sched.periods.size()); // one first-move per period

  // turnover[s] == l1_diff(book[s], book[s-1]); turnover[0] == l1_norm(book[0]).
  EXPECT_NEAR(got->turnover[0], l1_norm(got->books[0]), 1e-12);
  for (usize s = 1; s < got->books.size(); ++s) {
    EXPECT_NEAR(
        got->turnover[s],
        l1_diff(std::span<const f64>(got->books[s]), std::span<const f64>(got->books[s - 1])),
        1e-12)
        << "period " << s;
  }
}

// ===========================================================================
//  TEST 6 — Boundaries / errors.
// ===========================================================================
TEST(RiskMultiHorizon, RejectsInvalidTradeRate) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U}};
  const CostInputs cost{0.0, 0.0, 1e9};
  const auto sources_at = [&](usize) { return identity_source(std::span<const f64>(kAlpha)); };
  const auto model_at = [&](usize) -> const FactorModel & { return v; };

  MultiHorizonConfig zero = minimal_mh_cfg(0.0); // below (0,1]
  EXPECT_FALSE(MultiHorizonOptimizer{zero}.run(sched, sources_at, model_at, cost).has_value());
  MultiHorizonConfig over = minimal_mh_cfg(1.5); // above (0,1]
  EXPECT_FALSE(MultiHorizonOptimizer{over}.run(sched, sources_at, model_at, cost).has_value());
}

TEST(RiskMultiHorizon, RejectsStackedMpcAsNotImplemented) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U}};
  const CostInputs cost{0.0, 0.0, 1e9};
  MultiHorizonConfig cfg = minimal_mh_cfg(1.0);
  cfg.stacked_mpc = true; // a recorded lift
  auto res = MultiHorizonOptimizer{cfg}.run(
      sched, [&](usize) { return identity_source(std::span<const f64>(kAlpha)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::NotImplemented);
}

TEST(RiskMultiHorizon, EmptyScheduleGivesEmptyOkResult) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{}};
  const CostInputs cost{0.0, 0.0, 1e9};
  auto res = MultiHorizonOptimizer{minimal_mh_cfg(1.0)}.run(
      sched, [&](usize) { return identity_source(std::span<const f64>(kAlpha)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res->books.empty());
  EXPECT_TRUE(res->turnover.empty());
  EXPECT_TRUE(res->cost_bps.empty());
}

// A genuine multi-source, multi-horizon run produces FINITE, dollar-neutral books.
TEST(RiskMultiHorizon, MultiSourceMultiHorizonProducesFiniteBooks) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U}};
  const CostInputs cost{0.1, 5.0, 1e9};

  static const std::vector<f64> momentum = kAlpha;
  static const std::vector<f64> reversal = kAlphaNeg;
  const SignalHorizon slow{20.0};
  const SignalHorizon fast{1.0};

  MultiHorizonConfig cfg = minimal_mh_cfg(1.0);
  cfg.horizon = 4U; // a real multi-period lookahead
  MultiHorizonOptimizer mh{cfg};
  auto got = mh.run(
      sched,
      [&](usize) {
        HorizonSources hs;
        hs.pairs.emplace_back(std::span<const f64>(momentum), slow);
        hs.pairs.emplace_back(std::span<const f64>(reversal), fast);
        return hs;
      },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->books.size(), 2U);
  for (const auto &book : got->books) {
    for (const f64 w : book) {
      EXPECT_TRUE(std::isfinite(w));
    }
    EXPECT_NEAR(sum(book), 0.0, 1e-9); // dollar-neutral
  }
}

TEST(RiskMultiHorizon, SingleNameUniverse) {
  // M=1, K=1. A degenerate-but-valid model; the dispatch path must not crash and the
  // single-name dollar-neutral book is 0 (demean of one name is 0).
  MatX x(1, 1);
  x(0, 0) = 0.5;
  MatX f = MatX::Identity(1, 1);
  VecX d = VecX::Constant(1, 0.2);
  auto mr = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  ASSERT_TRUE(mr.has_value());
  const FactorModel v = std::move(*mr);

  const RebalanceSchedule sched{{0U}};
  const CostInputs cost{0.0, 0.0, 1e9};
  static const std::vector<f64> a1 = {1.5};
  auto got = MultiHorizonOptimizer{minimal_mh_cfg(1.0)}.run(
      sched, [&](usize) { return identity_source(std::span<const f64>(a1)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->books.size(), 1U);
  ASSERT_EQ(got->books[0].size(), 1U);
  EXPECT_TRUE(std::isfinite(got->books[0][0]));
}

} // namespace

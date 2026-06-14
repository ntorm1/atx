// risk_multi_period_test.cpp — S7-1: receding-horizon MultiPeriodOptimizer.
//
// MultiPeriodOptimizer is a multi-period DRIVER over the as-built single-period
// risk::PortfolioOptimizer::solve. It is NOT a new inner solver: it walks an
// ascending rebalance schedule, threads w_prev from the prior period's realized
// book (so turnover is measured across the REAL schedule, not from flat each
// period), sets the turnover penalty κ to the calibrated book::CostInputs.kappa,
// applies a Gârleanu-Pedersen partial trade-rate toward the target, and
// capacity-bounds the gross. The inner solve is reused VERBATIM, so the
// multi-period book chain inherits its fixed-iteration bit-determinism for free.
//
// Coverage (the S7-1 contract):
//   1. SinglePeriodScheduleEqualsSingleSolve — schedule {0}, trade_rate=1, zero
//      cost ⇒ book[0] is BIT-FOR-BIT the single-period solve (the regression pin).
//   2. ThreadsPrevBookAcrossSchedule — flip-sign schedule ⇒ turnover[1] > 0 and
//      equals l1_diff(book[1], book[0]) (w_prev threaded from the realized book).
//   3. CalibratedKappaCutsTurnover — κ=0.5 trades less in aggregate than κ=0.
//   4. CapacityBoundsGross — capacity ceiling clips the per-period gross.
//   5. TwoBuildsByteIdentical — two identical runs are byte-identical (R1).
//   6. TradeRatePartialStep — trade_rate=0.5 steps less from flat than 1.0.

#include <cmath>     // std::fabs
#include <cstdint>   // std::uint64_t
#include <bit>       // std::bit_cast
#include <functional>
#include <span>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/multi_period.hpp"
#include "atx/engine/risk/optimizer.hpp"

namespace atxtest_risk_multi_period_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::book::CostInputs;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MultiPeriodConfig;
using atx::engine::risk::MultiPeriodOptimizer;
using atx::engine::risk::MultiPeriodResult;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::PortfolioOptimizer;
using atx::engine::risk::RebalanceSchedule;

constexpr usize kM = 8U; // instruments
constexpr usize kK = 2U; // factors

// A small benign FactorModel: M=8 instruments, K=2 factors. X = M×K small
// constants, F = K×K identity (SPD), D = positive idiosyncratic variances.
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

// The canonical default OptimizerConfig the tests share (and that test #1 pins the
// MP book against). dollar_neutral, L=1, cap=1, κ=0, max_iters=64.
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

// Σ|a − b| with empty b ⇒ Σ|a| (mirrors MultiPeriodOptimizer::l1_diff).
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

// FNV-1a fold over the bit patterns of every book double (a byte digest of the
// whole MultiPeriodResult::books — the determinism fingerprint).
[[nodiscard]] std::uint64_t digest(const std::vector<std::vector<f64>> &books) {
  std::uint64_t h = 1469598103934665603ULL; // FNV offset basis
  for (const auto &book : books) {
    for (const f64 v : book) {
      const std::uint64_t bits = std::bit_cast<std::uint64_t>(v);
      for (int byte = 0; byte < 8; ++byte) {
        h ^= (bits >> (byte * 8)) & 0xFFULL;
        h *= 1099511628211ULL; // FNV prime
      }
    }
  }
  return h;
}

// A constant-alpha forecast (length M) usable as the alpha_at callback's source.
const std::vector<f64> kAlpha = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -2.0, 0.8};
std::vector<f64> kAlphaNeg = [] {
  std::vector<f64> v = kAlpha;
  for (f64 &x : v) {
    x = -x;
  }
  return v;
}();

[[nodiscard]] f64 sum(const std::vector<f64> &v) {
  f64 s = 0.0;
  for (const f64 x : v) {
    s += x;
  }
  return s;
}

// ===========================================================================
//  TEST 1 — schedule {0}, trade_rate=1, zero cost ⇒ BIT-FOR-BIT single solve.
//  LOAD-BEARING regression pin: the multi-period driver must reduce to the
//  as-built PortfolioOptimizer::solve when the schedule is a single period.
// ===========================================================================
TEST(MultiPeriodOptimizer, SinglePeriodScheduleEqualsSingleSolve) {
  const FactorModel v = make_model();

  MultiPeriodConfig cfg;
  cfg.single = default_oc();
  cfg.trade_rate = 1.0;
  cfg.capacity_bound_gross = true;
  MultiPeriodOptimizer mp{cfg};

  const RebalanceSchedule sched{{0U}};
  const CostInputs cost{/*kappa=*/0.0, /*round_trip_cost_bps=*/0.0, /*capacity_gross=*/1e9};

  auto res = mp.run(
      sched, [&](usize) { return std::span<const f64>(kAlpha); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  ASSERT_EQ(res->books.size(), 1U);

  // The oc the MP driver uses (single + κ=0 + cap≥gross) must equal default_oc().
  auto single = PortfolioOptimizer{default_oc()}.solve(std::span<const f64>(kAlpha), v, {});
  ASSERT_TRUE(single.has_value());
  ASSERT_EQ(res->books[0].size(), single->size());
  for (usize i = 0; i < single->size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(res->books[0][i]),
              std::bit_cast<std::uint64_t>((*single)[i]))
        << "i=" << i;
  }
  // Turnover from flat at s=0 equals the L1 norm of the book.
  EXPECT_NEAR(res->turnover[0], l1_norm(res->books[0]), 1e-12);
}

// ===========================================================================
//  TEST 2 — flip-sign schedule ⇒ w_prev threaded from the realized book.
// ===========================================================================
TEST(MultiPeriodOptimizer, ThreadsPrevBookAcrossSchedule) {
  const FactorModel v = make_model();

  MultiPeriodConfig cfg;
  cfg.single = default_oc(); // κ overridden to cost.kappa (=0) by run()
  cfg.trade_rate = 1.0;
  cfg.capacity_bound_gross = true;
  MultiPeriodOptimizer mp{cfg};

  const RebalanceSchedule sched{{0U, 1U}};
  const CostInputs cost{0.0, 0.0, 1e9};

  // alpha FLIPS sign between period 0 and 1 ⇒ the target book reverses.
  auto res = mp.run(
      sched,
      [&](usize s) {
        return std::span<const f64>(s == 0U ? kAlpha : kAlphaNeg);
      },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  ASSERT_EQ(res->books.size(), 2U);

  EXPECT_GT(res->turnover[1], 0.0);
  EXPECT_NEAR(res->turnover[1],
              l1_diff(std::span<const f64>(res->books[1]), std::span<const f64>(res->books[0])),
              1e-12);
}

// ===========================================================================
//  TEST 3 — calibrated κ>0 cuts aggregate turnover vs κ=0.
// ===========================================================================
TEST(MultiPeriodOptimizer, CalibratedKappaCutsTurnover) {
  const FactorModel v = make_model();

  MultiPeriodConfig cfg;
  cfg.single = default_oc();
  cfg.trade_rate = 1.0;
  cfg.capacity_bound_gross = true;
  MultiPeriodOptimizer mp{cfg};

  const RebalanceSchedule sched{{0U, 1U}};
  const auto alpha_at = [&](usize s) {
    return std::span<const f64>(s == 0U ? kAlpha : kAlphaNeg);
  };
  const auto model_at = [&](usize) -> const FactorModel & { return v; };

  auto lo = mp.run(sched, alpha_at, model_at, CostInputs{/*kappa=*/0.0, 0.0, 1e9});
  auto hi = mp.run(sched, alpha_at, model_at, CostInputs{/*kappa=*/0.5, 0.0, 1e9});
  ASSERT_TRUE(lo.has_value());
  ASSERT_TRUE(hi.has_value());

  f64 lo_total = 0.0;
  f64 hi_total = 0.0;
  for (const f64 t : lo->turnover) {
    lo_total += t;
  }
  for (const f64 t : hi->turnover) {
    hi_total += t;
  }
  EXPECT_LT(hi_total, lo_total) << "κ>0 must reduce aggregate turnover";
}

// ===========================================================================
//  TEST 4 — capacity ceiling clips the per-period gross.
// ===========================================================================
TEST(MultiPeriodOptimizer, CapacityBoundsGross) {
  const FactorModel v = make_model();

  MultiPeriodConfig cfg;
  cfg.single = default_oc(); // L=1.0, but capacity caps gross at 0.5
  cfg.trade_rate = 1.0;
  cfg.capacity_bound_gross = true;
  MultiPeriodOptimizer mp{cfg};

  const RebalanceSchedule sched{{0U, 1U}};
  const CostInputs cost{0.0, 0.0, /*capacity_gross=*/0.5};

  auto res = mp.run(
      sched,
      [&](usize s) {
        return std::span<const f64>(s == 0U ? kAlpha : kAlphaNeg);
      },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());

  for (const auto &book : res->books) {
    EXPECT_LE(l1_norm(book), 0.5 + 1e-9);
  }
}

// ===========================================================================
//  TEST 5 — two identical runs are byte-identical (R1 determinism).
// ===========================================================================
TEST(MultiPeriodOptimizer, TwoBuildsByteIdentical) {
  const FactorModel v = make_model();

  MultiPeriodConfig cfg;
  cfg.single = default_oc();
  cfg.single.turnover_penalty = 0.3; // exercise the κ path too
  cfg.trade_rate = 0.7;
  cfg.capacity_bound_gross = true;
  MultiPeriodOptimizer mp{cfg};

  const RebalanceSchedule sched{{0U, 1U, 2U}};
  const CostInputs cost{0.25, 7.5, 1e9};
  const auto alpha_at = [&](usize s) {
    return std::span<const f64>(s % 2U == 0U ? kAlpha : kAlphaNeg);
  };
  const auto model_at = [&](usize) -> const FactorModel & { return v; };

  auto a = mp.run(sched, alpha_at, model_at, cost);
  auto b = mp.run(sched, alpha_at, model_at, cost);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(digest(a->books), digest(b->books));
}

// ===========================================================================
//  TEST 6 — trade_rate=0.5 steps less from flat than trade_rate=1.0.
// ===========================================================================
TEST(MultiPeriodOptimizer, TradeRatePartialStep) {
  const FactorModel v = make_model();

  const RebalanceSchedule sched{{0U}};
  const CostInputs cost{0.0, 0.0, 1e9};
  const auto alpha_at = [&](usize) { return std::span<const f64>(kAlpha); };
  const auto model_at = [&](usize) -> const FactorModel & { return v; };

  MultiPeriodConfig full_cfg;
  full_cfg.single = default_oc();
  full_cfg.trade_rate = 1.0;
  full_cfg.capacity_bound_gross = true;

  MultiPeriodConfig half_cfg = full_cfg;
  half_cfg.trade_rate = 0.5;

  auto full = MultiPeriodOptimizer{full_cfg}.run(sched, alpha_at, model_at, cost);
  auto half = MultiPeriodOptimizer{half_cfg}.run(sched, alpha_at, model_at, cost);
  ASSERT_TRUE(full.has_value());
  ASSERT_TRUE(half.has_value());

  const std::vector<f64> zeros(kM, 0.0);
  EXPECT_LT(l1_diff(std::span<const f64>(half->books[0]), std::span<const f64>(zeros)),
            l1_diff(std::span<const f64>(full->books[0]), std::span<const f64>(zeros)));
  // sanity: both books are dollar-neutral.
  EXPECT_NEAR(sum(full->books[0]), 0.0, 1e-9);
  EXPECT_NEAR(sum(half->books[0]), 0.0, 1e-9);
}

// ===========================================================================
//  TEST 7 — a trade_rate outside the (0,1] domain is rejected at the boundary.
// ===========================================================================
TEST(MultiPeriodOptimizer, RejectsInvalidTradeRate) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U}};
  const CostInputs cost{0.0, 0.0, 1e9};
  const auto alpha_at = [&](usize) { return std::span<const f64>(kAlpha); };
  const auto model_at = [&](usize) -> const FactorModel & { return v; };

  MultiPeriodConfig zero_cfg;
  zero_cfg.single = default_oc();
  zero_cfg.trade_rate = 0.0; // below the (0,1] domain
  EXPECT_FALSE(MultiPeriodOptimizer{zero_cfg}.run(sched, alpha_at, model_at, cost).has_value());

  MultiPeriodConfig over_cfg;
  over_cfg.single = default_oc();
  over_cfg.trade_rate = 1.5; // above the (0,1] domain
  EXPECT_FALSE(MultiPeriodOptimizer{over_cfg}.run(sched, alpha_at, model_at, cost).has_value());
}


}  // namespace atxtest_risk_multi_period_test

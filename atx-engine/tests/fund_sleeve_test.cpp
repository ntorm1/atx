// fund_sleeve_test.cpp — P2-S2-1: the Sleeve (wrap a MultiHorizonOptimizer over a
// library subset).
//
// A Sleeve is a thin owning identity: its membership (an AlphaId id-list), its tags
// (universe × family), a per-sleeve capacity ceiling, and a wrapped S1
// risk::MultiHorizonOptimizer. It produces the per-period sleeve book series by PURE
// DELEGATION to the proven S1 optimizer.
//
// Coverage (the S2-1 contract):
//   1. DelegationTransparency (THE point of this unit — grounds R7): Sleeve{cfg}.run(...)
//      is BYTE-IDENTICAL to MultiHorizonOptimizer{cfg.mh}.run(...) over the same fixture.
//      This proves Sleeve::run is transparent, so the later meta-book's one-sleeve
//      reduction to S1 is STRUCTURAL.
//   2. NMembers_ReturnsMemberCount — the membership id-list size carries.
//   3. EmptySchedule_DelegatesToEmptyOk — empty schedule ⇒ empty Ok (pass-through).
//   4. TagAndCapacity_RoundTrip — the tag + capacity fields store verbatim.
//
// The fixture-construction pattern mirrors risk_multi_horizon_test.cpp EXACTLY so the
// delegation pin compares like-for-like.

#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs
#include <cstdint> // std::uint64_t
#include <functional>
#include <span>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/combine/store.hpp"   // combine::AlphaId
#include "atx/engine/fund/sleeve.hpp"     // fund::Sleeve, SleeveConfig, SleeveTag (UNIT UNDER TEST)
#include "atx/engine/library/library.hpp" // library::AlphaId (re-export)
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"
#include "atx/engine/risk/multi_period.hpp"

namespace {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::book::CostInputs;
using atx::engine::fund::Sleeve;
using atx::engine::fund::SleeveConfig;
using atx::engine::fund::SleeveTag;
using atx::engine::risk::FactorModel;
using atx::engine::risk::HorizonSources;
using atx::engine::risk::MultiHorizonConfig;
using atx::engine::risk::MultiHorizonOptimizer;
using atx::engine::risk::PositionCap;
using atx::engine::risk::RebalanceSchedule;
using atx::engine::risk::SignalHorizon;
using library_AlphaId = atx::engine::library::AlphaId;

constexpr usize kM = 8U; // instruments
constexpr usize kK = 2U; // factors

// A small benign FactorModel: M=8 instruments, K=2 factors. Mirrors the S1 test's
// make_model EXACTLY so the delegation pin compares like-for-like.
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

// A minimal MultiHorizonConfig matching default knobs (the dispatch path). H=1 +
// identity source ⇒ aim == α_t, the boundary-pin reduction. Mirrors minimal_mh_cfg.
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

// Constant alphas (length M) — the SAME values the S1 fixture uses.
const std::vector<f64> kAlpha = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -2.0, 0.8};
const std::vector<f64> kAlphaNeg = [] {
  std::vector<f64> v = kAlpha;
  for (f64 &x : v) {
    x = -x;
  }
  return v;
}();

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

// The SAME per-period α both drivers see (period 0 = kAlpha, else = neg).
[[nodiscard]] const std::vector<f64> &alpha_for(usize s) {
  return s % 2U == 0U ? kAlpha : kAlphaNeg;
}

// ===========================================================================
//  TEST 1 — DelegationTransparency (THE point of this unit — grounds R7).
//  Sleeve{cfg}.run(...) is BYTE-IDENTICAL to MultiHorizonOptimizer{cfg.mh}.run(...)
//  over the SAME fixture. Proves Sleeve::run is a transparent wrapper, so the later
//  meta-book's one-sleeve reduction to S1 is structural.
// ===========================================================================
TEST(FundSleeve, DelegationTransparency_ByteIdenticalToMultiHorizon) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{0U, 1U, 2U}};
  // Non-trivial cost: κ>0 (prox), round-trip charge, finite capacity.
  const CostInputs cost{/*kappa=*/0.25, /*round_trip_cost_bps=*/7.5, /*capacity_gross=*/1e9};

  const auto sources_at = [&](usize s) { return identity_source(std::span<const f64>(alpha_for(s))); };
  const auto model_at = [&](usize) -> const FactorModel & { return v; };

  SleeveConfig cfg;
  cfg.mh = minimal_mh_cfg(0.3); // a partial trade_rate exercises the GP blend too
  cfg.members = {library_AlphaId{0}, library_AlphaId{1}};

  // Oracle: the wrapped S1 optimizer directly.
  auto oracle = MultiHorizonOptimizer{cfg.mh}.run(sched, sources_at, model_at, cost);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().to_string());

  // Under test: the Sleeve delegating to the SAME wrapped optimizer.
  auto got = Sleeve{cfg}.run(sched, sources_at, model_at, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());

  // Books byte-identical: digest AND element-wise bit_cast so signed zeros match.
  EXPECT_EQ(digest(got->books), digest(oracle->books));
  ASSERT_EQ(got->books.size(), oracle->books.size());
  for (usize s = 0; s < oracle->books.size(); ++s) {
    ASSERT_EQ(got->books[s].size(), oracle->books[s].size()) << "period " << s;
    for (usize i = 0; i < oracle->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(got->books[s][i]),
                std::bit_cast<std::uint64_t>(oracle->books[s][i]))
          << "BYTE DIVERGENCE at period " << s << " name " << i;
    }
  }
  // Turnover + cost_bps byte-identical too.
  ASSERT_EQ(got->turnover.size(), oracle->turnover.size());
  ASSERT_EQ(got->cost_bps.size(), oracle->cost_bps.size());
  for (usize s = 0; s < oracle->turnover.size(); ++s) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->turnover[s]),
              std::bit_cast<std::uint64_t>(oracle->turnover[s]))
        << "turnover diverged at period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(got->cost_bps[s]),
              std::bit_cast<std::uint64_t>(oracle->cost_bps[s]))
        << "cost_bps diverged at period " << s;
  }
}

// ===========================================================================
//  TEST 2 — n_members reports the membership id-list size.
// ===========================================================================
TEST(FundSleeve, NMembers_ReturnsMemberCount) {
  SleeveConfig cfg;
  cfg.members = {library_AlphaId{3}, library_AlphaId{7}, library_AlphaId{11}};
  EXPECT_EQ(Sleeve{cfg}.n_members(), 3U);

  SleeveConfig empty;
  EXPECT_EQ(Sleeve{empty}.n_members(), 0U);
}

// ===========================================================================
//  TEST 3 — an empty schedule delegates to the optimizer's empty-Ok behavior.
// ===========================================================================
TEST(FundSleeve, EmptySchedule_DelegatesToEmptyOk) {
  const FactorModel v = make_model();
  const RebalanceSchedule sched{{}};
  const CostInputs cost{0.0, 0.0, 1e9};

  SleeveConfig cfg;
  cfg.mh = minimal_mh_cfg(1.0);

  auto got = Sleeve{cfg}.run(
      sched, [&](usize) { return identity_source(std::span<const f64>(kAlpha)); },
      [&](usize) -> const FactorModel & { return v; }, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  EXPECT_TRUE(got->books.empty());
  EXPECT_TRUE(got->turnover.empty());
  EXPECT_TRUE(got->cost_bps.empty());
}

// ===========================================================================
//  TEST 4 — the tag + capacity fields store verbatim (the carry seam for the
//  later meta-allocator's box / universe × family tagging).
// ===========================================================================
TEST(FundSleeve, TagAndCapacity_RoundTrip) {
  SleeveConfig cfg{.mh = {}, .members = {}, .tag = {"US", "momentum"}, .capacity_gross = 2.5e6};
  const Sleeve s{cfg};
  EXPECT_EQ(s.cfg.tag.universe, "US");
  EXPECT_EQ(s.cfg.tag.family, "momentum");
  EXPECT_EQ(s.cfg.capacity_gross, 2.5e6);
}

} // namespace

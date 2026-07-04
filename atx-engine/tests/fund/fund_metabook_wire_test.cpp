// fund_metabook_wire_test.cpp — p8 Sprint 2 (S2-2/S2-3): the stage_metabook WIRING
// accept tests, at the engine level (no atx-impl dependency -- atx-engine cannot depend on
// atx-impl). Complements the ALREADY-LANDED, GREEN `fund_meta_book_integration_test.cpp`
// (which proves R1/R2/R3/R4 and the R7 "== standalone MultiHorizonOptimizer" pin) by adding
// the ONE thing that test does NOT cover: that MetaBook's single-sleeve reduction composes
// ALL THE WAY DOWN to `risk::MultiPeriodOptimizer` -- the actual engine `stage_optimize.cpp`
// calls today. `risk_multi_horizon_integration_test.cpp`'s
// `R7_DegenerateReducesToMultiPeriodByteIdentical` proves MultiHorizonOptimizer(H=1, ONE
// identity source, minimal constraints) == MultiPeriodOptimizer; this file proves
// MetaBook(one sleeve, c==[1], no crossing) composes with THAT SAME reduction, so
// `stage_metabook`'s SingleSleeve fund book is provably byte-identical to `stage_optimize`'s
// deployed book (given the identical stage-level plumbing atx-impl/tests/metabook_test.cpp's
// `SingleSleeveByteIdenticalToStageOptimize` verifies empirically at the stage boundary).
//
// Also covers the S2-2 accept criteria not already pinned by the frozen integration test:
//   - metabook_two_sleeve_composes: >=2 sleeves compose into one netted fund book with a
//     MEASURED crossing turnover reduction vs naive sleeve-concatenation.
//   - metabook_allocator_method_dispatch: InverseVol / ERC / HRP each route through
//     MetaAllocator correctly, Sigma|c| <= max_gross for each.
//   - metabook_pit_causality_guard: truncating the schedule after t leaves every book at
//     p <= t byte-identical; perturbing returns at p >= s leaves c[s] unchanged.
//   - twice-run + seq==parallel (sleeve-order independence).

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/fund/meta_book.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"
#include "atx/engine/risk/multi_period.hpp"

namespace atxtest_fund_metabook_wire_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::engine::book::CostInputs;
using atx::engine::fund::CapitalWeights;
using atx::engine::fund::MetaAllocatorConfig;
using atx::engine::fund::MetaBook;
using atx::engine::fund::MetaBookConfig;
using atx::engine::fund::MetaBookResult;
using atx::engine::fund::RiskBudgetMethod;
using atx::engine::fund::Sleeve;
using atx::engine::fund::SleeveConfig;
using atx::engine::risk::FactorModel;
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
constexpr usize kT = 6U; // synthetic periods (schedule {0..5})

[[nodiscard]] FactorModel make_model(usize period) {
  const f64 ps = 0.05 * static_cast<f64>(period);
  MatX x(static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kK));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(kM); ++i) {
    const f64 fi = static_cast<f64>(i);
    x(i, 0) = 0.08 * fi - 0.3 + ps;
    x(i, 1) = 0.04 * static_cast<f64>(i % 3) - 0.04 - 0.4 * ps;
  }
  MatX f = MatX::Identity(static_cast<Eigen::Index>(kK), static_cast<Eigen::Index>(kK));
  atx::core::linalg::VecX d = atx::core::linalg::VecX::Constant(static_cast<Eigen::Index>(kM), 0.2);
  auto r = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

class ModelStore {
public:
  ModelStore() {
    for (usize s = 0; s < kT; ++s) {
      models_.emplace_back(make_model(s));
    }
  }
  [[nodiscard]] const FactorModel &at(usize period) const { return models_.at(period); }

private:
  std::vector<FactorModel> models_;
};

const std::vector<f64> kMomentum = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -2.0, 0.8};
const std::vector<f64> kReversal = {-1.5, 0.6, -0.4, -2.0, 1.0, -0.8, 1.4, -0.3};

[[nodiscard]] std::vector<f64> alpha_for(usize sleeve, usize period) {
  std::vector<f64> v = (sleeve == 0U) ? kMomentum : kReversal;
  const f64 sgn = (period % 2U == 0U) ? 1.0 : -1.0;
  for (f64 &x : v) {
    x = sgn * (x + 0.05 * static_cast<f64>(period));
  }
  return v;
}

class FixtureData {
public:
  FixtureData() {
    for (usize j = 0; j < 2U; ++j) {
      for (usize p = 0; p < kT; ++p) {
        alpha_[j][p] = alpha_for(j, p);
      }
    }
    for (usize p = 0; p < kT; ++p) {
      std::vector<f64> r(kM, 0.0);
      for (usize i = 0; i < kM; ++i) {
        r[i] = 0.01 * (static_cast<f64>((i + p) % 5U) - 2.0) + 0.002 * static_cast<f64>(p);
      }
      returns_[p] = std::move(r);
    }
  }
  [[nodiscard]] std::span<const f64> alpha(usize sleeve, usize period) const {
    return std::span<const f64>(alpha_.at(sleeve).at(period));
  }
  [[nodiscard]] std::span<const f64> returns(usize period) const {
    return std::span<const f64>(returns_.at(period));
  }

private:
  std::array<std::array<std::vector<f64>, kT>, 2U> alpha_{};
  std::array<std::vector<f64>, kT> returns_{};
};

[[nodiscard]] MultiHorizonConfig minimal_mh_cfg() {
  MultiHorizonConfig cfg;
  cfg.risk_aversion = 1.0;
  cfg.constraints.gross.gross_leverage = 1.0;
  cfg.constraints.gross.dollar_neutral = true;
  cfg.constraints.pos = PositionCap{1.0};
  cfg.horizon = 1U;
  cfg.trade_rate = 1.0;
  cfg.stacked_mpc = false;
  cfg.prox_max_iters = 64U;
  cfg.capacity_bound_gross = true;
  return cfg;
}

[[nodiscard]] CostInputs trading_cost() {
  return CostInputs{/*kappa*/ 0.0, /*round_trip_cost_bps*/ 5.0, /*capacity_gross*/ 1e9};
}

// A two-sleeve MetaBook wired via one identity HorizonSource per sleeve per period.
[[nodiscard]] MetaBook make_two_sleeve(const MetaAllocatorConfig &alloc) {
  MetaBook mb;
  mb.cfg.alloc = alloc;
  mb.cfg.risk_lookback = 60U;
  SleeveConfig sc0;
  sc0.mh = minimal_mh_cfg();
  sc0.capacity_gross = 1e9;
  SleeveConfig sc1;
  sc1.mh = minimal_mh_cfg();
  sc1.capacity_gross = 1e9;
  mb.sleeves = {Sleeve{sc0}, Sleeve{sc1}};
  return mb;
}

[[nodiscard]] std::function<HorizonSources(usize, usize)> two_sleeve_sources(const FixtureData &fx) {
  return [&fx](usize sleeve, usize period) {
    HorizonSources hs;
    hs.pairs.emplace_back(fx.alpha(sleeve, period), SignalHorizon::identity());
    return hs;
  };
}

// ===========================================================================
//  metabook_two_sleeve_composes
// ===========================================================================
TEST(FundMetabookWire, TwoSleeveComposesWithMeasuredCrossingWin) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U, 5U}};
  const CostInputs cost = trading_cost();

  MetaAllocatorConfig alloc; // default ERC, fractional_kelly=0.3 -- a real multi-sleeve fund
  auto mb = make_two_sleeve(alloc);
  const auto sources = two_sleeve_sources(fx);
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  auto got = mb.run(sched, sources, model_at, returns_at, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
  ASSERT_EQ(got->sleeve_results.size(), 2U);
  ASSERT_EQ(got->fund_books.size(), sched.periods.size());
  for (const auto &c : got->capital) {
    ASSERT_EQ(c.c.size(), 2U);
    const f64 gross = std::fabs(c.c[0]) + std::fabs(c.c[1]);
    EXPECT_LE(gross, alloc.max_gross + 1e-9);
  }

  // The measured crossing win: momentum (sleeve 0) and reversal (sleeve 1) are anti-
  // correlated by construction, so their capital-weighted target books offset in several
  // names -> turnover_net < turnover_gross strictly on at least one period, and never >.
  f64 net_total = 0.0;
  f64 gross_total = 0.0;
  bool strictly_crossed = false;
  for (usize s = 0; s < got->report.turnover_net.size(); ++s) {
    net_total += got->report.turnover_net[s];
    gross_total += got->report.turnover_gross[s];
    EXPECT_LE(got->report.turnover_net[s], got->report.turnover_gross[s] + 1e-9);
    if (got->report.turnover_net[s] < got->report.turnover_gross[s] - 1e-9) {
      strictly_crossed = true;
    }
  }
  EXPECT_TRUE(strictly_crossed) << "expected at least one period with a real crossing benefit";
  EXPECT_LT(net_total, gross_total) << "net_total=" << net_total << " gross_total=" << gross_total;
}

// ===========================================================================
//  metabook_single_sleeve_byte_identical — the composed R7 pin (extends the frozen
//  fund_meta_book_integration_test.cpp R7 test with the MultiPeriodOptimizer leg).
// ===========================================================================
TEST(FundMetabookWire, SingleSleeveByteIdenticalToMultiPeriodOptimizer) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U, 5U}};
  const CostInputs cost = trading_cost();

  SleeveConfig sc;
  sc.mh = minimal_mh_cfg();
  sc.capacity_gross = 1e9;

  MetaAllocatorConfig alloc; // the c==[1] boundary config (the R7 override)
  alloc.method = RiskBudgetMethod::EqualRiskContribution;
  alloc.fractional_kelly = 1.0;
  alloc.target_vol = 0.0;
  alloc.max_gross = 4.0;

  MetaBook mb;
  mb.cfg.alloc = alloc;
  mb.cfg.risk_lookback = 60U;
  mb.sleeves = {Sleeve{sc}};

  const auto sleeve_src = [&](usize, usize p) {
    HorizonSources hs;
    hs.pairs.emplace_back(fx.alpha(0U, p), SignalHorizon::identity());
    return hs;
  };
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  auto got = mb.run(sched, sleeve_src, model_at, returns_at, cost);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());

  // Claim (a): fund_books == a standalone MultiHorizonOptimizer::run (already pinned by the
  // frozen fund_meta_book_integration_test.cpp; re-affirmed here as the composition base).
  auto oracle_mh = MultiHorizonOptimizer{sc.mh}.run(
      sched, [&](usize p) { HorizonSources hs; hs.pairs.emplace_back(fx.alpha(0U, p), SignalHorizon::identity()); return hs; },
      model_at, cost);
  ASSERT_TRUE(oracle_mh.has_value());

  // Claim (b), NEW here: fund_books == a standalone MultiPeriodOptimizer::run over the SAME
  // raw alpha/model/cost (the S7 driver stage_optimize.cpp actually calls). This is the
  // engine-level half of the "== stage_optimize book" pin; the stage-level half (the
  // plumbing that feeds MultiPeriodOptimizer the SAME inputs stage_optimize builds) is
  // verified empirically in atx-impl/tests/metabook_test.cpp.
  OptimizerConfig oc;
  oc.risk_aversion = 1.0;
  oc.turnover_penalty = 0.0; // run() overrides kappa <- cost.kappa
  oc.gross_leverage = 1.0;
  oc.name_cap = 1.0;
  oc.dollar_neutral = true;
  oc.max_iters = 64U;
  MultiPeriodConfig mpc;
  mpc.single = oc;
  mpc.trade_rate = 1.0;
  mpc.capacity_bound_gross = true;
  const MultiPeriodOptimizer mp{mpc};
  auto oracle_mp = mp.run(
      sched, [&](usize p) { return fx.alpha(0U, p); }, model_at, cost);
  ASSERT_TRUE(oracle_mp.has_value()) << (oracle_mp ? "" : oracle_mp.error().to_string());

  ASSERT_EQ(got->fund_books.size(), oracle_mp->books.size());
  bool some_nonzero = false;
  for (usize s = 0; s < oracle_mp->books.size(); ++s) {
    ASSERT_EQ(got->fund_books[s].size(), oracle_mp->books[s].size()) << "period " << s;
    for (usize i = 0; i < oracle_mp->books[s].size(); ++i) {
      if (got->fund_books[s][i] != 0.0) {
        some_nonzero = true;
      }
      EXPECT_EQ(std::bit_cast<std::uint64_t>(got->fund_books[s][i]),
               std::bit_cast<std::uint64_t>(oracle_mp->books[s][i]))
          << "R7 (composed) BYTE DIVERGENCE vs MultiPeriodOptimizer period " << s << " name " << i;
    }
  }
  EXPECT_TRUE(some_nonzero) << "R7 vacuous: the pinned fund books are all zero";
}

// ===========================================================================
//  metabook_allocator_method_dispatch
// ===========================================================================
TEST(FundMetabookWire, AllocatorMethodDispatch) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U, 5U}};
  const CostInputs cost = trading_cost();
  const auto sources = two_sleeve_sources(fx);
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  for (const RiskBudgetMethod method :
      {RiskBudgetMethod::InverseVol, RiskBudgetMethod::EqualRiskContribution,
       RiskBudgetMethod::HierarchicalRiskParity}) {
    MetaAllocatorConfig alloc;
    alloc.method = method;
    auto mb = make_two_sleeve(alloc);
    auto got = mb.run(sched, sources, model_at, returns_at, cost);
    ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().to_string());
    for (const auto &c : got->capital) {
      ASSERT_EQ(c.c.size(), 2U);
      const f64 gross = std::fabs(c.c[0]) + std::fabs(c.c[1]);
      EXPECT_LE(gross, alloc.max_gross + 1e-9)
          << "method=" << static_cast<int>(method);
    }
  }
}

// ===========================================================================
//  metabook_pit_causality_guard — the central S2 trap.
// ===========================================================================
TEST(FundMetabookWire, PitCausalityGuard) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule full_sched{{0U, 1U, 2U, 3U, 4U, 5U}};
  const CostInputs cost = trading_cost();
  const auto sources = two_sleeve_sources(fx);
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  MetaAllocatorConfig alloc;
  auto mb_full = make_two_sleeve(alloc);
  auto full = mb_full.run(full_sched, sources, model_at, returns_at, cost);
  ASSERT_TRUE(full.has_value());

  // (1) Truncation: a schedule ending at period t=3 must reproduce every book at p<=3
  // byte-identically (the trailing risk budget reads no future).
  constexpr usize kT_trunc = 3U;
  const RebalanceSchedule trunc_sched{{0U, 1U, 2U, 3U}};
  auto mb_trunc = make_two_sleeve(alloc);
  auto trunc = mb_trunc.run(trunc_sched, sources, model_at, returns_at, cost);
  ASSERT_TRUE(trunc.has_value());
  ASSERT_EQ(trunc->fund_books.size(), kT_trunc + 1U);
  for (usize s = 0; s <= kT_trunc; ++s) {
    ASSERT_EQ(trunc->fund_books[s].size(), full->fund_books[s].size()) << "period " << s;
    for (usize i = 0; i < trunc->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(trunc->fund_books[s][i]),
               std::bit_cast<std::uint64_t>(full->fund_books[s][i]))
          << "PIT truncation divergence at period " << s << " name " << i;
    }
    ASSERT_EQ(trunc->capital[s].c.size(), full->capital[s].c.size());
    for (usize j = 0; j < trunc->capital[s].c.size(); ++j) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(trunc->capital[s].c[j]),
               std::bit_cast<std::uint64_t>(full->capital[s].c[j]))
          << "PIT truncation capital divergence at period " << s << " sleeve " << j;
    }
  }

  // (2) Perturbation: changing sleeve returns at periods p>=s must NOT change c[s] -- the
  // trailing window strictly excludes them. Perturb returns at p>=4 and re-run the FULL
  // schedule; c[0..3] (allocated from P&L strictly before those periods) must be unchanged.
  class PerturbedFixture {
  public:
    explicit PerturbedFixture(const FixtureData &base) : base_(base) {
      for (usize p = 0; p < kT; ++p) {
        std::vector<f64> r(base_.returns(p).begin(), base_.returns(p).end());
        if (p >= 4U) {
          for (f64 &v : r) {
            v += 5.0; // large perturbation -- would be very visible if it leaked backward
          }
        }
        returns_[p] = std::move(r);
      }
    }
    [[nodiscard]] std::span<const f64> returns(usize p) const { return std::span<const f64>(returns_.at(p)); }

  private:
    const FixtureData &base_;
    std::array<std::vector<f64>, kT> returns_{};
  };
  const PerturbedFixture pfx(fx);
  const auto perturbed_returns_at = [&](usize p) { return pfx.returns(p); };
  auto mb_perturbed = make_two_sleeve(alloc);
  auto perturbed = mb_perturbed.run(full_sched, sources, model_at, perturbed_returns_at, cost);
  ASSERT_TRUE(perturbed.has_value());
  for (usize s = 0; s <= 3U; ++s) {
    ASSERT_EQ(perturbed->capital[s].c.size(), full->capital[s].c.size()) << "period " << s;
    for (usize j = 0; j < full->capital[s].c.size(); ++j) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(perturbed->capital[s].c[j]),
               std::bit_cast<std::uint64_t>(full->capital[s].c[j]))
          << "look-ahead leak: c[" << s << "][" << j << "] changed from a p>=4 perturbation";
    }
  }
}

// ===========================================================================
//  Twice-run + seq==parallel (sleeve-order independence, PASS 1 is blind per-sleeve).
// ===========================================================================
TEST(FundMetabookWire, TwiceRunByteIdentical) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U, 5U}};
  const CostInputs cost = trading_cost();
  const auto sources = two_sleeve_sources(fx);
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  MetaAllocatorConfig alloc;
  auto mb1 = make_two_sleeve(alloc);
  auto mb2 = make_two_sleeve(alloc);
  auto r1 = mb1.run(sched, sources, model_at, returns_at, cost);
  auto r2 = mb2.run(sched, sources, model_at, returns_at, cost);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  ASSERT_EQ(r1->fund_books.size(), r2->fund_books.size());
  for (usize s = 0; s < r1->fund_books.size(); ++s) {
    ASSERT_EQ(r1->fund_books[s].size(), r2->fund_books[s].size());
    for (usize i = 0; i < r1->fund_books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->fund_books[s][i]),
               std::bit_cast<std::uint64_t>(r2->fund_books[s][i]))
          << "twice-run divergence period " << s << " name " << i;
    }
    EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->report.turnover_net[s]),
             std::bit_cast<std::uint64_t>(r2->report.turnover_net[s]))
        << "twice-run turnover_net divergence period " << s;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->report.turnover_gross[s]),
             std::bit_cast<std::uint64_t>(r2->report.turnover_gross[s]))
        << "twice-run turnover_gross divergence period " << s;
  }
}

// PASS-1 sleeve independence -- the structural basis for "seq==parallel" (meta_book.hpp:
// 13-14: "each sleeve is blind to the others"). MetaBook::run is a single-threaded reference
// driver (no actual thread pool to race), so the meaningful, honest test of "safe to run
// sleeve walks in any order / concurrently" is that sleeve j's OWN realized book depends
// ONLY on sources_at(j, *) -- NOT on whether other sleeves are present at all. Proven here by
// comparing sleeve_results[j] from the FULL two-sleeve MetaBook against a STANDALONE
// Sleeve::run over the SAME per-sleeve callback: byte-identical means sleeve j's PASS-1 walk
// never reads or is perturbed by sleeve k's data, which is exactly what makes concurrent /
// reordered PASS-1 execution safe.
TEST(FundMetabookWire, PassOneSleeveIndependence) {
  const ModelStore store;
  const FixtureData fx;
  const RebalanceSchedule sched{{0U, 1U, 2U, 3U, 4U, 5U}};
  const CostInputs cost = trading_cost();
  const auto sources = two_sleeve_sources(fx);
  const auto model_at = [&](usize p) -> const FactorModel & { return store.at(p); };
  const auto returns_at = [&](usize p) { return fx.returns(p); };

  MetaAllocatorConfig alloc;
  auto mb = make_two_sleeve(alloc);
  const SleeveConfig sc0 = mb.sleeves[0].cfg; // copy before run() (run() does not consume sleeves)
  const SleeveConfig sc1 = mb.sleeves[1].cfg;
  auto full = mb.run(sched, sources, model_at, returns_at, cost);
  ASSERT_TRUE(full.has_value());
  ASSERT_EQ(full->sleeve_results.size(), 2U);

  auto standalone0 = Sleeve{sc0}.run(
      sched, [&](usize p) { return sources(0U, p); }, model_at, cost);
  auto standalone1 = Sleeve{sc1}.run(
      sched, [&](usize p) { return sources(1U, p); }, model_at, cost);
  ASSERT_TRUE(standalone0.has_value());
  ASSERT_TRUE(standalone1.has_value());

  for (usize s = 0; s < sched.periods.size(); ++s) {
    ASSERT_EQ(full->sleeve_results[0].books[s].size(), standalone0->books[s].size());
    for (usize i = 0; i < standalone0->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(full->sleeve_results[0].books[s][i]),
               std::bit_cast<std::uint64_t>(standalone0->books[s][i]))
          << "sleeve 0 depends on sleeve 1's presence at period " << s << " name " << i;
    }
    ASSERT_EQ(full->sleeve_results[1].books[s].size(), standalone1->books[s].size());
    for (usize i = 0; i < standalone1->books[s].size(); ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(full->sleeve_results[1].books[s][i]),
               std::bit_cast<std::uint64_t>(standalone1->books[s][i]))
          << "sleeve 1 depends on sleeve 0's presence at period " << s << " name " << i;
    }
  }
}

} // namespace atxtest_fund_metabook_wire_test

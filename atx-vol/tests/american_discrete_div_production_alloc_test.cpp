// Allocation budget of the D1 production path (api/pricing/american_discrete_div_kernel.hpp
// section 4, api/pricing/american_discrete_div_batch.hpp): on a production workspace
// (DiscreteDivWorkspace(max_steps, max_pre_steps)) the route, the price, the seeded
// inversion and the Greek bundle allocate NOTHING; a batch allocates in `prepare()`
// (its setup stage) and never per contract.
//
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a
// replaced global operator new), linked ONLY into atx-vol-discrete-div-production-tests.
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_batch.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

[[nodiscard]] std::vector<CashDividend> quarterly(double first, double amount, double T) {
  std::vector<CashDividend> out;
  for (double t = first; t <= T + 1e-12; t += 0.25) {
    out.push_back({t, amount});
  }
  return out;
}

[[nodiscard]] DiscreteDivProductionOpts opts_of(DiscreteDivAccuracy accuracy) {
  DiscreteDivProductionOpts o;
  o.accuracy = accuracy;
  return o;
}

TEST(DiscreteDivProductionAllocation, RoutePriceInversionAndGreeksAllocateNothing) {
  DiscreteDivWorkspace ws(kDiscreteDivMaxRouteSteps, kDiscreteDivMaxPreSteps);
  const double dt = 1.0 / 301.0;
  const std::vector<std::vector<CashDividend>> schedules = {
      {}, quarterly(0.1, 1.0, 1.0), {{0.7 * dt, 3.0}}, {{0.3 * dt, 3.0}}};
  for (const auto &divs : schedules) {
    for (const DiscreteDivAccuracy acc :
         {DiscreteDivAccuracy::Standard, DiscreteDivAccuracy::Refined}) {
      for (const Side side : {Side::Call, Side::Put}) {
        bool ok = false;
        double price = 0.0;
        const std::uint64_t priced = testkit::count_allocations([&] {
          const auto p = american_discrete_div_production_price(
              100.0, 100.0, 1.0, 0.25, 0.04, 0.0, side, divs, ExerciseStyle::American,
              opts_of(acc), ws);
          ok = p.has_value();
          price = ok ? p->price : 0.0;
        });
        ASSERT_TRUE(ok);
        EXPECT_EQ(priced, 0U);
        const std::uint64_t inverted = testkit::count_allocations([&] {
          const auto iv = american_discrete_div_production_iv(
              price * 1.03, 100.0, 100.0, 1.0, 0.04, 0.0, side, divs, opts_of(acc),
              DiscreteDivIvOpts{}, ws);
          ok = iv.has_value();
        });
        EXPECT_TRUE(ok);
        EXPECT_EQ(inverted, 0U);
        std::vector<double> slots(divs.size(), 0.0);
        const std::uint64_t greeks = testkit::count_allocations([&] {
          const auto g = american_discrete_div_production_greeks(
              100.0, 100.0, 1.0, 0.25, 0.04, 0.0, side, divs, ExerciseStyle::American, slots,
              1.0 / 252.0, opts_of(acc), ws);
          ok = g.has_value();
        });
        EXPECT_TRUE(ok);
        EXPECT_EQ(greeks, 0U);
      }
    }
  }
}

TEST(DiscreteDivProductionAllocation, BatchAllocatesInPrepareAndNeverPerContract) {
  const auto c4 = quarterly(0.1, 1.0, 1.0);
  const auto c8 = quarterly(0.1, 1.0, 2.0);
  const std::vector<DiscreteDivBatchGroup> groups{
      {100.0, 1.0, 0.04, 0.0, c4}, {100.0, 2.0, 0.04, 0.0, c8}, {100.0, 0.5, 0.04, 0.0, {}}};
  std::vector<DiscreteDivBatchContract> contracts;
  for (std::uint32_t g = 0; g < groups.size(); ++g) {
    for (const double K : {80.0, 90.0, 100.0, 110.0, 120.0}) {
      for (const Side side : {Side::Call, Side::Put}) {
        contracts.push_back({g, K, side, 0.25, 0.0});
      }
    }
  }
  std::vector<DiscreteDivBatchResult> out(contracts.size());
  std::vector<DiscreteDivProductionGreeks> bundles(contracts.size());
  for (const DiscreteDivAccuracy acc :
       {DiscreteDivAccuracy::Standard, DiscreteDivAccuracy::Refined}) {
    DiscreteDivBatch batch(opts_of(acc));
    std::size_t refused = 1;
    const std::uint64_t setup = testkit::count_allocations([&] { refused = batch.prepare(groups); });
    EXPECT_EQ(refused, 0U);
    EXPECT_GE(setup, 1U);
    bool ok = false;
    const std::uint64_t priced = testkit::count_allocations(
        [&] { ok = batch.price(contracts, ExerciseStyle::American, out).has_value(); });
    ASSERT_TRUE(ok);
    EXPECT_EQ(priced, 0U);
    for (std::size_t i = 0; i < contracts.size(); ++i) {
      ASSERT_EQ(out[i].outcome, DiscreteDivBatchOutcome::Priced) << i;
      contracts[i].quote = out[i].value * 1.02 + 1.0e-3;
    }
    const std::uint64_t inverted = testkit::count_allocations(
        [&] { ok = batch.implied_vol(contracts, DiscreteDivIvOpts{}, out).has_value(); });
    ASSERT_TRUE(ok);
    EXPECT_EQ(inverted, 0U);
    for (std::size_t i = 0; i < contracts.size(); ++i) {
      EXPECT_EQ(out[i].outcome, DiscreteDivBatchOutcome::Priced) << i;
    }
    const std::uint64_t greeks = testkit::count_allocations([&] {
      ok = batch.greeks(contracts, ExerciseStyle::American, 1.0 / 252.0, bundles).has_value();
    });
    ASSERT_TRUE(ok);
    EXPECT_EQ(greeks, 0U);
  }
}

} // namespace
} // namespace atx::vol

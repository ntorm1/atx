// Allocation budget of the dated-cash lattice KERNEL objects
// (api/pricing/american_discrete_div_kernel.hpp): a DiscreteDivWorkspace allocates
// at construction and NEVER again — every price, the converged reference and the
// seeded inversion run allocation-free on it, for every scheme, step count and
// schedule it admits.
//
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a
// replaced global operator new), linked ONLY into atx-vol-discrete-div-kernel-tests,
// never into the monolithic atx-vol-tests (which has its own replacement).
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_discrete_div_kernel.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

constexpr DiscreteDivScheme kAllSchemes[] = {
    DiscreteDivScheme::SplicedCrr, DiscreteDivScheme::SplicedBbs, DiscreteDivScheme::SplicedBbsr,
    DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer};

TEST(DiscreteDivWorkspaceAllocation, ConstructionAllocatesAndNothingAfterwards) {
  std::uint64_t at_construction = 0;
  DiscreteDivWorkspace ws = [&] {
    testkit::alloc_counter_arm();
    DiscreteDivWorkspace fresh(1201);
    at_construction = testkit::alloc_counter_disarm();
    return fresh;
  }();
  EXPECT_GE(at_construction, 1U);
  EXPECT_EQ(ws.max_steps(), 1201);
  EXPECT_EQ(ws.reserved_bytes(), 4U * 1203U * sizeof(double));

  const std::vector<CashDividend> four{{0.05, 2.1}, {0.30, 2.1}, {0.55, 2.2}, {0.80, 2.2}};
  for (const Side side : {Side::Call, Side::Put}) {
    for (const DiscreteDivScheme scheme : kAllSchemes) {
      for (const int steps : {101, 301, 1200, 1201}) {
        bool priced = false;
        const std::uint64_t n = testkit::count_allocations([&] {
          const auto out = american_discrete_div_price(512.0, 500.0, 0.9, 0.31, 0.043, -0.002,
                                                       side, four, steps, ExerciseStyle::American,
                                                       scheme, ws);
          priced = out.has_value();
        });
        EXPECT_TRUE(priced) << to_string(scheme) << " steps " << steps;
        EXPECT_EQ(n, 0U) << to_string(scheme) << " steps " << steps;
      }
    }
  }
}

TEST(DiscreteDivWorkspaceAllocation, TheConvergedReferenceAndTheInversionAllocateNothing) {
  DiscreteDivWorkspace ws(kDiscreteDivConvergedSteps);
  const std::vector<CashDividend> two{{0.182, 1.8311}, {0.432, 1.8311}};
  bool ok = false;
  const std::uint64_t converged = testkit::count_allocations([&] {
    const auto out = american_discrete_div_converged_price(772.98, 760.0, 0.432, 0.2, 0.047,
                                                           -0.0009, Side::Put, two,
                                                           ExerciseStyle::American, ws);
    ok = out.has_value();
  });
  EXPECT_TRUE(ok);
  EXPECT_EQ(converged, 0U);

  const auto quote = american_discrete_div_price(772.98, 760.0, 0.432, 0.2, 0.047, -0.0009,
                                                 Side::Put, two, 301, ExerciseStyle::American,
                                                 DiscreteDivScheme::SplicedCrr, ws);
  ASSERT_TRUE(quote.has_value());
  for (const double seed : {0.0, 0.21}) {
    DiscreteDivIvOpts opts;
    opts.seed_sigma = seed;
    bool inverted = false;
    const std::uint64_t n = testkit::count_allocations([&] {
      const auto iv = american_discrete_div_implied_vol(*quote, 772.98, 760.0, 0.432, 0.047,
                                                        -0.0009, Side::Put, two, 301,
                                                        DiscreteDivScheme::SplicedCrr, opts, ws);
      inverted = iv.has_value();
    });
    EXPECT_TRUE(inverted) << "seed " << seed;
    EXPECT_EQ(n, 0U) << "seed " << seed;
  }
}

} // namespace
} // namespace atx::vol

// Allocation budget of the opt-in dated-cash lattice schemes: no allocation
// beyond the workspace the default spliced rollback already owns.
//
// One price call owns one LatticeBuffers (step amounts, levels, values and the
// splice's scratch row) and every scheme draws its rows from it; the Richardson
// scheme runs its coarse rollback second so it reuses the fine one's rows. So
// the allocation COUNT of each scheme is at most the default's, and it does not
// grow with the step count or the number of dividends.
//
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a
// replaced global operator new), linked ONLY into
// atx-vol-discrete-div-smoothing-tests, never into the monolithic atx-vol-tests
// (which has its own replacement).
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

constexpr DiscreteDivScheme kAllSchemes[] = {
    DiscreteDivScheme::SplicedCrr, DiscreteDivScheme::SplicedBbs, DiscreteDivScheme::SplicedBbsr,
    DiscreteDivScheme::EscrowedBbs, DiscreteDivScheme::EscrowedLeisenReimer};

// The largest workspace a rollback owns: four rows.
constexpr std::uint64_t kWorkspaceRows = 4;

[[nodiscard]] std::uint64_t allocations_of(DiscreteDivScheme scheme, int steps,
                                           const std::vector<CashDividend> &divs, Side side) {
  bool priced = false;
  const std::uint64_t n = testkit::count_allocations([&] {
    const auto out = american_discrete_div_price(512.0, 500.0, 0.9, 0.31, 0.043, -0.002, side,
                                                 divs, steps, ExerciseStyle::American, scheme);
    priced = out.has_value();
  });
  EXPECT_TRUE(priced) << to_string(scheme);
  return n;
}

TEST(DiscreteDivSchemeAllocation, EverySchemeStaysInsideTheDefaultWorkspace) {
  const std::vector<CashDividend> four{{0.05, 2.1}, {0.30, 2.1}, {0.55, 2.2}, {0.80, 2.2}};
  for (const Side side : {Side::Call, Side::Put}) {
    const std::uint64_t baseline = allocations_of(DiscreteDivScheme::SplicedCrr, 301, four, side);
    EXPECT_LE(baseline, kWorkspaceRows);
    for (const DiscreteDivScheme scheme : kAllSchemes) {
      const std::uint64_t at_301 = allocations_of(scheme, 301, four, side);
      EXPECT_LE(at_301, baseline) << to_string(scheme);
      // The count is a property of the workspace, not of the step count.
      EXPECT_EQ(allocations_of(scheme, 1201, four, side), at_301) << to_string(scheme);
    }
  }
}

TEST(DiscreteDivSchemeAllocation, TheCountDoesNotGrowWithTheDividendCount) {
  const std::vector<CashDividend> one{{0.40, 3.0}};
  std::vector<CashDividend> twelve;
  for (int i = 0; i < 12; ++i) {
    twelve.push_back({0.03 + 0.07 * i, 1.0});
  }
  for (const DiscreteDivScheme scheme : kAllSchemes) {
    EXPECT_EQ(allocations_of(scheme, 301, twelve, Side::Put),
              allocations_of(scheme, 301, one, Side::Put))
        << to_string(scheme);
  }
}

} // namespace
} // namespace atx::vol

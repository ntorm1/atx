// Allocation budget of the (σ, q) family's price path: ZERO.
//
// L2h-b sigma-q family (2026-10-21). After prepare(), AlSigmaQFamily::price() and
// price_and_chord() run on the caller's stack (an AlBoundary copy + the barycentric
// weights) and read only const state, so a fan-out over a chain allocates nothing.
// Counted with tests/support/alloc_counter.* (thread-local armed tally over a replaced
// global operator new), linked ONLY into atx-vol-sigma-q-family-alloc-tests, never into
// the monolithic atx-vol-tests or a slice (which must keep the default operator new).
#include <cstdint>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

TEST(AlSigmaQFamilyAlloc, PricePathAllocatesNothing) {
  const AlExpiryGeometry g = prepare_al_geometry(0.5, 0.05, 0.02);
  for (const Side side : {Side::Put, Side::Call}) {
    for (const std::uint16_t n_q : {std::uint16_t{1}, std::uint16_t{4}}) {
      AlSigmaQFamily fam(g, side, 100.0);
      const double q_hi = (n_q == 1) ? 0.02 : 0.03;
      ASSERT_TRUE(fam.prepare(0.1, 0.6, 12, 0.02, q_hi, n_q));
      double sum = 0.0;
      unsigned priced = 0;
      const std::uint64_t n = testkit::count_allocations([&] {
        for (const double K : {70.0, 95.0, 100.0, 120.0}) {
          for (const double sigma : {0.1, 0.23, 0.41, 0.59}) {
            const AlSigmaQFamilyPrice p = fam.price(100.0, K, sigma, 0.02);
            const AlSigmaQFamilyChord c = fam.price_and_chord(100.0, K, sigma, 0.02);
            const AlSigmaQFamilyPrice refused = fam.price(100.0, K, sigma, 0.5);
            if (p.status == AlSigmaQFamilyStatus::Ok && c.status == AlSigmaQFamilyStatus::Ok &&
                refused.status == AlSigmaQFamilyStatus::YieldOutsideBox) {
              sum += p.price + c.price_sigma_bumped;
              ++priced;
            }
          }
        }
      });
      EXPECT_EQ(n, 0u) << (side == Side::Put ? "put" : "call") << " n_q=" << n_q;
      EXPECT_EQ(priced, 16u);
      EXPECT_GT(sum, 0.0);
    }
  }
}

} // namespace
} // namespace atx::vol

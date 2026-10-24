// RiskKernel.SecondStateAllocationBounded — the family kernel's allocation bound per
// state (api/risk/risk_kernel.hpp "Allocation"). Its own executable
// (atx-vol-risk-alloc-tests) because support/alloc_counter.cpp replaces global
// operator new, which the monolithic atx-vol-tests already does elsewhere; the ctest
// name keeps the "risk/" prefix so the lane's anchored -R "^risk/RiskKernel\." runs it.
//
// The counter is thread-local, so the kernel runs at n_threads = 1 (every block on the
// calling thread). The bound is O(n_groups): a call family is re-anchored at the new
// spot (reconstructed), a put family keeps its median-strike anchor and re-prepares in
// place, a geometry re-prepares in place; the per-ROW path (resolve + family_greeks)
// allocates nothing, which the dense book (same groups, 4x the rows) pins exactly.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "atx/vol/api/risk/risk_kernel.hpp"
#include "support/alloc_counter.hpp"
#include "support/risk_synthetic_market.hpp"

using namespace atx::vol;
namespace rt = atx::vol::risk_test;

namespace {

// A book with every lane family-served (strikes inside 0.93-1.07 S: no exercised or
// near-edge lane, so no cold row allocates on the per-row path).
[[nodiscard]] std::vector<OptionLeg> mid_legs(std::size_t n_uids, std::size_t strikes_per_side) {
  std::vector<OptionLeg> legs;
  std::uint64_t id = 1;
  for (std::size_t ui = 0; ui < n_uids; ++ui) {
    const rt::UnderlierSpec &u = rt::kUnderliers[ui];
    for (std::size_t ei = 0; ei < rt::kExpiryUnits.size(); ++ei) {
      for (std::size_t s = 0; s < strikes_per_side; ++s) {
        const double t = strikes_per_side > 1u
                             ? static_cast<double>(s) / static_cast<double>(strikes_per_side - 1u)
                             : 0.5;
        legs.push_back(OptionLeg{id++, u.uid, u.spot * (0.93 + 0.07 * t), rt::expiry_ns(ei),
                                 Side::Put, 1.0, 100.0});
        legs.push_back(OptionLeg{id++, u.uid, u.spot * (1.0 + 0.07 * t), rt::expiry_ns(ei),
                                 Side::Call, 1.0, 100.0});
      }
    }
  }
  return legs;
}

struct Measured {
  std::uint64_t allocations{0};
  std::size_t n_groups{0};
  std::size_t n_rows{0};
};

[[nodiscard]] Measured measure(std::size_t n_uids, std::size_t strikes_per_side) {
  const auto s0 = rt::make_state(rt::StateMove{}, CarryFrom::Surface, n_uids);
  const auto s1 = rt::make_state(rt::shifted_move(), CarryFrom::Surface, n_uids);
  const RiskBook book = RiskBook::create(mid_legs(n_uids, strikes_per_side), {}).value();
  const PreparedRiskBook prep = PreparedRiskBook::create(book).value();
  FamilyKernelPolicy policy;
  policy.n_threads = 1;
  const RiskKernel kernel = RiskKernel::family(policy);
  RiskFrame frame;
  RiskWorkspace ws;
  frame.reserve(prep.n_unique(), prep.uids().size(), 0);
  ws.reserve(prep);
  EXPECT_TRUE(kernel.evaluate(prep, s0->state, RiskNeeds{}, frame, ws).has_value());
  Measured m;
  m.n_groups = prep.n_groups();
  m.n_rows = prep.n_unique();
  m.allocations = testkit::count_allocations(
      [&] { EXPECT_TRUE(kernel.evaluate(prep, s1->state, RiskNeeds{}, frame, ws).has_value()); });
  EXPECT_EQ(frame.counters.family_rows, prep.n_unique()) << "every lane must be family-served";
  EXPECT_EQ(frame.counters.cold_rows, 0u);
  return m;
}

} // namespace

TEST(RiskKernel, SecondStateAllocationBounded) {
  const Measured small = measure(1, 3);  // 8 groups, 24 rows
  const Measured full = measure(3, 3);   // 24 groups, 72 rows
  const Measured dense = measure(3, 12); // 24 groups, 288 rows
  ASSERT_EQ(small.n_groups, 8u);
  ASSERT_EQ(full.n_groups, 24u);
  ASSERT_EQ(dense.n_groups, 24u);
  // The per-row path allocates nothing: 4x the rows over the same groups, same count.
  EXPECT_EQ(dense.allocations, full.allocations);
  const double k = static_cast<double>(full.allocations - small.allocations) /
                   static_cast<double>(full.n_groups - small.n_groups);
  const double c = static_cast<double>(full.allocations) - k * static_cast<double>(full.n_groups);
  std::cout << "[RiskKernel] second-state allocations: groups=" << small.n_groups << " -> "
            << small.allocations << ", groups=" << full.n_groups << " -> " << full.allocations
            << ", groups=" << dense.n_groups << " rows=" << dense.n_rows << " -> "
            << dense.allocations << "; measured k=" << k << " per group, c=" << c << "\n";
  // Pinned bound. Measured (2026-10-24, vol-rel): k = 0.5, c = 0 — exactly one
  // allocation per CALL group (its family re-anchored at the moved spot); put families,
  // geometries and every row allocate nothing. Pinned at one per group, no constant.
  constexpr std::uint64_t kPerGroup = 1;
  constexpr std::uint64_t kConstant = 0;
  for (const Measured &m : std::array<Measured, 3>{small, full, dense}) {
    EXPECT_LE(m.allocations, kPerGroup * m.n_groups + kConstant)
        << "groups=" << m.n_groups << " rows=" << m.n_rows;
  }
}

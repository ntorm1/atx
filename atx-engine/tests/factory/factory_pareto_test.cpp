// atx::engine::factory::pareto — NSGA-II primitive tests (S4.1, plan §4.1).
//
// Differential / property coverage of the three engine-local NSGA-II primitives:
//   * dominates(a, b)            — maximization dominance with NaN == -inf.
//   * fast_nondominated_sort(..) — Deb et al. 2002 §III-A front assignment.
//   * crowding_distance(..)      — boundary +inf, interior normalized gaps.
//
// The load-bearing invariant under test is DETERMINISM: every sort / front order
// resolves in CANONICAL-ID order, so equal-objective genomes land in a fixed
// front sequence and crowding ties break the same way every run.

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/factory/pareto.hpp"

namespace {

using atx::f64;
using atx::u16;
using atx::usize;
using atx::engine::factory::crowding_distance;
using atx::engine::factory::dominates;
using atx::engine::factory::fast_nondominated_sort;
using atx::engine::factory::ObjMatrix;

// Identity canonical order [0, n) — the tests that exercise canonical tie-break
// pass an explicit non-identity order instead.
[[nodiscard]] std::vector<usize> iota_order(usize n) {
  std::vector<usize> o(n);
  for (usize i = 0; i < n; ++i) {
    o[i] = i;
  }
  return o;
}

// Brute-force front membership: peel front 0 = the non-dominated set over the
// remaining live genomes, repeat. The reference fast_nondominated_sort is checked
// against this O(N^3) oracle.
[[nodiscard]] std::vector<u16> brute_front_of(const ObjMatrix &obj) {
  const usize n = obj.n;
  std::vector<u16> front_of(n, std::numeric_limits<u16>::max());
  std::vector<bool> placed(n, false);
  u16 front = 0;
  usize remaining = n;
  while (remaining > 0) {
    std::vector<usize> this_front;
    for (usize i = 0; i < n; ++i) {
      if (placed[i]) {
        continue;
      }
      bool dominated = false;
      for (usize j = 0; j < n; ++j) {
        if (placed[j] || i == j) {
          continue;
        }
        if (dominates(obj.row(j), obj.row(i))) {
          dominated = true;
          break;
        }
      }
      if (!dominated) {
        this_front.push_back(i);
      }
    }
    for (const usize i : this_front) {
      front_of[i] = front;
      placed[i] = true;
      --remaining;
    }
    ++front;
  }
  return front_of;
}

// ---------------------------------------------------------------------------
//  dominates — the maximization truth table.
// ---------------------------------------------------------------------------
TEST(ParetoDominates, StrictlyBetter_Dominates) {
  const std::array<f64, 2> a{2.0, 2.0};
  const std::array<f64, 2> b{1.0, 1.0};
  EXPECT_TRUE(dominates(a, b));
  EXPECT_FALSE(dominates(b, a));
}

TEST(ParetoDominates, BetterAndEqual_Dominates) {
  const std::array<f64, 2> a{2.0, 1.0};
  const std::array<f64, 2> b{1.0, 1.0};
  EXPECT_TRUE(dominates(a, b)); // >= on both, > on one
  EXPECT_FALSE(dominates(b, a));
}

TEST(ParetoDominates, Equal_DoesNotDominate) {
  const std::array<f64, 2> a{1.0, 1.0};
  const std::array<f64, 2> b{1.0, 1.0};
  EXPECT_FALSE(dominates(a, b));
  EXPECT_FALSE(dominates(b, a));
}

TEST(ParetoDominates, TradeOff_NeitherDominates) {
  const std::array<f64, 2> a{2.0, 1.0};
  const std::array<f64, 2> b{1.0, 2.0};
  EXPECT_FALSE(dominates(a, b));
  EXPECT_FALSE(dominates(b, a));
}

TEST(ParetoDominates, NaN_TreatedAsNegInf_FiniteDominatesNaN) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::array<f64, 2> finite{0.0, 0.0};
  const std::array<f64, 2> with_nan{nan, 0.0};
  // finite (-> {0,0}) dominates with_nan (-> {-inf, 0}): >= on both, > on obj 0.
  EXPECT_TRUE(dominates(finite, with_nan));
  EXPECT_FALSE(dominates(with_nan, finite));
}

TEST(ParetoDominates, BothNaNSameSlot_NeitherDominates) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::array<f64, 2> a{nan, 1.0};
  const std::array<f64, 2> b{nan, 1.0};
  // Both -> {-inf, 1.0}: equal, so neither dominates.
  EXPECT_FALSE(dominates(a, b));
  EXPECT_FALSE(dominates(b, a));
}

// ---------------------------------------------------------------------------
//  fast_nondominated_sort — front assignment.
// ---------------------------------------------------------------------------
TEST(ParetoSort, SingleGenome_Front0) {
  const std::vector<f64> data{1.0, 2.0};
  const ObjMatrix obj{data, 1, 2};
  const std::vector<u16> front = fast_nondominated_sort(obj, iota_order(1));
  ASSERT_EQ(front.size(), 1u);
  EXPECT_EQ(front[0], 0u);
}

TEST(ParetoSort, TwoFronts_AgreesWithBruteForce) {
  // 4 genomes, 2 objectives (maximization):
  //   g0 {3,3} dominates everyone -> front 0
  //   g1 {2,1}, g2 {1,2} -> trade-off, front 1 (dominated only by g0)
  //   g3 {0,0} -> front 2
  const std::vector<f64> data{3.0, 3.0, 2.0, 1.0, 1.0, 2.0, 0.0, 0.0};
  const ObjMatrix obj{data, 4, 2};
  const std::vector<u16> got = fast_nondominated_sort(obj, iota_order(4));
  const std::vector<u16> ref = brute_front_of(obj);
  EXPECT_EQ(got, ref);
  EXPECT_EQ(got[0], 0u);
  EXPECT_EQ(got[1], 1u);
  EXPECT_EQ(got[2], 1u);
  EXPECT_EQ(got[3], 2u);
}

TEST(ParetoSort, NoLowerFrontDominatesHigherFront) {
  // Assert the structural invariant that no genome in a strictly-lower-indexed
  // front is dominated by any genome in a higher front, and that the result
  // matches the brute-force oracle.
  const std::vector<f64> data{
      5.0, 1.0, // g0
      1.0, 5.0, // g1
      3.0, 3.0, // g2
      2.0, 2.0, // g3
      4.0, 4.0, // g4
      0.0, 0.0  // g5
  };
  const ObjMatrix obj{data, 6, 2};
  const std::vector<u16> got = fast_nondominated_sort(obj, iota_order(6));
  const std::vector<u16> ref = brute_front_of(obj);
  EXPECT_EQ(got, ref);
  for (usize i = 0; i < obj.n; ++i) {
    for (usize j = 0; j < obj.n; ++j) {
      if (dominates(obj.row(j), obj.row(i))) {
        EXPECT_LT(got[j], got[i]) << "dominator j=" << j << " must precede i=" << i;
      }
    }
  }
}

TEST(ParetoSort, AllEqual_OneFront_CanonStable) {
  // All-equal objectives: none dominates any -> a single front 0, every genome.
  const std::vector<f64> data(8, 1.0); // 4 genomes x 2 objectives, all 1.0
  const ObjMatrix obj{data, 4, 2};
  const std::vector<u16> front = fast_nondominated_sort(obj, iota_order(4));
  for (usize i = 0; i < 4; ++i) {
    EXPECT_EQ(front[i], 0u);
  }
  // Canonical-order independence: a permuted canon_order yields the same per-id
  // front (front 0 for all), byte-identical across orders.
  const std::vector<usize> rev{3, 2, 1, 0};
  const std::vector<u16> front_rev = fast_nondominated_sort(obj, rev);
  EXPECT_EQ(front, front_rev);
}

TEST(ParetoSort, NaNObjective_DominatedByFinite) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  // g0 finite, g1 has a NaN objective -> g0 dominates g1; g1 lands in a later front.
  const std::vector<f64> data{1.0, 1.0, nan, 1.0};
  const ObjMatrix obj{data, 2, 2};
  const std::vector<u16> front = fast_nondominated_sort(obj, iota_order(2));
  EXPECT_EQ(front[0], 0u);
  EXPECT_GT(front[1], front[0]);
}

// ---------------------------------------------------------------------------
//  crowding_distance — boundary +inf, interior gaps.
// ---------------------------------------------------------------------------
TEST(ParetoCrowding, Boundaries_AreInfinite) {
  // 3 genomes on one front, 1 objective. Min + max are boundaries (+inf); the
  // middle is interior (finite).
  const std::vector<f64> data{1.0, 2.0, 3.0};
  const ObjMatrix obj{data, 3, 1};
  const std::vector<usize> members{0, 1, 2};
  const std::vector<f64> cd = crowding_distance(obj, members, iota_order(3));
  EXPECT_TRUE(std::isinf(cd[0])); // min boundary
  EXPECT_TRUE(std::isinf(cd[2])); // max boundary
  EXPECT_TRUE(std::isfinite(cd[1]));
  EXPECT_GT(cd[1], 0.0);
}

TEST(ParetoCrowding, TwoMembers_BothBoundary) {
  const std::vector<f64> data{1.0, 5.0, 2.0, 4.0};
  const ObjMatrix obj{data, 2, 2};
  const std::vector<usize> members{0, 1};
  const std::vector<f64> cd = crowding_distance(obj, members, iota_order(2));
  EXPECT_TRUE(std::isinf(cd[0]));
  EXPECT_TRUE(std::isinf(cd[1]));
}

TEST(ParetoCrowding, SingleMember_Infinite) {
  const std::vector<f64> data{3.0, 7.0};
  const ObjMatrix obj{data, 1, 2};
  const std::vector<usize> members{0};
  const std::vector<f64> cd = crowding_distance(obj, members, iota_order(1));
  EXPECT_TRUE(std::isinf(cd[0]));
}

TEST(ParetoCrowding, DegenerateObjective_NoDivByZero) {
  // One objective is constant across the front (range 0) -> that objective must
  // contribute 0, not NaN/inf, to the interior crowding. Interior stays finite.
  const std::vector<f64> data{
      1.0, 5.0, // g0 (obj1 constant = 5)
      2.0, 5.0, // g1
      3.0, 5.0  // g2
  };
  const ObjMatrix obj{data, 3, 2};
  const std::vector<usize> members{0, 1, 2};
  const std::vector<f64> cd = crowding_distance(obj, members, iota_order(3));
  EXPECT_TRUE(std::isinf(cd[0]));
  EXPECT_TRUE(std::isinf(cd[2]));
  EXPECT_TRUE(std::isfinite(cd[1]));
}

TEST(ParetoCrowding, CanonicalTieBreak_IsOrderStable) {
  // Two genomes share the same objective value on the sorted objective; the
  // canonical-id order must resolve the tie identically regardless of member
  // listing order -> the per-id crowding is permutation-invariant.
  const std::vector<f64> data{
      1.0, 1.0, // g0
      2.0, 2.0, // g1
      2.0, 2.0, // g2 (ties g1)
      3.0, 3.0  // g3
  };
  const ObjMatrix obj{data, 4, 2};
  const std::vector<usize> members_a{0, 1, 2, 3};
  const std::vector<usize> members_b{3, 2, 1, 0};
  const std::vector<f64> cd_a = crowding_distance(obj, members_a, iota_order(4));
  const std::vector<f64> cd_b = crowding_distance(obj, members_b, iota_order(4));
  // Per-id crowding identical regardless of member listing order.
  for (usize i = 0; i < 4; ++i) {
    EXPECT_DOUBLE_EQ(cd_a[i], cd_b[i]) << "id " << i;
  }
}

} // namespace

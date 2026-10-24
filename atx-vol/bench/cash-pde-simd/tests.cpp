#include "thomas.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>

#include "atx/vol/api/simd/cpu.hpp"

namespace {
using namespace atx::vol::ddiv_ref::research;

ThomasPack make_pack(std::size_t n, unsigned pattern) {
  ThomasPack p;
  p.rhs.resize(n + 1u);
  p.obstacle.resize(n + 1u);
  p.active.resize(n + 1u);
  p.cp.resize(n + 1u, Quad{123., 124., 125., 126.});
  p.dp = p.cp;
  p.work = p.cp;
  for (std::size_t lane = 0; lane < 4; ++lane) {
    const double x = static_cast<double>(lane + 1u);
    p.rows[lane] = {-.21 * x, 2. + .75 * x, -.16 * x, -x, 2. * x};
    for (std::size_t i = 0; i <= n; ++i) {
      p.rhs[i][lane] = static_cast<double>((i * 31u + lane * 17u) % 101u) / 13. - 3.;
      p.obstacle[i][lane] = static_cast<double>((i * 7u + lane * 5u) % 17u) / 11.;
      p.active[i][lane] = static_cast<std::uint8_t>(pattern == 1u ||
          (pattern == 2u && (i + lane) % 2u == 0) ||
          (pattern == 3u && (i * 19u + lane * 7u) % 11u < 4u));
    }
  }
  return p;
}

void exact(const ThomasPack &a, const ThomasPack &b) {
  for (const auto member : {&ThomasPack::cp, &ThomasPack::dp, &ThomasPack::work}) {
    ASSERT_EQ((a.*member).size(), (b.*member).size());
    for (std::size_t i = 0; i < (a.*member).size(); ++i) {
      for (std::size_t lane = 0; lane < 4; ++lane) {
        EXPECT_EQ(std::bit_cast<std::uint64_t>((a.*member)[i][lane]),
                  std::bit_cast<std::uint64_t>((b.*member)[i][lane]))
            << "row=" << i << " lane=" << lane;
      }
    }
  }
}

TEST(CashThomasResearch, HeterogeneousMasksAndEveryTailMatchAllScalarBits) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  for (const auto n : {1u, 2u, 7u, 64u, 1600u}) {
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
      for (std::uint8_t lanes = 0; lanes < 16; ++lanes) {
        auto scalar = make_pack(n, pattern);
        auto packed = scalar;
        const auto expected = solve_scalar(scalar, lanes);
        const auto actual = solve_avx2(packed, lanes);
        ASSERT_EQ(actual, expected) << "n=" << n << " mask=" << static_cast<unsigned>(lanes);
        exact(packed, scalar);
      }
    }
  }
}

TEST(CashThomasResearch, SignedZerosAndUnusedPoisonRetainBoundaryArithmetic) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  auto scalar = make_pack(7, 2);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (std::size_t lane = 0; lane < 4; ++lane) {
    scalar.rows[lane] = {-0., 1., -0., lane % 2u == 0 ? -0. : 0., -0.};
    for (std::size_t i = 0; i < scalar.rhs.size(); ++i) {
      scalar.rhs[i][lane] = scalar.active[i][lane] != 0 ? nan : -0.;
      scalar.obstacle[i][lane] = scalar.active[i][lane] != 0 ? -0. : nan;
    }
  }
  auto packed = scalar;
  ASSERT_EQ(solve_avx2(packed, 15), solve_scalar(scalar, 15));
  exact(packed, scalar);
}

TEST(CashThomasResearch, InvalidLaneRefusesWithoutContaminatingValidLanes) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  for (unsigned bad = 0; bad < 4; ++bad) {
    auto scalar = make_pack(7, 0);
    if (bad == 0) scalar.rows[1].lo = 1.;
    if (bad == 1) scalar.rows[1].di = 0.;
    if (bad == 2) scalar.rows[1].top = std::numeric_limits<double>::infinity();
    if (bad == 3) scalar.rhs[2][1] = std::numeric_limits<double>::quiet_NaN();
    auto packed = scalar;
    const auto expected = solve_scalar(scalar, 7);
    ASSERT_EQ(expected[1], LaneStatus::InvalidInput);
    ASSERT_EQ(expected[0], LaneStatus::Success);
    ASSERT_EQ(expected[2], LaneStatus::Success);
    ASSERT_EQ(expected[3], LaneStatus::Masked);
    ASSERT_EQ(solve_avx2(packed, 7), expected);
    exact(packed, scalar);
  }
}

TEST(CashThomasResearch, MalformedShapeRefusesBeforeScratchMutation) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  auto p = make_pack(7, 0);
  p.obstacle.pop_back();
  const auto before = p;
  const Statuses refused{LaneStatus::InvalidInput, LaneStatus::InvalidInput,
                         LaneStatus::InvalidInput, LaneStatus::InvalidInput};
  EXPECT_EQ(solve_avx2(p, 15), refused);
  exact(p, before);
  EXPECT_EQ(solve_scalar(p, 15), refused);
  exact(p, before);
}

TEST(CashThomasResearch, NonfiniteArithmeticRefusesOnlyItsOwnLane) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  auto scalar = make_pack(7, 0);
  scalar.rows[0] = {-1., 2., 0., 1.e308, 0.};
  scalar.rhs[1][0] = 1.e308;
  auto packed = scalar;
  const auto expected = solve_scalar(scalar, 15);
  ASSERT_EQ(expected[0], LaneStatus::NonfiniteResult);
  ASSERT_EQ(expected[1], LaneStatus::Success);
  ASSERT_EQ(solve_avx2(packed, 15), expected);
  exact(packed, scalar);
}

TEST(CashThomasResearch, MaskedPoisonDoesNotAlterOtherLanesOrItsScratch) {
  if (!atx::vol::simd::have_avx2()) GTEST_SKIP() << "AVX2+FMA required";
  auto scalar = make_pack(7, 3);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const auto lane : {1u, 3u}) {
    scalar.rows[lane] = {nan, nan, nan, nan, nan};
    for (std::size_t i = 0; i < scalar.rhs.size(); ++i) {
      scalar.rhs[i][lane] = nan;
      scalar.obstacle[i][lane] = nan;
      scalar.cp[i][lane] = nan;
      scalar.dp[i][lane] = nan;
      scalar.work[i][lane] = nan;
    }
  }
  auto packed = scalar;
  ASSERT_EQ(solve_avx2(packed, 5), solve_scalar(scalar, 5));
  exact(packed, scalar);
}

} // namespace

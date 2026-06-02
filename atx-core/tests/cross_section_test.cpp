// cross_section_test.cpp — TDD tests for atx::core::stats cross-sectional
// transforms (WorldQuant-style alpha primitives).
//
// Coverage strategy (agent profile §7):
//   - rank:          seed case, ties-averaged, n==1 boundary, monotonicity.
//   - zscore:        zero-mean output, unit population variance, constant input
//                    (no div0 / NaN), n==1 boundary.
//   - demean:        output sums to zero, n==1 boundary.
//   - winsorize:     upper-tail clamp (seed), lower-tail clamp, identity range,
//                    n==1 boundary.
//   - scale_to_unit: sum of |v| == 1, all-zero left untouched, sign preserved.
//
// All inputs/outputs use f64 spans of equal size per the module contract.

#include "atx/core/stats/cross_section.hpp"
#include "atx/core/types.hpp"

#include <array>
#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

using atx::f64;
using atx::usize;
using namespace atx::core::stats; // NOLINT(google-build-using-namespace)

namespace {

// Population standard deviation of v, used to assert zscore unit variance.
[[nodiscard]] f64 pop_std(std::span<const f64> v) {
    f64 mean = 0.0;
    for (const f64 x : v) { mean += x; }
    mean /= static_cast<f64>(v.size());
    f64 acc = 0.0;
    for (const f64 x : v) {
        const f64 d = x - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<f64>(v.size()));
}

[[nodiscard]] f64 sum_abs(std::span<const f64> v) {
    f64 s = 0.0;
    for (const f64 x : v) { s += std::fabs(x); }
    return s;
}

} // namespace

// ============================================================
//  rank
// ============================================================

TEST(CrossSection, RankNormalized) {
    // Seed case: {10,30,20} → smallest 10→0.0, largest 30→1.0, middle 20→0.5.
    const std::array<f64, 3> in{10.0, 30.0, 20.0};
    std::array<f64, 3> out{};
    rank(std::span<const f64>{in}, std::span<f64>{out});
    EXPECT_DOUBLE_EQ(out[0], 0.0);
    EXPECT_DOUBLE_EQ(out[1], 1.0);
    EXPECT_DOUBLE_EQ(out[2], 0.5);
}

TEST(CrossSection, RankTiesAveraged) {
    // {5,5,1}: smallest 1 → rank 0; the two 5s share rank positions 1 and 2,
    // averaged to 1.5 → normalized 1.5/2 = 0.75 each.
    const std::array<f64, 3> in{5.0, 5.0, 1.0};
    std::array<f64, 3> out{};
    rank(std::span<const f64>{in}, std::span<f64>{out});
    EXPECT_DOUBLE_EQ(out[0], 0.75);
    EXPECT_DOUBLE_EQ(out[1], 0.75);
    EXPECT_DOUBLE_EQ(out[2], 0.0);
}

TEST(CrossSection, RankSingleElement) {
    // n==1: documented to produce 0.0 (no spread to normalize over).
    const std::array<f64, 1> in{42.0};
    std::array<f64, 1> out{};
    rank(std::span<const f64>{in}, std::span<f64>{out});
    EXPECT_DOUBLE_EQ(out[0], 0.0);
}

TEST(CrossSection, RankAllTied) {
    // All equal: every element shares the average of ranks 0..n-1 = (n-1)/2,
    // normalized by (n-1) → 0.5 for all (n>1).
    const std::array<f64, 4> in{7.0, 7.0, 7.0, 7.0};
    std::array<f64, 4> out{};
    rank(std::span<const f64>{in}, std::span<f64>{out});
    for (const f64 r : out) { EXPECT_DOUBLE_EQ(r, 0.5); }
}

// ============================================================
//  zscore
// ============================================================

TEST(CrossSection, ZScoreZeroMean) {
    const std::array<f64, 5> in{1.0, 2.0, 3.0, 4.0, 5.0};
    std::array<f64, 5> out{};
    zscore(std::span<const f64>{in}, std::span<f64>{out});
    f64 s = 0.0;
    for (const f64 x : out) { s += x; }
    EXPECT_NEAR(s, 0.0, 1e-9);
}

TEST(CrossSection, ZScoreUnitVariance) {
    const std::array<f64, 5> in{1.0, 2.0, 3.0, 4.0, 5.0};
    std::array<f64, 5> out{};
    zscore(std::span<const f64>{in}, std::span<f64>{out});
    EXPECT_NEAR(pop_std(std::span<const f64>{out}), 1.0, 1e-9);
}

TEST(CrossSection, ZScoreConstantInputIsZeros) {
    // Population std == 0 → output all zeros, no NaN / divide-by-zero.
    const std::array<f64, 4> in{3.0, 3.0, 3.0, 3.0};
    std::array<f64, 4> out{};
    zscore(std::span<const f64>{in}, std::span<f64>{out});
    for (const f64 x : out) {
        EXPECT_DOUBLE_EQ(x, 0.0);
        EXPECT_FALSE(std::isnan(x));
    }
}

TEST(CrossSection, ZScoreSingleElementIsZero) {
    const std::array<f64, 1> in{99.0};
    std::array<f64, 1> out{};
    zscore(std::span<const f64>{in}, std::span<f64>{out});
    EXPECT_DOUBLE_EQ(out[0], 0.0);
}

// ============================================================
//  demean
// ============================================================

TEST(CrossSection, DemeanSumsZero) {
    std::array<f64, 3> v{1.0, 2.0, 3.0};
    demean(std::span<f64>{v});
    f64 s = 0.0;
    for (const f64 x : v) { s += x; }
    EXPECT_NEAR(s, 0.0, 1e-9);
}

TEST(CrossSection, DemeanSingleElementIsZero) {
    std::array<f64, 1> v{5.0};
    demean(std::span<f64>{v});
    EXPECT_DOUBLE_EQ(v[0], 0.0);
}

// ============================================================
//  winsorize
// ============================================================

TEST(CrossSection, WinsorizeClampsTails) {
    // Seed case: {1,2,3,4,100} winsorized to [Q(0.0), Q(0.8)] clamps 100 <= 4.0.
    // Nearest-rank Q(0.8) of 5 points is the order statistic at round(0.8*4)=3,
    // i.e. value 4.0, so the outlier is pinned to a real observed value.
    std::array<f64, 5> v{1.0, 2.0, 3.0, 4.0, 100.0};
    winsorize(std::span<f64>{v}, 0.0, 0.8);
    EXPECT_LE(v[4], 4.0);
    EXPECT_DOUBLE_EQ(v[4], 4.0);
}

TEST(CrossSection, WinsorizeClampsLowerTail) {
    // Lower bound at Q(0.2) raises the smallest element off the bottom.
    // Nearest-rank Q(0.2) of {-100,2,3,4,5} is round(0.2*4)=1 → value 2.0.
    std::array<f64, 5> v{-100.0, 2.0, 3.0, 4.0, 5.0};
    winsorize(std::span<f64>{v}, 0.2, 1.0);
    EXPECT_GE(v[0], 2.0);
    EXPECT_DOUBLE_EQ(v[0], 2.0);
}

TEST(CrossSection, WinsorizeFullRangeIsIdentity) {
    // [Q(0.0), Q(1.0)] == [min, max], so nothing is clamped.
    std::array<f64, 5> v{1.0, 2.0, 3.0, 4.0, 5.0};
    const std::array<f64, 5> before = v;
    winsorize(std::span<f64>{v}, 0.0, 1.0);
    for (usize i = 0U; i < v.size(); ++i) { EXPECT_DOUBLE_EQ(v[i], before[i]); }
}

TEST(CrossSection, WinsorizeSingleElement) {
    std::array<f64, 1> v{42.0};
    winsorize(std::span<f64>{v}, 0.1, 0.9);
    EXPECT_DOUBLE_EQ(v[0], 42.0);
}

// ============================================================
//  scale_to_unit (L1)
// ============================================================

TEST(CrossSection, ScaleToUnitL1SumsToOne) {
    std::array<f64, 4> v{1.0, -2.0, 3.0, -4.0};
    scale_to_unit(std::span<f64>{v});
    EXPECT_NEAR(sum_abs(std::span<const f64>{v}), 1.0, 1e-12);
}

TEST(CrossSection, ScaleToUnitPreservesSign) {
    std::array<f64, 3> v{2.0, -4.0, 6.0};
    scale_to_unit(std::span<f64>{v});
    EXPECT_GT(v[0], 0.0);
    EXPECT_LT(v[1], 0.0);
    EXPECT_GT(v[2], 0.0);
}

TEST(CrossSection, ScaleToUnitAllZeroUntouched) {
    std::array<f64, 3> v{0.0, 0.0, 0.0};
    scale_to_unit(std::span<f64>{v});
    for (const f64 x : v) { EXPECT_DOUBLE_EQ(x, 0.0); }
}

// Tests for atx::core::simd — vectorized span reductions over xsimd.
//
// Coverage strategy (agent profile §7):
//   - Known-value reductions (sum, dot, mean, min, max) for both f64 and f32.
//   - Tail handling: sizes that are NOT a multiple of the batch width
//     (the AxpyTailHandled / size-3 case) MUST be correct.
//   - Boundary: single-element spans (size 1) for every reduction.
//   - Elementwise ops: scale, add, axpy in place.
//   - Large fixed-seed random cross-check against a scalar reference; the
//     vectorized accumulation order differs from a naive left-fold, so the
//     comparison uses EXPECT_NEAR with a tolerance scaled to the magnitude,
//     not EXPECT_DOUBLE_EQ.

#include "atx/core/simd.hpp"
#include "atx/core/types.hpp"

#include <array>
#include <numeric>
#include <random>
#include <span>
#include <vector>

#include <gtest/gtest.h>

using namespace atx;             // NOLINT(google-build-using-namespace)
namespace simd = atx::core::simd;

namespace {

// Scalar reference reductions for cross-checking the vectorized paths.
template <std::floating_point T>
[[nodiscard]] T scalar_sum(std::span<const T> x) {
    T acc{0};
    for (const T v : x) { acc += v; }
    return acc;
}

template <std::floating_point T>
[[nodiscard]] T scalar_dot(std::span<const T> a, std::span<const T> b) {
    T acc{0};
    for (usize i = 0U; i < a.size(); ++i) { acc += a[i] * b[i]; }
    return acc;
}

} // namespace

// =====================================================================
//  sum
// =====================================================================

TEST(Simd, SumMatchesScalar) {
    std::vector<f64> v(1000U);
    std::iota(v.begin(), v.end(), 0.0); // 0, 1, 2, ... 999
    // Sum of 0..999 = 999 * 1000 / 2 = 499500.
    EXPECT_DOUBLE_EQ(simd::sum<f64>(v), 499500.0);
}

TEST(Simd, SumSingleElement) {
    const std::array<f64, 1> a{42.0};
    EXPECT_DOUBLE_EQ(simd::sum<f64>(a), 42.0);
}

TEST(Simd, SumF32MatchesScalar) {
    const std::array<f32, 5> a{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    EXPECT_FLOAT_EQ(simd::sum<f32>(a), 15.0F);
}

// =====================================================================
//  dot
// =====================================================================

TEST(Simd, DotProduct) {
    const std::array<f64, 4> a{1.0, 2.0, 3.0, 4.0};
    const std::array<f64, 4> b{5.0, 6.0, 7.0, 8.0};
    // 1*5 + 2*6 + 3*7 + 4*8 = 5 + 12 + 21 + 32 = 70.
    EXPECT_DOUBLE_EQ(simd::dot<f64>(a, b), 70.0);
}

TEST(Simd, DotProductF32) {
    const std::array<f32, 3> a{1.0F, 2.0F, 3.0F};
    const std::array<f32, 3> b{4.0F, 5.0F, 6.0F};
    // 4 + 10 + 18 = 32.
    EXPECT_FLOAT_EQ(simd::dot<f32>(a, b), 32.0F);
}

// =====================================================================
//  mean / min / max
// =====================================================================

TEST(Simd, MeanAndMinMax) {
    const std::array<f64, 8> a{3.0, 1.0, 4.0, 1.0, 5.0, 9.0, 2.0, 6.0};
    // Sum = 31, mean = 31/8.
    EXPECT_DOUBLE_EQ(simd::mean<f64>(a), 31.0 / 8.0);
    EXPECT_DOUBLE_EQ(simd::min<f64>(a), 1.0);
    EXPECT_DOUBLE_EQ(simd::max<f64>(a), 9.0);
}

TEST(Simd, MinMaxSingleElement) {
    const std::array<f64, 1> a{7.0};
    EXPECT_DOUBLE_EQ(simd::min<f64>(a), 7.0);
    EXPECT_DOUBLE_EQ(simd::max<f64>(a), 7.0);
    EXPECT_DOUBLE_EQ(simd::mean<f64>(a), 7.0);
}

TEST(Simd, MinMaxWithNegatives) {
    const std::array<f64, 6> a{-3.0, -1.0, -7.0, 2.0, 0.0, -5.0};
    EXPECT_DOUBLE_EQ(simd::min<f64>(a), -7.0);
    EXPECT_DOUBLE_EQ(simd::max<f64>(a), 2.0);
}

// =====================================================================
//  scale / add / axpy (elementwise, in place)
// =====================================================================

TEST(Simd, ScaleInPlace) {
    std::array<f64, 5> x{1.0, 2.0, 3.0, 4.0, 5.0};
    simd::scale<f64>(2.0, x);
    EXPECT_DOUBLE_EQ(x[0], 2.0);
    EXPECT_DOUBLE_EQ(x[1], 4.0);
    EXPECT_DOUBLE_EQ(x[2], 6.0);
    EXPECT_DOUBLE_EQ(x[3], 8.0);
    EXPECT_DOUBLE_EQ(x[4], 10.0);
}

TEST(Simd, AddElementwise) {
    const std::array<f64, 4> a{1.0, 2.0, 3.0, 4.0};
    const std::array<f64, 4> b{10.0, 20.0, 30.0, 40.0};
    std::array<f64, 4> out{};
    simd::add<f64>(a, b, out);
    EXPECT_DOUBLE_EQ(out[0], 11.0);
    EXPECT_DOUBLE_EQ(out[1], 22.0);
    EXPECT_DOUBLE_EQ(out[2], 33.0);
    EXPECT_DOUBLE_EQ(out[3], 44.0);
}

TEST(Simd, AxpyTailHandled) {
    // size 3 is smaller than the f64 batch width on x86-64 (2 or 4), so this
    // exercises the scalar-tail path exclusively.
    const std::array<f64, 3> x{1.0, 2.0, 3.0};
    std::array<f64, 3> y{10.0, 10.0, 10.0};
    simd::axpy<f64>(2.0, x, y);
    EXPECT_DOUBLE_EQ(y[0], 12.0);
    EXPECT_DOUBLE_EQ(y[1], 14.0);
    EXPECT_DOUBLE_EQ(y[2], 16.0);
}

TEST(Simd, AxpyFullPlusTail) {
    // 10 elements: at least one full batch plus a remainder on any width.
    std::array<f64, 10> y{};
    std::array<f64, 10> x{};
    for (usize i = 0U; i < x.size(); ++i) {
        x[i] = static_cast<f64>(i);
        y[i] = 1.0;
    }
    simd::axpy<f64>(3.0, x, y);
    for (usize i = 0U; i < y.size(); ++i) {
        EXPECT_DOUBLE_EQ(y[i], 1.0 + 3.0 * static_cast<f64>(i));
    }
}

TEST(Simd, ScaleF32) {
    std::array<f32, 3> x{1.0F, 2.0F, 3.0F};
    simd::scale<f32>(0.5F, x);
    EXPECT_FLOAT_EQ(x[0], 0.5F);
    EXPECT_FLOAT_EQ(x[1], 1.0F);
    EXPECT_FLOAT_EQ(x[2], 1.5F);
}

// =====================================================================
//  Empty-span edge cases (sum/scale/add over zero elements are well-defined)
// =====================================================================

TEST(Simd, SumEmptyIsZero) {
    const std::span<const f64> empty{};
    EXPECT_DOUBLE_EQ(simd::sum<f64>(empty), 0.0);
}

// =====================================================================
//  Large fixed-seed random cross-check vs scalar reference.
//
//  Vectorized reduction sums lanes in a different order than a naive
//  left-fold, so floating-point results differ in the last few ULPs.
//  We bound the gap with EXPECT_NEAR scaled to the running magnitude.
// =====================================================================

TEST(Simd, LargeRandomSumMatchesScalarWithinTolerance) {
    std::mt19937 rng(12345U);
    std::uniform_real_distribution<f64> dist(-1.0, 1.0);
    std::vector<f64> v(10000U);
    for (f64& e : v) { e = dist(rng); }

    const f64 ref = scalar_sum<f64>(v);
    const f64 got = simd::sum<f64>(v);
    // ~1e4 additions of O(1) magnitude: relative error well under 1e-9.
    EXPECT_NEAR(got, ref, 1e-9 * (1.0 + std::abs(ref)));
}

TEST(Simd, LargeRandomDotMatchesScalarWithinTolerance) {
    std::mt19937 rng(67890U);
    std::uniform_real_distribution<f64> dist(-1.0, 1.0);
    std::vector<f64> a(10000U);
    std::vector<f64> b(10000U);
    for (f64& e : a) { e = dist(rng); }
    for (f64& e : b) { e = dist(rng); }

    const f64 ref = scalar_dot<f64>(a, b);
    const f64 got = simd::dot<f64>(a, b);
    EXPECT_NEAR(got, ref, 1e-9 * (1.0 + std::abs(ref)));
}

TEST(Simd, LargeRandomSumF32MatchesScalarWithinTolerance) {
    std::mt19937 rng(13579U);
    std::uniform_real_distribution<f32> dist(-1.0F, 1.0F);
    std::vector<f32> v(10000U);
    for (f32& e : v) { e = dist(rng); }

    const f32 ref = scalar_sum<f32>(v);
    const f32 got = simd::sum<f32>(v);
    // f32 has far fewer mantissa bits; loosen the tolerance accordingly.
    EXPECT_NEAR(got, ref, 1e-3F * (1.0F + std::abs(ref)));
}

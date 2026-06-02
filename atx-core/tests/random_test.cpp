#include "atx/core/random.hpp"

#include <gtest/gtest.h>

using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// ---- determinism ------------------------------------------------------------

TEST(Random, Deterministic) {
    // Two instances with the same seed must produce identical sequences.
    Xoshiro256pp a{42}, b{42};
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.next_u64(), b.next_u64());
    }
}

TEST(Random, DifferentSeedsDiffer) {
    // Distinct seeds must (with overwhelming probability) diverge immediately.
    Xoshiro256pp a{1}, b{2};
    EXPECT_NE(a.next_u64(), b.next_u64());
}

TEST(Random, CopyReproducesSequence) {
    // A copy carries the full state, so it continues the identical sequence.
    Xoshiro256pp a{12345};
    for (int i = 0; i < 10; ++i) {
        static_cast<void>(a.next_u64()); // advance to a non-initial state
    }
    Xoshiro256pp b = a; // value copy
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.next_u64(), b.next_u64());
    }
}

// ---- uniform01 / uniform ----------------------------------------------------

TEST(Random, UniformInRange) {
    Xoshiro256pp r{7};
    for (int i = 0; i < 1000; ++i) {
        const double u = r.uniform01();
        EXPECT_GE(u, 0.0);
        EXPECT_LT(u, 1.0);
    }
}

TEST(Random, UniformBoundsRespected) {
    Xoshiro256pp r{99};
    constexpr double kLo = -3.5;
    constexpr double kHi = 12.25;
    for (int i = 0; i < 1000; ++i) {
        const double v = r.uniform(kLo, kHi);
        EXPECT_GE(v, kLo);
        EXPECT_LT(v, kHi);
    }
}

// ---- normal -----------------------------------------------------------------

TEST(Random, NormalMeanApproximatelyZero) {
    // Sample mean of standard normals should approach 0. With N=100000 the
    // standard error of the mean is 1/sqrt(N) ≈ 0.00316; a 0.05 tolerance is a
    // very loose (>15-sigma) bound that should never flake on a deterministic
    // stream while still catching gross implementation errors.
    Xoshiro256pp r{2024};
    constexpr int kN = 100000;
    double sum = 0.0;
    for (int i = 0; i < kN; ++i) {
        sum += r.normal();
    }
    const double mean = sum / static_cast<double>(kN);
    EXPECT_NEAR(mean, 0.0, 0.05);
}

TEST(Random, NormalVarianceApproximatelyOne) {
    // Sample variance of standard normals should approach 1. Loose tolerance
    // for the same reason as the mean test.
    Xoshiro256pp r{55};
    constexpr int kN = 100000;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double z = r.normal();
        sum += z;
        sum_sq += z * z;
    }
    const double mean = sum / static_cast<double>(kN);
    const double var = sum_sq / static_cast<double>(kN) - mean * mean;
    EXPECT_NEAR(var, 1.0, 0.05);
}

// ---- bernoulli --------------------------------------------------------------

TEST(Random, BernoulliZeroAlwaysFalse) {
    // p = 0 must never return true since uniform01() >= 0.0.
    Xoshiro256pp r{3};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_FALSE(r.bernoulli(0.0));
    }
}

TEST(Random, BernoulliOneAlwaysTrue) {
    // p = 1 must always return true since uniform01() < 1.0 always holds.
    Xoshiro256pp r{4};
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(r.bernoulli(1.0));
    }
}

TEST(Random, BernoulliRoughFrequency) {
    // p = 0.5 should yield roughly half true over many draws. Loose bound.
    Xoshiro256pp r{77};
    constexpr int kN = 100000;
    int trues = 0;
    for (int i = 0; i < kN; ++i) {
        if (r.bernoulli(0.5)) {
            ++trues;
        }
    }
    const double freq = static_cast<double>(trues) / static_cast<double>(kN);
    EXPECT_NEAR(freq, 0.5, 0.02);
}

// ---- jump -------------------------------------------------------------------

TEST(Random, JumpProducesIndependentStream) {
    // After jump(), a copy advances 2^128 steps and so produces a different
    // value than the un-jumped original.
    Xoshiro256pp a{42};
    auto b = a;
    b.jump();
    EXPECT_NE(a.next_u64(), b.next_u64());
}

TEST(Random, JumpStreamsDifferOverManyDraws) {
    // The original and jumped streams should differ at every position we test
    // (the streams are non-overlapping for 2^128 values).
    Xoshiro256pp a{2026};
    auto b = a;
    b.jump();
    for (int i = 0; i < 100; ++i) {
        EXPECT_NE(a.next_u64(), b.next_u64());
    }
}

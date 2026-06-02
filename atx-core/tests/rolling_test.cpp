// rolling_test.cpp — TDD tests for atx::core::stats rolling-window statistics.
//
// Coverage: the spec seed cases plus boundaries — pre-fill behaviour (window
// not yet full), exact eviction of the oldest sample once full, population
// std-dev, covariance/correlation for perfect linear relationships (positive
// and negative), z-score centring/scaling, and the zero-variance guards.

#include <atx/core/stats/rolling.hpp>

#include <cmath>

#include <gtest/gtest.h>

using atx::f64;
using namespace atx::core::stats;

namespace {

constexpr f64 kTol = 1e-9;

} // namespace

// ============================================================
// RollingMean
// ============================================================

TEST(RollingMean, WindowedAverage) {
    // Spec seed: W=3, the running mean over the last min(count,3) samples.
    RollingMean<3U> m;
    EXPECT_NEAR(m.update(1.0), 1.0, kTol); // {1}        -> 1.0
    EXPECT_NEAR(m.update(2.0), 1.5, kTol); // {1,2}      -> 1.5
    EXPECT_NEAR(m.update(3.0), 2.0, kTol); // {1,2,3}    -> 2.0
    EXPECT_NEAR(m.update(4.0), 3.0, kTol); // {2,3,4}    -> 3.0 (1 evicted)
}

TEST(RollingMean, WindowOfTwoEvictsOldest) {
    // Spec extra: W=2 over {1,2,3} -> last update returns mean of {2,3} = 2.5.
    RollingMean<2U> m;
    EXPECT_NEAR(m.update(1.0), 1.0, kTol);
    EXPECT_NEAR(m.update(2.0), 1.5, kTol);
    EXPECT_NEAR(m.update(3.0), 2.5, kTol);
}

// ============================================================
// RollingStd — population standard deviation
// ============================================================

TEST(RollingStd, PopulationStdOfOneToFive) {
    // Spec extra: population std of {1,2,3,4,5} is sqrt(2).
    RollingStd<5U> s;
    f64 last = 0.0;
    for (const f64 x : {1.0, 2.0, 3.0, 4.0, 5.0}) {
        last = s.update(x);
    }
    EXPECT_NEAR(last, std::sqrt(2.0), kTol);
}

TEST(RollingStd, ConstantInputHasZeroStd) {
    RollingStd<4U> s;
    for (int i = 0; i < 6; ++i) {
        EXPECT_NEAR(s.update(7.0), 0.0, kTol);
    }
}

TEST(RollingStd, MatchesWindowAfterEviction) {
    // Slide a width-3 window over a stream; std must reflect only the last 3.
    RollingStd<3U> s;
    (void)s.update(10.0);
    (void)s.update(10.0);
    (void)s.update(10.0);    // window {10,10,10} -> 0
    EXPECT_NEAR(s.update(13.0), // window {10,10,13}
                std::sqrt(2.0), 1e-9);
    // Population variance of {10,10,13}: mean=11, M2=2+1+4=... var=2, std=sqrt(2).
}

// ============================================================
// RollingZScore
// ============================================================

TEST(RollingZScore, CenteredScale) {
    // Spec seed: W=5, stream {1,2,3,4,5}; final z == 1.2649110640673518.
    // That literal is (5 - 3) / sqrt(2.5), i.e. the score uses the SAMPLE
    // std-dev of {1..5} (variance 10/4 = 2.5), per RollingZScore's contract.
    RollingZScore<5U> z;
    f64 last = 0.0;
    for (const f64 x : {1.0, 2.0, 3.0, 4.0, 5.0}) {
        last = z.update(x);
    }
    EXPECT_NEAR(last, 1.2649110640673518, kTol);
}

TEST(RollingZScore, ZeroVarianceReturnsZero) {
    // Spec extra: constant input -> std == 0 -> z-score guarded to 0.
    RollingZScore<4U> z;
    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(z.update(3.0), 0.0, kTol);
    }
}

// ============================================================
// RollingCovariance — population covariance
// ============================================================

TEST(RollingCovariance, PerfectLinear) {
    // y = 2x over {1..4}, W=4. Population cov = 2 * var(x).
    // var(1..4) population = 1.25, so cov = 2.5.
    RollingCovariance<4U> c;
    f64 last = 0.0;
    for (int i = 1; i <= 4; ++i) {
        last = c.update(static_cast<f64>(i), 2.0 * static_cast<f64>(i));
    }
    EXPECT_NEAR(last, 2.5, kTol);
}

TEST(RollingCovariance, WindowedEviction) {
    // Slide a width-2 window; final cov reflects only the last two pairs.
    RollingCovariance<2U> c;
    (void)c.update(1.0, 1.0);
    (void)c.update(2.0, 2.0);
    // window {(3,9),(4,16)}: mean_x=3.5, mean_y=12.5
    // cov = ((3-3.5)(9-12.5) + (4-3.5)(16-12.5))/2 = (1.75 + 1.75)/2 = 1.75
    (void)c.update(3.0, 9.0);
    EXPECT_NEAR(c.update(4.0, 16.0), 1.75, kTol);
}

// ============================================================
// RollingCorrelation — Pearson correlation
// ============================================================

TEST(RollingCorrelation, PerfectPositive) {
    // Spec seed: W=4, (i, 2i) for i=1..4 -> correlation 1.0.
    RollingCorrelation<4U> c;
    f64 last = 0.0;
    for (int i = 1; i <= 4; ++i) {
        last = c.update(static_cast<f64>(i), 2.0 * static_cast<f64>(i));
    }
    EXPECT_NEAR(last, 1.0, kTol);
}

TEST(RollingCorrelation, PerfectNegative) {
    // Spec extra: (i, -2i) -> correlation -1.0.
    RollingCorrelation<4U> c;
    f64 last = 0.0;
    for (int i = 1; i <= 4; ++i) {
        last = c.update(static_cast<f64>(i), -2.0 * static_cast<f64>(i));
    }
    EXPECT_NEAR(last, -1.0, kTol);
}

TEST(RollingCorrelation, ZeroVarianceReturnsZero) {
    // Spec extra: constant input on either axis -> correlation guarded to 0.
    RollingCorrelation<4U> c;
    f64 last = 1.0;
    for (int i = 1; i <= 4; ++i) {
        last = c.update(static_cast<f64>(i), 5.0); // y constant -> var_y == 0
    }
    EXPECT_NEAR(last, 0.0, kTol);
}

TEST(RollingCorrelation, StaysWithinUnitInterval) {
    // Noisy-ish stream: correlation must remain in [-1, 1].
    RollingCorrelation<5U> c;
    const f64 xs[] = {1.0, 3.0, 2.0, 5.0, 4.0, 6.0, 0.0, 7.0};
    const f64 ys[] = {2.0, 1.0, 4.0, 3.0, 6.0, 5.0, 8.0, 1.0};
    f64 last = 0.0;
    for (int i = 0; i < 8; ++i) {
        last = c.update(xs[i], ys[i]);
        EXPECT_GE(last, -1.0 - kTol);
        EXPECT_LE(last, 1.0 + kTol);
    }
}

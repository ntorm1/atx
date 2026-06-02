// online_stats_test.cpp — TDD tests for atx::core::stats online statistics.
//
// Coverage: the spec seed cases plus boundaries — empty/single-sample,
// population vs sample variance, std-dev, rolling remove(), and the
// sliding-window extremum case where the current extreme leaves the window.

#include <atx/core/stats/online_stats.hpp>

#include <array>

#include <gtest/gtest.h>

using atx::f64;
using atx::usize;
using namespace atx::core::stats;

namespace {

constexpr f64 kTol = 1e-9;

} // namespace

// ============================================================
// RunningMean
// ============================================================

TEST(RunningMean, EmptyHasZeroMeanAndCount) {
    RunningMean m;
    EXPECT_EQ(m.count(), 0U);
    EXPECT_DOUBLE_EQ(m.mean(), 0.0);
}

TEST(RunningMean, UpdateTracksArithmeticMean) {
    RunningMean m;
    m.update(2.0);
    m.update(4.0);
    m.update(6.0);
    EXPECT_EQ(m.count(), 3U);
    EXPECT_NEAR(m.mean(), 4.0, kTol);
}

TEST(RunningMean, RemoveSupportsRolling) {
    RunningMean m;
    m.update(1.0);
    m.update(2.0);
    m.update(3.0);
    m.remove(1.0); // drop the oldest sample of a rolling window
    EXPECT_EQ(m.count(), 2U);
    EXPECT_NEAR(m.mean(), 2.5, kTol);
}

TEST(RunningMean, RemoveDownToEmptyResetsMean) {
    RunningMean m;
    m.update(7.0);
    m.remove(7.0);
    EXPECT_EQ(m.count(), 0U);
    EXPECT_DOUBLE_EQ(m.mean(), 0.0);
}

// ============================================================
// RunningVariance — Welford
// ============================================================

TEST(RunningVariance, MeanAndVariance) {
    // Classic Welford reference dataset.
    const std::array<f64, 8> data{2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
    RunningVariance v;
    for (const f64 x : data) {
        v.update(x);
    }
    EXPECT_EQ(v.count(), 8U);
    EXPECT_NEAR(v.mean(), 5.0, kTol);
    EXPECT_NEAR(v.variance(), 4.0, 1e-9); // population variance
}

TEST(RunningVariance, SampleVsPopulationVariance) {
    const std::array<f64, 8> data{2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
    RunningVariance v;
    for (const f64 x : data) {
        v.update(x);
    }
    // Population variance = 32 / 8 = 4.0; sample variance = 32 / 7.
    EXPECT_NEAR(v.variance(), 4.0, 1e-9);
    EXPECT_NEAR(v.sample_variance(), 32.0 / 7.0, 1e-9);
}

TEST(RunningVariance, StdDevIsSqrtOfPopulationVariance) {
    const std::array<f64, 8> data{2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
    RunningVariance v;
    for (const f64 x : data) {
        v.update(x);
    }
    EXPECT_NEAR(v.std_dev(), 2.0, 1e-9); // sqrt(4.0)
}

TEST(RunningVariance, EmptyAndSingleSampleHaveZeroVariance) {
    RunningVariance v;
    EXPECT_EQ(v.count(), 0U);
    EXPECT_DOUBLE_EQ(v.variance(), 0.0);
    EXPECT_DOUBLE_EQ(v.sample_variance(), 0.0);
    EXPECT_DOUBLE_EQ(v.std_dev(), 0.0);

    v.update(42.0);
    EXPECT_EQ(v.count(), 1U);
    EXPECT_NEAR(v.mean(), 42.0, kTol);
    EXPECT_DOUBLE_EQ(v.variance(), 0.0); // variance of <2 samples is 0
    EXPECT_DOUBLE_EQ(v.sample_variance(), 0.0);
    EXPECT_DOUBLE_EQ(v.std_dev(), 0.0);
}

TEST(RunningVariance, RemoveSupportsRolling) {
    RunningVariance v;
    v.update(1.0);
    v.update(2.0);
    v.update(3.0);
    v.remove(1.0);
    EXPECT_EQ(v.count(), 2U);
    EXPECT_NEAR(v.mean(), 2.5, kTol);
    // Remaining {2,3}: population variance = ((2-2.5)^2 + (3-2.5)^2)/2 = 0.25.
    EXPECT_NEAR(v.variance(), 0.25, 1e-9);
    EXPECT_NEAR(v.sample_variance(), 0.5, 1e-9);
}

TEST(RunningVariance, RollingWindowMatchesDirectRecompute) {
    // Push five samples, then slide a width-3 window forward by remove+update
    // and confirm the variance matches a fresh accumulator over the same window.
    const std::array<f64, 6> stream{10.0, -3.0, 7.0, 100.0, 2.0, 50.0};
    RunningVariance roll;
    roll.update(stream[0]);
    roll.update(stream[1]);
    roll.update(stream[2]);
    for (usize i = 3U; i < stream.size(); ++i) {
        roll.remove(stream[i - 3U]);
        roll.update(stream[i]);
        RunningVariance fresh;
        fresh.update(stream[i - 2U]);
        fresh.update(stream[i - 1U]);
        fresh.update(stream[i]);
        EXPECT_EQ(roll.count(), 3U);
        EXPECT_NEAR(roll.mean(), fresh.mean(), 1e-9);
        EXPECT_NEAR(roll.variance(), fresh.variance(), 1e-9);
    }
}

// ============================================================
// Ewma
// ============================================================

TEST(Ewma, EmptyHasZeroValueAndCount) {
    Ewma e{0.5};
    EXPECT_EQ(e.count(), 0U);
    EXPECT_DOUBLE_EQ(e.value(), 0.0);
}

TEST(Ewma, FirstSampleSeedsValue) {
    Ewma e{0.3};
    e.update(9.0);
    EXPECT_EQ(e.count(), 1U);
    EXPECT_NEAR(e.value(), 9.0, kTol); // first sample initialises value=x
}

TEST(Ewma, BlendsAccordingToAlpha) {
    Ewma e{0.5};
    e.update(0.0);          // value = 0
    e.update(10.0);         // value = 0.5*10 + 0.5*0 = 5
    EXPECT_NEAR(e.value(), 5.0, kTol);
    e.update(10.0);         // value = 0.5*10 + 0.5*5 = 7.5
    EXPECT_NEAR(e.value(), 7.5, kTol);
}

TEST(Ewma, ConvergesToConstant) {
    Ewma e{0.1};
    for (int i = 0; i < 1000; ++i) {
        e.update(5.0);
    }
    EXPECT_EQ(e.count(), 1000U);
    EXPECT_NEAR(e.value(), 5.0, 1e-6);
}

TEST(Ewma, AlphaOneTracksLatest) {
    Ewma e{1.0};
    e.update(3.0);
    e.update(8.0);
    EXPECT_NEAR(e.value(), 8.0, kTol); // alpha==1 ignores history
}

// ============================================================
// RunningMinMax — sliding-window extremes
// ============================================================

TEST(RunningMinMax, EmptyHasZeroCount) {
    RunningMinMax<4U> mm;
    EXPECT_EQ(mm.count(), 0U);
}

TEST(RunningMinMax, TracksWindowExtremes) {
    RunningMinMax<4U> mm;
    for (const f64 x : {3.0, 1.0, 4.0, 1.0, 5.0}) {
        mm.update(x);
    }
    // Window is the last 4 samples {1,4,1,5}.
    EXPECT_EQ(mm.count(), 4U);
    EXPECT_NEAR(mm.max(), 5.0, kTol);
    EXPECT_NEAR(mm.min(), 1.0, kTol);
}

TEST(RunningMinMax, SingleSampleIsBothExtremes) {
    RunningMinMax<3U> mm;
    mm.update(42.0);
    EXPECT_EQ(mm.count(), 1U);
    EXPECT_NEAR(mm.max(), 42.0, kTol);
    EXPECT_NEAR(mm.min(), 42.0, kTol);
}

TEST(RunningMinMax, ExtremeLeavingWindowIsDropped) {
    RunningMinMax<3U> mm;
    mm.update(100.0);          // window {100}
    mm.update(1.0);            // window {100,1}
    mm.update(2.0);            // window {100,1,2} -> max 100
    EXPECT_NEAR(mm.max(), 100.0, kTol);
    mm.update(3.0);            // window {1,2,3} -> 100 slid out, max drops to 3
    EXPECT_EQ(mm.count(), 3U);
    EXPECT_NEAR(mm.max(), 3.0, kTol);
    EXPECT_NEAR(mm.min(), 1.0, kTol);
}

TEST(RunningMinMax, MinLeavingWindowIsDropped) {
    RunningMinMax<3U> mm;
    mm.update(-5.0);           // window {-5}
    mm.update(10.0);           // window {-5,10}
    mm.update(8.0);            // window {-5,10,8} -> min -5
    EXPECT_NEAR(mm.min(), -5.0, kTol);
    mm.update(7.0);            // window {10,8,7} -> -5 slid out, min rises to 7
    EXPECT_NEAR(mm.min(), 7.0, kTol);
    EXPECT_NEAR(mm.max(), 10.0, kTol);
}

TEST(RunningMinMax, MonotonicDecreasingStream) {
    // Strictly decreasing input stresses the max deque (every new sample is a
    // new max) and the min deque (front expires each step).
    RunningMinMax<3U> mm;
    for (const f64 x : {9.0, 8.0, 7.0, 6.0, 5.0}) {
        mm.update(x);
    }
    // Window {7,6,5}.
    EXPECT_NEAR(mm.max(), 7.0, kTol);
    EXPECT_NEAR(mm.min(), 5.0, kTol);
}

TEST(RunningMinMax, WindowOfOneAlwaysEqualsLatest) {
    RunningMinMax<1U> mm;
    for (const f64 x : {4.0, 9.0, 2.0}) {
        mm.update(x);
        EXPECT_NEAR(mm.max(), x, kTol);
        EXPECT_NEAR(mm.min(), x, kTol);
        EXPECT_EQ(mm.count(), 1U);
    }
}

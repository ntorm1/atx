// quantile_test.cpp — TDD tests for atx::core::stats::P2Quantile.
//
// The P² (Jain & Chlamtac) algorithm estimates a p-quantile from a stream in
// constant memory (5 markers) without storing the history.  It is an estimator,
// not an exact percentile, so the accuracy assertions use spec tolerances.
//
// Coverage: the spec seed cases (uniform median, ramp p90, uniform quartile)
// plus boundaries — fewer-than-5 samples, a small known sorted set, monotonic
// input, count() correctness, and precondition checks.

#include <atx/core/stats/quantile.hpp>

#include <random>

#include <gtest/gtest.h>

using atx::f64;
using atx::usize;
using atx::core::stats::P2Quantile;

// ============================================================
// Spec seed cases
// ============================================================

TEST(P2Quantile, ApproximatesMedianUniform) {
    // p=0.5 over 100k uniform[0,1) samples should land near 0.5.
    P2Quantile q{0.5};
    std::mt19937 rng{1};
    std::uniform_real_distribution<f64> dist{0.0, 1.0};
    for (int i = 0; i < 100000; ++i) {
        q.update(dist(rng));
    }
    EXPECT_EQ(q.count(), 100000U);
    EXPECT_NEAR(q.value(), 0.5, 0.02);
}

TEST(P2Quantile, P90OnRamp) {
    // p=0.9 over the ramp 1..1000 -> the 90th percentile is ~900.
    P2Quantile q{0.9};
    for (int i = 1; i <= 1000; ++i) {
        q.update(static_cast<f64>(i));
    }
    EXPECT_EQ(q.count(), 1000U);
    EXPECT_NEAR(q.value(), 900.0, 20.0);
}

TEST(P2Quantile, ApproximatesQuartileUniform) {
    // p=0.25 over 100k uniform[0,1) samples should land near 0.25.
    P2Quantile q{0.25};
    std::mt19937 rng{1};
    std::uniform_real_distribution<f64> dist{0.0, 1.0};
    for (int i = 0; i < 100000; ++i) {
        q.update(dist(rng));
    }
    EXPECT_NEAR(q.value(), 0.25, 0.02);
}

// ============================================================
// Small / fewer-than-5 sample behaviour
// ============================================================

TEST(P2Quantile, ThreeSamplesGivesMiddleForMedian) {
    // Fewer than 5 samples: value() interpolates over the buffered samples.
    // For p=0.5 and three sorted samples the answer is the middle element.
    P2Quantile q{0.5};
    q.update(3.0);
    q.update(1.0);
    q.update(2.0);
    EXPECT_EQ(q.count(), 3U);
    EXPECT_NEAR(q.value(), 2.0, 1e-12);
}

TEST(P2Quantile, SingleSampleReturnsThatSample) {
    P2Quantile q{0.5};
    q.update(42.0);
    EXPECT_EQ(q.count(), 1U);
    EXPECT_NEAR(q.value(), 42.0, 1e-12);
}

TEST(P2Quantile, FourSamplesMedianInterpolates) {
    // p=0.5 over {1,2,3,4}: linear-interpolated median is 2.5.
    P2Quantile q{0.5};
    q.update(4.0);
    q.update(1.0);
    q.update(3.0);
    q.update(2.0);
    EXPECT_EQ(q.count(), 4U);
    EXPECT_NEAR(q.value(), 2.5, 1e-12);
}

TEST(P2Quantile, ExactlyFiveSamplesMedianIsMiddle) {
    // At exactly 5 samples the markers initialise; q[2] is the sorted middle.
    P2Quantile q{0.5};
    for (const f64 x : {5.0, 3.0, 1.0, 4.0, 2.0}) {
        q.update(x);
    }
    EXPECT_EQ(q.count(), 5U);
    EXPECT_NEAR(q.value(), 3.0, 1e-12);
}

// ============================================================
// Monotonic input & known set
// ============================================================

TEST(P2Quantile, MedianOfSmallKnownSet) {
    // Median of 1..9 is 5.  P² is exact on a fully-sorted small ramp here.
    P2Quantile q{0.5};
    for (int i = 1; i <= 9; ++i) {
        q.update(static_cast<f64>(i));
    }
    EXPECT_NEAR(q.value(), 5.0, 1e-9);
}

TEST(P2Quantile, MonotonicInputStaysWithinRange) {
    // A strictly increasing stream must never push the estimate outside the
    // observed range, and the median should sit in the interior.
    P2Quantile q{0.5};
    for (int i = 0; i < 1000; ++i) {
        q.update(static_cast<f64>(i));
    }
    const f64 v = q.value();
    EXPECT_GE(v, 0.0);
    EXPECT_LE(v, 999.0);
    EXPECT_NEAR(v, 499.5, 20.0);
}

// ============================================================
// count() correctness
// ============================================================

TEST(P2Quantile, CountTracksSamples) {
    P2Quantile q{0.5};
    EXPECT_EQ(q.count(), 0U);
    for (usize i = 1U; i <= 20U; ++i) {
        q.update(static_cast<f64>(i));
        EXPECT_EQ(q.count(), i);
    }
}

// ============================================================
// Preconditions (debug-only ATX_ASSERT)
// ============================================================

#ifndef NDEBUG
TEST(P2Quantile, RejectsOutOfRangeProbability) {
    EXPECT_DEATH({ P2Quantile q{0.0}; (void)q; }, "");
    EXPECT_DEATH({ P2Quantile q{1.0}; (void)q; }, "");
}
#endif

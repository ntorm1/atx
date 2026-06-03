// Smoke test for the umbrella header: instantiate one representative type from
// each layer to prove <atx/core/core.hpp> compiles, links, and the modules
// interoperate. Per-module behaviour is covered by the dedicated test files.

#include "atx/core/core.hpp"

#include <gtest/gtest.h>

#include <array>
#include <span>

TEST(Core, UmbrellaHeaderExposesEveryLayer) {
    // L1 numeric: fixed-point decimal.
    const auto price = atx::core::Decimal::from_int(10);
    EXPECT_EQ(price.to_string(), "10.0");

    // L3 container: static-capacity vector.
    atx::core::container::FixedVector<int, 4> fv;
    fv.push_back(1);
    fv.push_back(2);
    EXPECT_EQ(fv.size(), 2U);

    // L4 concurrency: SPSC ring.
    atx::core::concurrent::SpscQueue<int, 8> q;
    EXPECT_TRUE(q.try_push(7));
    int popped = 0;
    EXPECT_TRUE(q.try_pop(popped));
    EXPECT_EQ(popped, 7);

    // L5 SIMD: span reduction.
    const std::array<double, 4> data{1.0, 2.0, 3.0, 4.0};
    EXPECT_DOUBLE_EQ(atx::core::simd::sum(std::span<const double>(data)), 10.0);

    // L6 statistics: Welford running variance.
    atx::core::stats::RunningVariance rv;
    for (const double x : data) {
        rv.update(x);
    }
    EXPECT_DOUBLE_EQ(rv.mean(), 2.5);

    // L8 time: nanosecond timestamp ordering.
    const auto t0 = atx::core::time::Timestamp::from_unix_seconds(1);
    EXPECT_GT(t0, atx::core::time::Timestamp::epoch());

    // L9 series: aligned columnar buffer.
    atx::core::series::Column<double> col;
    col.append(42.0);
    EXPECT_EQ(col.size(), 1U);
}

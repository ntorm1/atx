// linalg_test.cpp — TDD tests for atx::core::linalg
//
// Order: seed tests (from spec) + extras covering the zero-copy span bridges
// (write-through aliasing, const views, column-major matrix layout) and a few
// Eigen sanity ops routed through the typed aliases.

#include <atx/core/linalg/linalg.hpp>

#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>

using namespace atx::core::linalg;

// ============================================================
// Seed tests (from spec)
// ============================================================

TEST(Linalg, MapSpanToVector) {
    std::array<double, 3> a{1, 2, 3};
    auto v = as_vector(std::span<double>(a));
    EXPECT_DOUBLE_EQ(v.sum(), 6.0);
}

TEST(Linalg, MatVecProduct) {
    Mat2 m;
    m << 1, 2, 3, 4;
    Vec2 x;
    x << 1, 1;
    Vec2 y = m * x;
    EXPECT_DOUBLE_EQ(y[0], 3.0);
    EXPECT_DOUBLE_EQ(y[1], 7.0);
}

// ============================================================
// Extras
// ============================================================

// The Map aliases the caller's storage: writes through the view land in the
// backing array.
TEST(Linalg, AsVectorWritesThroughToSpan) {
    std::array<double, 3> a{1, 2, 3};
    auto v = as_vector(std::span<double>(a));
    v[1] = 42.0;
    EXPECT_DOUBLE_EQ(a[1], 42.0);
    // And scaling the whole view is reflected element-wise in the array.
    v *= 2.0;
    EXPECT_DOUBLE_EQ(a[0], 2.0);
    EXPECT_DOUBLE_EQ(a[1], 84.0);
    EXPECT_DOUBLE_EQ(a[2], 6.0);
}

// Column-major: a 2x2 span {1,2,3,4} fills column 0 first, then column 1.
TEST(Linalg, AsMatrixColumnMajorLayout) {
    std::array<double, 4> a{1, 2, 3, 4};
    auto m = as_matrix(std::span<double>(a), 2, 2);
    EXPECT_DOUBLE_EQ(m(0, 0), 1.0);
    EXPECT_DOUBLE_EQ(m(1, 0), 2.0);
    EXPECT_DOUBLE_EQ(m(0, 1), 3.0);
    EXPECT_DOUBLE_EQ(m(1, 1), 4.0);

    // mat * vec routed through the view: [[1,3],[2,4]] * [1,1] = [4,6].
    Vec2 x;
    x << 1, 1;
    Vec2 y = m * x;
    EXPECT_DOUBLE_EQ(y[0], 4.0);
    EXPECT_DOUBLE_EQ(y[1], 6.0);
}

// const std::span -> VecMapConst; read-only reductions still work.
TEST(Linalg, AsVectorConstViewReductions) {
    const std::array<double, 4> a{1, 2, 3, 4};
    auto v = as_vector(std::span<const double>(a));
    EXPECT_DOUBLE_EQ(v.sum(), 10.0);
    EXPECT_DOUBLE_EQ(v.dot(v), 30.0); // 1+4+9+16
}

// const matrix view sums to the same total regardless of layout.
TEST(Linalg, AsMatrixConstView) {
    const std::array<double, 6> a{1, 2, 3, 4, 5, 6};
    auto m = as_matrix(std::span<const double>(a), 3, 2);
    EXPECT_EQ(m.rows(), 3);
    EXPECT_EQ(m.cols(), 2);
    EXPECT_DOUBLE_EQ(m.sum(), 21.0);
}

// Dynamic-sized vector op through the VecX alias.
TEST(Linalg, DynamicVectorOp) {
    VecX a = VecX::Constant(5, 2.0);
    VecX b = VecX::Constant(5, 3.0);
    VecX c = a + b;
    EXPECT_EQ(c.size(), 5);
    EXPECT_DOUBLE_EQ(c.sum(), 25.0); // 5 * (2+3)
}

// 3x2 matrix view: Aᵀ·A is 2x2 and symmetric; check a couple of entries.
TEST(Linalg, AsMatrixTransposeTimesItself) {
    // Column-major: col0 = {1,2,3}, col1 = {4,5,6}.
    std::array<double, 6> a{1, 2, 3, 4, 5, 6};
    auto m = as_matrix(std::span<double>(a), 3, 2);
    Mat2 g = m.transpose() * m;
    EXPECT_DOUBLE_EQ(g(0, 0), 14.0); // 1+4+9
    EXPECT_DOUBLE_EQ(g(1, 1), 77.0); // 16+25+36
    EXPECT_DOUBLE_EQ(g(0, 1), 32.0); // 4+10+18
    EXPECT_DOUBLE_EQ(g(0, 1), g(1, 0));
}

// Non-array backing storage (std::vector) maps just as well.
TEST(Linalg, AsVectorOverVector) {
    std::vector<double> data{10, 20, 30, 40};
    auto v = as_vector(std::span<double>(data));
    EXPECT_EQ(v.size(), 4);
    EXPECT_DOUBLE_EQ(v.mean(), 25.0);
}

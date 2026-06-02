// algo_test.cpp — TDD tests for atx::core::stats ranking/selection algorithms.
//
// Coverage strategy (agent profile §7):
//   - argsort:      seed case, stability on ties, single element, both T types.
//   - argsort_desc: descending order, stability on ties (ties keep input order).
//   - topk:         seed case, k==n (full descending), k==1 (max), tie handling.
//   - partial_rank: inverse permutation of argsort, distinct ranks on ties.
// Element types exercised: both double and int, per the task spec.

#include "atx/core/stats/algo.hpp"
#include "atx/core/types.hpp"

#include <array>
#include <span>

#include <gtest/gtest.h>

using atx::usize;
using namespace atx::core::stats; // NOLINT(google-build-using-namespace)

namespace {

// Helper: wrap a fixed array of T as a span<const T>.
template <class T, std::size_t N>
[[nodiscard]] std::span<const T> cspan(const std::array<T, N>& a) noexcept {
    return std::span<const T>{a.data(), a.size()};
}

} // namespace

// ============================================================
//  argsort (ascending, stable)
// ============================================================

TEST(Algo, ArgsortAscending) {
    // Seed case: {3,1,2} sorts ascending to indices {1,2,0}.
    const std::array<double, 3> in{3.0, 1.0, 2.0};
    std::array<usize, 3> out{};
    argsort(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 3>{1U, 2U, 0U}));
}

TEST(Algo, ArgsortAscendingInt) {
    const std::array<int, 3> in{3, 1, 2};
    std::array<usize, 3> out{};
    argsort(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 3>{1U, 2U, 0U}));
}

TEST(Algo, ArgsortStableOnTies) {
    // Equal values must keep their original input order.
    // {2,1,2,1} → ascending: the two 1s first (indices 1,3), then the two 2s
    // (indices 0,2) → {1,3,0,2}.
    const std::array<int, 4> in{2, 1, 2, 1};
    std::array<usize, 4> out{};
    argsort(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 4>{1U, 3U, 0U, 2U}));
}

TEST(Algo, ArgsortSingleElement) {
    const std::array<double, 1> in{42.0};
    std::array<usize, 1> out{};
    argsort(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out[0], 0U);
}

TEST(Algo, ArgsortEmpty) {
    const std::array<double, 0> in{};
    std::array<usize, 0> out{};
    argsort(cspan(in), std::span<usize>{out}); // must not crash on empty input
    SUCCEED();
}

// ============================================================
//  argsort_desc (descending, stable)
// ============================================================

TEST(Algo, ArgsortDescending) {
    // {3,1,2} sorts descending to indices {0,2,1}.
    const std::array<double, 3> in{3.0, 1.0, 2.0};
    std::array<usize, 3> out{};
    argsort_desc(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 3>{0U, 2U, 1U}));
}

TEST(Algo, ArgsortDescStableOnTies) {
    // Descending and stable: equal values keep their original input order.
    // {2,1,2,1} → the two 2s first (indices 0,2), then the two 1s (1,3).
    const std::array<int, 4> in{2, 1, 2, 1};
    std::array<usize, 4> out{};
    argsort_desc(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 4>{0U, 2U, 1U, 3U}));
}

// ============================================================
//  topk (k largest, largest-first, ties by smaller original index)
// ============================================================

TEST(Algo, TopKIndices) {
    // Seed case: {5,1,4,2,3}, largest-2 → indices {0,2}.
    const std::array<double, 5> in{5.0, 1.0, 4.0, 2.0, 3.0};
    std::array<usize, 2> out{};
    topk(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 2>{0U, 2U}));
}

TEST(Algo, TopKIndicesInt) {
    const std::array<int, 5> in{5, 1, 4, 2, 3};
    std::array<usize, 2> out{};
    topk(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 2>{0U, 2U}));
}

TEST(Algo, TopKEqualsNIsFullDescending) {
    // k == n must return the full descending order (same as argsort_desc).
    const std::array<double, 3> in{3.0, 1.0, 2.0};
    std::array<usize, 3> out{};
    topk(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 3>{0U, 2U, 1U}));
}

TEST(Algo, TopKOneReturnsMaxIndex) {
    // k == 1 returns the single max index.
    const std::array<double, 5> in{5.0, 1.0, 4.0, 2.0, 3.0};
    std::array<usize, 1> out{};
    topk(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out[0], 0U);
}

TEST(Algo, TopKTieBreaksBySmallerIndex) {
    // {4,4,1}, k=2 → both 4s; ties broken by smaller original index → {0,1}.
    const std::array<int, 3> in{4, 4, 1};
    std::array<usize, 2> out{};
    topk(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 2>{0U, 1U}));
}

TEST(Algo, TopKZero) {
    // k == 0 is a valid no-op (empty output span).
    const std::array<double, 3> in{3.0, 1.0, 2.0};
    std::array<usize, 0> out{};
    topk(cspan(in), std::span<usize>{out});
    SUCCEED();
}

// ============================================================
//  partial_rank (inverse permutation of argsort)
// ============================================================

TEST(Algo, PartialRank) {
    // {3,1,2} → ascending ranks {2,0,1}: 3 is largest (rank 2), 1 smallest (0).
    const std::array<double, 3> in{3.0, 1.0, 2.0};
    std::array<usize, 3> out{};
    partial_rank(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 3>{2U, 0U, 1U}));
}

TEST(Algo, PartialRankInt) {
    const std::array<int, 3> in{3, 1, 2};
    std::array<usize, 3> out{};
    partial_rank(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 3>{2U, 0U, 1U}));
}

TEST(Algo, PartialRankTiesGetDistinctRanks) {
    // {2,1,2,1}: argsort is {1,3,0,2}, so ranks are the inverse:
    //   index 0 → rank 2, index 1 → rank 0, index 2 → rank 3, index 3 → rank 1.
    const std::array<int, 4> in{2, 1, 2, 1};
    std::array<usize, 4> out{};
    partial_rank(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out, (std::array<usize, 4>{2U, 0U, 3U, 1U}));
}

TEST(Algo, PartialRankSingleElement) {
    const std::array<double, 1> in{42.0};
    std::array<usize, 1> out{};
    partial_rank(cspan(in), std::span<usize>{out});
    EXPECT_EQ(out[0], 0U);
}

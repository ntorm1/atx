#pragma once

// atx::core::stats — ranking and selection algorithms over spans.
//
// Span-based, header-only templates that produce index permutations and ranks
// of an input sequence WITHOUT modifying it.  The element type is a single
// template parameter; the index/length type is std::size_t throughout.
//
//   argsort(in, out_idx)       — indices that sort `in` ASCENDING, STABLE.
//   argsort_desc(in, out_idx)  — descending variant, STABLE.
//   topk(in, out_idx)          — indices of the k largest, largest-first;
//                                ties broken by smaller original index.
//   partial_rank(in, out_rank) — out_rank[i] = ascending 0-based rank of i
//                                (the inverse permutation of argsort).
//
// Determinism / stability:
//   Every comparator breaks value ties by ORIGINAL INDEX, so the result is a
//   total order and fully deterministic.  For ascending sorts smaller index
//   wins a tie (input order preserved); for the descending sort we still break
//   ties by smaller index so equal values keep their input order (a stable
//   descending order, not a reversed ascending one).
//
// Allocation:
//   argsort / argsort_desc allocate NOTHING — they fill the caller-provided
//   out_idx span via std::iota and std::stable_sort in place, so they are
//   noexcept.
//   topk and partial_rank need an index array of size n = in.size() that is
//   independent of the (possibly smaller) output span, so each uses a local
//   std::vector<std::size_t> of size n.  This O(n) scratch is inherent to
//   k-selection and to building an inverse permutation; it is documented at
//   each call site and means those two functions are NOT noexcept.
//
// Complexity:
//   argsort / argsort_desc : O(n log n).
//   topk                   : O(n + k log k) via std::nth_element then a partial
//                            sort of the top-k slice.
//   partial_rank           : O(n log n) (one stable argsort + scatter).
//
// Thread-safety: these are pure functions of their inputs; no shared state.

#include <algorithm> // std::stable_sort, std::nth_element, std::sort
#include <cstddef>   // std::size_t
#include <numeric>   // std::iota
#include <span>      // std::span
#include <vector>    // std::vector (documented scratch for topk/partial_rank)

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // atx::usize

namespace atx::core::stats {

// ============================================================
//  argsort (ascending, stable)
// ============================================================

/// Fill out_idx with the indices that sort `in` in ASCENDING order, stably.
///
/// Ties (equal values) keep their original relative order.  out_idx is both the
/// scratch and the result: it is seeded 0..n-1 and stable-sorted in place by
/// comparing the referenced values, so no allocation occurs.
///
/// @param in       Values to rank (not modified).
/// @param out_idx  Output permutation; filled with a permutation of 0..n-1.
/// @pre out_idx.size() == in.size().
template <class T>
void argsort(std::span<const T> in, std::span<std::size_t> out_idx) noexcept {
    ATX_ASSERT(out_idx.size() == in.size());
    std::iota(out_idx.begin(), out_idx.end(), std::size_t{0U});
    // stable_sort makes the index tie-break redundant for equal values, but the
    // value comparison alone already yields the stable ascending permutation.
    std::stable_sort(out_idx.begin(), out_idx.end(),
                     [in](std::size_t a, std::size_t b) noexcept {
                         return in[a] < in[b];
                     });
}

// ============================================================
//  argsort_desc (descending, stable)
// ============================================================

/// Fill out_idx with the indices that sort `in` in DESCENDING order, stably.
///
/// Ties keep their original input order (this is a stable descending sort, not
/// a reversed ascending one): the comparator places larger values first and
/// leaves equal values in input order via std::stable_sort.
///
/// @param in       Values to rank (not modified).
/// @param out_idx  Output permutation; filled with a permutation of 0..n-1.
/// @pre out_idx.size() == in.size().
template <class T>
void argsort_desc(std::span<const T> in, std::span<std::size_t> out_idx) noexcept {
    ATX_ASSERT(out_idx.size() == in.size());
    std::iota(out_idx.begin(), out_idx.end(), std::size_t{0U});
    std::stable_sort(out_idx.begin(), out_idx.end(),
                     [in](std::size_t a, std::size_t b) noexcept {
                         return in[a] > in[b];
                     });
}

// ============================================================
//  topk (k largest, largest-first)
// ============================================================

/// Fill out_idx with the indices of the k LARGEST elements, largest-first.
///
/// k == out_idx.size().  Ties (equal values) are broken by SMALLER original
/// index, both for membership in the top-k and for the final ordering, so the
/// result is deterministic.  Uses std::nth_element to partition the largest k
/// indices in O(n), then sorts only that k-slice in O(k log k).
///
/// SAFETY/ALLOC: needs an index array of size n = in.size() that is distinct
/// from the (smaller) output span, so it uses a local std::vector<std::size_t>.
/// O(n) scratch is inherent to k-selection; hence this function is not noexcept.
///
/// @param in       Values to select from (not modified).
/// @param out_idx  Output, size k <= in.size(); filled largest-first.
/// @pre out_idx.size() <= in.size().
template <class T>
void topk(std::span<const T> in, std::span<std::size_t> out_idx) {
    const std::size_t n = in.size();
    const std::size_t k = out_idx.size();
    ATX_ASSERT(k <= n);
    if (k == 0U) { return; }

    // "Greater" order with smaller-index tie-break: a ranks before b if it is
    // larger, or equal-and-earlier.  This single comparator drives both the
    // selection and the final largest-first ordering.
    const auto greater = [in](std::size_t a, std::size_t b) noexcept {
        if (in[a] != in[b]) { return in[a] > in[b]; }
        return a < b;
    };

    std::vector<std::size_t> idx(n); // documented O(n) selection scratch
    std::iota(idx.begin(), idx.end(), std::size_t{0U});

    // Partition so the first k indices are the k largest (unordered among
    // themselves).  nth_element needs a strict-weak ordering, which `greater`
    // is.  When k == n the nth element is one-past-the-range, so skip it.
    if (k < n) {
        std::nth_element(idx.begin(),
                         idx.begin() + static_cast<std::ptrdiff_t>(k),
                         idx.end(), greater);
    }
    // Order the selected k slice largest-first (and by index on ties).
    std::sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(k), greater);
    std::copy(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(k),
              out_idx.begin());
}

// ============================================================
//  partial_rank (inverse permutation of argsort)
// ============================================================

/// Fill out_rank[i] with the ascending 0-based rank of element i.
///
/// This is the inverse permutation of argsort: if argsort(in) == p, then
/// out_rank[p[r]] == r.  Ties get DISTINCT ranks assigned by original order
/// (the smaller index gets the smaller rank), matching the stable argsort.
///
/// SAFETY/ALLOC: builds the sorted index permutation in a local
/// std::vector<std::size_t> of size n (out_rank holds the inverse, so it cannot
/// double as the argsort scratch).  Hence this function is not noexcept.
///
/// @param in        Values to rank (not modified).
/// @param out_rank  Output ranks; out_rank.size() == in.size().
/// @pre out_rank.size() == in.size().
template <class T>
void partial_rank(std::span<const T> in, std::span<std::size_t> out_rank) {
    const std::size_t n = in.size();
    ATX_ASSERT(out_rank.size() == n);

    std::vector<std::size_t> order(n); // documented O(n) permutation scratch
    std::iota(order.begin(), order.end(), std::size_t{0U});
    std::stable_sort(order.begin(), order.end(),
                     [in](std::size_t a, std::size_t b) noexcept {
                         return in[a] < in[b];
                     });
    // Scatter: the element at position r in ascending order has rank r.
    for (std::size_t r = 0U; r < n; ++r) {
        out_rank[order[r]] = r;
    }
}

} // namespace atx::core::stats

#pragma once

// atx::core::stats — cross-sectional transforms (WorldQuant-style alpha
// primitives).
//
// Span-based, header-only functions that normalize or reshape a vector of
// f64 observations relative to the OTHER elements in the same vector (the
// "cross section" at a single point in time). These are the bread-and-butter
// neutralization steps in systematic-alpha pipelines.
//
// Public API (namespace atx::core::stats), all over std::span<const f64> in /
// std::span<f64> out of EQUAL size, or in-place std::span<f64>:
//
//   rank(in, out)            — normalized ordinal rank in [0,1], TIES AVERAGED:
//                              smallest → 0.0, largest → 1.0, linear between as
//                              rank_index/(n-1). n==1 → 0.0 (no spread).
//   zscore(in, out)          — (x - mean)/std, POPULATION std. std==0 → zeros.
//   demean(v)                — subtract the mean in place.
//   winsorize(v, lo_q, hi_q) — clamp each value into [Q(lo_q), Q(hi_q)] where
//                              Q(q) is the nearest-rank q-quantile of v (snaps
//                              to an observed value; see quantile_sorted for the
//                              rationale vs. linear interpolation).
//   scale_to_unit(v)         — L1-normalize so Σ|v| == 1; all-zero left as-is.
//   scale_to_unit_l2(v)      — L2-normalize so the Euclidean norm == 1.
//
// Allocation:
//   rank and winsorize need a sorted view that is independent of the input,
//   so each uses a local std::vector<f64> (rank also a std::vector<usize>) of
//   size n. This O(n) scratch is documented at the call site and means those
//   two functions are NOT noexcept. zscore / demean / scale_to_unit / the L2
//   variant allocate nothing and are noexcept.
//
// Numerics:
//   zscore uses POPULATION std (divide by n, not n-1) so a degenerate constant
//   vector maps cleanly to all-zeros rather than a NaN. Quantiles (winsorize)
//   use the NEAREST-RANK order statistic — Q(q) == s[round(q*(n-1))] on a sorted
//   copy — NOT linear interpolation; see quantile_sorted for why (interpolating
//   across a wide tail gap would re-admit the outlier winsorize must clamp).
//
// Thread-safety: pure functions of their inputs; no shared state.

#include <algorithm> // std::sort, std::clamp, std::stable_sort
#include <cmath>     // std::sqrt, std::fabs, std::floor
#include <span>      // std::span
#include <vector>    // std::vector (documented scratch for rank/winsorize)

#include "atx/core/macro.hpp"     // ATX_ASSERT
#include "atx/core/simd.hpp"      // atx::core::simd::mean
#include "atx/core/stats/algo.hpp" // atx::core::stats::argsort (rank tie-break)
#include "atx/core/types.hpp"     // atx::f64, atx::usize

namespace atx::core::stats {

namespace detail {

/// Nearest-rank q-quantile of an ALREADY-SORTED span.
///
/// For sorted s of length n>=1 and q in [0,1], returns the element at the rank
/// position nearest to q*(n-1): pos = round(q*(n-1)), then s[pos].
///
/// RATIONALE (convention choice): the WorldQuant-style winsorize contract this
/// module must satisfy requires Q(0.8) of {1,2,3,4,100} to be <= 4.0 (the 100
/// must be clamped to the 4th order statistic, value 4). The textbook "type 7"
/// LINEAR-interpolation quantile instead yields 4 + 0.2*(100-4) = 23.2, which
/// does NOT clamp the outlier as required. Linear interpolation across the wide
/// gap between the 4th value (4) and the outlier (100) re-introduces exactly the
/// outlier influence winsorize exists to remove. We therefore use the
/// nearest-rank order statistic, which snaps each quantile to an actual observed
/// value and so caps the tails at genuine data points. This also makes both
/// bounds land on order statistics, keeping winsorize idempotent at the tails.
///
/// @param sorted  Ascending-sorted values, non-empty.
/// @param q       Quantile in [0,1].
/// @return        The order statistic at the nearest rank to q.
[[nodiscard]] inline f64 quantile_sorted(std::span<const f64> sorted, f64 q) noexcept {
    ATX_ASSERT(!sorted.empty());
    ATX_ASSERT(q >= 0.0 && q <= 1.0);
    const usize n = sorted.size();
    if (n == 1U) { return sorted[0]; }
    const f64 pos = std::floor(q * static_cast<f64>(n - 1U) + 0.5); // round-half-up
    const auto idx = static_cast<usize>(pos);
    // pos in [0, n-1] because q in [0,1]; clamp defensively against fp drift.
    return sorted[(idx < n) ? idx : (n - 1U)];
}

} // namespace detail

// ============================================================
//  rank — normalized ordinal rank in [0,1], ties averaged
// ============================================================

/// Fill out with the normalized ordinal rank of each element of in.
///
/// Smallest value → 0.0, largest → 1.0, evenly spaced by rank_index/(n-1).
/// TIES ARE AVERAGED: a run of equal values shares the mean of the rank
/// positions they occupy (e.g. ranks 1 and 2 → 1.5). For n==1 the output is
/// 0.0 (there is no spread to normalize against).
///
/// SAFETY/ALLOC: uses two local std::vectors of size n (an argsort permutation
/// of usize and an averaged-rank buffer of f64). O(n) scratch is inherent to
/// tie-averaging; hence this function is NOT noexcept.
///
/// @param in   Values to rank (not modified).
/// @param out  Output ranks in [0,1]; out.size() == in.size().
/// @pre out.size() == in.size().
inline void rank(std::span<const f64> in, std::span<f64> out) {
    ATX_ASSERT(in.size() == out.size());
    const usize n = in.size();
    if (n == 0U) { return; }
    if (n == 1U) { out[0] = 0.0; return; }

    std::vector<usize> order(n); // documented O(n) ascending permutation scratch
    argsort(in, std::span<usize>{order});

    // Walk the sorted order, grouping equal values into ties. Each member of a
    // tie run spanning sorted positions [lo, hi) gets the average position
    // (lo + hi - 1)/2, then normalized by (n-1) into [0,1].
    const f64 denom = static_cast<f64>(n - 1U);
    usize     lo    = 0U;
    while (lo < n) {
        usize hi = lo + 1U;
        while (hi < n && in[order[hi]] == in[order[lo]]) { ++hi; }
        const f64 avg_pos = (static_cast<f64>(lo) + static_cast<f64>(hi - 1U)) * 0.5;
        const f64 norm    = avg_pos / denom;
        for (usize p = lo; p < hi; ++p) { out[order[p]] = norm; }
        lo = hi;
    }
}

// ============================================================
//  zscore — (x - mean) / population std
// ============================================================

/// Fill out with the population z-score of each element of in.
///
/// Computes (x - mean)/std using the POPULATION standard deviation (÷n). If the
/// standard deviation is zero (a constant vector), out is filled with zeros
/// rather than NaN — the documented degenerate case.
///
/// @param in   Values to standardize (not modified).
/// @param out  Output z-scores; out.size() == in.size().
/// @pre out.size() == in.size().
inline void zscore(std::span<const f64> in, std::span<f64> out) noexcept {
    ATX_ASSERT(in.size() == out.size());
    const usize n = in.size();
    if (n == 0U) { return; }

    const f64 mean = simd::mean(in);
    f64       var  = 0.0;
    for (const f64 x : in) {
        const f64 d = x - mean;
        var += d * d;
    }
    var /= static_cast<f64>(n);
    const f64 sd = std::sqrt(var);
    if (sd == 0.0) {
        for (f64& o : out) { o = 0.0; }
        return;
    }
    const f64 inv = 1.0 / sd;
    for (usize i = 0U; i < n; ++i) { out[i] = (in[i] - mean) * inv; }
}

// ============================================================
//  demean — subtract the mean in place
// ============================================================

/// Subtract the arithmetic mean of v from every element, in place.
///
/// After the call Σv == 0 (to floating-point rounding). No-op for an empty
/// span; for n==1 the single element becomes 0.0.
///
/// @param v  Values to center (mutated). v.size() may be 0.
inline void demean(std::span<f64> v) noexcept {
    const usize n = v.size();
    if (n == 0U) { return; }
    const f64 mean = simd::mean(std::span<const f64>{v});
    for (f64& x : v) { x -= mean; }
}

// ============================================================
//  winsorize — clamp into [Q(lo_q), Q(hi_q)]
// ============================================================

/// Clamp every element of v into the quantile band [Q(lo_q), Q(hi_q)].
///
/// Q(q) is the NEAREST-RANK q-quantile of v's current values (computed on a
/// sorted copy taken before any clamping; see detail::quantile_sorted for the
/// convention rationale). Each element is then clamped into [lo, hi]. This caps
/// the influence of outliers without discarding data: tails are pinned to real
/// observed order statistics, so e.g. winsorize({1,2,3,4,100}, 0.0, 0.8) pulls
/// 100 down to 4 (the 0.8-quantile order statistic), not to an interpolated
/// value biased upward by the outlier itself.
///
/// SAFETY/ALLOC: uses a local std::vector<f64> of size n to hold the sorted
/// copy whose quantiles define the band; hence this function is NOT noexcept.
///
/// @param v     Values to winsorize (mutated). v.size() may be 0.
/// @param lo_q  Lower quantile in [0, hi_q].
/// @param hi_q  Upper quantile in [lo_q, 1].
/// @pre 0 <= lo_q <= hi_q <= 1.
inline void winsorize(std::span<f64> v, f64 lo_q, f64 hi_q) {
    ATX_ASSERT(lo_q >= 0.0 && lo_q <= hi_q && hi_q <= 1.0);
    const usize n = v.size();
    if (n == 0U) { return; }

    std::vector<f64> sorted(v.begin(), v.end()); // documented O(n) sort scratch
    std::sort(sorted.begin(), sorted.end());
    const std::span<const f64> s{sorted};
    const f64 lo = detail::quantile_sorted(s, lo_q);
    const f64 hi = detail::quantile_sorted(s, hi_q);
    // lo <= hi because lo_q <= hi_q and the sorted quantile is monotone in q.
    for (f64& x : v) { x = std::clamp(x, lo, hi); }
}

// ============================================================
//  scale_to_unit — L1 normalization (Σ|v| == 1)
// ============================================================

/// Scale v in place so that the sum of absolute values equals 1 (L1 norm).
///
/// If every element is zero (L1 norm == 0) the vector is left unchanged — there
/// is no finite scale that yields a unit norm. Signs are preserved.
///
/// @param v  Values to normalize (mutated). v.size() may be 0.
inline void scale_to_unit(std::span<f64> v) noexcept {
    f64 l1 = 0.0;
    for (const f64 x : v) { l1 += std::fabs(x); }
    if (l1 == 0.0) { return; }
    const f64 inv = 1.0 / l1;
    for (f64& x : v) { x *= inv; }
}

// ============================================================
//  scale_to_unit_l2 — L2 normalization (Euclidean norm == 1)
// ============================================================

/// Scale v in place so that its Euclidean (L2) norm equals 1.
///
/// If the L2 norm is zero (all elements zero) the vector is left unchanged.
/// Signs are preserved.
///
/// @param v  Values to normalize (mutated). v.size() may be 0.
inline void scale_to_unit_l2(std::span<f64> v) noexcept {
    f64 ss = 0.0;
    for (const f64 x : v) { ss += x * x; }
    if (ss == 0.0) { return; }
    const f64 inv = 1.0 / std::sqrt(ss);
    for (f64& x : v) { x *= inv; }
}

} // namespace atx::core::stats

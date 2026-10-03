#pragma once

// atx::engine::research::fields -- the numpy reductions a field manifest reports, reproduced
// operation for operation so the engine's numbers equal the Python builder's bit for bit (no
// tolerance), signed zeros included:
//
//   numpy_pairwise_sum     ndarray.sum() of a contiguous float64 vector (numpy 1.26
//                          DOUBLE_pairwise_sum): n < 8 sums from 0.0 in order; n <= 128 runs eight
//                          accumulators over blocks of eight, combines them as
//                          ((r0 + r1) + (r2 + r3)) + ((r4 + r5) + (r6 + r7)) and adds the tail in
//                          order; larger n splits at n2 = n / 2 rounded down to a multiple of 8 and
//                          recurses (depth at most log2(n), bounded by 64 for any size_t n).
//   numpy_reduce_min/max   ndarray.min() / max() of a contiguous finite float64 vector (numpy 1.26
//                          simd_reduce_c_{min,max}_f64 as dispatched on an AVX2 host, the
//                          reference host of the Python builder): the first element seeds four
//                          lanes, blocks of 32 and then 4 fold in lane-wise, the lanes fold
//                          (0,2), (1,3), then the pair, then the tail one by one. Every fold is
//                          x86 MINPD / MAXPD: `a < b ? a : b`, so of two equal values (-0.0 and
//                          +0.0) the second operand wins and the lane tree decides which zero comes
//                          back. On an AVX-512 or SSE-only host numpy uses 8 or 2 lanes and may
//                          return the other zero; only the sign of a zero can differ.
//   numpy_quantiles        numpy.quantile(values, probabilities, method="linear",
//                          overwrite_input=True): kth = unique([0, -1, previous, next]) made
//                          non-negative and sorted, values partitioned in place by numpy's
//                          introselect (selection.cpp: median-of-3 quickselect, median-of-medians-5
//                          fallback, dumb select for kth - low < 3, the max scan for kth = n - 1,
//                          a pivot stack of 50), then numpy's _lerp of the neighbours. Which zero
//                          lands at an index depends on the partition, so the partition is numpy's.
//   numpy_linear_quantile  the same interpolation on an already sorted vector (v = (n - 1) * q;
//                          previous floor(v), or -1 with both neighbours the last element when
//                          v >= n - 1; gamma = v - previous; gamma >= .5 ? b - (b - a)(1 - gamma)
//                          : a + (b - a) gamma).
//   rounded_fraction       Python round(finite / member, 6) (correctly rounded to 6 decimals,
//                          then parsed back).
//
// Every product sits in its own statement: clang's default -ffp-contract=on fuses a multiply and an
// add only inside one expression, so no FMA can change a result even in a build with FMA enabled
// (rel-avx2).

#include <optional>
#include <span>

#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

[[nodiscard]] f64 numpy_pairwise_sum(std::span<const f64> values) noexcept;

// Precondition: `values` is non-empty and finite.
[[nodiscard]] f64 numpy_reduce_min(std::span<const f64> values) noexcept;
[[nodiscard]] f64 numpy_reduce_max(std::span<const f64> values) noexcept;

// At most this many probabilities per call (the manifest asks five).
inline constexpr usize kMaxQuantiles = 8;

// Partitions `values` in place and writes one quantile per probability into `out`.
// Preconditions: `values` non-empty and finite; out.size() == probabilities.size() <=
// kMaxQuantiles; 0 <= p <= 1 (asserted).
void numpy_quantiles(std::span<f64> values, std::span<const f64> probabilities,
                     std::span<f64> out) noexcept;

// Precondition: `sorted` is non-empty and ascending; 0 <= q <= 1.
[[nodiscard]] f64 numpy_linear_quantile(std::span<const f64> sorted, f64 q) noexcept;

// nullopt when member == 0.
[[nodiscard]] std::optional<f64> rounded_fraction(u64 finite, u64 member);

} // namespace atx::engine::research::fields

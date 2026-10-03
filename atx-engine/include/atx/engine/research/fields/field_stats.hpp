#pragma once

// atx::engine::research::fields -- the numpy reductions a field manifest reports, reproduced
// operation for operation so the engine's numbers equal the Python builder's bit for bit (no
// tolerance):
//
//   numpy_pairwise_sum     ndarray.sum() of a contiguous float64 vector (numpy 1.26
//                          DOUBLE_pairwise_sum): n < 8 sums from 0.0 in order; n <= 128 runs eight
//                          accumulators over blocks of eight, combines them as
//                          ((r0 + r1) + (r2 + r3)) + ((r4 + r5) + (r6 + r7)) and adds the tail in
//                          order; larger n splits at n2 = n / 2 rounded down to a multiple of 8 and
//                          recurses (depth at most log2(n), bounded by 64 for any size_t n).
//   numpy_linear_quantile  numpy.quantile(..., method="linear") of sorted values:
//                          v = (n - 1) * q, lower index floor(v) (the last index, with "previous"
//                          -1, when v >= n - 1), gamma = v - previous, and numpy's _lerp:
//                          gamma >= .5 ? b - (b - a)(1 - gamma) : a + (b - a) gamma.
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

// Precondition: `sorted` is non-empty and ascending; 0 <= q <= 1.
[[nodiscard]] f64 numpy_linear_quantile(std::span<const f64> sorted, f64 q) noexcept;

// nullopt when member == 0.
[[nodiscard]] std::optional<f64> rounded_fraction(u64 finite, u64 member);

} // namespace atx::engine::research::fields

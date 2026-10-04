#pragma once

// atx::engine::research::numpy -- numpy 1.26's float64 reductions, in numpy's own order, so a
// statistic the research tools computed with numpy is reproduced to the bit where numpy's
// order is fixed (P9 B2, tie rule of the eval_tie fixture: tolerance 0 where reduction order
// allows). Header-only; shared by research/ledger and research/eval.
//
//   sum / mean / var / std   np.add.reduce over a contiguous 1-D run: numpy's pairwise_sum
//                            (loops_utils.h.src: n < 8 a plain loop, n <= 128 eight
//                            accumulators combined ((r0+r1)+(r2+r3))+((r4+r5)+(r6+r7)) then the
//                            tail, larger halves split at a multiple of 8), added to the
//                            reduction identity 0.0. mean = sum / n; var = sum((x - mean)^2) /
//                            (n - ddof) (numpy _var); std = sqrt(var).
//   quantile_linear          np.quantile(..., method="linear") on a sorted run: virtual index
//                            (n - 1) q, numpy's _lerp (b - (b - a)(1 - g) when g >= 0.5).
//
// A column reduction of a C-contiguous matrix (axis 0) is NOT this order: numpy adds rows one
// after another, so callers sum those sequentially. The equalities were checked bit for bit
// against numpy 1.26.4 for n = 1..300, 499, 500, 512, 1000, 1007 and 4999 (lane B2 report).
//
// pairwise_sum recurses on halves; the depth is bounded by log2(n / 128) + 1.

#include <array>
#include <cmath>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::research::numpy {

inline constexpr atx::usize kPairwiseBlock = 128U;

[[nodiscard]] inline atx::f64 pairwise_sum(std::span<const atx::f64> a) noexcept {
  const atx::usize n = a.size();
  if (n < 8U) {
    atx::f64 res = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      res += a[i];
    }
    return res;
  }
  if (n <= kPairwiseBlock) {
    std::array<atx::f64, 8> r = {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]};
    atx::usize i = 8U;
    const atx::usize body = n - (n % 8U);
    for (; i < body; i += 8U) {
      for (atx::usize k = 0; k < 8U; ++k) {
        r[k] += a[i + k];
      }
    }
    atx::f64 res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
    for (; i < n; ++i) {
      res += a[i];
    }
    return res;
  }
  atx::usize n2 = n / 2U;
  n2 -= n2 % 8U;
  return pairwise_sum(a.subspan(0, n2)) + pairwise_sum(a.subspan(n2));
}

// np.sum of a 1-D float64 run.
[[nodiscard]] inline atx::f64 sum(std::span<const atx::f64> a) noexcept {
  return 0.0 + pairwise_sum(a);
}

// np.mean; NaN for an empty run (numpy warns and returns NaN).
[[nodiscard]] inline atx::f64 mean(std::span<const atx::f64> a) noexcept {
  return sum(a) / static_cast<atx::f64>(a.size());
}

// np.var(a, ddof): NaN or inf when n <= ddof (numpy divides by max(n - ddof, 0)).
[[nodiscard]] inline atx::f64 var(std::span<const atx::f64> a, atx::usize ddof) {
  const atx::usize n = a.size();
  const atx::f64 m = sum(a) / static_cast<atx::f64>(n);
  std::vector<atx::f64> sq(n);
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 d = a[i] - m;
    sq[i] = d * d;
  }
  const atx::usize dof = n > ddof ? n - ddof : 0U;
  return sum(sq) / static_cast<atx::f64>(dof);
}

// np.std(a, ddof).
[[nodiscard]] inline atx::f64 std_dev(std::span<const atx::f64> a, atx::usize ddof) {
  return std::sqrt(var(a, ddof));
}

// np.quantile(a, q) (linear) of an ascending-sorted, non-empty run; q in [0, 1].
[[nodiscard]] inline atx::f64 quantile_linear(std::span<const atx::f64> sorted,
                                              atx::f64 q) noexcept {
  const atx::usize n = sorted.size();
  const atx::f64 v = static_cast<atx::f64>(n - 1U) * q;
  const atx::f64 prev = std::floor(v);
  const atx::f64 g = v - prev;
  atx::usize i0 = n - 1U;
  atx::usize i1 = n - 1U;
  if (v < static_cast<atx::f64>(n - 1U)) {
    i0 = static_cast<atx::usize>(prev);
    i1 = i0 + 1U;
  }
  const atx::f64 lo = sorted[i0];
  const atx::f64 hi = sorted[i1];
  const atx::f64 diff = hi - lo;
  if (g >= 0.5) {
    return hi - diff * (1.0 - g);
  }
  return lo + diff * g;
}

} // namespace atx::engine::research::numpy

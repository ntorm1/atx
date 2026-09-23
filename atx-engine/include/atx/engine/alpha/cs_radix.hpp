#pragma once

// atx::engine::alpha — LSD radix argsort for the cross-sectional rank family
// (Lane 1). A drop-in, BIT-IDENTICAL replacement for
//
//     std::stable_sort(ids, [&](a, b) { return x[a] < x[b]; })
//
// over a NaN-free id set, used by cs_rank_row / cs_quantile_row / cs_group_row
// (cs_ops.hpp) once a row is large enough for radix to beat the comparison sort.
//
// KEY MAP (order-preserving, f64 -> u64):
//   * v + 0.0 first, so -0.0 and +0.0 share ONE key — `<` treats them as equal,
//     so the comparison sort ties them and breaks the tie by input order; the
//     radix sort must do the same.
//   * sign-flip: a non-negative double gets its sign bit set; a negative double
//     has every bit inverted. Unsigned order of the result == numeric order of
//     the doubles (sub-normals and +/-inf included).
//   * NaN -> ~0 (sorts after +inf). All NaNs share that key, so they keep their
//     input order — deterministic, "NaNs last". (The row kernels never pass a
//     NaN: the valid set excludes them.)
//
// STABILITY: LSD radix with a forward, prefix-summed scatter is stable in every
// pass, so equal keys keep their INPUT order — exactly stable_sort's tie-break.
// That is what keeps the existing CsValidSet tie-order pins meaningful.
//
// COST: 8 byte passes over (key, id) pairs; a pass whose byte is constant across
// the row (common in the exponent bytes) is skipped. All 8 histograms are built
// in one read pass. Scratch is caller-owned and grows monotonically, so a warm
// call allocates nothing.
//
// Header-only; every function is `inline`.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::alpha::detail {

// Below this row size the comparison sort is as fast as radix (histogram and
// pass setup dominate), so cs_stable_argsort keeps std::stable_sort there. Both
// paths produce the SAME permutation, so the threshold is a pure perf knob.
inline constexpr atx::usize kCsRadixMinRow = 96;

// Order-preserving u64 key of a double. See the header comment for the map.
[[nodiscard]] inline atx::u64 cs_radix_key(atx::f64 v) noexcept {
  if (std::isnan(v)) {
    return ~atx::u64{0};
  }
  const atx::f64 canon = v + 0.0; // -0.0 -> +0.0 (IEEE round-to-nearest)
  atx::u64 bits = 0;
  std::memcpy(&bits, &canon, sizeof(bits));
  constexpr atx::u64 kSign = atx::u64{1} << 63U;
  return (bits & kSign) != 0 ? ~bits : (bits | kSign);
}

// Caller-owned radix scratch: ping-pong key and id buffers. Grow-only.
struct CsRadixScratch {
  std::vector<atx::u64> ka;
  std::vector<atx::u64> kb;
  std::vector<atx::u32> ia;
  std::vector<atx::u32> ib;

  void ensure(atx::usize n) {
    if (ka.size() < n) {
      ka.resize(n);
      kb.resize(n);
      ia.resize(n);
      ib.resize(n);
    }
  }
};

// Stable LSD radix sort of the (key, id) pairs in (k, id) of length n, using
// (k2, id2) as the ping-pong buffer. Returns true iff the sorted result ended in
// the SECOND buffer (odd number of executed passes).
// SAFETY: every scatter index is a prefix-summed bucket offset < n, and each
// bucket receives exactly its histogram count, so writes stay in [0, n).
inline bool cs_radix_sort_pairs(atx::u64 *k, atx::u32 *id, atx::u64 *k2, atx::u32 *id2,
                                atx::usize n) noexcept {
  std::array<std::array<atx::u32, 256>, 8> hist{};
  for (atx::usize i = 0; i < n; ++i) {
    const atx::u64 key = k[i];
    for (unsigned b = 0; b < 8U; ++b) {
      ++hist[b][static_cast<atx::usize>((key >> (8U * b)) & 0xFFU)];
    }
  }
  bool in_second = false;
  const atx::u64 first_key = k[0];
  for (unsigned b = 0; b < 8U; ++b) {
    std::array<atx::u32, 256> &h = hist[b];
    const atx::usize first_byte = static_cast<atx::usize>((first_key >> (8U * b)) & 0xFFU);
    if (h[first_byte] == n) {
      continue; // every key shares this byte: the pass is the identity
    }
    atx::u32 run = 0;
    for (atx::u32 &c : h) {
      const atx::u32 cnt = c;
      c = run;
      run += cnt;
    }
    const atx::u64 *src_k = in_second ? k2 : k;
    const atx::u32 *src_i = in_second ? id2 : id;
    atx::u64 *dst_k = in_second ? k : k2;
    atx::u32 *dst_i = in_second ? id : id2;
    for (atx::usize i = 0; i < n; ++i) {
      const atx::u64 key = src_k[i];
      const atx::u32 pos = h[static_cast<atx::usize>((key >> (8U * b)) & 0xFFU)]++;
      dst_k[pos] = key;
      dst_i[pos] = src_i[i];
    }
    in_second = !in_second;
  }
  return in_second;
}

// Sort `ids` in place ascending by x[id] with the radix path unconditionally,
// stable w.r.t. the input order of `ids`; NaN values sort last. Precondition:
// every id < x.size() and < 2^32.
inline void cs_radix_argsort(std::span<const atx::f64> x, std::span<atx::usize> ids,
                             CsRadixScratch &s) {
  const atx::usize n = ids.size();
  if (n < 2) {
    return;
  }
  s.ensure(n);
  for (atx::usize i = 0; i < n; ++i) {
    const atx::usize id = ids[i];
    ATX_ASSERT(id < x.size() && id <= std::numeric_limits<atx::u32>::max());
    s.ka[i] = cs_radix_key(x[id]);
    s.ia[i] = static_cast<atx::u32>(id);
  }
  const bool in_second = cs_radix_sort_pairs(s.ka.data(), s.ia.data(), s.kb.data(), s.ib.data(), n);
  const atx::u32 *res = in_second ? s.ib.data() : s.ia.data();
  for (atx::usize i = 0; i < n; ++i) {
    ids[i] = res[i];
  }
}

// Size-dispatched stable argsort: std::stable_sort for small rows, radix above
// kCsRadixMinRow. Both produce the identical permutation on NaN-free input.
inline void cs_stable_argsort(std::span<const atx::f64> x, std::span<atx::usize> ids,
                              CsRadixScratch &s) {
  if (ids.size() < kCsRadixMinRow) {
    std::stable_sort(ids.begin(), ids.end(),
                     [&](atx::usize a, atx::usize b) { return x[a] < x[b]; });
    return;
  }
  cs_radix_argsort(x, ids, s);
}

} // namespace atx::engine::alpha::detail

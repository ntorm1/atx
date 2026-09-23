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
// COST: one MSD bucketing scatter on the top ~log2(n) varying key bits, then
// insertion sort per (typically O(1)-sized) bucket; oversized buckets fall back
// to the byte-LSD radix (8 byte passes, constant bytes skipped). Scratch is
// caller-owned and grows monotonically, so a warm call allocates nothing.
//
// Header-only; every function is `inline`.

#include <algorithm>
#include <array>
#include <bit>
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
  std::vector<atx::u32> hist; // MSD bucket counts / offsets

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

// Stable insertion sort of the (key, id) pairs of length n by key. Strict `>`
// keeps equal keys in their current order. Used on the small buckets of the
// MSD split, where it beats any pass-based sort.
inline void cs_insertion_sort_pairs(atx::u64 *k, atx::u32 *id, atx::usize n) noexcept {
  for (atx::usize i = 1; i < n; ++i) {
    const atx::u64 key = k[i];
    const atx::u32 v = id[i];
    atx::usize j = i;
    while (j > 0 && k[j - 1] > key) {
      k[j] = k[j - 1];
      id[j] = id[j - 1];
      --j;
    }
    k[j] = key;
    id[j] = v;
  }
}

// Buckets of at most this many pairs are finished by insertion sort; larger ones
// (a dense cluster, e.g. every key but one outlier) by the LSD radix above.
inline constexpr atx::usize kCsBucketMaxInsertion = 32;

// Sort `ids` in place ascending by x[id] with the radix path unconditionally,
// stable w.r.t. the input order of `ids`; NaN values sort last. Precondition:
// every id < x.size() and < 2^32.
//
// ALGORITHM (MSD split + finish): keys are rebased on the row minimum and
// bucketed by their top ~log2(n) significant bits of (key - kmin) — one stable
// counting scatter. A dense row (the usual case: prices, returns, volumes) then
// has O(1) pairs per bucket, each finished by insertion sort; an oversized bucket
// (a cluster squeezed by an outlier) is finished by the byte-LSD radix, which
// skips the bytes its keys share. Every step is stable and orders by the full
// key, so the permutation is the stable_sort permutation — bucketing only
// changes the cost.
inline void cs_radix_argsort(std::span<const atx::f64> x, std::span<atx::usize> ids,
                             CsRadixScratch &s) {
  const atx::usize n = ids.size();
  if (n < 2) {
    return;
  }
  s.ensure(n);
  atx::u64 kmin = ~atx::u64{0};
  atx::u64 kmax = 0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::usize id = ids[i];
    ATX_ASSERT(id < x.size() && id <= std::numeric_limits<atx::u32>::max());
    const atx::u64 key = cs_radix_key(x[id]);
    s.ka[i] = key;
    s.ia[i] = static_cast<atx::u32>(id);
    kmin = std::min(kmin, key);
    kmax = std::max(kmax, key);
  }
  if (kmin == kmax) {
    return; // every key ties: the stable order is the input order
  }
  const auto range_bits = static_cast<unsigned>(std::bit_width(kmax - kmin));
  const unsigned bucket_bits =
      std::clamp(static_cast<unsigned>(std::bit_width(n)), 8U, 16U);
  // SAFETY: range_bits <= 64 and bucket_bits >= 8, so shift <= 56 < 64.
  const unsigned shift = range_bits > bucket_bits ? range_bits - bucket_bits : 0U;
  const atx::usize buckets = static_cast<atx::usize>((kmax - kmin) >> shift) + 1;
  s.hist.assign(buckets, 0);
  for (atx::usize i = 0; i < n; ++i) {
    ++s.hist[static_cast<atx::usize>((s.ka[i] - kmin) >> shift)];
  }
  atx::u32 run = 0;
  atx::u32 max_cnt = 0;
  for (atx::u32 &c : s.hist) {
    const atx::u32 cnt = c;
    max_cnt = std::max(max_cnt, cnt);
    c = run;
    run += cnt;
  }
  // Stable scatter ka/ia -> kb/ib; afterwards hist[b] == end of bucket b.
  for (atx::usize i = 0; i < n; ++i) {
    const atx::u64 key = s.ka[i];
    const atx::u32 pos = s.hist[static_cast<atx::usize>((key - kmin) >> shift)]++;
    s.kb[pos] = key;
    s.ib[pos] = s.ia[i];
  }
  if (max_cnt <= kCsBucketMaxInsertion) {
    // Every bucket is small: ONE insertion pass over the whole array finishes
    // them all (a pair never moves past a smaller-keyed bucket, since the bucket
    // is monotone in the key) with a predictable loop and O(n * max_cnt) worst.
    cs_insertion_sort_pairs(s.kb.data(), s.ib.data(), n);
    for (atx::usize i = 0; i < n; ++i) {
      ids[i] = s.ib[i];
    }
    return;
  }
  atx::usize start = 0;
  for (atx::usize b = 0; b < buckets; ++b) {
    const atx::usize end = s.hist[b];
    const atx::usize cnt = end - start;
    if (cnt > kCsBucketMaxInsertion) {
      // ka/ia are free now: the bucket's ping-pong partner is the same range.
      if (cs_radix_sort_pairs(s.kb.data() + start, s.ib.data() + start, s.ka.data() + start,
                              s.ia.data() + start, cnt)) {
        std::copy_n(s.ia.data() + start, cnt, s.ib.data() + start);
      }
    } else if (cnt > 1) {
      cs_insertion_sort_pairs(s.kb.data() + start, s.ib.data() + start, cnt);
    }
    start = end;
  }
  for (atx::usize i = 0; i < n; ++i) {
    ids[i] = s.ib[i];
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

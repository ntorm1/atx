#pragma once

// atx::engine::factory — output fingerprint: phenotype-level dedup (L3).
//
// The canonical hash (even the semantic one) dedups by STRUCTURE. Two genomes
// with different trees can still emit the same trading signal — `rank(close)`
// vs `rank(close*1.0001)`, `close` vs `rank(close)`, `ts_mean(x,1)` vs `x`. For
// a rank-traded long/short book those are the same alpha, and re-scoring them
// burns fitness work and inflates the trial count with near-clones.
//
// output_fingerprint hashes the RANK-QUANTIZED signal on a fixed probe slice:
//   * per probe row (one date), valid cells are ranked with AVERAGE tie ranks,
//     mapped to a percentile p in [0,1] (a singleton row -> 0.5), and bucketed
//     q = min(n_quant-1, floor(p * n_quant)); a NaN cell -> a reserved sentinel;
//   * the bucket stream is folded with a fixed, seedless FNV-1a (stable across
//     processes and platforms).
// So any strictly-monotone transform of the signal (per date) collides, while a
// sign flip or a genuinely different ordering does not. Average tie ranks make a
// constant row map to one bucket regardless of instrument order.
//
// The probe slice is picked by `probe_rows` — evenly spaced dates across the
// panel — so warm-up NaNs at the head do not dominate and the choice depends on
// (dates, rows) only: deterministic and worker-invariant.
//
// Header-only; the per-row sort allocates a scratch buffer owned by the caller
// (FingerprintScratch) so a worker reuses it across genomes.

#include <algorithm>
#include <cmath>
#include <span>
#include <unordered_map>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::factory {

inline constexpr atx::u32 kFingerprintDefaultQuant = 32;
inline constexpr atx::u32 kFingerprintNaNBucket = 0xFFFFU;

struct FingerprintScratch {
  std::vector<atx::usize> order;
  std::vector<atx::u32> bucket;
};

namespace detail {

inline constexpr atx::u64 kFpOffset = 1469598103934665603ULL;
inline constexpr atx::u64 kFpPrime = 1099511628211ULL;

[[nodiscard]] inline atx::u64 fp_fold(atx::u64 h, atx::u32 v) noexcept {
  for (int i = 0; i < 4; ++i) {
    h ^= static_cast<atx::u64>(v & 0xFFU);
    h *= kFpPrime;
    v >>= 8U;
  }
  return h;
}

// Quantize one row into scratch.bucket (size == row.size()).
inline void fp_quantize_row(std::span<const atx::f64> row, atx::u32 n_quant,
                            FingerprintScratch &s) {
  const atx::usize n = row.size();
  s.bucket.assign(n, kFingerprintNaNBucket);
  s.order.clear();
  for (atx::usize j = 0; j < n; ++j) {
    if (!std::isnan(row[j])) {
      s.order.push_back(j);
    }
  }
  const atx::usize m = s.order.size();
  if (m == 0) {
    return;
  }
  std::stable_sort(s.order.begin(), s.order.end(),
                   [&](atx::usize a, atx::usize b) { return row[a] < row[b]; });
  const atx::f64 denom = (m > 1) ? static_cast<atx::f64>(m - 1) : 0.0;
  atx::usize lo = 0;
  while (lo < m) { // bounded: `hi` strictly advances past `lo` each pass
    atx::usize hi = lo + 1;
    while (hi < m && !(row[s.order[lo]] < row[s.order[hi]])) {
      ++hi; // extend the tie run (equal values, incl. +0/-0)
    }
    const atx::f64 avg = 0.5 * static_cast<atx::f64>(lo + hi - 1);
    const atx::f64 p = (m > 1) ? avg / denom : 0.5;
    const auto raw = static_cast<atx::u32>(p * static_cast<atx::f64>(n_quant));
    const atx::u32 q = std::min(raw, n_quant - 1U);
    for (atx::usize r = lo; r < hi; ++r) {
      s.bucket[s.order[r]] = q;
    }
    lo = hi;
  }
}

} // namespace detail

// Fingerprint a row-major probe of `n_inst` columns (rows = probe.size()/n_inst).
// Precondition: n_inst > 0, probe.size() % n_inst == 0, 1 <= n_quant < 0xFFFF.
[[nodiscard]] inline atx::u64 output_fingerprint(std::span<const atx::f64> probe, atx::usize n_inst,
                                                 FingerprintScratch &scratch,
                                                 atx::u32 n_quant = kFingerprintDefaultQuant) {
  atx::u64 h = detail::fp_fold(detail::kFpOffset, n_quant);
  if (n_inst == 0 || n_quant == 0) {
    return h;
  }
  const atx::usize rows = probe.size() / n_inst;
  h = detail::fp_fold(h, static_cast<atx::u32>(n_inst));
  for (atx::usize r = 0; r < rows; ++r) {
    detail::fp_quantize_row(probe.subspan(r * n_inst, n_inst), n_quant, scratch);
    for (const atx::u32 q : scratch.bucket) {
      h = detail::fp_fold(h, q);
    }
  }
  return h;
}

// Convenience: a single-row probe with a local scratch.
[[nodiscard]] inline atx::u64 output_fingerprint(std::span<const atx::f64> signal_probe,
                                                 atx::u32 n_quant = kFingerprintDefaultQuant) {
  FingerprintScratch scratch;
  return output_fingerprint(signal_probe, signal_probe.size(), scratch, n_quant);
}

// The fixed probe dates: `rows` evenly spaced interior dates of [0, dates)
// (d_i = floor((i+1) * dates / (rows+1))), clamped to `dates` when rows >= dates.
[[nodiscard]] inline std::vector<atx::usize> probe_rows(atx::usize dates, atx::usize rows) {
  std::vector<atx::usize> out;
  if (dates == 0 || rows == 0) {
    return out;
  }
  if (rows >= dates) {
    out.resize(dates);
    for (atx::usize d = 0; d < dates; ++d) {
      out[d] = d;
    }
    return out;
  }
  out.reserve(rows);
  for (atx::usize i = 0; i < rows; ++i) {
    out.push_back((i + 1U) * dates / (rows + 1U));
  }
  return out;
}

// Fingerprint a full date-major [dates x n_inst] signal on the given probe rows.
[[nodiscard]] inline atx::u64 signal_fingerprint(std::span<const atx::f64> values,
                                                 atx::usize n_inst,
                                                 std::span<const atx::usize> rows,
                                                 FingerprintScratch &scratch,
                                                 atx::u32 n_quant = kFingerprintDefaultQuant) {
  atx::u64 h = detail::fp_fold(detail::kFpOffset, n_quant);
  if (n_inst == 0 || n_quant == 0) {
    return h;
  }
  h = detail::fp_fold(h, static_cast<atx::u32>(n_inst));
  const atx::usize dates = values.size() / n_inst;
  for (const atx::usize d : rows) {
    if (d >= dates) {
      continue;
    }
    detail::fp_quantize_row(values.subspan(d * n_inst, n_inst), n_quant, scratch);
    for (const atx::u32 q : scratch.bucket) {
      h = detail::fp_fold(h, q);
    }
  }
  return h;
}

// fingerprint -> canon_hash of the FIRST genome that produced it.
struct FingerprintIndex {
  std::unordered_map<atx::u64, atx::u64> first_canon;

  [[nodiscard]] const atx::u64 *find(atx::u64 fp) const noexcept {
    const auto it = first_canon.find(fp);
    return it == first_canon.end() ? nullptr : &it->second;
  }
  // Record fp -> canon (keeps the first owner); true iff fp was new.
  bool insert(atx::u64 fp, atx::u64 canon) { return first_canon.emplace(fp, canon).second; }
  [[nodiscard]] atx::usize size() const noexcept { return first_canon.size(); }
};

} // namespace atx::engine::factory

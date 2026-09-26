#pragma once

// Versioned correlation candidate index. V1 preserves the original one-sided
// 64-bit banding recipe. V2 compares both signs of 256 random-projection bits;
// exact pairwise correlation remains the admission score. The small signature
// scan is O(pool size), not a sublinear guarantee. Missing/nonfinite observations
// bypass screening: imputed-vector similarity cannot bound pairwise correlation.
// The 3.5-sigma Hamming margin targets recall at the requested absolute floor;
// it is probabilistic, and the 10k/T5000 acceptance benchmark remains required.

#include <algorithm>     // std::sort, std::unique (neighbor dedup)
#include <array>
#include <bit>
#include <numbers>
#include <cmath>         // std::isnan, std::sqrt
#include <cstddef>       // std::size_t
#include <span>          // std::span
#include <unordered_map> // bucket map
#include <vector>        // hyperplanes, bucket members, scratch

#include "atx/core/macro.hpp"  // ATX_ASSERT
#include "atx/core/random.hpp" // Xoshiro256pp::normal (seeded hyperplanes)
#include "atx/core/simd.hpp"   // simd::dot (FMA projection)
#include "atx/core/types.hpp"  // f64, u32, u64, usize

#include "atx/engine/combine/correlation.hpp" // pairwise_complete_corr (exact)
#include "atx/engine/combine/store.hpp"        // combine::AlphaId
#include "atx/engine/library/store.hpp"        // LibraryStore (corr-to-pool source)

namespace atx::engine::library {

// ===========================================================================
//  CorrNeighborIndex — SimHash LSH over admitted-alpha demeaned PnL vectors.
// ===========================================================================
enum class CorrIndexRule : atx::u8 { LegacyBandsV1 = 1, SignedHammingV2 = 2 };

class CorrNeighborIndex {
public:
  static constexpr atx::u32 kBandBits = 8U;
  static constexpr atx::u32 kSignedBits = 256U;

  // K retains its V1 meaning. V2 always uses 256 bits for useful selectivity at
  // |rho|=.7; signature() still exposes the first min(K,64) seeded bits.
  CorrNeighborIndex(atx::u64 master_seed, atx::usize T, atx::u32 K,
                    CorrIndexRule rule = CorrIndexRule::SignedHammingV2)
      : k_{K}, t_{T}, bands_{K / kBandBits}, rule_{rule} {
    ATX_CHECK(T > 0U && K > 0U && K <= 64U && (K % kBandBits) == 0U);
    ATX_CHECK(rule == CorrIndexRule::LegacyBandsV1 || rule == CorrIndexRule::SignedHammingV2);
    const atx::u32 planes = rule == CorrIndexRule::LegacyBandsV1 ? K : kSignedBits;
    scratch_.resize(T);
    h_.resize(planes);
    atx::core::Xoshiro256pp rng(master_seed);
    for (atx::u32 k = 0; k < planes; ++k) {
      std::vector<atx::f64> v(T);
      atx::f64 norm_sq = 0.0;
      for (atx::usize i = 0; i < T; ++i) {
        const atx::f64 g = rng.normal(); // N(0,1) component
        v[i] = g;
        norm_sq += g * g;
      }
      // Normalize to a unit vector. A zero-length vector (norm_sq == 0) is
      // astronomically improbable from a Gaussian draw over T>=1 dims; guard it
      // so the divide is never by 0 (leave it as the zero vector -> sign() == 0).
      if (norm_sq > 0.0) {
        const atx::f64 inv = 1.0 / std::sqrt(norm_sq);
        for (atx::usize i = 0; i < T; ++i) {
          v[i] *= inv;
        }
      }
      h_[k] = std::move(v);
    }
  }

  /// The K-bit SimHash signature of `pnl` (length must be T). Demeans IGNORING
  /// NaN (subtract the mean of the non-NaN cells), maps NaN -> 0, then sets bit k
  /// to the SIGN of the projection onto hyperplane k (Charikar: 1 if dot >= 0).
  /// The signature is approximate by design (NaN cells contribute 0 to the
  /// projection); the corr over the recalled candidates is exact via
  /// pairwise_complete_corr. Uses a reused scratch buffer (no per-call heap
  /// alloc); scratch_ is `mutable` so signature() is logically const (a pure
  /// function of pnl + the seeded hyperplanes) yet allocation-free. NOT thread-
  /// safe (mutates scratch_); the gate path is single-threaded per owning thread.
  [[nodiscard]] atx::u64 signature(std::span<const atx::f64> pnl) const noexcept {
    ATX_CHECK(pnl.size() == t_);
    demean_into_scratch(pnl);
    const std::span<const atx::f64> d{scratch_.data(), scratch_.size()};
    atx::u64 bits = 0U;
    for (atx::u32 k = 0; k < k_; ++k) {
      const std::span<const atx::f64> hk{h_[k].data(), h_[k].size()};
      // corr = cos(angle); the SIGN of the projection onto a random hyperplane is
      // the SimHash bit (Charikar). >= 0 -> 1 (the tie at 0 is deterministic).
      const atx::f64 proj = atx::core::simd::dot(d, hk);
      if (proj >= 0.0) {
        bits |= (atx::u64{1} << k);
      }
    }
    return bits;
  }

  /// Register alpha `id` (its PnL stream `pnl`) into every band bucket. Buckets
  /// are appended in AlphaId order across calls when add() is called in id order
  /// (the documented usage: add_all iterates 0..n_alphas), keeping member lists
  /// AlphaId-ordered for deterministic neighbor enumeration. COLD path (allocates
  /// bucket vectors). SAFETY: only the signature is retained — the span may dangle
  /// after a later store growth without affecting stored state.
  void add(combine::AlphaId id, std::span<const atx::f64> pnl) {
    if (rule_ == CorrIndexRule::SignedHammingV2) {
      entries_.push_back(Entry{id, signature_words(pnl), has_missing(pnl)});
      return;
    }
    const atx::u64 sig = signature(pnl);
    for (atx::u32 band = 0; band < bands_; ++band) {
      buckets_[band_key(sig, band)].push_back(id);
    }
  }

  /// The approximate near-neighbors of `pnl`: the union of its L band buckets,
  /// de-duplicated and returned in ascending AlphaId order (determinism). An
  /// empty union (no admitted alpha collides in any band) returns an empty
  /// vector. COLD path (allocates the union); this is the gate path, not the VM
  /// hot path, so a per-call allocation is acceptable (documented).
  [[nodiscard]] std::vector<combine::AlphaId>
  neighbors(std::span<const atx::f64> pnl, atx::f64 absolute_floor = 0.7) const {
    ATX_CHECK(pnl.size() == t_);
    if (rule_ == CorrIndexRule::SignedHammingV2) {
      const auto bits = signature_words(pnl);
      const bool all = has_missing(pnl) || !std::isfinite(absolute_floor) || absolute_floor <= 0.0;
      // For a unit Gaussian projection, P(sign differs)=acos(|rho|)/pi.
      const atx::f64 p = std::acos(all ? 0.0 : std::clamp(absolute_floor, 0.0, 1.0)) /
                         std::numbers::pi_v<atx::f64>;
      const auto limit = static_cast<atx::u32>(std::min(128.0, std::ceil(
          kSignedBits * p + 3.5 * std::sqrt(kSignedBits * p * (1.0 - p)))));
      std::vector<combine::AlphaId> out;
      for (const auto& entry : entries_) {
        atx::u32 different = 0U;
        for (atx::usize word = 0; word < bits.size(); ++word)
          different += static_cast<atx::u32>(std::popcount(bits[word] ^ entry.bits[word]));
        if (all || entry.missing || std::min(different, kSignedBits - different) <= limit)
          out.push_back(entry.id);
      }
      sort_unique(out);
      return out;
    }
    const atx::u64 sig = signature(pnl);
    std::vector<combine::AlphaId> out;
    for (atx::u32 band = 0; band < bands_; ++band) {
      const auto it = buckets_.find(band_key(sig, band));
      if (it != buckets_.end()) {
        out.insert(out.end(), it->second.begin(), it->second.end());
      }
    }
    sort_unique(out);
    return out;
  }

  [[nodiscard]] atx::u32 k() const noexcept { return k_; }
  [[nodiscard]] atx::usize t() const noexcept { return t_; }

private:
  struct Entry {
    combine::AlphaId id;
    std::array<atx::u64, 4> bits;
    bool missing;
  };

  static void sort_unique(std::vector<combine::AlphaId>& ids) {
    std::sort(ids.begin(), ids.end(), [](auto a, auto b) { return a.value < b.value; });
    ids.erase(std::unique(ids.begin(), ids.end(),
                          [](auto a, auto b) { return a.value == b.value; }), ids.end());
  }
  [[nodiscard]] static bool has_missing(std::span<const atx::f64> pnl) noexcept {
    return std::any_of(pnl.begin(), pnl.end(), [](auto x) { return !std::isfinite(x); });
  }
  [[nodiscard]] std::array<atx::u64, 4>
  signature_words(std::span<const atx::f64> pnl) const noexcept {
    ATX_CHECK(pnl.size() == t_);
    // Scale before centering to avoid overflow for otherwise finite vectors.
    atx::f64 scale = 0.0;
    for (const auto x : pnl) if (std::isfinite(x)) scale = std::max(scale, std::abs(x));
    atx::f64 mean = 0.0;
    atx::usize count = 0;
    for (const auto x : pnl) if (std::isfinite(x)) {
      mean += scale > 0.0 ? x / scale : 0.0;
      ++count;
    }
    if (count != 0U) mean /= static_cast<atx::f64>(count);
    for (atx::usize i = 0; i < pnl.size(); ++i)
      scratch_[i] = std::isfinite(pnl[i]) ? (scale > 0.0 ? pnl[i] / scale : 0.0) - mean : 0.0;
    std::array<atx::u64, 4> bits{};
    for (atx::u32 k = 0; k < kSignedBits; ++k)
      if (atx::core::simd::dot(std::span<const atx::f64>{scratch_},
                              std::span<const atx::f64>{h_[k]}) >= 0.0)
        bits[k / 64U] |= atx::u64{1} << (k % 64U);
    return bits;
  }

  /// Compose a per-band bucket key from the b-bit group at band `band` of `sig`.
  /// The band index is folded into the high bits so the SAME b-bit pattern in two
  /// different bands maps to DIFFERENT keys (bands are independent hash tables).
  [[nodiscard]] static constexpr atx::u64 band_key(atx::u64 sig, atx::u32 band) noexcept {
    const atx::u64 mask = (atx::u64{1} << kBandBits) - 1U;
    const atx::u64 group = (sig >> (band * kBandBits)) & mask;
    return (static_cast<atx::u64>(band) << kBandBits) | group;
  }

  /// Demean `pnl` ignoring NaN into scratch_: subtract the mean of the non-NaN
  /// cells, then map any NaN cell to 0 (so it contributes nothing to a
  /// projection). The signature is intentionally approximate under NaN; the
  /// candidate corr is exact via pairwise_complete_corr.
  void demean_into_scratch(std::span<const atx::f64> pnl) const noexcept {
    atx::f64 sum = 0.0;
    atx::usize n = 0U;
    for (const atx::f64 v : pnl) {
      if (!std::isnan(v)) {
        sum += v;
        ++n;
      }
    }
    const atx::f64 mean = (n > 0U) ? sum / static_cast<atx::f64>(n) : 0.0;
    for (atx::usize i = 0; i < pnl.size(); ++i) {
      const atx::f64 v = pnl[i];
      scratch_[i] = std::isnan(v) ? 0.0 : (v - mean);
    }
  }

  atx::u32 k_;                                // # hyperplanes / signature bits (<= 64)
  atx::usize t_;                              // PnL vector length
  atx::u32 bands_;                            // L = K / kBandBits (OR-amplification bands)
  CorrIndexRule rule_;
  std::vector<Entry> entries_;
  std::vector<std::vector<atx::f64>> h_;      // K random UNIT vectors over R^T (seeded)
  std::unordered_map<atx::u64, std::vector<combine::AlphaId>> buckets_; // band key -> ids
  mutable std::vector<atx::f64> scratch_;     // reused demean buffer (mutable: signature() is const)
};

// ===========================================================================
//  online_corr_to_pool — incremental MAX |corr| of a candidate vs. the pool.
//
//  Returns the maximum |pairwise_complete_corr| of `candidate_pnl` against the
//  admitted pool, computed EXACTLY but only over the candidate's SimHash
//  near-neighbors (the o(N) screen). An empty neighbor set returns 0.0 — the
//  same value an exhaustive scan returns for an empty pool, and the
//  pairwise_complete_corr degenerate convention for "no co-movement info". The
//  approximation is one-sided: this value is always <= the exhaustive max (the
//  recall test bounds how often they differ).
//
//  SAFETY: store.pnl(id) aliases the store's mapping/memtable; the query performs
//  NO store growth, so each span stays valid for its pairwise_complete_corr call.
//  `candidate_pnl` is the caller's own buffer (copy in before calling if it
//  aliases the store and the store may grow).
// ===========================================================================
[[nodiscard]] inline atx::f64 online_corr_to_pool(std::span<const atx::f64> candidate_pnl,
                                                  const LibraryStore &store,
                                                  CorrNeighborIndex &index,
                                                  atx::f64 absolute_floor = 0.7) {
  atx::f64 worst = 0.0;
  for (const combine::AlphaId id : index.neighbors(candidate_pnl, absolute_floor)) {
    // EXACT correlation over the recalled candidate (the accelerator only chose
    // WHICH ids to score; the score itself is the reference value).
    const atx::f64 c = combine::pairwise_complete_corr(candidate_pnl, store.pnl(id));
    const atx::f64 a = (c < 0.0) ? -c : c;
    worst = (a > worst) ? a : worst;
  }
  return worst;
}

} // namespace atx::engine::library

#pragma once

// atx::engine::library — CorrNeighborIndex: SimHash LSH corr-neighbor index +
// incremental corr-to-pool gate (S4-3).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The o(N) replacement for the O(N^2) corr-to-pool re-gate. The orthogonality
//  gate (P4-3) screens a candidate alpha by its MAX |correlation| against every
//  alpha already admitted to the pool; doing that with an exhaustive
//  combine::pairwise_complete_corr scan is O(N) per candidate and O(N^2) to
//  re-gate a whole factory generation. This index buckets each admitted alpha's
//  demeaned PnL vector with SimHash (Charikar 2002: signed random projections —
//  the sign of x·h_k for K random hyperplanes h_k is a K-bit locality-sensitive
//  signature), then returns ONLY a candidate's approximate near-neighbors. The
//  gate then computes the EXACT pairwise_complete_corr over just those few
//  candidates. The accelerator is APPROXIMATE only in WHICH ids it returns; the
//  correlation reported for each returned id is the exact reference value.
//
//  Why SimHash for correlation: a demeaned PnL vector's Pearson correlation with
//  another demeaned vector is exactly the COSINE of the angle between them
//  (corr = cos theta). SimHash's per-bit agreement probability is 1 - theta/pi,
//  monotone in cos theta — so cosine-near (== correlation-near) vectors share
//  more signature bits and collide in a shared bucket with high probability.
//
// ===========================================================================
//  Banding (the recall knob) — classic LSH OR-of-AND amplification
// ===========================================================================
//  A single K-bit signature compared whole is too strict (one flipped bit misses
//  a neighbor). Instead the K bits are partitioned into L contiguous BANDS of b
//  bits each (K = L*b); a band's bucket KEY is (band_index, the b-bit group). An
//  alpha is inserted into ONE bucket PER BAND (L buckets). Two vectors are
//  neighbors if they collide in >= 1 band (OR over bands) — each band is an AND
//  of its b bits. With per-bit agreement p, P(collide in a band) = p^b and
//  P(collide in >= 1 of L bands) = 1 - (1 - p^b)^L, which rises sharply with L
//  (recall) and falls with b (selectivity / neighbor-set size). This is the
//  standard (b, L) LSH tuning.
//
//  CHOSEN PARAMS (measured on the S4-3 test fixtures, T=128..256, K=64):
//      K = 64 hyperplanes, b = 8 band bits, L = 8 bands, probes = 1 (exact band
//      key only, no bit-flip multi-probe — unnecessary here).
//  For a genuine near-duplicate (corr ~0.98 -> theta ~0.2 -> p ~0.936):
//      P(collide >=1 band) = 1 - (1 - 0.936^8)^8 ~ 0.9996  (recall ~1.0).
//  For an unrelated pair (corr ~0 -> theta ~pi/2 -> p = 0.5):
//      E[band collisions] = L * p^b = 8 / 256 ~ 0.031 -> neighbor set ~3% of N,
//      well under the 0.2*N bound.
//  MEASURED (library_corr_index_test.cpp): recall = 64/64 = 1.00 on perturbed
//      near-duplicate queries (N=512, T=256); median neighbors = 62 of N=2000
//      => ratio 0.031 (T=128). Both match the predictions above. (For K != 64
//      the band count adapts: L = K / kBandBits, e.g. K=32 -> L=4.)
//
// ===========================================================================
//  Determinism (L7)
// ===========================================================================
//  The K hyperplanes are drawn from a Xoshiro256pp seeded SOLELY by the caller's
//  master_seed (never time / thread id), so two indices built with the same seed
//  produce identical signatures (SameSeedSameSignatures). Bucket member lists are
//  kept in AlphaId (insertion) order; neighbors() returns a sorted-unique id set.
//
//  SAFETY: add()/neighbors()/online_corr_to_pool() read PnL spans that ALIAS the
//  LibraryStore's segment mappings or live memtable (store.pnl()). Those dangle
//  on the next stage()/flush() (the AlphaStore growth/reset rule). The query path
//  here does NO store growth, so the spans it reads stay valid for the call; the
//  candidate pnl is the caller's own buffer. add() copies nothing from the span
//  beyond the signature, so a post-add store growth does not affect stored state.

#include <algorithm>     // std::sort, std::unique (neighbor dedup)
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
class CorrNeighborIndex {
public:
  // Signature bits per band (b). The number of bands L = K / kBandBits is derived
  // from K so the index supports any K that is a positive multiple of kBandBits
  // and <= 64. The chosen production setting is K = 64 => L = 8 bands of b = 8
  // bits (see the header "Banding" note for the (b, L) recall tuning).
  static constexpr atx::u32 kBandBits = 8U;

  /// Build the index for length-`T` PnL vectors with `K` SimHash hyperplanes.
  /// The hyperplanes are K random UNIT vectors over R^T drawn from a Xoshiro256pp
  /// seeded only by `master_seed` (determinism, L7). PRECONDITION: K is a positive
  /// multiple of kBandBits and K <= 64 (the signature packs into one u64) —
  /// asserted. The band count is L = K / kBandBits.
  CorrNeighborIndex(atx::u64 master_seed, atx::usize T, atx::u32 K)
      : k_{K}, t_{T}, bands_{K / kBandBits} {
    ATX_ASSERT(K > 0U && (K % kBandBits) == 0U);
    ATX_ASSERT(K <= 64U);
    scratch_.resize(T);
    h_.resize(K);
    atx::core::Xoshiro256pp rng(master_seed);
    for (atx::u32 k = 0; k < K; ++k) {
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
    ATX_ASSERT(pnl.size() == t_);
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
  [[nodiscard]] std::vector<combine::AlphaId> neighbors(std::span<const atx::f64> pnl) const {
    const atx::u64 sig = signature(pnl);
    std::vector<combine::AlphaId> out;
    for (atx::u32 band = 0; band < bands_; ++band) {
      const auto it = buckets_.find(band_key(sig, band));
      if (it != buckets_.end()) {
        out.insert(out.end(), it->second.begin(), it->second.end());
      }
    }
    // Sort + unique by AlphaId value (an id may appear in several bands).
    std::sort(out.begin(), out.end(),
              [](combine::AlphaId a, combine::AlphaId b) { return a.value < b.value; });
    out.erase(std::unique(out.begin(), out.end(),
                          [](combine::AlphaId a, combine::AlphaId b) { return a.value == b.value; }),
              out.end());
    return out;
  }

  [[nodiscard]] atx::u32 k() const noexcept { return k_; }
  [[nodiscard]] atx::usize t() const noexcept { return t_; }

private:
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
                                                  CorrNeighborIndex &index) {
  atx::f64 worst = 0.0;
  for (const combine::AlphaId id : index.neighbors(candidate_pnl)) {
    // EXACT correlation over the recalled candidate (the accelerator only chose
    // WHICH ids to score; the score itself is the reference value).
    const atx::f64 c = combine::pairwise_complete_corr(candidate_pnl, store.pnl(id));
    const atx::f64 a = (c < 0.0) ? -c : c;
    worst = (a > worst) ? a : worst;
  }
  return worst;
}

} // namespace atx::engine::library

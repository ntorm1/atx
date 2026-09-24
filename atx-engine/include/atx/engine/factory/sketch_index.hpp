#pragma once

// atx::engine::factory — SketchIndex: sub-linear-cost PnL-correlation search (L3).
//
// Redundancy checks ("is this candidate's PnL >= rho correlated with anything
// already found?") are O(N * L) per query against N stored series of length L.
// For a search that keeps 10^4..10^5 alphas that dominates the novelty step.
//
// SketchIndex stores, per series, its z-normalized PnL (mean 0, unit L2 norm; a
// non-finite day -- NaN or +-inf -- counts as 0 after centering) and a 128-dim random-projection sketch
// s = R z, with R a fixed seeded +-1/sqrt(D) (Achlioptas) matrix. For unit
// vectors <s_a, s_b> is an unbiased estimate of Pearson corr(a, b) with std
// ~ sqrt((1 + rho^2) / D). A query:
//   1. flat scan of the N x 128 f32 sketch matrix (contiguous, fixed inner
//      length -> the compiler emits packed SIMD dot products);
//   2. keeps the `shortlist` best |estimate| (nth_element, no heap churn);
//   3. full-length recheck of the shortlist against the stored f32 z-vectors
//      (f64 accumulation), returning the top-k by |corr| (ties -> lower id).
//      This is Pearson correlation up to f32 storage rounding (~1e-6), not
//      bit-exact f64 Pearson.
// Recall@10 vs brute force >= 0.95 on 10k clustered series (FactorySketchIndex).
//
// FarthestPointArchive keeps a bounded set of mutually-distant PnL profiles: a
// new profile replaces the archive's most-redundant member (smallest nearest-
// neighbour distance) only when it is itself farther from the archive than that
// member is — so the archive's minimum pairwise distance never decreases.
//
// Determinism: no RNG beyond the fixed-seed projection; every sort has an id
// tie-break. Header-only; allocation happens in add()/topk() (cold relative to
// the VM). add() mutates; const queries use local buffers only, so concurrent
// topk() calls on an index that is not being added to are safe.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::factory {

inline constexpr atx::usize kSketchDim = 128;

struct Neighbor {
  atx::u64 id{0};
  atx::f64 corr{0.0}; // Pearson correlation of the stored f32 z-vectors (f64 sum; signed)
};

namespace detail {

// z-normalize to mean 0 / unit L2 norm (non-finite -> 0 after centering, so a
// +-inf day cannot poison the mean into NaN scores that would break the strict
// weak ordering of the nth_element / partial_sort comparators). A constant
// or empty series maps to the zero vector (correlation 0 with everything).
inline void sketch_znorm(std::span<const atx::f64> x, std::vector<atx::f32> &out) {
  out.assign(x.size(), 0.0F);
  atx::f64 sum = 0.0;
  atx::usize n = 0;
  for (const atx::f64 v : x) {
    if (std::isfinite(v)) {
      sum += v;
      ++n;
    }
  }
  if (n < 2) {
    return;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (const atx::f64 v : x) {
    if (std::isfinite(v)) {
      ss += (v - mean) * (v - mean);
    }
  }
  if (!std::isfinite(mean) || !std::isfinite(ss) || !(ss > 0.0)) {
    return; // degenerate or overflowing series -> zero vector (corr 0)
  }
  const atx::f64 inv = 1.0 / std::sqrt(ss);
  for (atx::usize i = 0; i < x.size(); ++i) {
    out[i] = !std::isfinite(x[i]) ? 0.0F : static_cast<atx::f32>((x[i] - mean) * inv);
  }
}

[[nodiscard]] inline atx::f64 dot_f32(const atx::f32 *a, const atx::f32 *b,
                                      atx::usize n) noexcept {
  // Four independent accumulators break the add dependency chain.
  atx::f32 s0 = 0.0F, s1 = 0.0F, s2 = 0.0F, s3 = 0.0F;
  atx::usize i = 0;
  for (; i + 4 <= n; i += 4) {
    s0 += a[i] * b[i];
    s1 += a[i + 1] * b[i + 1];
    s2 += a[i + 2] * b[i + 2];
    s3 += a[i + 3] * b[i + 3];
  }
  for (; i < n; ++i) {
    s0 += a[i] * b[i];
  }
  return static_cast<atx::f64>((s0 + s1) + (s2 + s3));
}

// f32 inputs, f64 accumulation: the full-length recheck dot (length up to ~10^4)
// where f32 accumulation error would be visible in the reported correlation.
[[nodiscard]] inline atx::f64 dot_f32_acc64(const atx::f32 *a, const atx::f32 *b,
                                            atx::usize n) noexcept {
  atx::f64 s0 = 0.0, s1 = 0.0;
  atx::usize i = 0;
  for (; i + 2 <= n; i += 2) {
    s0 += static_cast<atx::f64>(a[i]) * static_cast<atx::f64>(b[i]);
    s1 += static_cast<atx::f64>(a[i + 1]) * static_cast<atx::f64>(b[i + 1]);
  }
  for (; i < n; ++i) {
    s0 += static_cast<atx::f64>(a[i]) * static_cast<atx::f64>(b[i]);
  }
  return s0 + s1;
}

[[nodiscard]] inline atx::u64 sketch_splitmix(atx::u64 &s) noexcept {
  s += 0x9E3779B97F4A7C15ULL;
  atx::u64 z = s;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

} // namespace detail

class SketchIndex {
public:
  // `length` is the fixed PnL length every added/queried series must have.
  // `shortlist` is the number of sketch candidates rechecked exactly per query.
  explicit SketchIndex(atx::usize length, atx::usize shortlist = 256,
                       atx::u64 seed = 0x5EEC4ULL)
      : length_{length}, shortlist_{shortlist}, proj_(kSketchDim * length) {
    const atx::f32 scale = 1.0F / std::sqrt(static_cast<atx::f32>(kSketchDim));
    atx::u64 s = seed;
    // Row-major D x L; 64 signs per splitmix draw.
    atx::u64 bits = 0;
    for (atx::usize i = 0; i < proj_.size(); ++i) {
      if (i % 64 == 0) {
        bits = detail::sketch_splitmix(s);
      }
      proj_[i] = ((bits >> (i % 64)) & 1ULL) != 0 ? scale : -scale;
    }
  }

  [[nodiscard]] atx::usize size() const noexcept { return ids_.size(); }
  [[nodiscard]] atx::usize length() const noexcept { return length_; }

  // Add a series. Precondition: pnl.size() == length().
  void add(atx::u64 id, std::span<const atx::f64> pnl) {
    detail::sketch_znorm(pnl.first(std::min(pnl.size(), length_)), zbuf_);
    zbuf_.resize(length_, 0.0F);
    ids_.push_back(id);
    z_.insert(z_.end(), zbuf_.begin(), zbuf_.end());
    const atx::usize base = sketch_.size();
    sketch_.resize(base + kSketchDim);
    project(zbuf_.data(), sketch_.data() + base);
  }

  // Top-k by full-length |corr| among the sketch shortlist. Result sorted by
  // descending |corr|, ties -> ascending insertion order.
  [[nodiscard]] std::vector<Neighbor> topk(std::span<const atx::f64> pnl, atx::usize k) const {
    std::vector<atx::f32> zq;
    detail::sketch_znorm(pnl.first(std::min(pnl.size(), length_)), zq);
    zq.resize(length_, 0.0F);
    std::vector<atx::f32> sq(kSketchDim);
    project(zq.data(), sq.data());

    const atx::usize n = ids_.size();
    std::vector<Cand> cands(n);
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 est = detail::dot_f32(sq.data(), sketch_.data() + i * kSketchDim, kSketchDim);
      cands[i] = Cand{std::abs(est), i};
    }
    const atx::usize m = std::min(n, std::max(shortlist_, k));
    if (m < n) {
      std::nth_element(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(m), cands.end(),
                       better);
      cands.resize(m);
    }
    for (Cand &c : cands) { // exact recheck
      c.score = detail::dot_f32_acc64(zq.data(), z_.data() + c.idx * length_, length_);
    }
    return finish(cands, k);
  }

  // Exact brute-force top-k (reference for recall tests / small indexes).
  [[nodiscard]] std::vector<Neighbor> topk_exact(std::span<const atx::f64> pnl,
                                                 atx::usize k) const {
    std::vector<atx::f32> zq;
    detail::sketch_znorm(pnl.first(std::min(pnl.size(), length_)), zq);
    zq.resize(length_, 0.0F);
    std::vector<Cand> cands(ids_.size());
    for (atx::usize i = 0; i < ids_.size(); ++i) {
      cands[i] = Cand{detail::dot_f32_acc64(zq.data(), z_.data() + i * length_, length_), i};
    }
    return finish(cands, k);
  }

private:
  struct Cand {
    atx::f64 score{0.0}; // |estimate| during the scan, signed full-length corr after
    atx::usize idx{0};
  };

  [[nodiscard]] static bool better(const Cand &a, const Cand &b) noexcept {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.idx < b.idx;
  }

  [[nodiscard]] std::vector<Neighbor> finish(std::vector<Cand> &cands, atx::usize k) const {
    auto abs_better = [](const Cand &a, const Cand &b) noexcept {
      const atx::f64 aa = std::abs(a.score);
      const atx::f64 bb = std::abs(b.score);
      if (aa != bb) {
        return aa > bb;
      }
      return a.idx < b.idx;
    };
    const atx::usize take = std::min(k, cands.size());
    std::partial_sort(cands.begin(), cands.begin() + static_cast<std::ptrdiff_t>(take),
                      cands.end(), abs_better);
    std::vector<Neighbor> out;
    out.reserve(take);
    for (atx::usize i = 0; i < take; ++i) {
      out.push_back(Neighbor{ids_[cands[i].idx], cands[i].score});
    }
    return out;
  }

  void project(const atx::f32 *z, atx::f32 *s) const noexcept {
    for (atx::usize d = 0; d < kSketchDim; ++d) {
      s[d] = static_cast<atx::f32>(detail::dot_f32(proj_.data() + d * length_, z, length_));
    }
  }

  atx::usize length_;
  atx::usize shortlist_;
  std::vector<atx::f32> proj_;   // D x L projection (row-major)
  std::vector<atx::u64> ids_;    // insertion order
  std::vector<atx::f32> z_;      // N x L z-normalized series
  std::vector<atx::f32> sketch_; // N x D sketches
  std::vector<atx::f32> zbuf_;   // add() scratch
};

// =========================================================================
//  FarthestPointArchive — bounded max-min-distance archive of PnL profiles.
// =========================================================================
class FarthestPointArchive {
public:
  FarthestPointArchive(atx::usize capacity, atx::usize length)
      : capacity_{capacity}, length_{length} {}

  [[nodiscard]] atx::usize size() const noexcept { return ids_.size(); }
  [[nodiscard]] const std::vector<atx::u64> &ids() const noexcept { return ids_; }

  // Offer a profile. Returns true iff it was admitted (appended, or replaced the
  // most-redundant member). Distance = 1 - |corr|.
  bool offer(atx::u64 id, std::span<const atx::f64> pnl) {
    if (capacity_ == 0) {
      return false;
    }
    std::vector<atx::f32> z;
    detail::sketch_znorm(pnl.first(std::min(pnl.size(), length_)), z);
    z.resize(length_, 0.0F);
    if (ids_.size() < capacity_) {
      append(id, z);
      return true;
    }
    // Most-redundant member: smallest NN distance, ties -> lowest slot.
    atx::usize worst = 0;
    for (atx::usize i = 1; i < nn_.size(); ++i) {
      if (nn_[i] < nn_[worst]) {
        worst = i;
      }
    }
    // Replacing `worst` must not create a closer pair than the one it removes:
    // compare against the new profile's distance to the archive minus `worst`.
    atx::f64 d_new_wo = 2.0;
    for (atx::usize i = 0; i < ids_.size(); ++i) {
      if (i != worst) {
        d_new_wo = std::min(d_new_wo, dist(z.data(), row(i)));
      }
    }
    if (!(d_new_wo > nn_[worst])) {
      return false;
    }
    ids_[worst] = id;
    std::copy(z.begin(), z.end(), z_.begin() + static_cast<std::ptrdiff_t>(worst * length_));
    recompute_nn();
    return true;
  }

  // Minimum pairwise distance (2.0 when fewer than two members).
  [[nodiscard]] atx::f64 min_pairwise() const noexcept {
    atx::f64 m = 2.0;
    for (const atx::f64 v : nn_) {
      m = std::min(m, v);
    }
    return m;
  }

private:
  [[nodiscard]] const atx::f32 *row(atx::usize i) const noexcept {
    return z_.data() + i * length_;
  }
  [[nodiscard]] atx::f64 dist(const atx::f32 *a, const atx::f32 *b) const noexcept {
    return 1.0 - std::min(1.0, std::abs(detail::dot_f32_acc64(a, b, length_)));
  }
  void append(atx::u64 id, const std::vector<atx::f32> &z) {
    ids_.push_back(id);
    z_.insert(z_.end(), z.begin(), z.end());
    recompute_nn();
  }
  void recompute_nn() {
    const atx::usize n = ids_.size();
    nn_.assign(n, 2.0);
    for (atx::usize i = 0; i < n; ++i) {
      for (atx::usize j = i + 1; j < n; ++j) {
        const atx::f64 d = dist(row(i), row(j));
        nn_[i] = std::min(nn_[i], d);
        nn_[j] = std::min(nn_[j], d);
      }
    }
  }

  atx::usize capacity_;
  atx::usize length_;
  std::vector<atx::u64> ids_;
  std::vector<atx::f32> z_;
  std::vector<atx::f64> nn_; // nearest-neighbour distance per member
};

} // namespace atx::engine::factory

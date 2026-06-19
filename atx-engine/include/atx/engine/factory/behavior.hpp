#pragma once

// atx::engine::factory — behavioral / phenotypic diversity (S4.2, plan §4.2).
//
// ===========================================================================
//  WHAT THIS IS — phenotype, not genotype
// ===========================================================================
//  S4.1 added an NSGA-II objective vector {wq, diversify, robust}. The
//  search_driver ALSO carries a canonical-hash structural novelty penalty (don't
//  pile the live SEARCH onto one source motif) and the fitness `diversify` term
//  (don't duplicate the admitted POOL). S4.2 adds a THIRD, orthogonal diversity
//  pressure: don't let the live search collapse onto one BEHAVIOR — one realized
//  PnL profile — even when the source structures differ and the pool is far away.
//
//  Two genomes can hash DISTANTLY (different ASTs) yet realize near-IDENTICAL
//  signals (e.g. rank(close) vs rank(close*1.0001)); the structural metric calls
//  them novel, the market calls them the same bet. Conversely two genomes can
//  hash CLOSE yet behave distinctly. Behavioral diversity is therefore NOT a
//  function of genotypic diversity — it needs its own descriptor + distance.
//
// ===========================================================================
//  THE DESCRIPTOR — zero extra eval
// ===========================================================================
//  The candidate's behavioral descriptor IS its realized OOS PnL profile, already
//  materialized by the fitness pass as detail::FitnessCore.oos_pnl (fitness.hpp).
//  No re-evaluation: the descriptor is a pure function of (genome, panel), so it
//  is canon-cacheable alongside the objective vector (the SearchDriver caches it
//  in CachedScore). The NOVELTY computed FROM it, however, is population-relative
//  (mean distance to the k nearest live/archived peers) and is therefore NOT
//  cacheable — it is recomputed fresh every generation.
//
// ===========================================================================
//  THE DISTANCE — 1 - |corr| over the held-out profile
// ===========================================================================
//  behavioral_distance(a, b) = 1 - |corr(a, b)| over the pairwise-complete (both-
//  non-NaN) overlap, reusing combine::pairwise_complete_corr (the ONE shared
//  Pearson NaN convention — not reinvented). Why |corr| and not corr: a perfectly
//  ANTI-correlated profile (corr = -1) is the SAME bet with the sign flipped —
//  behaviorally redundant — so it maps to distance 0, exactly like a perfectly
//  correlated one. An orthogonal profile (corr ~ 0) maps to distance ~ 1.
//
//  FALLBACK FORK (BehaviorMetric): PnlCorr (the shipped default) is plain Pearson
//  on the PnL profile. RankIc ranks each leg over the complete-case overlap first
//  (Spearman = Pearson on ranks), collapsing monotone-equivalent signals more
//  aggressively — selectable as a ONE-LINE switch if the PnL-corr distance proves
//  too collinear with the marginal-corr `diversify` objective. The switch is
//  exhaustive (no default); adding a metric is a compile error until handled.
//
// ===========================================================================
//  DETERMINISM (load-bearing — F1/F2)
// ===========================================================================
//  Every operation here is a pure function of its inputs: no RNG, no global
//  state, no float-order ambiguity. The archive inserts in the order it is GIVEN
//  (the SearchDriver hands it the current front in CANONICAL-ID order) and evicts
//  oldest-first; novelty's k-nearest selection sorts distances with a stable
//  index tie-break, so equal distances resolve to the lower neighbour index
//  deterministically. Same inputs => byte-identical output across {1,2,4,8}
//  DetPool workers.
//
//  Header-only, alloc-light. behavioral_distance allocates O(valid-pairs) scratch
//  only on the RankIc path (the cold per-generation selection loop, never the VM
//  hot path); PnlCorr is allocation-free. The archive owns its descriptors by
//  value (bounded by capacity C).

#include <algorithm> // std::sort, std::min
#include <cmath>     // std::abs, std::isnan
#include <span>      // std::span
#include <vector>    // std::vector (archive storage, rank scratch)

#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::usize

#include "atx/engine/combine/correlation.hpp" // combine::pairwise_complete_corr
#include "atx/engine/factory/fitness.hpp"     // detail::FitnessCore (oos_pnl descriptor)

namespace atx::engine::factory {

// =========================================================================
//  BehaviorMetric — which profile distance the behavioral objective uses.
//
//  PnlCorr : 1 - |Pearson(PnL_a, PnL_b)| over the both-non-NaN overlap. SHIPPED
//            default; the candidate's realized return profile is the phenotype.
//  RankIc  : 1 - |Pearson(rank(PnL_a), rank(PnL_b))| (Spearman) over the overlap;
//            the documented fallback fork — collapses monotone-equivalent signals
//            and is robust to a few outlier days. Selectable when PnlCorr proves
//            too collinear with the marginal-corr `diversify` objective (§4.2).
// =========================================================================
enum class BehaviorMetric : atx::u8 { PnlCorr, RankIc };

// =========================================================================
//  behavioral_descriptor — the candidate's phenotype: its full realized OOS PnL
//  profile (detail::FitnessCore.oos_pnl). Zero extra eval — this is the SAME
//  stream the fitness pass already materialized. The returned span ALIASES
//  `core.oos_pnl`; it dangles when `core` dies — copy out before the core's
//  lifetime ends (the SearchDriver copies it into CachedScore). noexcept, pure.
// =========================================================================
[[nodiscard]] inline std::span<const atx::f64>
behavioral_descriptor(const detail::FitnessCore &core) noexcept {
  return std::span<const atx::f64>{core.oos_pnl};
}

namespace detail {

// Dense-rank the complete-case (both-non-NaN) overlap of `a` and `b` into the
// parallel output vectors `ra`/`rb` (same length == #valid pairs). Standard
// average-rank tie handling so the rank transform is a deterministic, monotone
// re-expression. The pairing is by index (paired observations of the same date);
// an index where EITHER leg is NaN is skipped (pairwise-complete, matching the
// PnlCorr path's NaN policy). Cold path; allocates O(valid-pairs) scratch.
inline void rank_complete_case(std::span<const atx::f64> a, std::span<const atx::f64> b,
                               std::vector<atx::f64> &ra, std::vector<atx::f64> &rb) {
  ra.clear();
  rb.clear();
  const atx::usize bound = std::min(a.size(), b.size());
  std::vector<atx::f64> va; // valid-pair values of leg a
  std::vector<atx::f64> vb; // valid-pair values of leg b
  va.reserve(bound);
  vb.reserve(bound);
  for (atx::usize i = 0; i < bound; ++i) {
    if (std::isnan(a[i]) || std::isnan(b[i])) {
      continue;
    }
    va.push_back(a[i]);
    vb.push_back(b[i]);
  }
  const atx::usize m = va.size();
  ra.assign(m, 0.0);
  rb.assign(m, 0.0);
  // Average-rank a single leg into `out`, given its values `v`. Sort indices by
  // value (stable by index), then assign each tie-group its mean 1-based rank.
  auto rank_leg = [m](const std::vector<atx::f64> &v, std::vector<atx::f64> &out) {
    std::vector<atx::usize> idx(m);
    for (atx::usize i = 0; i < m; ++i) {
      idx[i] = i;
    }
    std::sort(idx.begin(), idx.end(), [&v](atx::usize x, atx::usize y) {
      if (v[x] != v[y]) {
        return v[x] < v[y];
      }
      return x < y; // stable, deterministic tie-break by original index
    });
    atx::usize i = 0;
    while (i < m) {
      atx::usize j = i;
      while (j + 1 < m && v[idx[j + 1]] == v[idx[i]]) {
        ++j;
      }
      // Tie-group [i, j] gets the mean of 1-based ranks (i+1 .. j+1).
      const atx::f64 mean_rank =
          (static_cast<atx::f64>(i + 1) + static_cast<atx::f64>(j + 1)) / 2.0;
      for (atx::usize t = i; t <= j; ++t) {
        out[idx[t]] = mean_rank;
      }
      i = j + 1;
    }
  };
  rank_leg(va, ra);
  rank_leg(vb, rb);
}

} // namespace detail

// =========================================================================
//  behavioral_distance — 1 - |corr| over the held-out profile (§4.2).
//
//  PnlCorr (default): 1 - |combine::pairwise_complete_corr(a, b)| — plain Pearson
//  over the both-non-NaN overlap. RankIc: rank each leg over the overlap, then
//  1 - |Pearson(ranks)| (Spearman). Both inherit the shared degenerate
//  convention: < 2 valid pairs or a zero-variance leg -> corr 0 -> distance 1 (a
//  candidate with no comparable observations is MAXIMALLY novel against that
//  neighbour — documented edge). Output is in [0, 1]. Pure, deterministic.
//
//  The switch is exhaustive (no default) so a new BehaviorMetric is a compile
//  error until handled — the §4.2 "one-line fork" property.
// =========================================================================
[[nodiscard]] inline atx::f64 behavioral_distance(std::span<const atx::f64> a,
                                                  std::span<const atx::f64> b,
                                                  BehaviorMetric metric = BehaviorMetric::PnlCorr) {
  atx::f64 corr = 0.0;
  switch (metric) {
  case BehaviorMetric::PnlCorr:
    corr = combine::pairwise_complete_corr(a, b);
    break;
  case BehaviorMetric::RankIc: {
    std::vector<atx::f64> ra;
    std::vector<atx::f64> rb;
    detail::rank_complete_case(a, b, ra, rb);
    corr = combine::pairwise_complete_corr(std::span<const atx::f64>{ra},
                                           std::span<const atx::f64>{rb});
    break;
  }
  }
  const atx::f64 dist = 1.0 - std::abs(corr);
  // Clamp floating-point overshoot at the perfect-correlation extreme.
  if (dist < 0.0) {
    return 0.0;
  }
  if (dist > 1.0) {
    return 1.0;
  }
  return dist;
}

// =========================================================================
//  BehavioralArchive — bounded FIFO of past-elite descriptors (§4.2).
//
//  Holds up to capacity `C` past-elite behavioral descriptors (owned by value).
//  Each generation the SearchDriver inserts the current front in CANONICAL-ID
//  order; the archive evicts oldest-first once it exceeds `C` (a ring of recent
//  elite behaviors). No RNG, no global state — fully deterministic.
//
//  novelty(desc, population, k) = mean distance to the k NEAREST descriptors in
//  population ∪ archive. The population is the live generation's descriptors (a
//  span of spans, NOT yet in the archive); the archive is the rolling elite
//  history. If the union has FEWER than k neighbours, the mean is taken over ALL
//  available (documented edge); an EMPTY neighbourhood -> 1.0 (maximally novel —
//  nothing to be redundant with). RNG-free; the k-nearest selection sorts with an
//  index tie-break so equal distances resolve deterministically.
// =========================================================================
class BehavioralArchive {
public:
  // Construct with a fixed FIFO capacity. A capacity of 0 is a valid (degenerate)
  // archive that holds nothing — novelty then reads the population only.
  explicit BehavioralArchive(atx::usize capacity) noexcept : capacity_{capacity} {}

  // Insert one descriptor (copied by value). Evicts the OLDEST entries until the
  // size is within capacity. With capacity 0 the insert is a no-op. The caller
  // feeds descriptors in canonical-id order so the archive contents — and the
  // eviction sequence — are deterministic.
  void insert(std::span<const atx::f64> desc) {
    if (capacity_ == 0) {
      return;
    }
    entries_.emplace_back(desc.begin(), desc.end());
    while (entries_.size() > capacity_) {
      entries_.erase(entries_.begin()); // evict oldest (FIFO); bounded by capacity
    }
  }

  [[nodiscard]] atx::usize size() const noexcept { return entries_.size(); }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

  [[nodiscard]] atx::usize capacity() const noexcept { return capacity_; }

  // Read-only view of the owned descriptors in ring/insertion order (oldest first).
  // Used by the resumable-discover checkpoint to serialize the archive contents
  // EXACTLY so a resumed run's novelty() neighbourhoods match byte-for-byte. The
  // returned reference aliases internal storage — valid for the archive's lifetime.
  [[nodiscard]] const std::vector<std::vector<atx::f64>> &entries() const noexcept {
    return entries_;
  }

  // Mean distance from `desc` to its k nearest neighbours in population ∪ archive.
  // `population` is a span of descriptor spans (the live generation, excluding
  // `desc`'s own entry if the caller chooses — the caller controls membership).
  // `metric` selects the profile distance (PnlCorr default / RankIc fork) — the
  // SAME metric is applied to both the population and the archived legs so the
  // neighbourhood is comparable. Edge: empty neighbourhood -> 1.0; < k neighbours
  // -> mean over all available. Deterministic: distances are collected then sorted
  // with a stable compare, so equal distances pick the lower-index neighbour.
  [[nodiscard]] atx::f64 novelty(std::span<const atx::f64> desc,
                                 std::span<const std::span<const atx::f64>> population,
                                 atx::usize k,
                                 BehaviorMetric metric = BehaviorMetric::PnlCorr) const {
    std::vector<atx::f64> dists;
    dists.reserve(population.size() + entries_.size());
    for (const std::span<const atx::f64> p : population) {
      dists.push_back(behavioral_distance(desc, p, metric));
    }
    for (const std::vector<atx::f64> &e : entries_) {
      dists.push_back(behavioral_distance(desc, std::span<const atx::f64>{e}, metric));
    }
    if (dists.empty()) {
      return 1.0; // nothing to be redundant with -> maximally novel (edge)
    }
    // k nearest = the k SMALLEST distances. Full sort (cold path, small n); the
    // value compare is total (finite distances in [0,1]) so ties are stable.
    std::sort(dists.begin(), dists.end());
    const atx::usize take = std::min(k, dists.size()); // < k neighbours -> all avail
    atx::f64 sum = 0.0;
    for (atx::usize i = 0; i < take; ++i) {
      sum += dists[i];
    }
    return sum / static_cast<atx::f64>(take == 0 ? 1 : take);
  }

private:
  atx::usize capacity_;                        // FIFO bound (ring of recent elites)
  std::vector<std::vector<atx::f64>> entries_; // owned descriptors, oldest first
};

} // namespace atx::engine::factory

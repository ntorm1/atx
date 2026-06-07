#pragma once

// atx::engine::eval — Purged + Embargoed Combinatorial Purged Cross-Validation
// (CPCV) fold generator.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  CPCV is the cross-validation scheme of M. López de Prado, "Advances in
//  Financial Machine Learning" (Wiley, 2018), Ch. 7 ("Cross-Validation in
//  Finance"). It generalizes k-fold CV to financial series whose observations
//  carry OVERLAPPING labels: each observation i owns a half-open label span
//  [t0_i, t1_i). For a fitted object this span IS its [fit_begin, fit_end)
//  window — so a CPCV fold is the many-fold generalization of P4's single
//  fit/apply firewall: a train index whose label overlaps a test label leaks
//  information across the train/test boundary and must be removed (PURGED), and
//  train observations immediately FOLLOWING a test observation are dropped
//  (EMBARGOED) to defeat serial-correlation leakage past the test block.
//
//  Given K contiguous groups and a test-group count k, CPCV forms EVERY one of
//  the C(K, k) group combinations as a test set (versus k-fold's K disjoint
//  folds), yielding far more train/test paths from the same data. Each fold's
//  train set is then purged and embargoed against ITS test set.
//
// ===========================================================================
//  The procedure (as implemented)
// ===========================================================================
//  Input: N observations, each a half-open label span [t0_i, t1_i). Config:
//  group count K (n_groups), test-group count k (n_test_groups), embargo
//  fraction h (embargo).
//   1. GROUP PARTITION (contiguous, ascending): observation i is in group g
//      where group_start(g) = (g*N)/K (integer division); group g spans indices
//      [group_start(g), group_start(g+1)). Near-equal contiguous groups (e.g.
//      N=60, K=6 -> 10 each).
//   2. Enumerate ALL C(K, k) test-group combinations LEXICOGRAPHICALLY via a
//      deterministic next_combination index walk over [0, K) choosing k (NO
//      RNG). Fold order == lexicographic combination order.
//   3. Per combination:
//        * test_idx  = sorted-ascending union of the selected groups' indices.
//        * train candidates = all indices NOT in the selected test groups.
//        * PURGE: drop a train index j iff its label overlaps the label of ANY
//          test observation (per-test-observation definition — keeps non-
//          contiguous combinations usable; a single global [min t0, max t1)
//          window would span the whole range and empty such folds).
//        * EMBARGO: embargo_len = ceil(h * N); drop a train index j iff some
//          test index ti has ti < j && j <= ti + embargo_len (forward-in-index
//          embargo after each test observation). h == 0 -> embargo_len == 0 ->
//          no embargo.
//        * train_idx = remaining indices, ascending.
//   4. Output one CpcvFold{train_idx, test_idx} per combination.
//
// ===========================================================================
//  Determinism (load-bearing)
// ===========================================================================
//  No RNG anywhere — the combination walk and all index sets are built in fixed
//  ascending order, so the fold vector is run-to-run byte-identical. Reductions
//  are O(N^2) (CPCV is not a hot path; correctness first).

#include <cmath>    // std::ceil
#include <cstddef>  // (size_type idioms)
#include <span>     // std::span
#include <vector>   // std::vector

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // atx::f64, atx::usize

namespace atx::engine::eval {

// ===========================================================================
//  LabelSpan — one observation's half-open label window [t0, t1).
//
//  For a fitted object this IS its [fit_begin, fit_end) window: t0 is the first
//  bar the label depends on, t1 the one-past-last. Rule of Zero POD.
// ===========================================================================
struct LabelSpan {
  atx::usize t0; // inclusive lower bound of the label's information window
  atx::usize t1; // exclusive upper bound (half-open: bar t1 is NOT included)
};

// ===========================================================================
//  CpcvConfig — CPCV knobs (López de Prado AFML Ch. 7 defaults).
//    n_groups       — K contiguous observation groups.
//    n_test_groups  — k groups held out per fold; C(K, k) folds in total.
//    embargo        — embargo fraction h of N (embargo_len = ceil(h*N)).
// ===========================================================================
struct CpcvConfig {
  atx::usize n_groups = 6;
  atx::usize n_test_groups = 2;
  atx::f64 embargo = 0.01;
};

// ===========================================================================
//  CpcvFold — one combinatorial fold: ascending train and test index sets.
//  Rule of Zero aggregate owning its two index vectors.
// ===========================================================================
struct CpcvFold {
  std::vector<atx::usize> train_idx;
  std::vector<atx::usize> test_idx;
};

namespace detail {

// ---------------------------------------------------------------------------
//  binomial — C(n, k), computed multiplicatively to size the fold vector
//  without overflowing a factorial. Returns 0 when k > n. Uses the symmetry
//  C(n, k) == C(n, n-k) to keep the running product small.
//
//  SAFETY (no intermediate overflow): after iteration i the running product is
//  EXACTLY C(n - kk + 1 + i, i + 1), itself a binomial coefficient and so an
//  integer that never exceeds the final result; the division by (i + 1) is
//  exact at every step. Practical CPCV uses tiny K (default 6 -> C(6,2)=15), so
//  the result is far inside usize. (Small duplicate of pbo.hpp's helper — a
//  trivial static detail:: utility, intentionally NOT promoted to atx-core.)
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::usize binomial(atx::usize n, atx::usize k) noexcept {
  if (k > n) {
    return 0U;
  }
  const atx::usize kk = (k > n - k) ? (n - k) : k; // smaller arm, by symmetry
  atx::usize result = 1U;
  for (atx::usize i = 0U; i < kk; ++i) {
    const atx::usize prev = result;
    result = result * (n - kk + 1U + i) / (i + 1U);
    ATX_ASSERT(result >= prev); // monotone walk; a decrease would mean overflow
  }
  return result;
}

// ---------------------------------------------------------------------------
//  next_combination — advance a strictly-ascending k-subset of [0, n) to the
//  lexicographically next one. Returns false when `comb` is the final (largest)
//  combination, leaving it unchanged. Deterministic; no allocation; no RNG.
//
//  Standard index walk: find the rightmost element with headroom to increment
//  (comb[i] < n - k + i), bump it, then reset every element to its right to the
//  minimal increasing run. `comb` must hold k strictly-ascending indices < n.
//  (Small duplicate of pbo.hpp's helper, by design — see binomial above.)
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool next_combination(std::span<atx::usize> comb, atx::usize n) noexcept {
  const atx::usize k = comb.size();
  if (k == 0U) {
    return false; // the empty subset is its own only combination
  }
  atx::usize i = k; // one past the last; loop decrements before use
  while (i > 0U) {
    --i;
    const atx::usize ceiling = n - k + i; // max legal value at position i
    if (comb[i] < ceiling) {
      ++comb[i];
      for (atx::usize j = i + 1U; j < k; ++j) {
        comb[j] = comb[j - 1U] + 1U;
      }
      return true;
    }
  }
  return false; // already the last combination
}

// ---------------------------------------------------------------------------
//  group_start — first observation index of group g in the contiguous,
//  ascending K-group partition of N observations: (g * N) / K (integer
//  division). group g spans [group_start(g), group_start(g+1)); group_start(K)
//  == N closes the last group. Near-equal widths (N=60,K=6 -> 10 each).
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::usize group_start(atx::usize g, atx::usize n,
                                            atx::usize n_groups) noexcept {
  ATX_ASSERT(n_groups != 0U);
  return (g * n) / n_groups;
}

// ---------------------------------------------------------------------------
//  spans_overlap — half-open label-interval overlap predicate.
//
//  SAFETY (half-open intervals): a = [a.t0, a.t1), b = [b.t0, b.t1). They share
//  at least one bar iff a starts strictly before b ends AND b starts strictly
//  before a ends: (a.t0 < b.t1) && (b.t0 < a.t1). The STRICT '<' is essential —
//  with half-open windows, abutting spans [x, y) and [y, z) touch only at the
//  excluded endpoint y and so do NOT overlap (no shared bar), which the strict
//  comparison correctly reports as false.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool spans_overlap(const LabelSpan &a, const LabelSpan &b) noexcept {
  return (a.t0 < b.t1) && (b.t0 < a.t1);
}

// ---------------------------------------------------------------------------
//  build_test_idx — ascending union of the observation indices of the selected
//  test groups. `combo` holds the k selected group ids (strictly ascending), so
//  appending each group's contiguous [group_start(g), group_start(g+1)) run in
//  combo order already yields a globally ascending index vector.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<atx::usize>
build_test_idx(std::span<const atx::usize> combo, atx::usize n, atx::usize n_groups) {
  std::vector<atx::usize> test_idx;
  for (const atx::usize g : combo) {
    const atx::usize lo = group_start(g, n, n_groups);
    const atx::usize hi = group_start(g + 1U, n, n_groups);
    for (atx::usize i = lo; i < hi; ++i) {
      test_idx.push_back(i);
    }
  }
  return test_idx;
}

// ---------------------------------------------------------------------------
//  purged_embargoed_train — the train set for one fold: every index NOT in a
//  test group, minus those purged (label overlaps ANY test observation) or
//  embargoed (forward-in-index within embargo_len of some test observation).
//
//  `is_test` is the per-observation test-membership table for this fold (true
//  for indices in the selected test groups). The scan is ascending, so the
//  returned train_idx is ascending. O(N * |test_idx|) — CPCV is not hot.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<atx::usize>
purged_embargoed_train(std::span<const LabelSpan> spans, std::span<const atx::usize> test_idx,
                       const std::vector<bool> &is_test, atx::usize embargo_len) {
  std::vector<atx::usize> train_idx;
  const atx::usize n = spans.size();
  for (atx::usize j = 0U; j < n; ++j) {
    if (is_test[j]) {
      continue; // a test observation is never a train observation
    }
    bool drop = false;
    for (const atx::usize ti : test_idx) {
      // PURGE: train label j overlaps test label ti (information leak).
      if (spans_overlap(spans[j], spans[ti])) {
        drop = true;
        break;
      }
      // EMBARGO: j sits within embargo_len observations AFTER test obs ti.
      if (ti < j && j <= ti + embargo_len) {
        drop = true;
        break;
      }
    }
    if (!drop) {
      train_idx.push_back(j);
    }
  }
  return train_idx;
}

} // namespace detail

// ===========================================================================
//  cpcv_folds — enumerate all C(K, k) purged + embargoed CPCV folds.
//
//  PURE, deterministic (no RNG). `spans` carries one half-open label window per
//  observation (the fitted object's [fit_begin, fit_end) per Ch. 7). Returns one
//  CpcvFold per lexicographic test-group combination, in that order. Each fold's
//  train_idx and test_idx are ascending. Preconditions (fail-fast under debug):
//  n_groups >= 1, n_test_groups in [1, n_groups]. embargo_len = ceil(embargo*N).
//  An empty `spans` yields C(K,k) folds with empty index sets.
// ===========================================================================
[[nodiscard]] inline std::vector<CpcvFold> cpcv_folds(std::span<const LabelSpan> spans,
                                                      const CpcvConfig &cfg) {
  const atx::usize n = spans.size();
  const atx::usize k_groups = cfg.n_groups;
  const atx::usize k_test = cfg.n_test_groups;
  ATX_ASSERT(k_groups >= 1U);
  ATX_ASSERT(k_test >= 1U && k_test <= k_groups);

  // embargo_len = ceil(h * N); h == 0 (or N == 0) -> 0 -> no embargo.
  const atx::f64 raw = cfg.embargo * static_cast<atx::f64>(n);
  const atx::usize embargo_len = static_cast<atx::usize>(std::ceil(raw));

  std::vector<CpcvFold> folds;
  folds.reserve(detail::binomial(k_groups, k_test));

  // Lexicographic test-group combination walk over group ids [0, K) choosing k.
  std::vector<atx::usize> combo(k_test);
  for (atx::usize i = 0U; i < k_test; ++i) {
    combo[i] = i; // first combination: 0,1,...,k-1
  }

  std::vector<bool> is_test(n, false); // reused per fold (cleared then refilled)
  do {
    std::vector<atx::usize> test_idx = detail::build_test_idx(std::span<const atx::usize>{combo},
                                                              n, k_groups);
    // Refresh the membership table for this combination.
    for (atx::usize i = 0U; i < n; ++i) {
      is_test[i] = false;
    }
    for (const atx::usize ti : test_idx) {
      is_test[ti] = true;
    }
    std::vector<atx::usize> train_idx =
        detail::purged_embargoed_train(spans, std::span<const atx::usize>{test_idx},
                                       is_test, embargo_len);
    folds.push_back(CpcvFold{std::move(train_idx), std::move(test_idx)});
  } while (detail::next_combination(std::span<atx::usize>{combo}, k_groups));

  return folds;
}

} // namespace atx::engine::eval

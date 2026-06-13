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

#include <span>     // std::span
#include <vector>   // std::vector

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // atx::f64, atx::usize

// detail::binomial / detail::next_combination are the SHARED combinatorics
// helpers. They were originally duplicated verbatim here and in pbo.hpp with the
// documented assumption that a single TU never includes both. S3-4 (pool-aware
// fitness) is the first consumer to include BOTH cpcv.hpp (folds) and, via
// validation/bias_audit.hpp, pbo.hpp — which made the two inline definitions
// collide (redefinition in one TU). The dedup: cpcv.hpp now REUSES pbo.hpp's
// detail::binomial / detail::next_combination instead of redefining them (the
// bodies were byte-identical), so a TU may include both headers safely.
#include "atx/engine/eval/pbo.hpp" // eval::detail::binomial, eval::detail::next_combination

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

// binomial / next_combination are REUSED from pbo.hpp (included above) — they
// were previously duplicated here verbatim. See the include-site note for why
// the dedup was required (S3-4 includes both headers in one TU).

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
[[nodiscard]] std::vector<atx::usize>
build_test_idx(std::span<const atx::usize> combo, atx::usize n, atx::usize n_groups);

// ---------------------------------------------------------------------------
//  purged_embargoed_train — the train set for one fold: every index NOT in a
//  test group, minus those purged (label overlaps ANY test observation) or
//  embargoed (forward-in-index within embargo_len of some test observation).
//
//  `is_test` is the per-observation test-membership table for this fold (true
//  for indices in the selected test groups). The scan is ascending, so the
//  returned train_idx is ascending. O(N * |test_idx|) — CPCV is not hot.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<atx::usize>
purged_embargoed_train(std::span<const LabelSpan> spans, std::span<const atx::usize> test_idx,
                       const std::vector<bool> &is_test, atx::usize embargo_len);

} // namespace detail

// ===========================================================================
//  cpcv_folds — enumerate all C(K, k) purged + embargoed CPCV folds.
//
//  PURE, deterministic (no RNG). `spans` carries one half-open label window per
//  observation (the fitted object's [fit_begin, fit_end) per Ch. 7). Returns one
//  CpcvFold per lexicographic test-group combination, in that order. Each fold's
//  train_idx and test_idx are ascending. Preconditions (fail-fast under debug):
//  n_groups >= 1, n_test_groups in [1, n_groups], embargo >= 0. embargo_len =
//  ceil(embargo*N). An empty `spans` yields C(K,k) folds with empty index sets.
// ===========================================================================
[[nodiscard]] std::vector<CpcvFold> cpcv_folds(std::span<const LabelSpan> spans,
                                               const CpcvConfig &cfg);

} // namespace atx::engine::eval

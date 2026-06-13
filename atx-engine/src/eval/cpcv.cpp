#include "atx/engine/eval/cpcv.hpp"

#include <cmath>    // std::ceil
#include <utility>  // std::move

namespace atx::engine::eval {

namespace detail {

// ---------------------------------------------------------------------------
//  build_test_idx — ascending union of the observation indices of the selected
//  test groups. `combo` holds the k selected group ids (strictly ascending), so
//  appending each group's contiguous [group_start(g), group_start(g+1)) run in
//  combo order already yields a globally ascending index vector.
// ---------------------------------------------------------------------------
std::vector<atx::usize>
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
std::vector<atx::usize>
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
//  n_groups >= 1, n_test_groups in [1, n_groups], embargo >= 0. embargo_len =
//  ceil(embargo*N). An empty `spans` yields C(K,k) folds with empty index sets.
// ===========================================================================
std::vector<CpcvFold> cpcv_folds(std::span<const LabelSpan> spans,
                                 const CpcvConfig &cfg) {
  const atx::usize n = spans.size();
  const atx::usize k_groups = cfg.n_groups;
  const atx::usize k_test = cfg.n_test_groups;
  ATX_ASSERT(k_groups >= 1U);
  ATX_ASSERT(k_test >= 1U && k_test <= k_groups);

  // SAFETY: embargo must be non-negative — a negative fraction would make
  // std::ceil(embargo*N) negative, and casting a negative f64 to the unsigned
  // usize embargo_len is undefined behavior. Fail fast on the contract instead.
  ATX_ASSERT(cfg.embargo >= 0.0);
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

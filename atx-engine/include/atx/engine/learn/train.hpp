#pragma once

// atx::engine::learn — training scaffold: date-axis CPCV label spans, date-fold
// -> feature-row expansion, the seeded RNG derivation, and the deflation trial
// counter (S5-1; later units grow these).
//
// =====================================================================
//  Why a date-axis CPCV, expanded to feature rows (§0.2)
// =====================================================================
//  Purge/embargo are TEMPORAL — they defeat label-overlap and serial-correlation
//  leakage across the train/test boundary IN TIME. So the learn layer feeds
//  eval::cpcv_folds one LabelSpan per DATE (its forward-return label window
//  [date, min(date+horizon, n_dates))), gets back folds whose index sets are
//  DATE indices, and then EXPANDS each date-fold to all the (date x instrument)
//  feature rows at those dates. The no-look-ahead firewall is INHERITED from
//  eval::cpcv (consumed unchanged), never re-implemented here.
//
//  Only the distinct dates that actually appear in the FeatureMatrix's rows get a
//  LabelSpan (an out-of-universe-everywhere date emits no rows and so is not a CV
//  observation). The LabelSpan vector is indexed by USED-date ordinal; the
//  expansion maps a fold's used-date ordinals back to the actual date indices and
//  collects every valid feature row at those dates.
//
// =====================================================================
//  Determinism (M1) — seed_for mirrors the S3 factory precedent
// =====================================================================
//  Every RNG draw in the learn layer seeds from a fixed SplitMix-style mix of
//  (master_seed, tag, a, b) — never wall-clock / thread / address / map order.
//  This is the learn-layer analog of factory::detail::seed_for(master, gen, idx);
//  the extra string `tag` lets distinct unit/horizon streams stay well-separated.
//
// Hot inline bits (seed_for, TrialCounter) stay here; cold run-once span/fold
// builders are declared here and defined in src/learn/train.cpp.

#include <string_view>
#include <vector>

#include "atx/core/types.hpp" // f64, u16, u64, usize

#include "atx/engine/eval/cpcv.hpp" // eval::LabelSpan, eval::CpcvFold, eval::cpcv_folds
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix

namespace atx::engine::learn {

// ===========================================================================
//  RowFold / Folds — a CPCV date-fold expanded to FEATURE-ROW index sets.
//
//  train_rows / test_rows are row indices into the FeatureMatrix (NOT date
//  indices). Both are ascending. The TrainScaffold tests iterate
//  `for (auto& f : folds) for (auto tr : f.train_rows) for (auto te : f.test_rows)`.
// ===========================================================================
struct RowFold {
  std::vector<atx::usize> train_rows;
  std::vector<atx::usize> test_rows;
};
using Folds = std::vector<RowFold>;

// ===========================================================================
//  TrialCounter — the monotone ML deflation trial count N (§0.3).
//
//  A learned model has no population; the deflation analog is a distinct fit
//  (per CV config x horizon x ensemble candidate). next() returns the post-
//  increment count to pass as N to eval::deflated_sharpe — more configs tried =>
//  a higher deflation bar.
// ===========================================================================
struct TrialCounter {
  atx::usize n{0};
  atx::usize next() noexcept { return ++n; }
};

// ===========================================================================
//  seed_for — the learn-layer per-(tag, a, b) seed derivation (M1).
//
//  A fixed SplitMix-style mix of (master, tag, a, b); pure, portable (no
//  std::hash / platform-width hash), depending on NOTHING else (never
//  worker/thread/time/address). Mirrors factory::detail::seed_for; the string
//  `tag` is folded in so distinct unit/horizon streams are well-separated.
// ===========================================================================
[[nodiscard]] inline atx::u64 seed_for(atx::u64 master, std::string_view tag, atx::usize a,
                                       atx::usize b) noexcept {
  auto mix = [](atx::u64 x) noexcept -> atx::u64 {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
  };
  atx::u64 h = mix(master);
  // Fold the tag bytes in (FNV-style accumulate, then avalanche) so distinct
  // string tags produce well-separated streams without std::hash.
  atx::u64 t = 0xCBF29CE484222325ULL;
  for (const char c : tag) {
    t = (t ^ static_cast<atx::u64>(static_cast<unsigned char>(c))) * 0x00000100000001B3ULL;
  }
  h = mix(h ^ t);
  h = mix(h ^ (static_cast<atx::u64>(a) + 0x9E3779B97F4A7C15ULL));
  h = mix(h ^ (static_cast<atx::u64>(b) + 0x632BE59BD9B4E019ULL));
  return h;
}

// ===========================================================================
//  date_label_spans — one half-open forward-return label window per USED date.
//
//  For each distinct date that appears in the FeatureMatrix's rows (in ascending
//  date order), emit LabelSpan{date, min(date + horizon, n_dates)} — the window
//  the horizon-`horizon` forward-return label at that date depends on. The result
//  is indexed by used-date ordinal; expand_date_folds maps fold indices (which
//  are ordinals into THIS vector) back to actual date indices.
// ===========================================================================
[[nodiscard]] std::vector<eval::LabelSpan> date_label_spans(const FeatureMatrix &fm,
                                                            atx::u16 horizon);

namespace detail {

// The ascending list of distinct USED dates (the date axis of date_label_spans):
// the same dates, in the same order. A CPCV fold's index `o` is an ordinal into
// THIS list, so used_dates[o] is the actual date index.
[[nodiscard]] std::vector<atx::usize> used_dates(const FeatureMatrix &fm);

// Collect every VALID feature row whose date is in `date_set`, ascending by row.
// `in_set[date]` is the per-date membership table for this side of the fold.
// Invalid rows (a non-finite feature, M8) are dropped from the fit.
[[nodiscard]] std::vector<atx::usize> rows_for_dates(const FeatureMatrix &fm,
                                                     const std::vector<bool> &in_set);

} // namespace detail

// ===========================================================================
//  expand_date_folds — map each CPCV DATE-fold to FEATURE-ROW index sets.
//
//  A fold's train_idx / test_idx are ordinals into the used-date list (the same
//  axis date_label_spans was built on). For each fold: build per-date membership
//  tables, then collect train_rows = {valid rows whose date is a train date} and
//  test_rows likewise. Because train and test date ordinals are disjoint (CPCV
//  guarantees it) and each date maps to one ordinal, no train row can share a
//  date with any test row (§0.2 firewall, carried to the rows).
// ===========================================================================
[[nodiscard]] Folds expand_date_folds(const std::vector<eval::CpcvFold> &folds,
                                      const FeatureMatrix &fm);

} // namespace atx::engine::learn

#pragma once

// atx::engine::learn — PIT latent factor + interaction feature extraction (S5-2).
//
// =====================================================================
//  What this header is
// =====================================================================
//  The "hidden feature" layer: two point-in-time transforms that surface
//  higher-order structure the raw FeatureMatrix columns do not express on their
//  own, WITHOUT ever reading the future.
//
//    * Latent factors (trailing-fit PCA). fit_latent fits a core::linalg::pca
//      basis on the trailing valid rows only (date <= t - embargo), then
//      apply_latent projects ANY given rows onto that frozen basis. The basis is
//      truncation-invariant: rows at dates > t can never change it (M2 / §0.5).
//
//    * Bounded interactions. select_interactions ranks features by trailing
//      |Spearman IC| against the horizon-0 label and returns the crossed pairs
//      (a < b) among the top-m, in a fixed deterministic order (M1 / §4.2b). The
//      pair value a later linear model consumes is standardize(X[:,a]) *
//      standardize(X[:,b]); S5-2 only SELECTS the pairs (S5-3 materializes them),
//      so a small materialize helper is provided for that downstream use.
//
// =====================================================================
//  The two firewalls this unit must honor (M1, M2 / §0.5)
// =====================================================================
//  PIT fit (M2). Both transforms are fit on a TRAILING window only: rows with
//  date <= t - embargo AND row_valid. Adding later dates to the FeatureMatrix can
//  never change the fitted basis or the selected interaction set — the
//  truncation-invariance test pins this.
//
//  Determinism (M1). The trailing-row gather walks rows in index order; the
//  interaction ranking breaks |IC| ties by ascending feature index (a total
//  order); the crossed-pair emission is a fixed nested loop. No map iteration, no
//  RNG, no float-non-associative reduction over an unordered set.
//
//  Leakage fixes (W0-L0). select_interactions reads only MATURED labels (L-02,
//  LabelMaturityRule). CV fits refit the augmentation recipe per fold on the fold's
//  train rows (L-03, FoldAugRule / fit_fold_augmentation). This header also carries
//  the small protocol vocabulary (TrialCountRule, HorizonBlendIc, LearnProtocol) and
//  the LearnFitTrace audit record shared by the linear / GBT / sequence fitters.
//
// The small helpers are inline here; the fitters (fit_latent, select_interactions and
// the row-explicit / fold-local variants) live in src/learn/latent.cpp. Fitting is a
// COLD path (once per training window), so std::vector / Eigen allocation is fine.

#include <algorithm> // std::stable_sort (mean_date_ic date grouping)
#include <cmath>     // std::sqrt, std::isfinite
#include <optional>  // std::optional
#include <span>      // std::span (the rows view apply_latent projects)
#include <utility>   // std::pair, std::move
#include <vector>    // std::vector

#include <Eigen/Dense> // Eigen::Index (gather / score dimensions)

#include "atx/core/macro.hpp" // ATX_CHECK
#include "atx/core/types.hpp" // f64, u8, u16, u32, usize, i64

#include "atx/core/linalg/linalg.hpp"      // MatX, VecX
#include "atx/core/linalg/pca.hpp"         // core::linalg::pca, PcaResult, transform
#include "atx/core/stats/cross_section.hpp" // core::stats::rank (Spearman = Pearson on ranks)

#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix

namespace atx::engine::learn {

namespace lin = atx::core::linalg;

// ===========================================================================
//  LatentBasis — a frozen PCA basis fit on a trailing PIT window.
//
//  model       : the fitted core::linalg::pca result (mean + components, columns
//                are unit eigenvectors, descending explained variance).
//  fit_upto_date: the t the basis was fit at — the window was date <= t - embargo.
//  k           : the number of components actually kept (0 when PCA is disabled or
//                the trailing window was too small to fit; model is empty then).
// ===========================================================================
struct LatentBasis {
  atx::core::linalg::PcaResult model;
  atx::usize fit_upto_date{0};
  atx::u32 k{0};
};

// ===========================================================================
//  FoldAugRule — how a CPCV fold obtains its augmentation (W0-L0, L-03).
//
//  FullWindowV1 : legacy. The caller's fitted augmentation (PCA basis + selected
//                 interactions, typically fit on the WHOLE window, test rows included)
//                 is copied verbatim into every fold, so a fold's OOS predictions can
//                 depend on its own test rows. Kept only to re-derive frozen artifacts.
//  FoldLocalV2  : default. Each fold REFITS the augmentation recipe on its own train
//                 rows (fit_fold_augmentation): PCA with the same k, supervised
//                 interactions re-selected with the same top-m. Fixed (label-free)
//                 interactions are reused verbatim. The deployed model keeps the
//                 caller's augmentation unchanged.
// ===========================================================================
enum class FoldAugRule : atx::u8 { FullWindowV1 = 0, FoldLocalV2 = 1 };

// ===========================================================================
//  LatentAugmentation — the full hidden-feature recipe at a point in time.
//
//  pca          : the latent basis, if PCA is enabled (k > 0).
//  interactions : the selected feature pairs (a < b) to cross, in fixed order.
//  interactions_fixed : provenance of `interactions` (W0-L0, L-03). false (default)
//                 == they were SELECTED with labels (select_interactions), so a CV fold
//                 must re-select them on its own train rows; the top-m is recovered as
//                 the number of distinct features in the pair set (select_interactions
//                 emits every pair of its chosen set). true == explicit, label-free
//                 pairs that need no statistical refit and are reused in every fold.
// ===========================================================================
struct LatentAugmentation {
  std::optional<LatentBasis> pca;
  std::vector<std::pair<atx::u32, atx::u32>> interactions;
  bool interactions_fixed{false};
};

// ===========================================================================
//  LabelMaturityRule — which labels select_interactions may read (W0-L0, L-02).
//
//  FiniteLabelV1 : legacy. Any row with date <= t - embargo and a FINITE Y[0] label.
//                  A finite label anchored at r with horizon H > embargo still depends
//                  on prices after t (it matures at r + H), so this leaks the future.
//  MaturedV2     : default. Additionally requires the label to be realized by the
//                  cutoff: row_date + H <= t - embargo (label_matured), with H =
//                  fm.label_horizons[0]. An UNANNOTATED matrix (no label_horizons)
//                  selects nothing — the maturity of its labels cannot be proven.
// ===========================================================================
enum class LabelMaturityRule : atx::u8 { FiniteLabelV1 = 0, MaturedV2 = 1 };

// ===========================================================================
//  Learn fit protocol vocabulary shared by fit_linear / fit_gbt / fit_tcn|gru|attn
//  (W0-L0, L-08). Each rule keeps the legacy numeric behaviour reproducible behind
//  its V1 value; the defaults are the corrected behaviour.
//
//  TrialCountRule   PerFoldFitV1       : legacy, trial_count += 1 per fold fit, i.e.
//                                        folds x horizons. Folds are pieces of ONE
//                                        out-of-fold series, not independent trials.
//                   PerConfigurationV2 : default, one fit_* call evaluates ONE
//                                        configuration -> trial_count = 1 (0 when no
//                                        fold fit happened). Registry-side accounting
//                                        of how many configurations were tried is
//                                        W0-E0b's.
//  HorizonBlendIc   PooledPearsonV1    : legacy, the horizon-blend IC is one Pearson over
//                                        every fold's pooled (pred, label) pairs, which
//                                        mixes dates and rewards market timing.
//                   MeanDateIcV2       : default, the mean over dates of the per-date
//                                        cross-sectional IC of the fold-averaged OOF
//                                        prediction (the same statistic as the
//                                        oos_score_series, at horizon h).
// ===========================================================================
enum class TrialCountRule : atx::u8 { PerFoldFitV1 = 0, PerConfigurationV2 = 1 };
enum class HorizonBlendIc : atx::u8 { PooledPearsonV1 = 0, MeanDateIcV2 = 1 };

struct LearnProtocol {
  TrialCountRule trials{TrialCountRule::PerConfigurationV2};
  HorizonBlendIc blend_ic{HorizonBlendIc::MeanDateIcV2};
  FoldAugRule fold_aug{FoldAugRule::FoldLocalV2};
};

// ===========================================================================
//  LearnFitTrace — the per-fold audit record of a CV fit (W0-L0 leakage proofs).
//
//  Filled only when a caller passes a non-null trace to a fit_* overload; the
//  default path records nothing. One LearnFoldRecord per (horizon, fold) that
//  produced a fit, in fit order.
//    test_keys / test_pred : the FeatureMatrix rows (linear, gbt) or SequenceTensor
//                            samples (tcn/gru/attn) the fold predicted out-of-fold, and
//                            the fold model's prediction for each, in the same order.
//    artifact              : the fold's TRAINING artifact flattened to f64 (fold
//                            standardization, fold augmentation, coefficients / forest
//                            nodes / ensemble member states). Two fits whose fold
//                            artifacts are byte-identical made the same fold model.
//    fit_keys / val_keys   : the keys the fold model was trained on and (sequence fits)
//                            selected its checkpoint on; val_keys is empty for fits
//                            with no checkpoint selection (linear, gbt).
//  oof_pred / oof_cnt : the horizon-0 out-of-fold prediction per key (fold-averaged;
//                       NaN where oof_cnt == 0), i.e. what the oos_score_series scores.
//  deploy_fit_keys / deploy_val_keys : the samples the DEPLOYED sequence ensemble was
//                       trained on / checkpointed on (empty for linear, gbt).
// ===========================================================================
struct LearnFoldRecord {
  atx::usize horizon_idx{0};
  atx::usize fold_idx{0};
  std::vector<atx::usize> test_keys;
  std::vector<atx::f64> test_pred;
  std::vector<atx::f64> artifact;
  std::vector<atx::usize> fit_keys;
  std::vector<atx::usize> val_keys;
};

struct LearnFitTrace {
  std::vector<LearnFoldRecord> folds;
  std::vector<atx::f64> oof_pred;
  std::vector<atx::u32> oof_cnt;
  std::vector<atx::usize> deploy_fit_keys;
  std::vector<atx::usize> deploy_val_keys;
};

namespace detail {

// The trailing-window upper bound: the largest date allowed into a PIT fit at t
// with the given embargo. Returns false when t < embargo (the window is empty —
// there is no admissible date), so callers short-circuit to an empty fit rather
// than underflowing usize.
[[nodiscard]] inline bool trailing_cutoff(atx::usize t, atx::u16 embargo,
                                          atx::usize &cutoff_out) noexcept {
  const atx::usize e = static_cast<atx::usize>(embargo);
  if (t < e) {
    return false; // no date satisfies date <= t - embargo
  }
  cutoff_out = t - e;
  return true;
}

// Gather the indices of valid rows inside the trailing window (date <= cutoff),
// in ascending row order (deterministic — a single forward walk, no map).
[[nodiscard]] inline std::vector<atx::usize>
trailing_valid_rows(const FeatureMatrix &fm, atx::usize cutoff) {
  std::vector<atx::usize> rows;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    if (fm.row_date[r] <= cutoff && fm.row_valid[r] != 0) {
      rows.push_back(r);
    }
  }
  return rows;
}

// Materialize the gathered rows into an (|rows| x n_features) column-major MatX.
// The FeatureMatrix stores X ROW-MAJOR (X[r*n_features + f]) while MatX is
// COLUMN-MAJOR, so we fill element-wise (correct + clear) rather than aliasing.
[[nodiscard]] inline lin::MatX gather_matrix(const FeatureMatrix &fm,
                                             std::span<const atx::usize> rows) {
  const auto n = static_cast<Eigen::Index>(rows.size());
  const auto p = static_cast<Eigen::Index>(fm.n_features);
  lin::MatX m(n, p);
  for (Eigen::Index i = 0; i < n; ++i) {
    const atx::usize base = rows[static_cast<atx::usize>(i)] * fm.n_features;
    // SAFETY: row index < n_rows and base + n_features <= X.size() because each
    // emitted row wrote exactly n_features cells (FeatureMatrix invariant); the
    // gather only ever references rows from trailing_valid_rows, all < n_rows.
    ATX_CHECK(base + fm.n_features <= fm.X.size());
    for (Eigen::Index j = 0; j < p; ++j) {
      m(i, j) = fm.X[base + static_cast<atx::usize>(j)];
    }
  }
  return m;
}

// One feature column over the gathered rows, in row order.
[[nodiscard]] inline std::vector<atx::f64>
column(const FeatureMatrix &fm, std::span<const atx::usize> rows, atx::usize feat) {
  std::vector<atx::f64> col;
  col.reserve(rows.size());
  for (const atx::usize r : rows) {
    col.push_back(fm.X[r * fm.n_features + feat]);
  }
  return col;
}

// Pearson correlation of two equal-length vectors. Returns 0 for a degenerate
// (constant) input — the same all-zero convention core::stats::zscore uses, so a
// flat feature contributes no information rather than a NaN. <= 60 lines, one job.
[[nodiscard]] inline atx::f64 pearson(std::span<const atx::f64> a,
                                      std::span<const atx::f64> b) noexcept {
  const atx::usize n = a.size();
  if (n < 2U) {
    return 0.0;
  }
  atx::f64 ma = 0.0;
  atx::f64 mb = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    ma += a[i];
    mb += b[i];
  }
  ma /= static_cast<atx::f64>(n);
  mb /= static_cast<atx::f64>(n);
  atx::f64 cov = 0.0;
  atx::f64 va = 0.0;
  atx::f64 vb = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 da = a[i] - ma;
    const atx::f64 db = b[i] - mb;
    cov += da * db;
    va += da * da;
    vb += db * db;
  }
  if (va == 0.0 || vb == 0.0) {
    return 0.0; // a constant series has no linear relationship to anything
  }
  return cov / std::sqrt(va * vb);
}

// Spearman correlation = Pearson on the rank transforms (ties averaged, per
// core::stats::rank). Used as the feature-vs-label IC for interaction selection.
[[nodiscard]] inline atx::f64 spearman(std::span<const atx::f64> a,
                                       std::span<const atx::f64> b) {
  const atx::usize n = a.size();
  std::vector<atx::f64> ra(n);
  std::vector<atx::f64> rb(n);
  atx::core::stats::rank(a, std::span<atx::f64>{ra});
  atx::core::stats::rank(b, std::span<atx::f64>{rb});
  return pearson(std::span<const atx::f64>{ra}, std::span<const atx::f64>{rb});
}

// Mean over dates of the per-date cross-sectional Pearson IC (W0-L0, L-08: the
// HorizonBlendIc::MeanDateIcV2 statistic). Observation i sits at date date[i] with
// prediction pred[i] and label label[i]; the three spans are parallel (checked). Pairs
// with a non-finite side are skipped; a date contributes only with >= 2 finite pairs.
// Dates are visited ascending and pairs in index order within a date (a stable sort of
// the indices), so the reduction order is fixed (M1). No contributing date -> 0.
[[nodiscard]] inline atx::f64 mean_date_ic(std::span<const atx::usize> date,
                                           std::span<const atx::f64> pred,
                                           std::span<const atx::f64> label) {
  ATX_CHECK(date.size() == pred.size() && pred.size() == label.size());
  std::vector<atx::usize> order;
  order.reserve(date.size());
  for (atx::usize i = 0; i < date.size(); ++i) {
    if (std::isfinite(pred[i]) && std::isfinite(label[i])) {
      order.push_back(i);
    }
  }
  std::stable_sort(order.begin(), order.end(),
                   [&date](atx::usize a, atx::usize b) { return date[a] < date[b]; });
  atx::f64 sum = 0.0;
  atx::usize n_dates = 0;
  std::vector<atx::f64> pv;
  std::vector<atx::f64> lv;
  atx::usize lo = 0;
  while (lo < order.size()) {
    const atx::usize d = date[order[lo]];
    pv.clear();
    lv.clear();
    atx::usize hi = lo;
    for (; hi < order.size() && date[order[hi]] == d; ++hi) {
      pv.push_back(pred[order[hi]]);
      lv.push_back(label[order[hi]]);
    }
    if (pv.size() >= 2U) {
      sum += pearson(std::span<const atx::f64>{pv}, std::span<const atx::f64>{lv});
      ++n_dates;
    }
    lo = hi;
  }
  return (n_dates == 0U) ? 0.0 : sum / static_cast<atx::f64>(n_dates);
}

// mean_date_ic over accumulated out-of-fold predictions: key k (a FeatureMatrix row or a
// sequence sample) sits at date key_date[k], carries label[k], and was predicted
// oof_cnt[k] times with prediction sum oof_sum[k]. Uncovered keys (cnt 0) are skipped;
// a covered key contributes its fold-averaged prediction. All four spans are parallel.
[[nodiscard]] inline atx::f64 oof_mean_date_ic(std::span<const atx::usize> key_date,
                                               std::span<const atx::f64> label,
                                               std::span<const atx::f64> oof_sum,
                                               std::span<const atx::u32> oof_cnt) {
  ATX_CHECK(key_date.size() == label.size() && label.size() == oof_sum.size() &&
            oof_sum.size() == oof_cnt.size());
  std::vector<atx::usize> d;
  std::vector<atx::f64> p;
  std::vector<atx::f64> l;
  for (atx::usize k = 0; k < key_date.size(); ++k) {
    if (oof_cnt[k] == 0U) {
      continue;
    }
    d.push_back(key_date[k]);
    p.push_back(oof_sum[k] / static_cast<atx::f64>(oof_cnt[k]));
    l.push_back(label[k]);
  }
  return mean_date_ic(std::span<const atx::usize>{d}, std::span<const atx::f64>{p},
                      std::span<const atx::f64>{l});
}

// The deflation trial count a fit reports under `rule` after `n_fold_fits` successful
// fold fits (TrialCountRule, L-08): V1 = the fold-fit count; V2 = one configuration
// (1 when any fold fit happened, else 0).
[[nodiscard]] constexpr atx::usize protocol_trial_count(TrialCountRule rule,
                                                        atx::usize n_fold_fits) noexcept {
  switch (rule) {
  case TrialCountRule::PerFoldFitV1:
    return n_fold_fits;
  case TrialCountRule::PerConfigurationV2:
    return (n_fold_fits > 0U) ? 1U : 0U;
  }
  return n_fold_fits; // unreachable for a valid enum value; conservative (larger N)
}

// Row-explicit PCA fit (the fold-local half of L-03): a basis over exactly `rows`
// (valid rows the caller already chose). k == 0, fewer than two rows, or a pca() error
// -> a disabled basis (k = 0). `fit_upto_date` is recorded on the basis verbatim.
[[nodiscard]] LatentBasis fit_latent_on_rows(const FeatureMatrix &fm,
                                             std::span<const atx::usize> rows, atx::u32 k,
                                             atx::usize fit_upto_date);

// Row-explicit supervised interaction selection (shared by select_interactions and the
// fold-local refit): rank features by |Spearman IC| against Y[label_idx] over the rows
// of `rows` whose Y[label_idx] is finite, keep the top-m (ties -> lower index), and
// emit every crossed pair (a < b) of the chosen set in ascending order. The CALLER
// owns admissibility (maturity / CPCV purge) of `rows`. m == 0, fewer than two
// features, label_idx out of range or fewer than two finite-label rows -> empty.
[[nodiscard]] std::vector<std::pair<atx::u32, atx::u32>>
select_interactions_on_rows(const FeatureMatrix &fm, std::span<const atx::usize> rows,
                            atx::usize label_idx, atx::u32 m);

// The top-m recovered from a selected pair set: the number of distinct features that
// appear in it (select_interactions emits all C(m,2) pairs of its m chosen features).
[[nodiscard]] atx::u32 interaction_top_m(const std::vector<std::pair<atx::u32, atx::u32>> &pairs);

// Append a fitted augmentation to an f64 artifact buffer (PCA k, mean, components,
// then the interaction pairs) — used by LearnFitTrace fold artifacts.
void append_augmentation(const LatentAugmentation &aug, std::vector<atx::f64> &out);

} // namespace detail

// ===========================================================================
//  fit_fold_augmentation — refit an augmentation RECIPE on one fold's train rows
//  (W0-L0, L-03; FoldAugRule::FoldLocalV2).
//
//  `deployed` is the caller's fitted augmentation; only its recipe is read:
//    * PCA      : deployed.pca->k components refit on `train_rows` (valid rows).
//    * selected : interactions (interactions_fixed == false) are re-selected on
//                 `train_rows` against Y[label_idx] with top-m = interaction_top_m.
//    * fixed    : explicit label-free pairs are copied unchanged.
//  Nothing outside `train_rows` is read, so a held-out row's features or labels can
//  never change the result (the LearnFoldLocalAug tests pin this). The caller picks a
//  label channel whose span the fold's CPCV purge already covers.
// ===========================================================================
[[nodiscard]] LatentAugmentation fit_fold_augmentation(const FeatureMatrix &fm,
                                                       const LatentAugmentation &deployed,
                                                       std::span<const atx::usize> train_rows,
                                                       atx::usize label_idx);

// The augmentation one CV fold uses under `rule`: the caller's verbatim (FullWindowV1,
// legacy) or refit on the fold's train rows (FoldLocalV2, fit_fold_augmentation).
[[nodiscard]] inline LatentAugmentation fold_augmentation(const FeatureMatrix &fm,
                                                          const LatentAugmentation &deployed,
                                                          std::span<const atx::usize> train_rows,
                                                          atx::usize label_idx, FoldAugRule rule) {
  switch (rule) {
  case FoldAugRule::FullWindowV1:
    return deployed;
  case FoldAugRule::FoldLocalV2:
    return fit_fold_augmentation(fm, deployed, train_rows, label_idx);
  }
  return fit_fold_augmentation(fm, deployed, train_rows, label_idx); // invalid enum: safe side
}

// The label channel a CV fold at horizon index h uses for supervised augmentation
// selection. The fold's CPCV purge covers the horizon-h label span, so Y[0] is safe
// only when its span nests inside it (horizons[0] <= horizons[h]); otherwise the fold
// selects against its own label Y[h]. PRECONDITION: h < horizons.size().
[[nodiscard]] inline atx::usize fold_selection_label(std::span<const atx::u16> horizons,
                                                     atx::usize h) noexcept {
  return (horizons[0] <= horizons[h]) ? 0U : h;
}

// ===========================================================================
//  fit_latent — fit a PCA basis on the trailing PIT window (date <= t - embargo).
//
//  rows = { r : row_date[r] <= t - embargo AND row_valid[r] }. Gathered into an
//  (|rows| x n_features) MatX and handed to core::linalg::pca(X, k). The result +
//  t + k are frozen into a LatentBasis applied forward by apply_latent.
//
//  Edge cases (handled without UB, k reported as 0 so callers can skip):
//    * k == 0           -> PCA disabled: empty model, k = 0.
//    * t < embargo      -> empty trailing window: empty model, k = 0.
//    * < 2 trailing rows -> pca needs >= 2 samples: empty model, k = 0.
//    * pca() Err        -> (e.g. k > n_features) treated as a disabled basis: k=0.
// ===========================================================================
[[nodiscard]] LatentBasis fit_latent(const FeatureMatrix &fm, atx::usize t,
                                     atx::u16 embargo, atx::u32 k);

// ===========================================================================
//  apply_latent — project the given rows onto a fitted basis -> (|rows| x k).
//
//  Deterministic: the rows are gathered in the order supplied, projected through
//  core::linalg::transform ((X - mean)*components). A disabled basis (k == 0) or
//  an empty row set yields a 0-column / 0-row matrix (no crash).
// ===========================================================================
[[nodiscard]] inline lin::MatX apply_latent(const LatentBasis &b, const FeatureMatrix &fm,
                                            std::span<const atx::usize> rows) {
  if (b.k == 0U || rows.empty()) {
    return lin::MatX(static_cast<Eigen::Index>(rows.size()), static_cast<Eigen::Index>(b.k));
  }
  const lin::MatX X = detail::gather_matrix(fm, rows);
  auto scores = lin::transform(b.model, X);
  // The basis was fit on this FeatureMatrix's feature count, so the dimensions
  // match; transform only Errs on a feature-count mismatch, which cannot happen
  // here. Guard it anyway (the deref below must hold under NDEBUG).
  ATX_CHECK(scores.has_value());
  return std::move(scores).value();
}

// ===========================================================================
//  select_interactions — top-m features by trailing |Spearman IC| vs Y[0], crossed.
//
//  ic[f] = |spearman(X[:,f], Y[0])| over the admissible rows at decision date t. The
//  top-m features by |IC| are chosen, ties broken by ASCENDING feature index (a total
//  order -> deterministic). All crossed pairs (a, b) with a < b among the chosen set
//  are returned, in ascending (a, then b) order.
//
//  Admissible rows (L-02):
//    * MaturedV2 (default): valid rows with a finite Y[0] whose label has MATURED,
//      row_date + label_horizons[0] <= t - embargo (label_matured). A label that is
//      finite in the matrix but realized after t is never read, so data after t can
//      never change the selection. An unannotated matrix (!has_label_horizons())
//      yields no pairs.
//    * FiniteLabelV1 (legacy, leaks): valid rows with date <= t - embargo and a finite
//      Y[0], whatever the label horizon.
//
//  m == 0, an empty window, or < 2 admissible rows -> no pairs (empty).
// ===========================================================================
[[nodiscard]] std::vector<std::pair<atx::u32, atx::u32>>
select_interactions(const FeatureMatrix &fm, atx::usize t, atx::u16 embargo, atx::u32 m,
                    LabelMaturityRule rule = LabelMaturityRule::MaturedV2);

// ===========================================================================
//  interaction_value — the crossed value for one row of a selected pair (a, b),
//  standardized per the trailing window. Provided for S5-3's column materializer;
//  S5-2 only needs select_interactions, but exposing this keeps the contract in
//  one place. mean_a / sd_a (and b) are the trailing-window standardization stats.
// ===========================================================================
[[nodiscard]] inline atx::f64 interaction_value(atx::f64 xa, atx::f64 xb, atx::f64 mean_a,
                                                atx::f64 sd_a, atx::f64 mean_b,
                                                atx::f64 sd_b) noexcept {
  const atx::f64 za = (sd_a == 0.0) ? 0.0 : (xa - mean_a) / sd_a;
  const atx::f64 zb = (sd_b == 0.0) ? 0.0 : (xb - mean_b) / sd_b;
  return za * zb;
}

} // namespace atx::engine::learn

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
// Header-only; every function is defined inline. Fitting is a COLD path (once per
// training window), so std::vector / Eigen allocation is fine.

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
//  LatentAugmentation — the full hidden-feature recipe at a point in time.
//
//  pca          : the latent basis, if PCA is enabled (k > 0).
//  interactions : the selected feature pairs (a < b) to cross, in fixed order.
// ===========================================================================
struct LatentAugmentation {
  std::optional<LatentBasis> pca;
  std::vector<std::pair<atx::u32, atx::u32>> interactions;
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

} // namespace detail

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
//  ic[f] = |spearman(X[:,f], Y[0])| over the trailing valid rows (date <=
//  t - embargo). The top-m features by |IC| are chosen, ties broken by ASCENDING
//  feature index (a total order -> deterministic). All crossed pairs (a, b) with
//  a < b among the chosen set are returned, in ascending (a, then b) order.
//
//  m == 0, an empty trailing window, or < 2 trailing rows -> no pairs (empty).
// ===========================================================================
[[nodiscard]] std::vector<std::pair<atx::u32, atx::u32>>
select_interactions(const FeatureMatrix &fm, atx::usize t, atx::u16 embargo, atx::u32 m);

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

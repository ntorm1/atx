#pragma once

// atx::engine::learn — the walk-forward learned LINEAR alpha (S5-3, §4.3/§0.6).
//
// =====================================================================
//  What this header is
// =====================================================================
//  fit_linear assembles a deflation-gated, firewall-fit, multi-horizon LINEAR
//  learned alpha from a FeatureMatrix + a LatentAugmentation, using the
//  elastic-net CD kernel (or a ridge baseline) inside the S1 CPCV harness:
//
//    * Per HORIZON, walk the CPCV date-folds (expand_date_folds): on each fold
//      fit a FOLD-LOCAL standardization on the TRAIN rows only, fit the
//      standardized train rows (ridge baseline OR elastic_net), then predict OOS
//      on the test rows applying that same fold-local transform + coeff forward
//      (M2 — no test row is ever standardized or scored by a model that saw it),
//      and accumulate the OOS predictions + their labels. Each distinct fit bumps
//      the deflation trial_count (§0.3). The horizon-0 out-of-fold predictions are
//      kept (averaged across folds for a row covered by >1 test fold) and rolled
//      into the per-date OOS skill series stored on the model.
//    * The DEPLOYED per-horizon coefficient is a refit on the FULL trailing
//      window; the base-feature standardization stats are themselves fitted on
//      that window and applied forward (M2). The augmented row (standardized base
//      + latent factor scores + interaction products) is built by the shared
//      build_augmented_row, so train-time and eval-time layouts cannot drift.
//    * Horizon BLEND weights (§0.6) = normalize(max(oos_IC_h, 0)) across horizons
//      — a horizon with non-positive OOS information coefficient gets zero weight;
//      if every horizon is non-positive the blend falls back to uniform (so the
//      model still emits a defined, deflation-testable signal).
//
//  oos_deflated_sharpe scores the model's GENUINE per-date out-of-fold skill
//  series (frozen on the model at fit time from the CPCV out-of-fold predictions),
//  derives its Sharpe and higher moments via eval::stats_ext, and calls
//  eval::deflated_sharpe with N == trial_count — the anti-snooping gate (M3): a
//  no-edge / over-searched model returns DSR <= 0. The series carries no
//  look-ahead, so the gate cannot be fooled by an in-sample fit.
//
// =====================================================================
//  Firewalls (M1/M2/M3)
// =====================================================================
//  M1 determinism: elastic-net is RNG-free cyclic CD; every reduction walks rows
//  in ascending index; the fold walk is the deterministic CPCV order. Same cfg ->
//  byte-identical model AND signal.
//  M2 look-ahead: standardization stats, the latent basis, the coefficients, and
//  the blend weights are ALL trailing-fit and applied forward; predict_at at a
//  date depends only on data the deployed (trailing-fit) model carries, so adding
//  later dates cannot change an earlier prediction.
//  M3 deflation: the GENUINE out-of-fold IC series (each point a fold-local,
//  train-only-fit OOS prediction — no row scored by a model that saw it) is scored
//  through eval::deflated_sharpe with N == the trial_count, so a model that only
//  fit noise is rejected and the gate itself carries no in-sample look-ahead.
//
// Header-only; fitting is a COLD path, so std::vector / Eigen allocation is fine.

#include <optional> // std::optional (m.aug.pca.has_value())
#include <span>     // std::span
#include <vector>   // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include "atx/core/types.hpp" // f64, u16, u32, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/elastic_net.hpp"    // ElasticNetCfg
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, build_augmented_row, predict_blended

namespace atx::engine::learn {

namespace lin = atx::core::linalg;

// ===========================================================================
//  LinearAlphaCfg — the learned-linear-alpha knobs.
//
//  en                : the elastic-net penalty/stopping config (used when
//                      use_ridge_baseline is false).
//  use_ridge_baseline: fit via atx-core ridge instead of elastic_net (the M4
//                      baseline arm). The ridge penalty is en.lambda * n (the
//                      scaling that matches elastic_net's (1/2n) objective).
//  cpcv              : the CPCV fold config for the OOS walk.
//  master_seed       : the deterministic seed root (M1; the linear arm is
//                      RNG-free, carried for ensemble-level reproducibility).
//  horizons          : the forward-return horizons to fit + blend (§0.6).
// ===========================================================================
struct LinearAlphaCfg {
  ElasticNetCfg en;
  bool use_ridge_baseline;
  eval::CpcvConfig cpcv;
  atx::u64 master_seed;
  std::vector<atx::u16> horizons;
};

namespace detail {

// Per-base-feature trailing-fit standardization stats (mean + population std)
// over the given rows of the FeatureMatrix. sd == 0 (constant column) is left at
// 0 so build_augmented_row zeroes that column rather than dividing by zero.
void fit_standardization(const FeatureMatrix &fm, std::span<const atx::usize> rows,
                         std::vector<atx::f64> &mean_out, std::vector<atx::f64> &sd_out);

// Build the (|rows| x aug_dim) augmented design + the (|rows|) label vector for
// one horizon over the given rows, using the shared build_augmented_row so the
// train layout is byte-identical to the eval layout. Rows whose augmented row is
// non-finite or whose label is non-finite are SKIPPED (kept out of the fit).
// `model_shell` carries the (already-fitted) standardization stats + aug + base
// count that define the layout; its coeffs are not read here. When `kept_out` is
// non-null, the FeatureMatrix row index of every written design row is appended to
// it in the SAME order as the design rows — so a caller can map each emitted
// prediction back to its (date, instrument) row (used for the OOF series).
[[nodiscard]] atx::usize
build_design(const FeatureMatrix &fm, const LearnedModel &model_shell,
             std::span<const atx::usize> rows, atx::usize horizon_idx, lin::MatX &X_out,
             lin::VecX &y_out, std::vector<atx::usize> *kept_out = nullptr);

// Fit one coefficient vector on a standardized design (X already augmented). The
// ridge baseline uses atx-core ridge with the n*lambda penalty that matches the
// elastic_net (1/2n) objective; otherwise the elastic-net CD kernel. An empty /
// degenerate design returns a zero coefficient vector of width X.cols().
[[nodiscard]] lin::VecX fit_coeff(const lin::MatX &X, const lin::VecX &y,
                                  const LinearAlphaCfg &cfg);

// NOTE: the Pearson correlation used below is detail::pearson from latent.hpp
// (included transitively) — reused rather than redefined so there is one
// order-fixed, constant-series-to-zero definition in atx::engine::learn::detail.

// Build the per-date OUT-OF-FOLD information-coefficient series from accumulated
// horizon-0 OOF predictions. `oof_sum[r]` / `oof_cnt[r]` are the summed OOF
// prediction and the fold-coverage count for FeatureMatrix row r (cnt == 0 -> the
// row was never an out-of-fold test row, so it contributes nothing); the OOF
// prediction for a covered row is oof_sum[r] / oof_cnt[r] (the average-across-folds
// dedup rule). For each date (ascending; rows are emitted in (date,instrument)
// order so row_date is non-decreasing — a single forward walk, no map, M1), the
// cross-sectional Pearson of (OOF pred, realized horizon-0 label) over the covered,
// finite-label rows is one series point, emitted only when >= 2 such rows exist.
// This is the genuine OOS skill series the deflation gate scores (M3).
[[nodiscard]] std::vector<atx::f64> oof_ic_series(const FeatureMatrix &fm,
                                                  std::span<const atx::f64> oof_sum,
                                                  std::span<const atx::u32> oof_cnt);

} // namespace detail

// ===========================================================================
//  fit_linear — assemble the deployed multi-horizon learned LINEAR alpha.
//
//  See the header: per-horizon CPCV OOS fit (trial_count per fit), a deployed
//  refit on the full trailing window, and §0.6 horizon-blend weights from the
//  OOS IC. The latent basis / interactions in `aug` are taken AS GIVEN (S5-2
//  fit them on the trailing window); fit_linear adds only the standardization
//  stats + coefficients + blend. PURE in (fm, aug, cfg).
// ===========================================================================
[[nodiscard]] LearnedModel fit_linear(const FeatureMatrix &fm, const LatentAugmentation &aug,
                                      const LinearAlphaCfg &cfg);

// ===========================================================================
//  predict_at — the emitted cross-section at a single date (deployed model).
//
//  For every VALID FeatureMatrix row at `date` (in row order), build the
//  augmented row and emit predict_blended. The result is one f64 per emitted
//  in-universe instrument at that date, in row order. Depends only on the
//  deployed (trailing-fit) model -> truncation-invariant (M2).
// ===========================================================================
[[nodiscard]] inline lin::VecX predict_at(const LearnedModel &m, const FeatureMatrix &fm,
                                          atx::usize date) {
  const atx::usize p = fm.n_features;
  const atx::usize adim = m.augmented_dim();
  const atx::usize k = m.aug.pca.has_value() ? static_cast<atx::usize>(m.aug.pca->k) : 0U;
  std::vector<atx::f64> base(p, 0.0);
  std::vector<atx::f64> latent(k, 0.0);
  std::vector<atx::f64> aug(adim, 0.0);
  std::vector<atx::f64> out;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    if (fm.row_date[r] != date || fm.row_valid[r] == 0) {
      continue;
    }
    for (atx::usize f = 0; f < p; ++f) {
      base[f] = fm.X[r * p + f];
    }
    const bool finite = build_augmented_row(m, std::span<const atx::f64>{base},
                                            std::span<atx::f64>{latent}, std::span<atx::f64>{aug});
    out.push_back(finite ? predict_blended(m, std::span<const atx::f64>{aug}) : 0.0);
  }
  lin::VecX v(static_cast<Eigen::Index>(out.size()));
  for (atx::usize i = 0; i < out.size(); ++i) {
    v(static_cast<Eigen::Index>(i)) = out[i];
  }
  return v;
}

// ===========================================================================
//  oos_deflated_sharpe — the anti-snooping gate (M3): DSR of the OOS skill series.
//
//  Scores the model's GENUINE out-of-fold series (m.oos_score_series), assembled
//  during fit_linear from the CPCV out-of-fold predictions (fold-local
//  standardization + fold-local coeffs — NO row was scored by a model that saw it,
//  M2). Computes its Sharpe (mean/std), skewness, excess kurtosis, and length, and
//  runs eval::deflated_sharpe with N == m.trial_count (the single-stream variance
//  estimator). Returns the DSR: a no-edge model returns <= 0; a real-edge model
//  with enough OOS dates and a low-enough trial count returns > 0. A degenerate
//  series (< 2 points, or zero dispersion) returns 0.0. `fm` is retained for the
//  stable two-arg call surface (the series already lives on the model).
// ===========================================================================
[[nodiscard]] atx::f64 oos_deflated_sharpe(const LearnedModel &m, const FeatureMatrix &fm);

} // namespace atx::engine::learn

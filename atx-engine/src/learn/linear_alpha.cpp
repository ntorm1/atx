#include "atx/engine/learn/linear_alpha.hpp"

#include <cmath>    // std::sqrt, std::isfinite
#include <limits>   // std::numeric_limits (trace NaN for uncovered rows)
#include <optional> // std::nullopt (single-stream DSR variance)
#include <utility>  // std::move
#include <span>     // std::span
#include <vector>   // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX/VecX

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp" // f64, u16, u32, usize

#include "atx/core/linalg/linalg.hpp"     // MatX, VecX
#include "atx/core/linalg/regression.hpp" // core::linalg::ridge

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig, eval::cpcv_folds
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, excess_kurtosis, mean_std_pop
#include "atx/engine/learn/elastic_net.hpp"    // elastic_net, ElasticNetCfg
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, build_augmented_row, predict_blended
#include "atx/engine/learn/train.hpp"          // date_label_spans, expand_date_folds, RowFold

namespace atx::engine::learn {

atx::core::Result<DatasetLinearFit> fit_linear_dataset(const PanelDataset& dataset,
    atx::usize begin, atx::usize end, atx::usize asof, atx::u64 max_bytes,
    const LatentAugmentation& aug, const LinearAlphaCfg& cfg, LearnFitTrace* trace) {
  if (cfg.cpcv.rule != eval::CpcvRule::DateV2)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "dataset linear fit requires explicit date-unit CPCV");
  std::vector<atx::u16> endpoints;
  for (auto horizon : dataset.config().holding_horizons)
    endpoints.push_back(static_cast<atx::u16>(horizon + dataset.config().execution_delay));
  if (cfg.horizons != endpoints)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "dataset linear fit horizon/endpoint identity mismatch");
  if (aug.pca && aug.pca->fit_upto_date > asof)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "dataset linear fit cannot use a future-fitted PCA basis");
  ATX_TRY(auto fm, read_dataset_features(dataset, begin, end, asof, max_bytes));
  ATX_TRY(auto model, fit_linear_checked(fm, aug, cfg, trace));
  return atx::core::Ok(DatasetLinearFit{std::move(model), fm.dataset_manifest_sha256, fm.dataset_recipe});
}

namespace detail {

void fit_standardization(const FeatureMatrix &fm, std::span<const atx::usize> rows,
                         std::vector<atx::f64> &mean_out, std::vector<atx::f64> &sd_out) {
  const atx::usize p = fm.n_features;
  mean_out.assign(p, 0.0);
  sd_out.assign(p, 0.0);
  if (rows.empty()) {
    return;
  }
  const atx::f64 nf = static_cast<atx::f64>(rows.size());
  for (atx::usize f = 0; f < p; ++f) {
    atx::f64 sum = 0.0;
    for (const atx::usize r : rows) {
      sum += fm.X[r * p + f];
    }
    const atx::f64 mean = sum / nf;
    atx::f64 sq = 0.0;
    for (const atx::usize r : rows) {
      const atx::f64 d = fm.X[r * p + f] - mean;
      sq += d * d;
    }
    mean_out[f] = mean;
    sd_out[f] = std::sqrt(sq / nf);
  }
}

atx::usize
build_design(const FeatureMatrix &fm, const LearnedModel &model_shell,
             std::span<const atx::usize> rows, atx::usize horizon_idx, lin::MatX &X_out,
             lin::VecX &y_out, std::vector<atx::usize> *kept_out) {
  const atx::usize p = fm.n_features;
  const atx::usize adim = model_shell.augmented_dim();
  const atx::usize k =
      model_shell.aug.pca.has_value() ? static_cast<atx::usize>(model_shell.aug.pca->k) : 0U;
  std::vector<atx::f64> base(p, 0.0);
  std::vector<atx::f64> latent(k, 0.0);
  std::vector<atx::f64> aug(adim, 0.0);
  // First pass: count the usable rows so the matrix is sized once.
  std::vector<atx::usize> usable;
  usable.reserve(rows.size());
  for (const atx::usize r : rows) {
    if (std::isfinite(fm.Y[horizon_idx][r])) {
      usable.push_back(r);
    }
  }
  X_out.resize(static_cast<Eigen::Index>(usable.size()), static_cast<Eigen::Index>(adim));
  y_out.resize(static_cast<Eigen::Index>(usable.size()));
  if (kept_out != nullptr) {
    kept_out->clear();
    kept_out->reserve(usable.size());
  }
  atx::usize w = 0;
  for (const atx::usize r : usable) {
    for (atx::usize f = 0; f < p; ++f) {
      base[f] = fm.X[r * p + f];
    }
    const bool finite = build_augmented_row(model_shell, std::span<const atx::f64>{base},
                                            std::span<atx::f64>{latent}, std::span<atx::f64>{aug});
    if (!finite) {
      continue;
    }
    for (atx::usize j = 0; j < adim; ++j) {
      X_out(static_cast<Eigen::Index>(w), static_cast<Eigen::Index>(j)) = aug[j];
    }
    y_out(static_cast<Eigen::Index>(w)) = fm.Y[horizon_idx][r];
    if (kept_out != nullptr) {
      kept_out->push_back(r);
    }
    ++w;
  }
  // Shrink to the actually-written row count (some rows may have been dropped).
  X_out.conservativeResize(static_cast<Eigen::Index>(w), static_cast<Eigen::Index>(adim));
  y_out.conservativeResize(static_cast<Eigen::Index>(w));
  return w;
}

lin::VecX fit_coeff(const lin::MatX &X, const lin::VecX &y,
                    const LinearAlphaCfg &cfg) {
  if (X.rows() == 0 || X.cols() == 0) {
    return lin::VecX::Zero(X.cols());
  }
  if (cfg.use_ridge_baseline) {
    const atx::f64 ridge_lambda = cfg.en.lambda * static_cast<atx::f64>(X.rows());
    const auto r = atx::core::linalg::ridge(X, y, ridge_lambda);
    if (r.has_value()) {
      return r->beta;
    }
    return lin::VecX::Zero(X.cols()); // singular lambda==0 system -> no signal
  }
  return elastic_net(X, y, cfg.en);
}

std::vector<atx::f64> oof_ic_series(const FeatureMatrix &fm,
                                    std::span<const atx::f64> oof_sum,
                                    std::span<const atx::u32> oof_cnt) {
  std::vector<atx::f64> series;
  const atx::usize nr = fm.n_rows();
  atx::usize r = 0;
  while (r < nr) {
    const atx::usize date = fm.row_date[r];
    std::vector<atx::f64> pv;
    std::vector<atx::f64> lv;
    atx::usize e = r;
    for (; e < nr && fm.row_date[e] == date; ++e) {
      if (oof_cnt[e] == 0U) {
        continue; // not covered out-of-fold at this date
      }
      const atx::f64 label = fm.Y[0][e];
      if (!std::isfinite(label)) {
        continue;
      }
      pv.push_back(oof_sum[e] / static_cast<atx::f64>(oof_cnt[e]));
      lv.push_back(label);
    }
    if (pv.size() >= 2U) {
      series.push_back(pearson(std::span<const atx::f64>{pv}, std::span<const atx::f64>{lv}));
    }
    r = e;
  }
  return series;
}

} // namespace detail

namespace {

// The fold TRAINING artifact of one linear fold fit, flattened for LearnFitTrace:
// fold standardization, fold augmentation, then the coefficient vector.
[[nodiscard]] std::vector<atx::f64> linear_fold_artifact(const LearnedModel &fold_shell,
                                                         const lin::VecX &coeff) {
  std::vector<atx::f64> a;
  a.insert(a.end(), fold_shell.feat_mean.begin(), fold_shell.feat_mean.end());
  a.insert(a.end(), fold_shell.feat_sd.begin(), fold_shell.feat_sd.end());
  detail::append_augmentation(fold_shell.aug, a);
  for (Eigen::Index j = 0; j < coeff.size(); ++j) {
    a.push_back(coeff(j));
  }
  return a;
}

} // namespace

LearnedModel fit_linear(const FeatureMatrix &fm, const LatentAugmentation &aug,
                        const LinearAlphaCfg &cfg) {
  return fit_linear(fm, aug, cfg, nullptr);
}

LearnedModel fit_linear(const FeatureMatrix &fm, const LatentAugmentation &aug,
                        const LinearAlphaCfg &cfg, LearnFitTrace *trace) {
  auto result = fit_linear_checked(fm, aug, cfg, trace);
  ATX_CHECK(result.has_value());
  return std::move(*result);
}

atx::core::Result<LearnedModel> fit_linear_checked(
    const FeatureMatrix &fm, const LatentAugmentation &aug,
    const LinearAlphaCfg &cfg, LearnFitTrace *trace) {
  ATX_TRY_VOID(validate_date_cpcv_inputs(fm, cfg.horizons, cfg.cpcv));
  LearnedModel m;
  std::vector<eval::CpcvMetadata> cpcv_metadata;
  m.kind = ModelKind::Linear;
  m.aug = aug;
  m.n_base_features = static_cast<atx::u32>(fm.n_features);
  m.horizons = cfg.horizons;
  m.trial_count = 0;
  atx::usize n_fold_fits = 0; // successful fold fits (TrialCountRule input, L-08)
  if (trace != nullptr) {
    *trace = LearnFitTrace{}; // a reused trace starts empty
  }

  // The deployed standardization is fit on ALL valid rows (the full trailing
  // window). This is the transform applied forward at predict time (M2).
  std::vector<atx::usize> all_valid;
  for (atx::usize r = 0; r < fm.n_rows(); ++r) {
    if (fm.row_valid[r] != 0) {
      all_valid.push_back(r);
    }
  }
  detail::fit_standardization(fm, std::span<const atx::usize>{all_valid}, m.feat_mean, m.feat_sd);

  m.coeffs.assign(cfg.horizons.size(), {});
  m.blend_w.assign(cfg.horizons.size(), 0.0);
  std::vector<atx::f64> oos_ic(cfg.horizons.size(), 0.0);

  // Horizon-0 out-of-fold prediction accumulators, keyed by FeatureMatrix row.
  // CPCV with n_test_groups > 1 can cover a (date,row) in several test folds; the
  // deterministic dedup rule is to AVERAGE the fold-local OOF predictions for that
  // row (sum / count). Both are sized to n_rows so the keying needs no map (M1).
  std::vector<atx::f64> oof_pred_sum(fm.n_rows(), 0.0);
  std::vector<atx::u32> oof_pred_cnt(fm.n_rows(), 0U);

  for (atx::usize h = 0; h < cfg.horizons.size(); ++h) {
    // CPCV date-folds for this horizon's label span.
    ATX_TRY(auto plan, learn_cpcv_plan(fm, cfg.horizons[h], cfg.cpcv));
    ATX_TRY(auto folds, expand_date_folds_checked(plan.folds, fm, cfg.cpcv));
    if (cfg.cpcv.rule == eval::CpcvRule::DateV2)
      ATX_TRY_VOID(retain_cpcv_metadata(cpcv_metadata, std::move(plan.metadata), cfg.cpcv.max_working_bytes));

    // OOS prediction + label accumulation across folds (for the horizon IC).
    std::vector<atx::f64> oos_pred;
    std::vector<atx::f64> oos_label;
    std::vector<atx::f64> oof_sum_h(fm.n_rows(), 0.0); // horizon-h OOF (MeanDateIcV2)
    std::vector<atx::u32> oof_cnt_h(fm.n_rows(), 0U);
    const atx::usize sel_label =
        fold_selection_label(std::span<const atx::u16>{cfg.horizons}, h);
    atx::usize fold_idx = 0;
    for (const RowFold &f : folds) {
      // Defect-1 firewall: fit a FOLD-LOCAL standardization on the TRAIN rows ONLY
      // and apply it forward to BOTH the train and the OOS test design of this fold,
      // so an OOS test row is never standardized with statistics that saw it (or any
      // out-of-window / future row). The deployed refit below keeps m's full-window
      // stats — that is the legitimate forward-deployment transform.
      LearnedModel fold_shell = m;
      detail::fit_standardization(fm, std::span<const atx::usize>{f.train_rows},
                                  fold_shell.feat_mean, fold_shell.feat_sd);
      // L-03 firewall: the fold's augmentation (PCA basis + selected interactions) is
      // refit on the fold's TRAIN rows only (FoldLocalV2), never copied from the
      // caller's full-window fit that saw this fold's test rows.
      fold_shell.aug = fold_augmentation(fm, aug, std::span<const atx::usize>{f.train_rows},
                                         sel_label, cfg.protocol.fold_aug);
      lin::MatX Xtr;
      lin::VecX ytr;
      const atx::usize ntr = detail::build_design(
          fm, fold_shell, std::span<const atx::usize>{f.train_rows}, h, Xtr, ytr);
      if (ntr == 0U) {
        ++fold_idx;
        continue; // no usable training rows in this fold
      }
      const lin::VecX coeff = detail::fit_coeff(Xtr, ytr, cfg);
      ++n_fold_fits;
      // Predict OOS on the test rows with the FOLD-LOCAL coeff + std (forward only).
      lin::MatX Xte;
      lin::VecX yte;
      std::vector<atx::usize> te_rows;
      const atx::usize nte = detail::build_design(
          fm, fold_shell, std::span<const atx::usize>{f.test_rows}, h, Xte, yte, &te_rows);
      LearnFoldRecord rec;
      for (atx::usize i = 0; i < nte; ++i) {
        atx::f64 pred = 0.0;
        for (Eigen::Index j = 0; j < coeff.size(); ++j) {
          pred += coeff(j) * Xte(static_cast<Eigen::Index>(i), j);
        }
        oos_pred.push_back(pred);
        oos_label.push_back(yte(static_cast<Eigen::Index>(i)));
        oof_sum_h[te_rows[i]] += pred;
        oof_cnt_h[te_rows[i]] += 1U;
        if (h == 0U) {
          // Accumulate the genuine OOF prediction for this row (Defect-2 series).
          oof_pred_sum[te_rows[i]] += pred;
          oof_pred_cnt[te_rows[i]] += 1U;
        }
        if (trace != nullptr) {
          rec.test_keys.push_back(te_rows[i]);
          rec.test_pred.push_back(pred);
        }
      }
      if (trace != nullptr) {
        rec.horizon_idx = h;
        rec.fold_idx = fold_idx;
        rec.artifact = linear_fold_artifact(fold_shell, coeff);
        rec.fit_keys = f.train_rows;
        trace->folds.push_back(std::move(rec));
      }
      ++fold_idx;
    }
    oos_ic[h] = (cfg.protocol.blend_ic == HorizonBlendIc::PooledPearsonV1)
                    ? detail::pearson(std::span<const atx::f64>{oos_pred},
                                      std::span<const atx::f64>{oos_label})
                    : detail::oof_mean_date_ic(std::span<const atx::usize>{fm.row_date},
                                               std::span<const atx::f64>{fm.Y[h]},
                                               std::span<const atx::f64>{oof_sum_h},
                                               std::span<const atx::u32>{oof_cnt_h});

    // The DEPLOYED per-horizon coefficient: refit on the full trailing window with
    // m's full-window standardization (the forward-applied deployment transform).
    lin::MatX Xfull;
    lin::VecX yfull;
    const atx::usize nfull =
        detail::build_design(fm, m, std::span<const atx::usize>{all_valid}, h, Xfull, yfull);
    if (nfull > 0U) {
      m.coeffs[h] = detail::fit_coeff(Xfull, yfull, cfg);
    } else {
      m.coeffs[h] = lin::VecX::Zero(static_cast<Eigen::Index>(m.augmented_dim()));
    }
  }

  // Defect-2: assemble the genuine per-date OOS skill series from the horizon-0
  // out-of-fold predictions (fold-local std + fold-local coeffs above), in
  // ascending date order, and freeze it for the deflation gate (M3).
  m.oos_score_series =
      detail::oof_ic_series(fm, std::span<const atx::f64>{oof_pred_sum},
                            std::span<const atx::u32>{oof_pred_cnt});
  m.trial_count = detail::protocol_trial_count(cfg.protocol.trials, n_fold_fits);
  if (trace != nullptr) {
    trace->oof_cnt = oof_pred_cnt;
    trace->oof_pred.assign(fm.n_rows(), std::numeric_limits<atx::f64>::quiet_NaN());
    for (atx::usize r = 0; r < fm.n_rows(); ++r) {
      if (oof_pred_cnt[r] > 0U) {
        trace->oof_pred[r] = oof_pred_sum[r] / static_cast<atx::f64>(oof_pred_cnt[r]);
      }
    }
  }

  // §0.6 horizon blend: normalize(max(oos_IC_h, 0)). All non-positive -> uniform.
  atx::f64 sum = 0.0;
  for (atx::usize h = 0; h < cfg.horizons.size(); ++h) {
    const atx::f64 w = (oos_ic[h] > 0.0) ? oos_ic[h] : 0.0;
    m.blend_w[h] = w;
    sum += w;
  }
  if (sum > 0.0) {
    for (atx::f64 &w : m.blend_w) {
      w /= sum;
    }
  } else {
    const atx::f64 u = (cfg.horizons.empty()) ? 0.0 : 1.0 / static_cast<atx::f64>(cfg.horizons.size());
    for (atx::f64 &w : m.blend_w) {
      w = u;
    }
  }
  m.cpcv_metadata = std::move(cpcv_metadata);
  return atx::core::Ok(std::move(m));
}

atx::f64 oos_deflated_sharpe(const LearnedModel &m, const FeatureMatrix &fm) {
  (void)fm; // the OOS series is frozen on the model at fit time (no re-prediction)
  const std::vector<atx::f64> &series = m.oos_score_series;
  if (series.size() < 2U) {
    return 0.0;
  }
  const eval::MeanStd ms = eval::mean_std_pop(std::span<const atx::f64>{series});
  if (ms.std == 0.0) {
    return 0.0; // no dispersion -> Sharpe undefined; treat as no edge
  }
  const atx::f64 sr = ms.mean / ms.std; // per-period Sharpe of the IC series
  const atx::f64 skew = eval::skewness(std::span<const atx::f64>{series});
  const atx::f64 exkurt = eval::excess_kurtosis(std::span<const atx::f64>{series});
  const atx::usize T = series.size();
  const atx::usize N = (m.trial_count == 0U) ? 1U : m.trial_count;
  const eval::DsrResult dsr = eval::deflated_sharpe(sr, T, skew, exkurt, N, std::nullopt);
  return dsr.dsr;
}

} // namespace atx::engine::learn

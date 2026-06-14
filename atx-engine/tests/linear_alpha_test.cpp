// atx::engine::learn — walk-forward learned LINEAR alpha tests (S5-3).
//
// Suite LinearAlpha — the load-bearing semantics of the deflation-gated,
// firewall-fit, multi-horizon linear learned alpha + its ISignalSource adapter:
//
//   1. NoEdgePanel_RejectedByDeflation (M3) — a pure-noise fixture (labels
//      independent of features) yields oos_deflated_sharpe <= 0.
//   2. RealSignalPanel_PositiveDeflatedSharpe (M3) — a planted-signal fixture
//      (y = 0.3*f0 + noise) yields oos_deflated_sharpe > 0. (1) and (2) use the
//      SAME cfg/bar, so the contrast is non-vacuous: same gate, edge survives,
//      noise dies.
//   3. FitOnTrailing_OosSeriesIsGenuinelyOutOfFold (M2/M3) — the model's stored
//      oos_score_series is byte-identical to an independent GENUINE out-of-fold
//      reconstruction (fold-local standardization + fold-local coeffs), and a
//      single fold's OOF prediction is invariant to truncating the matrix to that
//      fold's [train ∪ test] rows (a global-standardization leak would shift it),
//      plus predict_at(date 12) is run-to-run deterministic.
//   4. SameSeed_ByteIdenticalModelAndSignal (M1) — two fits with the same cfg
//      produce an identical model hash AND identical predict_at signal.
//   5. HorizonBlend_WeightsNonNegative_SumToOne (§0.6) — blend_w all >= 0, sum 1.
//   6. EmitsAsISignalSource_CrossSectionLength (M6) — LearnedSignalSource.evaluate
//      over a PanelView returns a SignalView with values.size() == n_instruments.
//
// Fixtures are FeatureMatrix aggregates built directly (the dense public vectors
// suffice; build_features is exercised elsewhere). Naming:
// Subject_Condition_ExpectedResult.

#include <algorithm> // std::sort, std::find (fold row remap in the firewall test)
#include <cmath>     // std::isfinite
#include <cstdint>   // std::uint64_t (PanelView backing arrays)
#include <limits>    // std::numeric_limits (tail NaN label)
#include <span>      // std::span (PanelView universe view)
#include <vector>

#include <Eigen/Dense> // Eigen::Index, VecX hashing

#include <gtest/gtest.h>

#include "atx/core/hash.hpp"          // atx::core::hash_bytes
#include "atx/core/linalg/linalg.hpp" // VecX
#include "atx/core/types.hpp"         // f64, u16, u64, usize

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/elastic_net.hpp"    // ElasticNetCfg
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix, FeatureSpec
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, LearnedSignalSource
#include "atx/engine/learn/linear_alpha.hpp"   // fit_linear, predict_at, oos_deflated_sharpe
#include "atx/engine/learn/train.hpp"          // date_label_spans, expand_date_folds
#include "atx/engine/loop/panel_types.hpp"     // PanelView
#include "atx/engine/loop/types.hpp"           // InstrumentId

namespace atxtest_linear_alpha_test {

using atx::f64;
using atx::u16;
using atx::u64;
using atx::usize;
using atx::engine::learn::FeatureMatrix;
using atx::engine::learn::FeatureSpec;
using atx::engine::learn::LatentAugmentation;
using atx::engine::learn::LearnedModel;
using atx::engine::learn::LinearAlphaCfg;
namespace eval = atx::engine::eval;

// A small deterministic LCG so fixtures carry reproducible "noise" without an RNG
// dependency; pure function of its state (test-local).
struct Lcg {
  u64 s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    // Top 53 bits -> [0,1), then map to [-1, 1).
    const u64 top = s >> 11U;
    const f64 u = static_cast<f64>(top) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// Build a single-horizon-friendly FeatureMatrix with `n_dates` dates and
// `n_inst` instruments per date (all in-universe / valid). Feature 0 carries the
// planted signal weight `signal_w` into the horizon-h label (here h is the
// horizon index 0 with the §0.6 default horizons collapsed); the remaining
// features are noise. Labels for horizon index hi are y = signal_w*f0 + noise,
// with NaN at the unknowable tail (date + horizon >= n_dates).
[[nodiscard]] FeatureMatrix make_panel(usize n_dates, usize n_inst, usize n_features,
                                       const std::vector<u16> &horizons, f64 signal_w, u64 seed) {
  FeatureMatrix fm;
  fm.n_dates = n_dates;
  fm.n_instruments = n_inst;
  fm.n_features = n_features;
  fm.Y.assign(horizons.size(), {});
  Lcg rng{seed};
  // We need the planted f0 per (date,inst) to build a label that genuinely
  // correlates with it; store rows then compute forward-style labels from f0.
  std::vector<f64> f0_of_row;
  for (usize d = 0; d < n_dates; ++d) {
    for (usize i = 0; i < n_inst; ++i) {
      const usize row = fm.push_row(d, i);
      (void)row;
      f0_of_row.push_back(rng.next());
      for (usize f = 0; f < n_features; ++f) {
        fm.X.push_back((f == 0U) ? f0_of_row.back() : rng.next());
      }
      fm.row_valid.push_back(1U);
    }
  }
  // Labels: per horizon, y = signal_w*f0 + noise; NaN where date+H >= n_dates.
  for (usize hi = 0; hi < horizons.size(); ++hi) {
    Lcg lrng{seed ^ (0x1234ULL + hi)};
    for (usize r = 0; r < fm.n_rows(); ++r) {
      const usize d = fm.row_date[r];
      const usize ahead = d + static_cast<usize>(horizons[hi]);
      if (ahead >= n_dates) {
        fm.Y[hi].push_back(std::numeric_limits<f64>::quiet_NaN());
      } else {
        fm.Y[hi].push_back(signal_w * f0_of_row[r] + 0.15 * lrng.next());
      }
    }
  }
  return fm;
}

// Hash a LearnedModel's fitted parameters into one digest by serializing every
// fitted f64 (coeffs, blend, standardization) plus the trial_count into a flat
// byte buffer and hashing it. Non-vacuous: any change to a fitted coefficient
// changes the buffer and thus the digest.
[[nodiscard]] u64 hash_model(const LearnedModel &m) {
  std::vector<f64> buf;
  for (const auto &c : m.coeffs) {
    for (Eigen::Index j = 0; j < c.size(); ++j) {
      buf.push_back(c(j));
    }
  }
  buf.insert(buf.end(), m.blend_w.begin(), m.blend_w.end());
  buf.insert(buf.end(), m.feat_mean.begin(), m.feat_mean.end());
  buf.insert(buf.end(), m.feat_sd.begin(), m.feat_sd.end());
  // The genuine OOF skill series is a fitted output too — pin it into the digest
  // so the M1 determinism test proves it is byte-identical across builds.
  buf.insert(buf.end(), m.oos_score_series.begin(), m.oos_score_series.end());
  buf.push_back(static_cast<f64>(m.trial_count));
  // SAFETY: std::vector<f64> stores doubles contiguously; buf.data() points at
  // buf.size()*sizeof(f64) live bytes for the call's duration.
  return atx::core::hash_bytes(buf.data(), buf.size() * sizeof(f64));
}

[[nodiscard]] u64 hash_signal(const atx::core::linalg::VecX &v) {
  // SAFETY: Eigen VecX stores doubles contiguously; v.data() points at
  // size()*sizeof(f64) live bytes for the vector's lifetime.
  return atx::core::hash_bytes(v.data(), static_cast<usize>(v.size()) * sizeof(f64));
}

// A standard cfg used by BOTH the noise and signal M3 tests (same bar).
[[nodiscard]] LinearAlphaCfg standard_cfg() {
  LinearAlphaCfg cfg;
  cfg.en = atx::engine::learn::ElasticNetCfg{/*lambda=*/0.02, /*alpha=*/0.5, /*max_iter=*/2000,
                                             /*tol=*/1e-9};
  cfg.use_ridge_baseline = false;
  cfg.cpcv = eval::CpcvConfig{/*n_groups=*/5, /*n_test_groups=*/1, /*embargo=*/0.0};
  cfg.master_seed = 42ULL;
  cfg.horizons = {1};
  return cfg;
}

// ---- 1 + 2: M3 deflation contrast (same bar) ---------------------------------

TEST(LinearAlpha, NoEdgePanel_RejectedByDeflation) {
  const FeatureMatrix fm = make_panel(/*n_dates=*/40, /*n_inst=*/12, /*n_features=*/4,
                                      /*horizons=*/{1}, /*signal_w=*/0.0, /*seed=*/7ULL);
  const LatentAugmentation aug; // no latent / interactions for the bare contrast
  const LearnedModel m = atx::engine::learn::fit_linear(fm, aug, standard_cfg());
  const f64 dsr = atx::engine::learn::oos_deflated_sharpe(m, fm);
  EXPECT_LE(dsr, 0.0) << "a no-edge model must not survive deflation";
}

TEST(LinearAlpha, RealSignalPanel_PositiveDeflatedSharpe) {
  const FeatureMatrix fm = make_panel(/*n_dates=*/40, /*n_inst=*/12, /*n_features=*/4,
                                      /*horizons=*/{1}, /*signal_w=*/0.3, /*seed=*/7ULL);
  const LatentAugmentation aug;
  const LearnedModel m = atx::engine::learn::fit_linear(fm, aug, standard_cfg());
  const f64 dsr = atx::engine::learn::oos_deflated_sharpe(m, fm);
  EXPECT_GT(dsr, 0.0) << "a real-edge model must survive the SAME deflation bar";
}

// ---- 3: M2/M3 fitting-firewall (the OOS gate is genuinely out-of-fold) --------

// Keep only the FeatureMatrix rows whose row index is in `keep` (ascending),
// preserving (date,inst) provenance, features, validity, and labels VERBATIM
// (no relabeling — we are slicing the SAME observations, not re-windowing). Used
// to prove a fold-local OOF prediction depends ONLY on its train+test rows.
[[nodiscard]] FeatureMatrix keep_rows(const FeatureMatrix &fm, const std::vector<usize> &keep) {
  FeatureMatrix out;
  out.n_dates = fm.n_dates;
  out.n_instruments = fm.n_instruments;
  out.n_features = fm.n_features;
  out.Y.assign(fm.Y.size(), {});
  for (const usize r : keep) {
    out.push_row(fm.row_date[r], fm.row_inst[r]);
    for (usize f = 0; f < fm.n_features; ++f) {
      out.X.push_back(fm.X[r * fm.n_features + f]);
    }
    out.row_valid.push_back(fm.row_valid[r]);
    for (usize hi = 0; hi < fm.Y.size(); ++hi) {
      out.Y[hi].push_back(fm.Y[hi][r]);
    }
  }
  return out;
}

// Recompute the genuine out-of-fold horizon-0 prediction for one (date,inst) row
// of one CPCV fold, with FOLD-LOCAL standardization fit on the TRAIN rows only and
// the elastic-net coeff fit on those standardized train rows — the firewalled
// reference. `train_rows`/`test_rows` are row indices into `src`. Returns the OOF
// prediction for the FIRST emitted test row (sufficient to pin the firewall).
[[nodiscard]] f64 firewalled_first_oof(const FeatureMatrix &src,
                                       const std::vector<usize> &train_rows,
                                       const std::vector<usize> &test_rows,
                                       const LearnedModel &shell, const LinearAlphaCfg &cfg) {
  namespace d = atx::engine::learn::detail;
  LearnedModel fs = shell;
  d::fit_standardization(src, std::span<const usize>{train_rows}, fs.feat_mean, fs.feat_sd);
  atx::core::linalg::MatX xtr;
  atx::core::linalg::VecX ytr;
  const usize ntr = d::build_design(src, fs, std::span<const usize>{train_rows}, 0U, xtr, ytr);
  EXPECT_GT(ntr, 0U);
  const atx::core::linalg::VecX coeff = d::fit_coeff(xtr, ytr, cfg);
  atx::core::linalg::MatX xte;
  atx::core::linalg::VecX yte;
  const usize nte = d::build_design(src, fs, std::span<const usize>{test_rows}, 0U, xte, yte);
  EXPECT_GT(nte, 0U);
  f64 pred = 0.0;
  for (Eigen::Index j = 0; j < coeff.size(); ++j) {
    pred += coeff(j) * xte(0, j);
  }
  return pred;
}

// Build a "model shell" carrying only the layout (aug + base count) that
// build_augmented_row needs; its standardization is overwritten per fold.
[[nodiscard]] LearnedModel layout_shell(const FeatureMatrix &fm, const LatentAugmentation &aug) {
  LearnedModel s;
  s.kind = atx::engine::learn::ModelKind::Linear;
  s.aug = aug;
  s.n_base_features = static_cast<atx::u32>(fm.n_features);
  s.feat_mean.assign(fm.n_features, 0.0);
  s.feat_sd.assign(fm.n_features, 0.0);
  return s;
}

// THE firewall test. It pins TWO properties that a correct fit MUST satisfy and
// that BOTH fail under the adopted code's pre-fix defects:
//
//   (A) Defect-2 fixed — the model's stored oos_score_series is the GENUINE
//       out-of-fold IC series: byte-identical to an independent fold-local
//       reconstruction (fold-local standardization + fold-local coeffs). Under the
//       old code the series did not exist (the gate re-predicted IN-SAMPLE).
//   (B) Defect-1 fixed — a single fold's out-of-fold test prediction depends ONLY
//       on that fold's [train ∪ test] rows: truncating the FeatureMatrix to those
//       rows leaves the OOF prediction byte-identical. Under a GLOBAL (all-dates)
//       standardization the test row would be standardized with stats that saw
//       rows outside the fold (and future dates), so truncation WOULD shift it.
//
// (C) the original per-date predict_at determinism assertion is retained.
TEST(LinearAlpha, FitOnTrailing_OosSeriesIsGenuinelyOutOfFold) {
  const FeatureMatrix fm = make_panel(/*n_dates=*/40, /*n_inst=*/12, /*n_features=*/4,
                                      /*horizons=*/{1}, /*signal_w=*/0.3, /*seed=*/11ULL);
  const LatentAugmentation aug;
  const LinearAlphaCfg cfg = standard_cfg();
  const LearnedModel m = atx::engine::learn::fit_linear(fm, aug, cfg);

  // Reconstruct the genuine OOF series independently, mirroring the firewall:
  // per fold fit a TRAIN-ONLY standardization + coeff, predict the test rows, and
  // accumulate horizon-0 OOF predictions by row (average across folds), then build
  // the per-date IC series with the same helper the trainer uses.
  namespace d = atx::engine::learn::detail;
  const std::vector<eval::LabelSpan> spans = atx::engine::learn::date_label_spans(fm, cfg.horizons[0]);
  const std::vector<eval::CpcvFold> dfolds =
      eval::cpcv_folds(std::span<const eval::LabelSpan>{spans}, cfg.cpcv);
  const auto folds = atx::engine::learn::expand_date_folds(dfolds, fm);
  ASSERT_GE(folds.size(), 2U); // a non-trivial CPCV partition

  const LearnedModel shell = layout_shell(fm, aug);
  std::vector<f64> oof_sum(fm.n_rows(), 0.0);
  std::vector<atx::u32> oof_cnt(fm.n_rows(), 0U);
  for (const auto &f : folds) {
    LearnedModel fs = shell;
    d::fit_standardization(fm, std::span<const usize>{f.train_rows}, fs.feat_mean, fs.feat_sd);
    atx::core::linalg::MatX xtr;
    atx::core::linalg::VecX ytr;
    const usize ntr = d::build_design(fm, fs, std::span<const usize>{f.train_rows}, 0U, xtr, ytr);
    if (ntr == 0U) {
      continue;
    }
    const atx::core::linalg::VecX coeff = d::fit_coeff(xtr, ytr, cfg);
    atx::core::linalg::MatX xte;
    atx::core::linalg::VecX yte;
    std::vector<usize> te_rows;
    const usize nte =
        d::build_design(fm, fs, std::span<const usize>{f.test_rows}, 0U, xte, yte, &te_rows);
    for (usize i = 0; i < nte; ++i) {
      f64 pred = 0.0;
      for (Eigen::Index j = 0; j < coeff.size(); ++j) {
        pred += coeff(j) * xte(static_cast<Eigen::Index>(i), j);
      }
      oof_sum[te_rows[i]] += pred;
      oof_cnt[te_rows[i]] += 1U;
    }
  }
  const std::vector<f64> ref =
      d::oof_ic_series(fm, std::span<const f64>{oof_sum}, std::span<const atx::u32>{oof_cnt});

  // (A) the stored OOS series is exactly this genuine OOF reconstruction.
  ASSERT_FALSE(m.oos_score_series.empty()) << "the OOS gate must have a non-empty OOF series";
  ASSERT_EQ(m.oos_score_series.size(), ref.size());
  for (usize i = 0; i < ref.size(); ++i) {
    EXPECT_EQ(m.oos_score_series[i], ref[i]) << "OOS series point " << i << " is not the OOF value";
  }

  // (B) a single fold's first OOF prediction depends ONLY on [train ∪ test] rows.
  // Pick the first fold with usable train+test rows; truncate the matrix to its
  // train+test rows and assert the OOF prediction is byte-identical. A GLOBAL-stats
  // fit (Defect-1) would shift it, because rows outside the fold feed the transform.
  bool checked_truncation = false;
  for (const auto &f : folds) {
    if (f.train_rows.empty() || f.test_rows.empty()) {
      continue;
    }
    std::vector<usize> keep = f.train_rows;
    keep.insert(keep.end(), f.test_rows.begin(), f.test_rows.end());
    std::sort(keep.begin(), keep.end());
    const FeatureMatrix sub = keep_rows(fm, keep);
    // Remap the fold's train/test row indices into the truncated matrix.
    std::vector<usize> tr_sub;
    std::vector<usize> te_sub;
    for (usize r = 0; r < sub.n_rows(); ++r) {
      const bool is_test =
          std::find(f.test_rows.begin(), f.test_rows.end(),
                    fm.row_of(sub.row_date[r], sub.row_inst[r])) != f.test_rows.end();
      (is_test ? te_sub : tr_sub).push_back(r);
    }
    const f64 full_pred = firewalled_first_oof(fm, f.train_rows, f.test_rows, shell, cfg);
    const f64 sub_pred = firewalled_first_oof(sub, tr_sub, te_sub, layout_shell(sub, aug), cfg);
    EXPECT_EQ(full_pred, sub_pred)
        << "a fold's OOF prediction must not depend on rows outside [train ∪ test]";
    checked_truncation = true;
    break;
  }
  EXPECT_TRUE(checked_truncation) << "no fold had both train and test rows to truncation-test";

  // (C) predict_at at one date is run-to-run deterministic on the fitted model.
  const LearnedModel m2 = atx::engine::learn::fit_linear(fm, aug, cfg);
  const auto p1 = atx::engine::learn::predict_at(m, fm, 12U);
  const auto p2 = atx::engine::learn::predict_at(m2, fm, 12U);
  ASSERT_EQ(p1.size(), p2.size());
  EXPECT_EQ(hash_signal(p1), hash_signal(p2));
}

// ---- 4: M1 determinism (model + signal) -------------------------------------

TEST(LinearAlpha, SameSeed_ByteIdenticalModelAndSignal) {
  const FeatureMatrix fm = make_panel(/*n_dates=*/30, /*n_inst=*/8, /*n_features=*/4,
                                      /*horizons=*/{1, 5}, /*signal_w=*/0.25, /*seed=*/99ULL);
  const LatentAugmentation aug;
  LinearAlphaCfg cfg = standard_cfg();
  cfg.horizons = {1, 5};

  const LearnedModel m1 = atx::engine::learn::fit_linear(fm, aug, cfg);
  const LearnedModel m2 = atx::engine::learn::fit_linear(fm, aug, cfg);
  EXPECT_EQ(hash_model(m1), hash_model(m2));

  const auto s1 = atx::engine::learn::predict_at(m1, fm, 10U);
  const auto s2 = atx::engine::learn::predict_at(m2, fm, 10U);
  ASSERT_EQ(s1.size(), s2.size());
  EXPECT_EQ(hash_signal(s1), hash_signal(s2));
}

// ---- 5: §0.6 horizon-blend weights ------------------------------------------

TEST(LinearAlpha, HorizonBlend_WeightsNonNegative_SumToOne) {
  const FeatureMatrix fm = make_panel(/*n_dates=*/36, /*n_inst=*/10, /*n_features=*/3,
                                      /*horizons=*/{1, 5, 21}, /*signal_w=*/0.3, /*seed=*/5ULL);
  const LatentAugmentation aug;
  LinearAlphaCfg cfg = standard_cfg();
  cfg.horizons = {1, 5, 21};
  const LearnedModel m = atx::engine::learn::fit_linear(fm, aug, cfg);

  ASSERT_EQ(m.blend_w.size(), 3U);
  f64 sum = 0.0;
  for (const f64 w : m.blend_w) {
    EXPECT_GE(w, 0.0);
    sum += w;
  }
  EXPECT_NEAR(sum, 1.0, 1e-12);
}

// ---- 6: M6 ISignalSource cross-section emission ------------------------------

TEST(LinearAlpha, EmitsAsISignalSource_CrossSectionLength) {
  // A raw-field-only spec (close, volume) so the PanelView adapter can rebuild the
  // base row from the panel; 2 base features.
  const usize n_inst = 6U;
  const FeatureMatrix fm = make_panel(/*n_dates=*/20, /*n_inst=*/n_inst, /*n_features=*/2,
                                      /*horizons=*/{1}, /*signal_w=*/0.3, /*seed=*/3ULL);
  const LatentAugmentation aug;
  const LearnedModel m = atx::engine::learn::fit_linear(fm, aug, standard_cfg());

  FeatureSpec spec;
  spec.raw_fields = {"close", "volume"};
  spec.max_lookback = 0U;
  atx::engine::learn::LearnedSignalSource src{m, spec, n_inst};

  // Build a minimal PanelView directly over backing arrays: 1 valid row, n_inst
  // instruments, all present. fields layout is [field][cap][n_inst] (cap=1).
  const usize cap = 1U;
  const usize n_fields = 5U; // kPanelFieldCount
  std::vector<f64> fields(n_fields * cap * n_inst, 0.0);
  // close == field 3, volume == field 4. Give each instrument a distinct value.
  for (usize i = 0; i < n_inst; ++i) {
    fields[3U * cap * n_inst + i] = 100.0 + static_cast<f64>(i); // close
    fields[4U * cap * n_inst + i] = 1000.0 + static_cast<f64>(i); // volume
  }
  const usize mask_words = 1U; // n_inst <= 64
  std::vector<std::uint64_t> mask(cap * mask_words, 0ULL);
  for (usize i = 0; i < n_inst; ++i) {
    mask[0] |= (1ULL << i); // all present at the single row
  }
  std::vector<atx::engine::InstrumentId> universe(n_inst);
  for (usize i = 0; i < n_inst; ++i) {
    universe[i].id = static_cast<atx::u32>(i);
  }
  atx::engine::PanelView panel{fields.data(),
                               mask.data(),
                               std::span<const atx::engine::InstrumentId>{universe},
                               /*cap=*/cap,
                               /*head=*/0U,
                               /*valid_rows=*/1U,
                               /*mask_words=*/mask_words};

  const auto r = src.evaluate(panel);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->values.size(), n_inst);
  // At least one instrument got a finite (non-"no opinion") score.
  bool any_finite = false;
  for (const f64 v : r->values) {
    any_finite = any_finite || std::isfinite(v);
  }
  EXPECT_TRUE(any_finite);
}


}  // namespace atxtest_linear_alpha_test

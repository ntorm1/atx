// W0-L0 — held-out label mutation invariance (plan v2 §7 W0-L0; findings L-01, L-03, L-07).
//
// Acceptance items proved here:
//   (A1) Mutating a CPCV fold's TEST labels leaves that fold's out-of-fold predictions
//        byte-identical.
//   (A2) The same held-out-label perturbation leaves that fold's TRAINING ARTIFACT
//        (ensemble member states / fold standardization + augmentation + coefficients
//        / forest nodes) byte-identical.
// Every invariance test is paired with the LEGACY rule (V1) on the same fixture and
// mutation, asserting that the artifact DOES change there — so the test is not vacuous
// and documents the pre-fix leak (L-01: the test fold was the checkpoint-selection set;
// L-03: the caller's full-window augmentation, fit with the held-out labels, reached
// every fold).
//
// LearnLabelMutationInvariance_Deploy pins L-07: the deployed ensemble selects its
// checkpoint on the inner validation block, not on training loss.

#include <cmath>   // std::fabs
#include <cstring> // std::memcmp
#include <iostream> // measured-number lines for the lane report
#include <limits>  // std::numeric_limits
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/engine/learn/feature_matrix.hpp"    // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"               // fit_gbt, GbtCfg
#include "atx/engine/learn/latent.hpp"            // select_interactions, LearnFitTrace
#include "atx/engine/learn/linear_alpha.hpp"      // fit_linear, LinearAlphaCfg
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor
#include "atx/engine/learn/tcn_alpha.hpp"         // fit_tcn, fit_gru, cfgs, SeqFitProtocol

namespace atx_test_w0_l0_label_mutation {

using atx::f64;
using atx::u16;
using atx::u64;
using atx::u8;
using atx::usize;
namespace learn = atx::engine::learn;

// ---------------------------------------------------------------------------
//  Shared helpers
// ---------------------------------------------------------------------------

[[nodiscard]] bool bytes_equal(const std::vector<f64> &a, const std::vector<f64> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)) == 0);
}

[[nodiscard]] const learn::LearnFoldRecord *find_rec(const learn::LearnFitTrace &t, usize h,
                                                     usize fold) {
  for (const learn::LearnFoldRecord &r : t.folds) {
    if (r.horizon_idx == h && r.fold_idx == fold) {
      return &r;
    }
  }
  return nullptr;
}

// Sequence fixture: n_dates x n_inst anchors with a full L-deep window; the label is a
// clean function of the trailing step's feature 0 (a learnable signal).
[[nodiscard]] learn::SequenceTensor make_seq(usize n_dates, usize n_inst, usize L, usize F,
                                             usize n_h, u64 seed) {
  learn::SequenceTensor st;
  st.lookback = L;
  st.n_features = F;
  st.y.assign(n_h, {});
  atx::core::Xoshiro256pp rng{seed};
  for (usize d = L - 1U; d < n_dates; ++d) {
    for (usize i = 0; i < n_inst; ++i) {
      std::vector<f64> window(L * F, 0.0);
      for (usize l = 0; l < L; ++l) {
        const usize wd = d - (L - 1U) + l;
        for (usize f = 0; f < F; ++f) {
          window[l * F + f] = 0.1 * static_cast<f64>(wd) + 0.3 * static_cast<f64>(i) +
                              0.05 * static_cast<f64>(f) + 0.2 * rng.normal();
        }
      }
      const f64 trailing0 = window[(L - 1U) * F];
      for (usize h = 0; h < n_h; ++h) {
        st.y[h].push_back(trailing0 + 0.01 * static_cast<f64>(h));
      }
      st.x.insert(st.x.end(), window.begin(), window.end());
      st.date_of.push_back(d);
      st.inst_of.push_back(i);
      st.sample_valid.push_back(static_cast<u8>(1));
      ++st.n_samples;
    }
  }
  return st;
}

template <typename Cfg> void tiny_common(Cfg &cfg) {
  cfg.dropout = 0.0;
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1, 2};
  cfg.train.epochs = 12;
  cfg.train.batch_size = 16;
  cfg.train.ckpt_every = 4;
  cfg.train.ensemble_size = 2;
  cfg.train.master_seed = 4242;
}

[[nodiscard]] learn::TcnAlphaCfg tiny_tcn() {
  learn::TcnAlphaCfg cfg;
  cfg.blocks = 2;
  cfg.kernel = 2;
  cfg.channels = 6;
  tiny_common(cfg);
  return cfg;
}

[[nodiscard]] learn::GruAlphaCfg tiny_gru() {
  learn::GruAlphaCfg cfg;
  cfg.hidden = 6;
  tiny_common(cfg);
  return cfg;
}

// Mutate every horizon's label of the given samples (a large affine flip, so a
// checkpoint chosen on these labels prefers the untrained initial state).
[[nodiscard]] learn::SequenceTensor mutate_seq_labels(const learn::SequenceTensor &st,
                                                      const std::vector<usize> &samples) {
  learn::SequenceTensor out = st;
  for (std::vector<f64> &yh : out.y) {
    for (const usize s : samples) {
      yh[s] = -4.0 * yh[s] + 1.5;
    }
  }
  return out;
}

// Result of one fit + mutate + refit round for fold `fold` (both horizons checked).
struct SeqRound {
  bool fold_recorded = false;
  bool preds_identical = true;
  bool artifacts_identical = true;
  bool oof_identical = true;
  usize n_test = 0;
  usize artifact_len = 0;
  f64 max_pred_delta = 0.0; // max |pred_after - pred_before| over the fold's test keys
};

// Max absolute element difference of two equal-length vectors (0 when sizes differ).
[[nodiscard]] f64 max_abs_delta(const std::vector<f64> &a, const std::vector<f64> &b) {
  f64 m = 0.0;
  if (a.size() != b.size()) {
    return m;
  }
  for (usize i = 0; i < a.size(); ++i) {
    const f64 d = std::fabs(a[i] - b[i]);
    m = (d > m) ? d : m;
  }
  return m;
}

template <typename Cfg, typename FitFn>
[[nodiscard]] SeqRound seq_round(const learn::SequenceTensor &st, const Cfg &cfg, FitFn fit,
                                 usize fold) {
  SeqRound out;
  learn::LearnFitTrace t0;
  const auto m0 = fit(st, cfg, &t0);
  EXPECT_TRUE(m0.has_value());
  const learn::LearnFoldRecord *r0 = find_rec(t0, 0U, fold);
  if (r0 == nullptr) {
    return out;
  }
  out.fold_recorded = true;
  out.n_test = r0->test_keys.size();
  const learn::SequenceTensor st_mut = mutate_seq_labels(st, r0->test_keys);
  learn::LearnFitTrace t1;
  const auto m1 = fit(st_mut, cfg, &t1);
  EXPECT_TRUE(m1.has_value());
  for (usize h = 0; h < cfg.horizons.size(); ++h) {
    const learn::LearnFoldRecord *a = find_rec(t0, h, fold);
    const learn::LearnFoldRecord *b = find_rec(t1, h, fold);
    if (a == nullptr || b == nullptr || a->test_keys != b->test_keys) {
      out.fold_recorded = false;
      return out;
    }
    out.preds_identical = out.preds_identical && bytes_equal(a->test_pred, b->test_pred);
    out.artifacts_identical = out.artifacts_identical && bytes_equal(a->artifact, b->artifact);
    out.artifact_len += a->artifact.size();
    const f64 d = max_abs_delta(a->test_pred, b->test_pred);
    out.max_pred_delta = (d > out.max_pred_delta) ? d : out.max_pred_delta;
  }
  for (const usize s : r0->test_keys) {
    const f64 p0 = t0.oof_pred[s];
    const f64 p1 = t1.oof_pred[s];
    out.oof_identical = out.oof_identical && (std::memcmp(&p0, &p1, sizeof(f64)) == 0);
  }
  return out;
}

const auto kFitTcn = [](const learn::SequenceTensor &s, const learn::TcnAlphaCfg &c,
                        learn::LearnFitTrace *t) { return learn::fit_tcn(s, c, t); };
const auto kFitGru = [](const learn::SequenceTensor &s, const learn::GruAlphaCfg &c,
                        learn::LearnFitTrace *t) { return learn::fit_gru(s, c, t); };

// ---------------------------------------------------------------------------
//  L-01 — sequence fits (TCN, GRU)
// ---------------------------------------------------------------------------

TEST(LearnLabelMutationInvariance_Seq, TcnTestFoldLabelsDoNotReachFoldModel) {
  const learn::SequenceTensor st = make_seq(14, 4, 3, 2, 2, 11);
  const learn::TcnAlphaCfg cfg = tiny_tcn(); // default protocol: InnerPurgedV2
  for (usize fold = 0; fold < 4U; ++fold) {
    const SeqRound r = seq_round(st, cfg, kFitTcn, fold);
    ASSERT_TRUE(r.fold_recorded) << "fold " << fold;
    ASSERT_GT(r.n_test, 0U);
    EXPECT_TRUE(r.preds_identical) << "A1: fold " << fold << " OOS predictions moved";
    EXPECT_TRUE(r.oof_identical) << "A1: fold " << fold << " OOF predictions moved";
    EXPECT_TRUE(r.artifacts_identical) << "A2: fold " << fold << " member states moved";
    std::cout << "[W0-L0 tcn V2] fold=" << fold << " n_test=" << r.n_test
              << " artifact_f64=" << r.artifact_len << " max|dpred|=" << r.max_pred_delta
              << " identical=" << (r.preds_identical && r.artifacts_identical) << "\n";
  }
}

TEST(LearnLabelMutationInvariance_Seq, TcnLegacyTestFoldValidationLeaks) {
  const learn::SequenceTensor st = make_seq(14, 4, 3, 2, 2, 11);
  learn::TcnAlphaCfg cfg = tiny_tcn();
  cfg.protocol.validation = learn::SeqValidationRule::TestFoldV1; // pre-fix behaviour
  const SeqRound r = seq_round(st, cfg, kFitTcn, 1U);
  ASSERT_TRUE(r.fold_recorded);
  // L-01: the checkpoint was selected on the mutated test labels.
  EXPECT_FALSE(r.artifacts_identical) << "legacy V1 must show the test-label leak";
  EXPECT_FALSE(r.preds_identical);
  std::cout << "[W0-L0 tcn V1] fold=1 n_test=" << r.n_test << " max|dpred|=" << r.max_pred_delta
            << " identical=" << (r.preds_identical && r.artifacts_identical) << "\n";
}

TEST(LearnLabelMutationInvariance_Seq, GruTestFoldLabelsDoNotReachFoldModel) {
  const learn::SequenceTensor st = make_seq(14, 4, 3, 2, 2, 23);
  const learn::GruAlphaCfg cfg = tiny_gru();
  for (usize fold = 0; fold < 4U; ++fold) {
    const SeqRound r = seq_round(st, cfg, kFitGru, fold);
    ASSERT_TRUE(r.fold_recorded) << "fold " << fold;
    EXPECT_TRUE(r.preds_identical) << "A1: fold " << fold;
    EXPECT_TRUE(r.oof_identical) << "A1: fold " << fold;
    EXPECT_TRUE(r.artifacts_identical) << "A2: fold " << fold;
  }
  learn::GruAlphaCfg legacy = cfg;
  legacy.protocol.validation = learn::SeqValidationRule::TestFoldV1;
  const SeqRound leak = seq_round(st, legacy, kFitGru, 1U);
  ASSERT_TRUE(leak.fold_recorded);
  EXPECT_FALSE(leak.artifacts_identical) << "legacy V1 must show the test-label leak";
  std::cout << "[W0-L0 gru V1] fold=1 max|dpred|=" << leak.max_pred_delta << "\n";
}

// The inner validation block is carved from the fold's TRAIN dates: it never touches
// the test dates, and every inner-train date is purged against it (|d - v| >= H).
TEST(LearnLabelMutationInvariance_Seq, InnerValidationIsPurgedTrainBlock) {
  const learn::SequenceTensor st = make_seq(14, 4, 3, 2, 2, 11);
  learn::TcnAlphaCfg cfg = tiny_tcn();
  cfg.train.epochs = 1;
  learn::LearnFitTrace t;
  ASSERT_TRUE(learn::fit_tcn(st, cfg, &t).has_value());
  ASSERT_FALSE(t.folds.empty());
  for (const learn::LearnFoldRecord &r : t.folds) {
    ASSERT_FALSE(r.val_keys.empty());
    ASSERT_FALSE(r.fit_keys.empty());
    const usize horizon = cfg.horizons[r.horizon_idx];
    for (const usize v : r.val_keys) {
      for (const usize te : r.test_keys) {
        EXPECT_NE(st.date_of[v], st.date_of[te]) << "validation sample on a test date";
      }
      for (const usize f : r.fit_keys) {
        const usize dv = st.date_of[v];
        const usize df = st.date_of[f];
        const usize gap = (dv > df) ? dv - df : df - dv;
        EXPECT_GE(gap, horizon) << "inner-train sample not purged against the block";
      }
    }
  }
}

// ---------------------------------------------------------------------------
//  L-07 — the deployed ensemble selects on the inner validation block
// ---------------------------------------------------------------------------

// The latest inner_val_frac of dates carry INVERTED labels. Selecting on that block
// keeps the untrained initial members (training on the rest only moves predictions
// away from the inverted labels), so the deployed states equal a zero-epoch fit.
// Selecting on training loss (legacy V1) keeps trained states instead.
TEST(LearnLabelMutationInvariance_Deploy, DeployCheckpointUsesInnerValidation) {
  learn::SequenceTensor st = make_seq(26, 5, 3, 2, 1, 37);
  learn::GruAlphaCfg cfg = tiny_gru();
  cfg.horizons = {1};
  learn::LearnFitTrace t;
  const auto m = learn::fit_gru(st, cfg, &t);
  ASSERT_TRUE(m.has_value());
  ASSERT_FALSE(t.deploy_val_keys.empty());
  ASSERT_FALSE(t.deploy_fit_keys.empty());
  usize first_val_date = st.date_of[t.deploy_val_keys.front()];
  for (const usize s : t.deploy_val_keys) {
    first_val_date = (st.date_of[s] < first_val_date) ? st.date_of[s] : first_val_date;
  }
  for (const usize s : t.deploy_fit_keys) {
    EXPECT_LT(st.date_of[s], first_val_date) << "the block must be the latest dates";
  }
  // Invert the block's labels, then compare against a zero-epoch fit (the init states).
  for (const usize s : t.deploy_val_keys) {
    st.y[0][s] = -3.0 * st.y[0][s];
  }
  const auto trained = learn::fit_gru(st, cfg, nullptr);
  learn::GruAlphaCfg zero = cfg;
  zero.train.epochs = 0;
  const auto init = learn::fit_gru(st, zero, nullptr);
  ASSERT_TRUE(trained.has_value() && init.has_value());
  ASSERT_EQ(trained->nn.member_states.size(), cfg.train.ensemble_size);
  for (usize k = 0; k < trained->nn.member_states.size(); ++k) {
    EXPECT_TRUE(bytes_equal(trained->nn.member_states[k], init->nn.member_states[k]))
        << "member " << k << ": the checkpoint must be chosen on the inner block";
  }
  learn::GruAlphaCfg legacy = cfg;
  legacy.protocol.deploy = learn::SeqDeployRule::TrainLossV1;
  const auto on_train = learn::fit_gru(st, legacy, nullptr);
  ASSERT_TRUE(on_train.has_value());
  EXPECT_FALSE(bytes_equal(on_train->nn.member_states[0], init->nn.member_states[0]))
      << "legacy V1 selects on training loss and keeps trained states";
}

TEST(LearnLabelMutationInvariance_Deploy, RejectsOutOfContractValFraction) {
  const learn::SequenceTensor st = make_seq(10, 3, 3, 2, 1, 5);
  learn::GruAlphaCfg cfg = tiny_gru();
  cfg.horizons = {1};
  for (const f64 bad : {0.0, -0.1, 0.51, 1.0}) {
    cfg.protocol.inner_val_frac = bad;
    EXPECT_FALSE(learn::fit_gru(st, cfg).has_value()) << "frac " << bad;
  }
  // Both rules legacy -> the fraction is unused and not validated.
  cfg.protocol.validation = learn::SeqValidationRule::TestFoldV1;
  cfg.protocol.deploy = learn::SeqDeployRule::TrainLossV1;
  cfg.protocol.inner_val_frac = 0.0;
  cfg.train.epochs = 1;
  EXPECT_TRUE(learn::fit_gru(st, cfg).has_value());
  // An embargo fraction outside [0, 1] (NaN included) is rejected, not cast.
  for (const f64 bad : {-0.01, 1.5, std::numeric_limits<f64>::quiet_NaN()}) {
    cfg.cpcv.embargo = bad;
    EXPECT_FALSE(learn::fit_gru(st, cfg).has_value()) << "embargo " << bad;
  }
}

// ---------------------------------------------------------------------------
//  L-03 — linear and GBT fits with a caller-selected augmentation
// ---------------------------------------------------------------------------

// A 4-feature panel. The clean label favours features 2 and 3 (weakly), so the
// caller's full-window interaction selection picks {2,3}; the mutation writes a large
// label driven by features 0 and 1 into one fold's test rows, which flips that
// selection to {0,1} when the caller refits it on the mutated labels.
[[nodiscard]] learn::FeatureMatrix make_fm(usize n_dates, usize n_inst, u64 seed) {
  learn::FeatureMatrix fm;
  fm.n_dates = n_dates;
  fm.n_instruments = n_inst;
  fm.n_features = 4;
  fm.Y.assign(1U, {});
  fm.label_horizons.assign(1U, static_cast<u16>(1));
  atx::core::Xoshiro256pp rng{seed};
  for (usize d = 0; d < n_dates; ++d) {
    for (usize i = 0; i < n_inst; ++i) {
      std::vector<f64> x(4);
      for (f64 &v : x) {
        v = rng.normal();
      }
      fm.row_date.push_back(d);
      fm.row_inst.push_back(i);
      fm.X.insert(fm.X.end(), x.begin(), x.end());
      fm.row_valid.push_back(static_cast<u8>(1));
      fm.Y[0].push_back(0.35 * x[2] + 0.3 * x[3] + rng.normal());
    }
  }
  return fm;
}

[[nodiscard]] learn::FeatureMatrix mutate_fm_labels(const learn::FeatureMatrix &fm,
                                                    const std::vector<usize> &rows) {
  learn::FeatureMatrix out = fm;
  for (const usize r : rows) {
    out.Y[0][r] = 50.0 * (fm.X[r * 4U + 0U] + fm.X[r * 4U + 1U]);
  }
  return out;
}

[[nodiscard]] learn::LatentAugmentation caller_aug(const learn::FeatureMatrix &fm) {
  learn::LatentAugmentation aug;
  aug.interactions = learn::select_interactions(fm, fm.n_dates - 1U, /*embargo=*/0U, /*m=*/2U);
  return aug;
}

[[nodiscard]] learn::LinearAlphaCfg linear_cfg() {
  learn::LinearAlphaCfg cfg;
  cfg.en.lambda = 1e-3;
  cfg.use_ridge_baseline = true;
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.master_seed = 7;
  cfg.horizons = {1};
  return cfg;
}

struct TabRound {
  bool aug_flipped = false;
  bool preds_identical = true;
  bool artifacts_identical = true;
  bool oof_identical = true;
  usize n_test = 0;
  usize artifact_len = 0;
  f64 max_pred_delta = 0.0;
};

template <typename Cfg, typename FitFn>
[[nodiscard]] TabRound tab_round(const learn::FeatureMatrix &fm, const Cfg &cfg, FitFn fit,
                                 usize fold) {
  TabRound out;
  const learn::LatentAugmentation aug0 = caller_aug(fm);
  learn::LearnFitTrace t0;
  static_cast<void>(fit(fm, aug0, cfg, &t0));
  const learn::LearnFoldRecord *r0 = find_rec(t0, 0U, fold);
  EXPECT_NE(r0, nullptr);
  if (r0 == nullptr) {
    return out;
  }
  out.n_test = r0->test_keys.size();
  // The CALLER refits its full-window augmentation on the mutated labels.
  const learn::FeatureMatrix fm_mut = mutate_fm_labels(fm, r0->test_keys);
  const learn::LatentAugmentation aug1 = caller_aug(fm_mut);
  out.aug_flipped = aug0.interactions != aug1.interactions;
  learn::LearnFitTrace t1;
  static_cast<void>(fit(fm_mut, aug1, cfg, &t1));
  const learn::LearnFoldRecord *r1 = find_rec(t1, 0U, fold);
  EXPECT_NE(r1, nullptr);
  if (r1 == nullptr || r0->test_keys != r1->test_keys) {
    out.preds_identical = false;
    return out;
  }
  out.preds_identical = bytes_equal(r0->test_pred, r1->test_pred);
  out.artifacts_identical = bytes_equal(r0->artifact, r1->artifact);
  out.artifact_len = r0->artifact.size();
  out.max_pred_delta = max_abs_delta(r0->test_pred, r1->test_pred);
  for (const usize r : r0->test_keys) {
    const f64 p0 = t0.oof_pred[r];
    const f64 p1 = t1.oof_pred[r];
    out.oof_identical = out.oof_identical && (std::memcmp(&p0, &p1, sizeof(f64)) == 0);
  }
  return out;
}

const auto kFitLinear = [](const learn::FeatureMatrix &fm, const learn::LatentAugmentation &a,
                           const learn::LinearAlphaCfg &c, learn::LearnFitTrace *t) {
  return learn::fit_linear(fm, a, c, t);
};
const auto kFitGbt = [](const learn::FeatureMatrix &fm, const learn::LatentAugmentation &a,
                        const learn::GbtCfg &c, learn::LearnFitTrace *t) {
  return learn::fit_gbt(fm, a, c, t);
};

TEST(LearnLabelMutationInvariance_Tabular, LinearTestFoldLabelsDoNotReachFoldModel) {
  const learn::FeatureMatrix fm = make_fm(24, 8, 101);
  const learn::LinearAlphaCfg cfg = linear_cfg(); // default protocol: FoldLocalV2
  for (usize fold = 0; fold < 4U; ++fold) {
    const TabRound r = tab_round(fm, cfg, kFitLinear, fold);
    ASSERT_GT(r.n_test, 0U);
    EXPECT_TRUE(r.preds_identical) << "A1: fold " << fold;
    EXPECT_TRUE(r.oof_identical) << "A1: fold " << fold;
    EXPECT_TRUE(r.artifacts_identical) << "A2: fold " << fold;
    std::cout << "[W0-L0 linear V2] fold=" << fold << " n_test=" << r.n_test
              << " artifact_f64=" << r.artifact_len << " aug_flipped=" << r.aug_flipped
              << " identical=" << (r.preds_identical && r.artifacts_identical) << "\n";
  }
}

TEST(LearnLabelMutationInvariance_Tabular, LinearLegacyFullWindowAugLeaks) {
  const learn::FeatureMatrix fm = make_fm(24, 8, 101);
  learn::LinearAlphaCfg cfg = linear_cfg();
  cfg.protocol.fold_aug = learn::FoldAugRule::FullWindowV1;
  const TabRound r = tab_round(fm, cfg, kFitLinear, 1U);
  ASSERT_TRUE(r.aug_flipped) << "fixture: the mutation must flip the caller's selection";
  EXPECT_FALSE(r.artifacts_identical) << "legacy V1 copies the leaked selection into folds";
  EXPECT_FALSE(r.preds_identical);
  std::cout << "[W0-L0 linear V1] fold=1 n_test=" << r.n_test
            << " max|dpred|=" << r.max_pred_delta << "\n";
}

TEST(LearnLabelMutationInvariance_Tabular, GbtTestFoldLabelsDoNotReachFoldModel) {
  const learn::FeatureMatrix fm = make_fm(24, 8, 101);
  learn::GbtCfg cfg;
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.min_split_gain = 0.0;
  cfg.master_seed = 9;
  for (usize fold = 0; fold < 4U; ++fold) {
    const TabRound r = tab_round(fm, cfg, kFitGbt, fold);
    ASSERT_GT(r.n_test, 0U);
    EXPECT_TRUE(r.preds_identical) << "A1: fold " << fold;
    EXPECT_TRUE(r.oof_identical) << "A1: fold " << fold;
    EXPECT_TRUE(r.artifacts_identical) << "A2: fold " << fold;
  }
  learn::GbtCfg legacy = cfg;
  legacy.protocol.fold_aug = learn::FoldAugRule::FullWindowV1;
  const TabRound leak = tab_round(fm, legacy, kFitGbt, 1U);
  ASSERT_TRUE(leak.aug_flipped);
  EXPECT_FALSE(leak.artifacts_identical) << "legacy V1 copies the leaked selection into folds";
  std::cout << "[W0-L0 gbt V1] fold=1 max|dpred|=" << leak.max_pred_delta << "\n";
}

} // namespace atx_test_w0_l0_label_mutation

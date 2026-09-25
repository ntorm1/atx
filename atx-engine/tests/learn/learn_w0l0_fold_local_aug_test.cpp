// W0-L0 — fold-local augmentation fitting (plan v2 §7 W0-L0; finding L-03).
//
// fit_linear / fit_gbt used to copy the caller's full-window augmentation (PCA basis +
// label-selected interactions) into every CPCV fold. Under FoldAugRule::FoldLocalV2
// (the default) each fold refits the augmentation RECIPE on its own train rows:
//   * fit_fold_augmentation reads nothing outside the fold's train rows;
//   * a fitted selection passed by the caller never reaches the folds (only its
//     recipe, the top-m, does); explicit fixed interactions are reused verbatim;
//   * the PCA basis of a fold is the basis of that fold's train rows.
// Every V2 invariance is paired with the legacy FullWindowV1 result on the same input.

#include <cstddef> // std::ptrdiff_t
#include <cstring> // std::memcmp
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <Eigen/Dense> // Eigen::Index

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"            // fit_gbt, GbtCfg
#include "atx/engine/learn/latent.hpp"         // fit_fold_augmentation, fit_latent, ...
#include "atx/engine/learn/linear_alpha.hpp"   // fit_linear, LinearAlphaCfg

namespace atx_test_w0_l0_fold_local_aug {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
namespace learn = atx::engine::learn;
using Pairs = std::vector<std::pair<u32, u32>>;

[[nodiscard]] bool bytes_equal(const std::vector<f64> &a, const std::vector<f64> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(f64)) == 0);
}

[[nodiscard]] std::vector<f64> flat(const learn::LatentAugmentation &aug) {
  std::vector<f64> out;
  learn::detail::append_augmentation(aug, out);
  return out;
}

// 5 features, 20 dates x 6 instruments; label = 0.8*x3 + 0.6*x4 + noise (rows of dates
// < 10) and 0.8*x0 + 0.6*x1 + noise (dates >= 10), so the supervised selection depends
// on which rows it reads.
[[nodiscard]] learn::FeatureMatrix make_fm(u64 seed) {
  learn::FeatureMatrix fm;
  fm.n_dates = 20;
  fm.n_instruments = 6;
  fm.n_features = 5;
  fm.Y.assign(1U, {});
  fm.label_horizons.assign(1U, static_cast<u16>(1));
  atx::core::Xoshiro256pp rng{seed};
  for (usize d = 0; d < fm.n_dates; ++d) {
    for (usize i = 0; i < fm.n_instruments; ++i) {
      std::vector<f64> x(5);
      for (f64 &v : x) {
        v = rng.normal();
      }
      x[2] = 0.7 * x[0] + 0.3 * rng.normal(); // a correlated column for a non-trivial PCA
      fm.row_date.push_back(d);
      fm.row_inst.push_back(i);
      fm.X.insert(fm.X.end(), x.begin(), x.end());
      fm.row_valid.push_back(static_cast<u8>(1));
      const f64 y = (d < 10U) ? 0.8 * x[3] + 0.6 * x[4] : 0.8 * x[0] + 0.6 * x[1];
      fm.Y[0].push_back(y + 0.2 * rng.normal());
    }
  }
  return fm;
}

[[nodiscard]] std::vector<usize> rows_where(const learn::FeatureMatrix &fm, bool early) {
  std::vector<usize> rows;
  for (usize r = 0; r < fm.n_rows(); ++r) {
    if ((fm.row_date[r] < 10U) == early) {
      rows.push_back(r);
    }
  }
  return rows;
}

// ---------------------------------------------------------------------------
//  fit_fold_augmentation reads only the fold's train rows
// ---------------------------------------------------------------------------

TEST(LearnFoldLocalAug_Fit, HeldOutFeaturesAndLabelsCannotChangeFoldAug) {
  const learn::FeatureMatrix fm = make_fm(3);
  learn::LatentAugmentation deployed;
  deployed.pca = learn::fit_latent(fm, fm.n_dates - 1U, 0U, 2U);
  deployed.interactions = learn::select_interactions(fm, fm.n_dates - 1U, 0U, 2U);
  ASSERT_EQ(deployed.pca->k, 2U);
  ASSERT_EQ(deployed.interactions.size(), 1U);
  const std::vector<usize> train = rows_where(fm, /*early=*/true);
  const std::vector<usize> held = rows_where(fm, /*early=*/false);

  const learn::LatentAugmentation a0 =
      learn::fit_fold_augmentation(fm, deployed, std::span<const usize>{train}, 0U);
  // Perturb EVERY held-out row: features, labels and a NaN label.
  learn::FeatureMatrix fm_mut = fm;
  for (const usize r : held) {
    for (usize f = 0; f < fm.n_features; ++f) {
      fm_mut.X[r * fm.n_features + f] = 100.0 + static_cast<f64>(f) * fm.X[r * fm.n_features];
    }
    fm_mut.Y[0][r] = (r % 7U == 0U) ? std::numeric_limits<f64>::quiet_NaN() : -9.0 * fm.Y[0][r];
  }
  const learn::LatentAugmentation a1 =
      learn::fit_fold_augmentation(fm_mut, deployed, std::span<const usize>{train}, 0U);
  EXPECT_TRUE(bytes_equal(flat(a0), flat(a1))) << "a held-out row reached the fold fit";
  ASSERT_TRUE(a0.pca.has_value());
  EXPECT_EQ(a0.pca->k, 2U);
  EXPECT_EQ(a0.interactions, (Pairs{{3U, 4U}})) << "the train rows' own leaders";

  // Non-vacuous: perturbing a TRAIN row does move the fold fit.
  learn::FeatureMatrix fm_train_mut = fm;
  fm_train_mut.X[train[0] * fm.n_features + 1U] += 25.0;
  const learn::LatentAugmentation a2 =
      learn::fit_fold_augmentation(fm_train_mut, deployed, std::span<const usize>{train}, 0U);
  EXPECT_FALSE(bytes_equal(flat(a0), flat(a2)));
}

TEST(LearnFoldLocalAug_Fit, SelectionRecipeIsTopMOfPassedPairs) {
  const learn::FeatureMatrix fm = make_fm(3);
  learn::LatentAugmentation deployed;
  deployed.interactions = {{0U, 1U}, {0U, 2U}, {1U, 2U}}; // a top-3 clique
  EXPECT_EQ(learn::detail::interaction_top_m(deployed.interactions), 3U);
  const std::vector<usize> train = rows_where(fm, /*early=*/true);
  const learn::LatentAugmentation a =
      learn::fit_fold_augmentation(fm, deployed, std::span<const usize>{train}, 0U);
  EXPECT_EQ(a.interactions.size(), 3U) << "C(3,2) pairs re-selected on the train rows";
  EXPECT_EQ(a.interactions,
            learn::detail::select_interactions_on_rows(fm, std::span<const usize>{train}, 0U, 3U));
  EXPECT_FALSE(a.pca.has_value()) << "no PCA recipe -> no PCA";
}

TEST(LearnFoldLocalAug_Fit, FixedInteractionsAreReusedVerbatim) {
  const learn::FeatureMatrix fm = make_fm(3);
  learn::LatentAugmentation deployed;
  deployed.interactions = {{0U, 4U}};
  deployed.interactions_fixed = true;
  const std::vector<usize> train = rows_where(fm, /*early=*/true);
  const learn::LatentAugmentation a =
      learn::fit_fold_augmentation(fm, deployed, std::span<const usize>{train}, 0U);
  EXPECT_EQ(a.interactions, deployed.interactions);
  EXPECT_TRUE(a.interactions_fixed);
}

// Fix pass 1 (review minor): only a complete canonical C(m,2) set is a selection recipe.
TEST(LearnFoldLocalAug_Fit, OnlyCompleteCanonicalCliqueIsASelection) {
  EXPECT_TRUE(learn::detail::is_selected_clique(Pairs{{0U, 1U}}));
  EXPECT_TRUE(learn::detail::is_selected_clique(Pairs{{0U, 1U}, {0U, 2U}, {1U, 2U}}));
  EXPECT_TRUE(learn::detail::is_selected_clique(Pairs{{1U, 3U}, {1U, 4U}, {3U, 4U}}));
  EXPECT_FALSE(learn::detail::is_selected_clique(Pairs{}));
  EXPECT_FALSE(learn::detail::is_selected_clique(Pairs{{0U, 1U}, {2U, 3U}})) << "m=4, 2 pairs";
  EXPECT_FALSE(learn::detail::is_selected_clique(Pairs{{0U, 1U}, {0U, 2U}})) << "missing (1,2)";
  EXPECT_FALSE(learn::detail::is_selected_clique(Pairs{{0U, 2U}, {0U, 1U}, {1U, 2U}}))
      << "not in selection order";
  EXPECT_FALSE(learn::detail::is_selected_clique(Pairs{{1U, 0U}})) << "a > b";
  EXPECT_FALSE(learn::detail::is_selected_clique(Pairs{{2U, 2U}})) << "self pair";
  EXPECT_FALSE(learn::detail::is_selected_clique(Pairs{{0U, 1U}, {0U, 1U}, {0U, 1U}}))
      << "duplicates";
  // Every real selection output qualifies.
  const learn::FeatureMatrix fm = make_fm(3);
  for (u32 m = 2U; m <= 5U; ++m) {
    EXPECT_TRUE(learn::detail::is_selected_clique(learn::select_interactions(fm, fm.n_dates - 1U,
                                                                             0U, m)))
        << "m=" << m;
  }
}

TEST(LearnFoldLocalAug_Fit, NonCliquePairsAreReusedVerbatimNotRefit) {
  const learn::FeatureMatrix fm = make_fm(3);
  learn::LatentAugmentation deployed;
  deployed.interactions = {{0U, 1U}, {2U, 3U}}; // hand-built, interactions_fixed left false
  ASSERT_FALSE(deployed.interactions_fixed);
  const std::vector<usize> train = rows_where(fm, /*early=*/true);
  const learn::LatentAugmentation a =
      learn::fit_fold_augmentation(fm, deployed, std::span<const usize>{train}, 0U);
  EXPECT_EQ(a.interactions, deployed.interactions)
      << "the fold must evaluate the deployed structure, not a refit top-4 clique";
  // Non-vacuous: the same four features as a complete clique ARE re-selected (top-4 of
  // the train rows gives 6 pairs, not the 2 the hand-built list carries).
  learn::LatentAugmentation clique;
  clique.interactions = {{0U, 1U}, {0U, 2U}, {0U, 3U}, {1U, 2U}, {1U, 3U}, {2U, 3U}};
  const learn::LatentAugmentation c =
      learn::fit_fold_augmentation(fm, clique, std::span<const usize>{train}, 0U);
  EXPECT_EQ(c.interactions,
            learn::detail::select_interactions_on_rows(fm, std::span<const usize>{train}, 0U, 4U));
  EXPECT_NE(c.interactions, clique.interactions) << "the train rows' top-4 differ";
}

TEST(LearnFoldLocalAug_Fit, PcaBasisIsTheFoldTrainRowsBasis) {
  const learn::FeatureMatrix fm = make_fm(3);
  learn::LatentAugmentation deployed;
  deployed.pca = learn::fit_latent(fm, fm.n_dates - 1U, 0U, 2U);
  const std::vector<usize> train = rows_where(fm, /*early=*/true);
  const learn::LatentAugmentation a =
      learn::fit_fold_augmentation(fm, deployed, std::span<const usize>{train}, 0U);
  const learn::LatentBasis direct =
      learn::detail::fit_latent_on_rows(fm, std::span<const usize>{train}, 2U, 9U);
  ASSERT_TRUE(a.pca.has_value());
  EXPECT_EQ(a.pca->fit_upto_date, 9U) << "the last train date";
  ASSERT_EQ(a.pca->model.mean.size(), direct.model.mean.size());
  for (Eigen::Index j = 0; j < direct.model.mean.size(); ++j) {
    EXPECT_EQ(a.pca->model.mean(j), direct.model.mean(j));
  }
  // The full-window mean differs (the fold does not reuse it).
  bool any_diff = false;
  for (Eigen::Index j = 0; j < direct.model.mean.size(); ++j) {
    any_diff = any_diff || (deployed.pca->model.mean(j) != a.pca->model.mean(j));
  }
  EXPECT_TRUE(any_diff);
}

TEST(LearnFoldLocalAug_Fit, FoldSelectionLabelNestsInsidePurge) {
  const std::vector<u16> asc{1, 5, 21};
  EXPECT_EQ(learn::fold_selection_label(std::span<const u16>{asc}, 0U), 0U);
  EXPECT_EQ(learn::fold_selection_label(std::span<const u16>{asc}, 2U), 0U);
  const std::vector<u16> desc{21, 5};
  EXPECT_EQ(learn::fold_selection_label(std::span<const u16>{desc}, 1U), 1U)
      << "Y[0] (21d) is not covered by a 5d purge -> the fold's own label";
}

// ---------------------------------------------------------------------------
//  A caller's fitted selection never reaches the folds (end to end)
// ---------------------------------------------------------------------------

[[nodiscard]] learn::LinearAlphaCfg linear_cfg() {
  learn::LinearAlphaCfg cfg;
  cfg.en.lambda = 1e-3;
  cfg.use_ridge_baseline = true;
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.master_seed = 1;
  cfg.horizons = {1};
  return cfg;
}

[[nodiscard]] std::vector<f64> all_artifacts(const learn::LearnFitTrace &t) {
  std::vector<f64> out;
  for (const learn::LearnFoldRecord &r : t.folds) {
    out.insert(out.end(), r.artifact.begin(), r.artifact.end());
    out.insert(out.end(), r.test_pred.begin(), r.test_pred.end());
  }
  return out;
}

TEST(LearnFoldLocalAug_Linear, PassedSelectionDoesNotReachFolds) {
  const learn::FeatureMatrix fm = make_fm(5);
  learn::LatentAugmentation a;
  a.interactions = {{0U, 1U}};
  learn::LatentAugmentation b;
  b.interactions = {{3U, 4U}}; // same recipe (top-2), different fitted pairs
  learn::LinearAlphaCfg cfg = linear_cfg();
  learn::LearnFitTrace ta;
  learn::LearnFitTrace tb;
  const learn::LearnedModel ma = learn::fit_linear(fm, a, cfg, &ta);
  const learn::LearnedModel mb = learn::fit_linear(fm, b, cfg, &tb);
  ASSERT_EQ(ta.folds.size(), 4U);
  EXPECT_TRUE(bytes_equal(all_artifacts(ta), all_artifacts(tb)));
  EXPECT_TRUE(bytes_equal(ma.oos_score_series, mb.oos_score_series));
  EXPECT_TRUE(bytes_equal(ma.blend_w, mb.blend_w));
  EXPECT_EQ(ma.aug.interactions, a.interactions) << "the DEPLOYED model keeps the caller's aug";

  cfg.protocol.fold_aug = learn::FoldAugRule::FullWindowV1;
  learn::LearnFitTrace la;
  learn::LearnFitTrace lb;
  const learn::LearnedModel va = learn::fit_linear(fm, a, cfg, &la);
  const learn::LearnedModel vb = learn::fit_linear(fm, b, cfg, &lb);
  EXPECT_FALSE(bytes_equal(all_artifacts(la), all_artifacts(lb))) << "legacy V1 leaks it";
  EXPECT_FALSE(bytes_equal(va.oos_score_series, vb.oos_score_series));
}

TEST(LearnFoldLocalAug_Linear, LegacyRuleReproducesOldFoldAugmentation) {
  // FullWindowV1 must reproduce the pre-fix numbers: every fold carries the caller's
  // augmentation verbatim (the appended aug section of each artifact equals it).
  const learn::FeatureMatrix fm = make_fm(5);
  learn::LatentAugmentation a;
  a.pca = learn::fit_latent(fm, fm.n_dates - 1U, 0U, 1U);
  a.interactions = {{0U, 1U}};
  learn::LinearAlphaCfg cfg = linear_cfg();
  cfg.protocol.fold_aug = learn::FoldAugRule::FullWindowV1;
  learn::LearnFitTrace t;
  static_cast<void>(learn::fit_linear(fm, a, cfg, &t));
  const std::vector<f64> want = flat(a);
  ASSERT_FALSE(t.folds.empty());
  for (const learn::LearnFoldRecord &r : t.folds) {
    // artifact = feat_mean (5) + feat_sd (5) + aug + coeff.
    const std::vector<f64> got(r.artifact.begin() + 10,
                               r.artifact.begin() + 10 + static_cast<std::ptrdiff_t>(want.size()));
    EXPECT_TRUE(bytes_equal(got, want));
  }
}

TEST(LearnFoldLocalAug_Gbt, PassedSelectionDoesNotReachFolds) {
  const learn::FeatureMatrix fm = make_fm(5);
  learn::LatentAugmentation a;
  a.interactions = {{0U, 1U}};
  learn::LatentAugmentation b;
  b.interactions = {{3U, 4U}};
  learn::GbtCfg cfg;
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.min_split_gain = 0.0;
  cfg.master_seed = 2;
  learn::LearnFitTrace ta;
  learn::LearnFitTrace tb;
  const learn::LearnedModel ma = learn::fit_gbt(fm, a, cfg, &ta);
  const learn::LearnedModel mb = learn::fit_gbt(fm, b, cfg, &tb);
  ASSERT_EQ(ta.folds.size(), 4U);
  EXPECT_TRUE(bytes_equal(all_artifacts(ta), all_artifacts(tb)));
  EXPECT_TRUE(bytes_equal(ma.oos_score_series, mb.oos_score_series));

  cfg.protocol.fold_aug = learn::FoldAugRule::FullWindowV1;
  learn::LearnFitTrace la;
  learn::LearnFitTrace lb;
  static_cast<void>(learn::fit_gbt(fm, a, cfg, &la));
  static_cast<void>(learn::fit_gbt(fm, b, cfg, &lb));
  EXPECT_FALSE(bytes_equal(all_artifacts(la), all_artifacts(lb))) << "legacy V1 leaks it";
}

// Fix pass 1 (review minor): a hand-built non-clique list is the structure every fold
// evaluates (it used to be read as a top-4 recipe and refit to 6 pairs per fold).
TEST(LearnFoldLocalAug_Linear, NonCliquePairsReachEveryFoldUnchanged) {
  const learn::FeatureMatrix fm = make_fm(5);
  learn::LatentAugmentation a;
  a.interactions = {{0U, 1U}, {2U, 3U}};
  const learn::LinearAlphaCfg cfg = linear_cfg();
  learn::LearnFitTrace t;
  const learn::LearnedModel m = learn::fit_linear(fm, a, cfg, &t);
  EXPECT_EQ(m.aug.interactions, a.interactions);
  const std::vector<f64> want = flat(a);
  ASSERT_EQ(t.folds.size(), 4U);
  for (const learn::LearnFoldRecord &r : t.folds) {
    // artifact = feat_mean (5) + feat_sd (5) + aug + coeff.
    ASSERT_GE(r.artifact.size(), 10U + want.size());
    const std::vector<f64> got(r.artifact.begin() + 10,
                               r.artifact.begin() + 10 + static_cast<std::ptrdiff_t>(want.size()));
    EXPECT_TRUE(bytes_equal(got, want)) << "fold structure == deployed structure";
  }
}

} // namespace atx_test_w0_l0_fold_local_aug

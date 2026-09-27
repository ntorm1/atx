#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/learn/train.hpp"
#include "atx/engine/learn/linear_alpha.hpp"
#include "atx/engine/learn/gbt.hpp"
#include "atx/engine/learn/tcn_alpha.hpp"

namespace atxtest_learn_cpcv_date {
namespace learn=atx::engine::learn;
namespace eval=atx::engine::eval;
using atx::usize;
learn::FeatureMatrix fixture() {
  learn::FeatureMatrix fm;
  fm.n_dates=36; fm.n_instruments=2; fm.n_features=2;
  fm.Y.resize(1); fm.label_horizons={3};
  for (usize d=0; d<36; ++d) for (usize i=0; i<2; ++i) {
    (void)fm.push_row(d,i); fm.row_valid.push_back(1);
    const auto x=std::sin(static_cast<double>(d*2+i));
    fm.X.push_back(x); fm.X.push_back(std::cos(static_cast<double>(d+i)));
    fm.Y[0].push_back(d+3<36 ? 0.1*x : std::numeric_limits<double>::quiet_NaN());
  }
  return fm;
}
TEST(LearnCpcvDate, FullEndpointAndSparseDatesArePreserved) {
  const std::array<usize,5> dates{0,0,5,20,20};
  const auto spans=learn::date_label_spans_v2(dates,3);
  ASSERT_TRUE(spans); ASSERT_EQ(spans->size(),3U);
  EXPECT_EQ((*spans)[0].t1,4U); EXPECT_EQ((*spans)[1].t0,5U);
  EXPECT_EQ((*spans)[2].t1,24U); // no clamp to last observed anchor+1
  const std::array<usize,1> bad{std::numeric_limits<usize>::max()-2U};
  EXPECT_FALSE(learn::date_label_spans_v2(bad,3));
}
TEST(LearnCpcvDate, LinearAndGbtUseCheckedDatePlansAndCarryActualPaths) {
  const auto fm=fixture(); const learn::LatentAugmentation aug;
  learn::LinearAlphaCfg cfg{}; cfg.use_ridge_baseline=true;
  cfg.en.lambda=0.02; cfg.master_seed=7; cfg.horizons={3};
  cfg.cpcv={3,1,0.0}; cfg.cpcv.rule=eval::CpcvRule::DateV2; cfg.cpcv.embargo_dates=2;
  learn::LearnFitTrace trace;
  const auto fitted=learn::fit_linear_checked(fm,aug,cfg,&trace);
  ASSERT_TRUE(fitted); ASSERT_EQ(fitted->cpcv_metadata.size(),1U);
  EXPECT_EQ(fitted->cpcv_metadata[0].paths.size(),1U);
  ASSERT_FALSE(trace.folds.empty());
  for (const auto& fold:trace.folds) for (const auto tr:fold.fit_keys)
    for (const auto te:fold.test_keys) {
      const auto train=fm.row_date[tr], test=fm.row_date[te];
      EXPECT_TRUE(train+4U<=test || train>=test+4U+2U);
    }
  learn::GbtCfg gbt; gbt.n_trees=2; gbt.max_depth=1; gbt.n_bins=4;
  gbt.horizons=cfg.horizons; gbt.cpcv=cfg.cpcv;
  const auto tree=learn::fit_gbt_checked(fm,aug,gbt);
  ASSERT_TRUE(tree); ASSERT_EQ(tree->cpcv_metadata.size(),1U);
  EXPECT_EQ(tree->cpcv_metadata[0].paths,fitted->cpcv_metadata[0].paths);
  cfg.cpcv.max_working_bytes=gbt.cpcv.max_working_bytes=1;
  EXPECT_FALSE(learn::fit_linear_checked(fm,aug,cfg));
  EXPECT_FALSE(learn::fit_gbt_checked(fm,aug,gbt));
}
TEST(LearnCpcvDate, InactiveKnobsPreserveLegacyFitAndWrongHorizonFailsChecked) {
  auto fm=fixture(); const learn::LatentAugmentation aug;
  learn::LinearAlphaCfg cfg{}; cfg.use_ridge_baseline=true;
  cfg.en.lambda=0.02; cfg.master_seed=7; cfg.horizons={3}; cfg.cpcv={3,1,0.0};
  const auto legacy=learn::fit_linear_checked(fm,aug,cfg); ASSERT_TRUE(legacy);
  cfg.cpcv.embargo_dates=999; cfg.cpcv.max_working_bytes=1;
  const auto same=learn::fit_linear_checked(fm,aug,cfg); ASSERT_TRUE(same);
  EXPECT_TRUE(same->cpcv_metadata.empty()); EXPECT_EQ(same->blend_w,legacy->blend_w);
  ASSERT_EQ(same->coeffs.size(),legacy->coeffs.size());
  for (usize h=0; h<same->coeffs.size(); ++h)
    for (Eigen::Index i=0; i<same->coeffs[h].size(); ++i)
      EXPECT_DOUBLE_EQ(same->coeffs[h](i),legacy->coeffs[h](i));
  cfg.cpcv.rule=eval::CpcvRule::DateV2; cfg.horizons={1};
  EXPECT_FALSE(learn::fit_linear_checked(fm,aug,cfg));
}
TEST(LearnCpcvDate, SparseAxisAndExpandedRowsCannotBypassWorkspaceLimits) {
  auto fm=fixture(); eval::CpcvConfig cfg{3,1,0.0}; cfg.rule=eval::CpcvRule::DateV2;
  const auto plan=learn::learn_cpcv_plan(fm,3,cfg); ASSERT_TRUE(plan);
  const auto rows=learn::expand_date_folds_checked(plan->folds,fm,cfg); ASSERT_TRUE(rows);
  cfg.max_working_bytes=1000;
  EXPECT_FALSE(learn::expand_date_folds_checked(plan->folds,fm,cfg));
  cfg.max_working_bytes=1U<<20U; fm.n_dates=std::numeric_limits<usize>::max();
  // The expansion itself uses only the 36 observed dates, not a huge bitset.
  EXPECT_TRUE(learn::expand_date_folds_checked(plan->folds,fm,cfg));
  const std::array<atx::u16,1> horizons{3};
  EXPECT_FALSE(learn::validate_date_cpcv_inputs(fm,horizons,cfg));
}

TEST(LearnCpcvDate, SequenceInnerAndOuterDatePlansKeepTestLabelsOutOfCheckpoint) {
  learn::SequenceTensor seq;
  seq.lookback=2; seq.n_features=2; seq.y.resize(1);
  for (usize d=0; d<36; ++d) for (usize i=0; i<2; ++i) {
    // Deliberately sparse original session ordinals; embargo must not compress them.
    seq.date_of.push_back(2*d); seq.inst_of.push_back(i);
    seq.sample_valid.push_back(1); ++seq.n_samples;
    for (usize j=0; j<4; ++j)
      seq.x.push_back(std::sin(static_cast<double>(d*3+i+j)));
    seq.y[0].push_back(0.1*seq.x.back());
  }
  learn::TcnAlphaCfg cfg;
  cfg.blocks=1; cfg.kernel=2; cfg.channels=2; cfg.dropout=0;
  cfg.horizons={2}; cfg.cpcv={3,1,0.0};
  cfg.cpcv.rule=eval::CpcvRule::DateV2; cfg.cpcv.embargo_dates=1;
  cfg.train.epochs=2; cfg.train.batch_size=16; cfg.train.ckpt_every=1;
  cfg.train.ensemble_size=1; cfg.train.master_seed=19;
  learn::LearnFitTrace trace;
  const auto fitted=learn::fit_tcn(seq,cfg,&trace);
  ASSERT_TRUE(fitted);
  ASSERT_EQ(fitted->cpcv_metadata.size(),1U);
  ASSERT_FALSE(trace.folds.empty());
  const auto separated=[&](usize training,usize held_out) {
    const auto a=seq.date_of[training], b=seq.date_of[held_out];
    return a+3<=b || a>=b+3+cfg.cpcv.embargo_dates;
  };
  for (const auto& fold:trace.folds) {
    ASSERT_FALSE(fold.fit_keys.empty());
    ASSERT_FALSE(fold.val_keys.empty());
    for (const auto tr:fold.fit_keys) {
      for (const auto te:fold.test_keys) EXPECT_TRUE(separated(tr,te));
      for (const auto val:fold.val_keys) EXPECT_TRUE(separated(tr,val));
    }
    for (const auto val:fold.val_keys)
      for (const auto te:fold.test_keys) EXPECT_TRUE(separated(val,te));
  }
  const auto& reference=trace.folds.front();
  auto mutated=seq;
  for (const auto key:reference.test_keys) mutated.y[0][key]+=10;
  learn::LearnFitTrace changed_trace;
  ASSERT_TRUE(learn::fit_tcn(mutated,cfg,&changed_trace));
  const auto changed=std::find_if(changed_trace.folds.begin(),changed_trace.folds.end(),
      [&](const auto& fold) { return fold.fold_idx==reference.fold_idx && fold.horizon_idx==reference.horizon_idx; });
  ASSERT_NE(changed,changed_trace.folds.end());
  EXPECT_EQ(changed->test_keys,reference.test_keys);
  ASSERT_FALSE(reference.artifact.empty());
  ASSERT_EQ(changed->artifact.size(),reference.artifact.size());
  EXPECT_EQ(std::memcmp(changed->artifact.data(),reference.artifact.data(),reference.artifact.size()*sizeof(double)),0);
  ASSERT_EQ(changed->test_pred.size(),reference.test_pred.size());
  EXPECT_EQ(std::memcmp(changed->test_pred.data(),reference.test_pred.data(),reference.test_pred.size()*sizeof(double)),0);
}

}

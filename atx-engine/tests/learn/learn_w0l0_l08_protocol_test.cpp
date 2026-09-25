// W0-L0 — the remaining learn-side L-08 items (plan v2 §7 W0-L0; finding L-08).
//
//   * TrialCountRule: trial_count = configurations (1 per fit_* call), not
//     folds x horizons. PerFoldFitV1 keeps the legacy count.
//   * HorizonBlendIc: the §0.6 horizon-blend IC is the mean over dates of the per-date
//     cross-sectional IC of the fold-averaged OOF prediction (MeanDateIcV2), not one
//     pooled Pearson over every fold's (pred, label) pairs (PooledPearsonV1), which
//     rewards predicting the date level.
// Suite prefix LearnIcLossPerDate_ (the lane's per-date-IC suite): the blend now uses the
// same per-date statistic as the date-grouped IcLoss.

#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/engine/learn/feature_matrix.hpp"    // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"               // fit_gbt, GbtCfg
#include "atx/engine/learn/latent.hpp"            // LearnFitTrace, detail::oof_mean_date_ic
#include "atx/engine/learn/linear_alpha.hpp"      // fit_linear, LinearAlphaCfg
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor
#include "atx/engine/learn/tcn_alpha.hpp"         // fit_gru, GruAlphaCfg

namespace atx_test_w0_l0_l08_protocol {

using atx::f64;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
namespace learn = atx::engine::learn;

// Two horizons over 30 dates x 10 names. Y[0] is a cross-sectional signal on x0; Y[1]
// is the DATE LEVEL (x1, constant within a date) plus within-date noise — predictable
// across dates (market timing) but carrying no cross-sectional information.
[[nodiscard]] learn::FeatureMatrix make_fm(u64 seed) {
  learn::FeatureMatrix fm;
  fm.n_dates = 30;
  fm.n_instruments = 10;
  fm.n_features = 2;
  fm.Y.assign(2U, {});
  fm.label_horizons = {1, 2};
  atx::core::Xoshiro256pp rng{seed};
  for (usize d = 0; d < fm.n_dates; ++d) {
    const f64 level = 2.0 * rng.normal();
    for (usize i = 0; i < fm.n_instruments; ++i) {
      const f64 x0 = rng.normal();
      fm.row_date.push_back(d);
      fm.row_inst.push_back(i);
      fm.X.push_back(x0);
      fm.X.push_back(level);
      fm.row_valid.push_back(static_cast<u8>(1));
      fm.Y[0].push_back(0.5 * x0 + rng.normal());
      fm.Y[1].push_back(level + rng.normal());
    }
  }
  return fm;
}

[[nodiscard]] learn::LinearAlphaCfg linear_cfg() {
  learn::LinearAlphaCfg cfg;
  cfg.en.lambda = 1e-3;
  cfg.use_ridge_baseline = true;
  cfg.cpcv.n_groups = 5;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.master_seed = 3;
  cfg.horizons = {1, 2};
  return cfg;
}

// ---------------------------------------------------------------------------
//  Trial count
// ---------------------------------------------------------------------------

TEST(LearnIcLossPerDate_TrialCount, OneConfigurationIsOneTrial) {
  const learn::FeatureMatrix fm = make_fm(1);
  learn::LinearAlphaCfg cfg = linear_cfg();
  const learn::LatentAugmentation aug;
  EXPECT_EQ(learn::fit_linear(fm, aug, cfg).trial_count, 1U);
  cfg.protocol.trials = learn::TrialCountRule::PerFoldFitV1;
  EXPECT_EQ(learn::fit_linear(fm, aug, cfg).trial_count, 5U * 2U) << "legacy: folds x horizons";

  learn::GbtCfg g;
  g.cpcv = cfg.cpcv;
  g.horizons = {1, 2};
  EXPECT_EQ(learn::fit_gbt(fm, aug, g).trial_count, 1U);
  g.protocol.trials = learn::TrialCountRule::PerFoldFitV1;
  EXPECT_EQ(learn::fit_gbt(fm, aug, g).trial_count, 10U);
}

TEST(LearnIcLossPerDate_TrialCount, SequenceFitCountsOneConfiguration) {
  learn::SequenceTensor st;
  st.lookback = 2;
  st.n_features = 1;
  st.y.assign(1U, {});
  atx::core::Xoshiro256pp rng{4};
  for (usize d = 1; d < 16U; ++d) {
    for (usize i = 0; i < 3U; ++i) {
      const f64 a = rng.normal();
      st.x.push_back(rng.normal());
      st.x.push_back(a);
      st.y[0].push_back(a);
      st.date_of.push_back(d);
      st.inst_of.push_back(i);
      st.sample_valid.push_back(static_cast<u8>(1));
      ++st.n_samples;
    }
  }
  learn::GruAlphaCfg cfg;
  cfg.hidden = 3;
  cfg.dropout = 0.0;
  cfg.cpcv.n_groups = 3;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1};
  cfg.train.epochs = 1;
  cfg.train.ensemble_size = 1;
  const auto v2 = learn::fit_gru(st, cfg);
  ASSERT_TRUE(v2.has_value());
  EXPECT_EQ(v2->trial_count, 1U);
  cfg.protocol.common.trials = learn::TrialCountRule::PerFoldFitV1;
  const auto v1 = learn::fit_gru(st, cfg);
  ASSERT_TRUE(v1.has_value());
  EXPECT_EQ(v1->trial_count, 3U);
}

TEST(LearnIcLossPerDate_TrialCount, NoFitIsZeroTrials) {
  EXPECT_EQ(learn::detail::protocol_trial_count(learn::TrialCountRule::PerConfigurationV2, 0U), 0U);
  EXPECT_EQ(learn::detail::protocol_trial_count(learn::TrialCountRule::PerConfigurationV2, 9U), 1U);
  EXPECT_EQ(learn::detail::protocol_trial_count(learn::TrialCountRule::PerFoldFitV1, 9U), 9U);
}

// ---------------------------------------------------------------------------
//  Horizon blend IC
// ---------------------------------------------------------------------------

// Rebuild both blend statistics from the trace (every fold's per-key OOS predictions)
// and check fit_linear used exactly the one its rule names.
TEST(LearnIcLossPerDate_HorizonBlend, BlendWeightsAreTheRuleStatistic) {
  const learn::FeatureMatrix fm = make_fm(2);
  const learn::LatentAugmentation aug;
  for (const learn::HorizonBlendIc rule :
       {learn::HorizonBlendIc::MeanDateIcV2, learn::HorizonBlendIc::PooledPearsonV1}) {
    learn::LinearAlphaCfg cfg = linear_cfg();
    cfg.protocol.blend_ic = rule;
    learn::LearnFitTrace t;
    const learn::LearnedModel m = learn::fit_linear(fm, aug, cfg, &t);
    std::vector<f64> ic(2, 0.0);
    for (usize h = 0; h < 2U; ++h) {
      std::vector<f64> sum(fm.n_rows(), 0.0);
      std::vector<u32> cnt(fm.n_rows(), 0U);
      std::vector<f64> pooled_p;
      std::vector<f64> pooled_l;
      for (const learn::LearnFoldRecord &r : t.folds) {
        if (r.horizon_idx != h) {
          continue;
        }
        for (usize i = 0; i < r.test_keys.size(); ++i) {
          sum[r.test_keys[i]] += r.test_pred[i];
          cnt[r.test_keys[i]] += 1U;
          pooled_p.push_back(r.test_pred[i]);
          pooled_l.push_back(fm.Y[h][r.test_keys[i]]);
        }
      }
      ic[h] = (rule == learn::HorizonBlendIc::MeanDateIcV2)
                  ? learn::detail::oof_mean_date_ic(
                        std::span<const usize>{fm.row_date}, std::span<const f64>{fm.Y[h]},
                        std::span<const f64>{sum}, std::span<const u32>{cnt})
                  : learn::detail::pearson(std::span<const f64>{pooled_p},
                                           std::span<const f64>{pooled_l});
    }
    const f64 w0 = std::fmax(ic[0], 0.0);
    const f64 w1 = std::fmax(ic[1], 0.0);
    ASSERT_GT(w0 + w1, 0.0);
    EXPECT_DOUBLE_EQ(m.blend_w[0], w0 / (w0 + w1)) << "rule " << static_cast<int>(rule);
    EXPECT_DOUBLE_EQ(m.blend_w[1], w1 / (w0 + w1)) << "rule " << static_cast<int>(rule);
  }
}

TEST(LearnIcLossPerDate_HorizonBlend, DateLevelHorizonEarnsNoCrossSectionalWeight) {
  const learn::FeatureMatrix fm = make_fm(2);
  const learn::LatentAugmentation aug;
  learn::LinearAlphaCfg cfg = linear_cfg();
  const learn::LearnedModel v2 = learn::fit_linear(fm, aug, cfg);
  cfg.protocol.blend_ic = learn::HorizonBlendIc::PooledPearsonV1;
  const learn::LearnedModel v1 = learn::fit_linear(fm, aug, cfg);
  // Horizon 1 is pure market timing: pooled Pearson rewards it, the per-date IC does not.
  EXPECT_GT(v1.blend_w[1], 0.5) << "legacy pooled IC favours the date-level horizon";
  EXPECT_LT(v2.blend_w[1], 0.25) << "per-date IC sees no cross-sectional skill in it";
  EXPECT_GT(v2.blend_w[0], v2.blend_w[1]);
}

TEST(LearnIcLossPerDate_HorizonBlend, MeanDateIcHandlesUnsortedKeysAndThinDates) {
  // Keys out of date order; date 5 has one finite pair (skipped); a NaN label skipped.
  const std::vector<usize> date{3, 1, 3, 1, 5, 1, 3};
  const std::vector<f64> pred{1.0, 1.0, 2.0, 2.0, 9.0, 3.0, 3.0};
  const std::vector<f64> lab{1.0, 3.0, 2.0, 2.0, 9.0, 1.0, std::nan("")};
  // date 1: pred (1,2,3) vs label (3,2,1) -> -1; date 3: (1,2) vs (1,2) -> +1.
  EXPECT_DOUBLE_EQ(learn::detail::mean_date_ic(std::span<const usize>{date},
                                               std::span<const f64>{pred},
                                               std::span<const f64>{lab}),
                   0.0);
  EXPECT_EQ(learn::detail::mean_date_ic({}, {}, {}), 0.0);
}

} // namespace atx_test_w0_l0_l08_protocol

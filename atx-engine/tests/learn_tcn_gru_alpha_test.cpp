// atx::engine::learn — fit_tcn / fit_gru (p2 S5-2b-ii) tests.
//
// TDD pins for the TCN + GRU-lite SEQUENCE alphas. The unit mirrors fit_linear:
// a deflation-gated, CPCV-firewall-fit, multi-horizon learned alpha — only the
// per-fold "fit" is a deterministic seed-ensemble net instead of a linear solve,
// and the data is a SequenceTensor (windows) instead of a FeatureMatrix (rows).
//
// These tests assert exactly the load-bearing properties:
//   - happy path: Ok, kind Tcn/Gru, non-empty member_states (== ensemble_size),
//     non-empty oos_score_series, trial_count > 0, blend_w normalized.
//   - R1 two-builds: same master_seed -> byte-identical member_states AND
//     identical predict_nn on a fixed window (the determinism proof).
//   - R2 causal / truncation-invariance: predict_nn on a shared sample is
//     bit-identical whether or not later dates exist in the source panel.
//   - seed-ensemble determinism (R8/R1): predict_nn == ascending mean of the
//     per-member forwards.
//   - planted-signal non-vacuous: a clean signal -> positive OOS skill series;
//     pure noise -> ~0 / negative (the deflation has something to reject).
//   - errors: empty / F=0 / L=0 -> Err(InvalidArgument).
//   - predict_blended dispatch: a Tcn/Gru routed through predict_blended equals
//     predict_nn (the no-default switch arm works).
//
// Suite name is `LearnTcnGruAlpha`. FIXTURE STRATEGY: construct SequenceTensor
// DIRECTLY (full control over the window bytes + planted labels), exactly as the
// S5-1 build_sequences tests construct FeatureMatrix directly.

#include <cmath>   // std::isfinite
#include <cstring> // std::memcmp
#include <limits>  // std::numeric_limits (quiet NaN — off-panel-end label)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"  // ErrorCode
#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u8, u16, u64, usize

#include "atx/engine/learn/learned_source.hpp"    // LearnedModel, ModelKind, predict_nn, predict_blended
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor
#include "atx/engine/learn/tcn_alpha.hpp"         // fit_tcn, fit_gru, TcnAlphaCfg, GruAlphaCfg

namespace {

using atx::f64;
using atx::u16;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::engine::learn::fit_gru;
using atx::engine::learn::fit_tcn;
using atx::engine::learn::GruAlphaCfg;
using atx::engine::learn::LearnedModel;
using atx::engine::learn::ModelKind;
using atx::engine::learn::predict_blended;
using atx::engine::learn::predict_nn;
using atx::engine::learn::SequenceTensor;
using atx::engine::learn::TcnAlphaCfg;

// ---------------------------------------------------------------------------
//  Build a small synthetic SequenceTensor directly. n_dates x n_inst anchors,
//  each a complete L x F window. The window feature (date d, inst i, step l,
//  feat f) is a smooth + seeded-noise value; the horizon-h label is either a
//  CLEAN function of the window's trailing step (planted == true) or pure noise.
// ---------------------------------------------------------------------------
struct SynthCfg {
  usize n_dates;
  usize n_inst;
  usize L;
  usize F;
  usize n_horizons;
  bool planted; // true: label is a clean fn of the window; false: pure noise
  u64 seed;
};

SequenceTensor make_seq(const SynthCfg &c) {
  SequenceTensor st;
  st.lookback = c.L;
  st.n_features = c.F;
  st.y.assign(c.n_horizons, {});
  atx::core::Xoshiro256pp rng{c.seed};
  // Anchors with a full L-deep trailing window: dates [L-1, n_dates).
  for (usize d = c.L - 1; d < c.n_dates; ++d) {
    for (usize i = 0; i < c.n_inst; ++i) {
      // Pack the window in ascending (l, f) order (idx(l,f) = l*F + f).
      std::vector<f64> window(c.L * c.F, 0.0);
      for (usize l = 0; l < c.L; ++l) {
        const usize wd = d - (c.L - 1) + l; // window date for step l
        for (usize f = 0; f < c.F; ++f) {
          const f64 smooth = 0.1 * static_cast<f64>(wd) + 0.3 * static_cast<f64>(i) +
                             0.05 * static_cast<f64>(f);
          window[l * c.F + f] = smooth + 0.2 * rng.normal();
        }
      }
      // Planted signal: the trailing step's feature-0 drives the label (a clean,
      // monotone, cross-sectionally informative target). Pure-noise variant: the
      // label is independent of the window.
      const f64 trailing0 = window[(c.L - 1) * c.F + 0];
      for (usize h = 0; h < c.n_horizons; ++h) {
        const f64 label = c.planted ? (trailing0 + 0.01 * static_cast<f64>(h)) : rng.normal();
        st.y[h].push_back(label);
      }
      for (usize j = 0; j < window.size(); ++j) {
        st.x.push_back(window[j]);
      }
      st.date_of.push_back(d);
      st.inst_of.push_back(i);
      st.sample_valid.push_back(static_cast<u8>(1));
      ++st.n_samples;
    }
  }
  return st;
}

// A tiny, fast, deterministic config shared by most tests (R6: tiny net; few
// epochs so the suite is quick but the fit still moves).
TcnAlphaCfg tiny_tcn_cfg() {
  TcnAlphaCfg cfg;
  cfg.blocks = 2;
  cfg.kernel = 2;
  cfg.channels = 6;
  cfg.dropout = 0.0; // deterministic + no per-call RNG divergence in tests
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1, 2};
  cfg.train.epochs = 12;
  cfg.train.batch_size = 16;
  cfg.train.ckpt_every = 4;
  cfg.train.ensemble_size = 2;
  cfg.train.master_seed = 4242;
  return cfg;
}

GruAlphaCfg tiny_gru_cfg() {
  GruAlphaCfg cfg;
  cfg.hidden = 6;
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
  return cfg;
}

// A standardized window row for predict_nn (here standardization is identity, so
// the row is just the verbatim flattened window of one sample).
std::vector<f64> window_of(const SequenceTensor &st, usize sample) {
  const usize wlen = st.lookback * st.n_features;
  std::vector<f64> row(wlen);
  for (usize j = 0; j < wlen; ++j) {
    row[j] = st.x[sample * wlen + j];
  }
  return row;
}

constexpr f64 kBlendTol = 1e-12;

// =====================================================================
//  Happy path — fit_tcn.
// =====================================================================
TEST(LearnTcnGruAlpha, FitTcn_PlantedSignal_ReturnsWellFormedModel) {
  const SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 11});
  const TcnAlphaCfg cfg = tiny_tcn_cfg();

  const auto r = fit_tcn(st, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const LearnedModel &m = *r;

  EXPECT_EQ(m.kind, ModelKind::Tcn);
  EXPECT_EQ(m.nn.lookback, st.lookback);
  EXPECT_EQ(m.nn.n_seq_features, st.n_features);
  EXPECT_EQ(m.n_base_features, st.lookback * st.n_features);
  EXPECT_EQ(m.augmented_dim(), st.lookback * st.n_features) << "aug empty => augmented_dim == L*F";
  // Deployed ensemble: one serialized state per member.
  EXPECT_EQ(m.nn.member_states.size(), cfg.train.ensemble_size);
  EXPECT_FALSE(m.oos_score_series.empty()) << "the gate needs a non-empty OOS series";
  EXPECT_GT(m.trial_count, 0U) << "every fold fit must bump the deflation N";

  // blend_w normalized (sum 1, all >= 0).
  ASSERT_EQ(m.blend_w.size(), cfg.horizons.size());
  f64 sum = 0.0;
  for (const f64 w : m.blend_w) {
    EXPECT_GE(w, 0.0);
    sum += w;
  }
  EXPECT_NEAR(sum, 1.0, kBlendTol);
}

// =====================================================================
//  Happy path — fit_gru.
// =====================================================================
TEST(LearnTcnGruAlpha, FitGru_PlantedSignal_ReturnsWellFormedModel) {
  const SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 13});
  const GruAlphaCfg cfg = tiny_gru_cfg();

  const auto r = fit_gru(st, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const LearnedModel &m = *r;

  EXPECT_EQ(m.kind, ModelKind::Gru);
  EXPECT_EQ(m.nn.member_states.size(), cfg.train.ensemble_size);
  EXPECT_FALSE(m.oos_score_series.empty());
  EXPECT_GT(m.trial_count, 0U);
  f64 sum = 0.0;
  for (const f64 w : m.blend_w) {
    sum += w;
  }
  EXPECT_NEAR(sum, 1.0, kBlendTol);
}

// =====================================================================
//  R1 — two builds with the same master_seed are byte-identical AND predict the
//  same scalar on a fixed window (the determinism proof).
// =====================================================================
TEST(LearnTcnGruAlpha, R1_TwoBuilds_ByteIdenticalStatesAndPredict) {
  const SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 17});
  const TcnAlphaCfg cfg = tiny_tcn_cfg();

  const auto a = fit_tcn(st, cfg);
  const auto b = fit_tcn(st, cfg);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a->nn.member_states.size(), b->nn.member_states.size());
  for (usize mi = 0; mi < a->nn.member_states.size(); ++mi) {
    ASSERT_EQ(a->nn.member_states[mi].size(), b->nn.member_states[mi].size());
    EXPECT_EQ(std::memcmp(a->nn.member_states[mi].data(), b->nn.member_states[mi].data(),
                          a->nn.member_states[mi].size() * sizeof(f64)),
              0)
        << "member " << mi << " not byte-identical across builds";
  }
  // Identical predict_nn on a fixed window.
  const std::vector<f64> row = window_of(st, 0);
  const f64 pa = predict_nn(*a, std::span<const f64>{row});
  const f64 pb = predict_nn(*b, std::span<const f64>{row});
  EXPECT_EQ(pa, pb) << "predict_nn must be byte-deterministic across builds";
}

// =====================================================================
//  R2 — truncation invariance: predict_nn on a sample anchored at date <= t_cut
//  is bit-identical whether or not later dates exist in the source panel. The
//  windows for date-<=t samples are unchanged (S5-1) and the model is causal, so
//  the deployed model fit on a fixed master_seed produces the same prediction.
//
//  We fit on the TRUNCATED panel (the deployed model is what ships), then assert
//  predict_nn on a shared sample's window equals the prediction on the same
//  window bytes drawn from the FULL panel — i.e. the window is invariant and so
//  is the (causal) forward.
// =====================================================================
TEST(LearnTcnGruAlpha, R2_TruncationInvariant_PredictBitIdentical) {
  const usize L = 3, Fdim = 2, H = 2, t_cut = 5;
  const SequenceTensor full = make_seq({9, 3, L, Fdim, H, /*planted=*/true, 23});
  // Truncated build: identical generator + shape, fewer dates. Because make_seq
  // draws per (date,inst) in ascending order with the SAME seed, the window bytes
  // of every shared (date<=t_cut, inst) sample are byte-identical to the full
  // build (the trailing window never reads a future date — R2 from S5-1).
  const SequenceTensor trunc = make_seq({t_cut + 1, 3, L, Fdim, H, /*planted=*/true, 23});

  // Pick a sample present in BOTH builds (anchor date <= t_cut). Sample 0 anchors
  // at date L-1 <= t_cut in both, same inst.
  const usize s_full = 0, s_trunc = 0;
  ASSERT_EQ(full.date_of[s_full], trunc.date_of[s_trunc]);
  ASSERT_EQ(full.inst_of[s_full], trunc.inst_of[s_trunc]);
  const std::vector<f64> wf = window_of(full, s_full);
  const std::vector<f64> wt = window_of(trunc, s_trunc);
  ASSERT_EQ(wf.size(), wt.size());
  for (usize j = 0; j < wf.size(); ++j) {
    ASSERT_EQ(wf[j], wt[j]) << "window byte differs at j=" << j << " — S5-1 trailing invariant broke";
  }

  // Deploy a model (fixed seed) and predict the shared window from both byte
  // sources — they must agree exactly (the forward is a pure fn of the window).
  TcnAlphaCfg cfg = tiny_tcn_cfg();
  cfg.cpcv.n_groups = 3;
  const auto r = fit_tcn(trunc, cfg);
  ASSERT_TRUE(r.has_value());
  const f64 pf = predict_nn(*r, std::span<const f64>{wf});
  const f64 pt = predict_nn(*r, std::span<const f64>{wt});
  EXPECT_EQ(pf, pt) << "causal predict must be invariant to the (identical) window bytes' source";
}

// =====================================================================
//  Seed-ensemble determinism (R8/R1): predict_nn == ascending mean of the
//  per-member forwards. We reload each member individually and average.
// =====================================================================
TEST(LearnTcnGruAlpha, SeedEnsemble_PredictEqualsAscendingMemberMean) {
  SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 29});
  TcnAlphaCfg cfg = tiny_tcn_cfg();
  cfg.train.ensemble_size = 3; // > 1 so the mean is non-trivial
  const auto r = fit_tcn(st, cfg);
  ASSERT_TRUE(r.has_value());
  const LearnedModel &m = *r;
  ASSERT_EQ(m.nn.member_states.size(), 3U);

  const std::vector<f64> row = window_of(st, 0);
  const f64 ensemble = predict_nn(m, std::span<const f64>{row});

  // Build single-member models (same payload, one member each) and average their
  // predict_nn outputs ascending — must equal the full ensemble forward.
  f64 acc = 0.0;
  for (usize mi = 0; mi < m.nn.member_states.size(); ++mi) {
    LearnedModel one = m;
    one.nn.member_states = {m.nn.member_states[mi]};
    acc += predict_nn(one, std::span<const f64>{row});
  }
  const f64 mean = acc / static_cast<f64>(m.nn.member_states.size());
  EXPECT_NEAR(ensemble, mean, 1e-9) << "ensemble forward must be the ascending-member mean";
}

// =====================================================================
//  predict_blended dispatch: a Tcn/Gru routed through predict_blended equals
//  predict_nn (the no-default switch arm works). The augmented row IS the window.
// =====================================================================
TEST(LearnTcnGruAlpha, PredictBlended_DispatchesToPredictNn) {
  const SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 31});
  const auto rt = fit_tcn(st, tiny_tcn_cfg());
  const auto rg = fit_gru(st, tiny_gru_cfg());
  ASSERT_TRUE(rt.has_value());
  ASSERT_TRUE(rg.has_value());
  const std::vector<f64> row = window_of(st, 0);

  EXPECT_EQ(predict_blended(*rt, std::span<const f64>{row}),
            predict_nn(*rt, std::span<const f64>{row}))
      << "Tcn predict_blended must route to predict_nn";
  EXPECT_EQ(predict_blended(*rg, std::span<const f64>{row}),
            predict_nn(*rg, std::span<const f64>{row}))
      << "Gru predict_blended must route to predict_nn";
}

// =====================================================================
//  L2 regularization is ACTIVE (R8): fitting the same data with a large l2 must
//  produce a SMALLER deployed-weight L2-norm than l2=0 (decoupled weight-decay
//  shrinks weights). Proves cfg.l2 is wired through fit_* -> nn::TrainConfig.l2 ->
//  the Trainer's per-step penalty, not a silently-ignored knob.
// =====================================================================
TEST(LearnTcnGruAlpha, L2Penalty_ShrinksDeployedWeightNorm) {
  const SequenceTensor st = make_seq({12, 5, 3, 2, 2, /*planted=*/true, 53});

  // Sum of squares of the deployed first member's serialized params (its weight
  // L2-norm²). A larger penalty must drive this strictly down.
  const auto weight_sq = [](const LearnedModel &m) -> f64 {
    f64 s = 0.0;
    for (const f64 p : m.nn.member_states[0]) {
      s += p * p;
    }
    return s;
  };

  TcnAlphaCfg base = tiny_tcn_cfg();
  base.train.epochs = 40;        // enough steps for decay to bite
  base.train.ensemble_size = 1;  // one member -> a clean single-net comparison
  base.train.master_seed = 909;  // identical init across the two arms

  TcnAlphaCfg none = base;
  none.l2 = 0.0;
  TcnAlphaCfg heavy = base;
  heavy.l2 = 0.1; // a large decay relative to the 0.01 lr

  const auto r0 = fit_tcn(st, none);
  const auto rL = fit_tcn(st, heavy);
  ASSERT_TRUE(r0.has_value());
  ASSERT_TRUE(rL.has_value());
  ASSERT_FALSE(r0->nn.member_states.empty());
  ASSERT_FALSE(rL->nn.member_states.empty());

  EXPECT_LT(weight_sq(*rL), weight_sq(*r0))
      << "L2 weight-decay must shrink the deployed weight norm (l2=0.1 vs l2=0)";
}

// =====================================================================
//  Zero-weight-horizon NaN label must NOT drop a sample from the deployed fit
//  (the 0.0 * NaN = NaN guard). Horizon 1's labels are ALL NaN (its forward
//  return runs off the panel end), so its OOS IC is 0 and §0.6 sets blend_w[1] =
//  0; horizon 0 carries the planted signal (blend_w[0] > 0). The blended deployed
//  target must therefore be FINITE for every valid sample (the zero-weight NaN
//  horizon is skipped, not multiplied), so the deployed ensemble still trains and
//  predict_nn is finite. Without the fix the NaN poisons every target, nfull == 0,
//  and member_states is empty.
// =====================================================================
TEST(LearnTcnGruAlpha, ZeroWeightHorizonNaNLabel_DoesNotDropDeployedSamples) {
  SequenceTensor st = make_seq({12, 5, 3, 2, 2, /*planted=*/true, 67});
  // Poison horizon 1's labels with NaN (a legitimate off-panel-end forward return
  // for a sample whose window is still valid). Horizon 0 stays finite + planted.
  const f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
  for (f64 &v : st.y[1]) {
    v = kNaN;
  }

  const auto r = fit_tcn(st, tiny_tcn_cfg());
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const LearnedModel &m = *r;

  // Horizon 1 (all-NaN labels) earns zero OOS IC -> zero blend weight; horizon 0
  // carries the signal. The deployed ensemble must still exist (samples not
  // dropped) and predict a finite scalar.
  EXPECT_DOUBLE_EQ(m.blend_w[1], 0.0) << "all-NaN horizon must earn zero blend weight";
  EXPECT_GT(m.blend_w[0], 0.0) << "the finite planted horizon must carry the blend";
  ASSERT_FALSE(m.nn.member_states.empty())
      << "the deployed fit must not be emptied by a zero-weight NaN horizon";
  const std::vector<f64> row = window_of(st, 0);
  EXPECT_TRUE(std::isfinite(predict_nn(m, std::span<const f64>{row})))
      << "predict_nn must be finite when the nonzero-weight target is finite";
}

// =====================================================================
//  Planted-signal non-vacuous: a clean signal -> positive mean OOS skill series;
//  pure noise -> mean ~0 / negative. Proves the series carries genuine OOS skill
//  (the deflation gate has real signal to score), not an in-sample artefact.
// =====================================================================
TEST(LearnTcnGruAlpha, PlantedSignal_OosSeriesPositive_NoiseNonPositive) {
  TcnAlphaCfg cfg = tiny_tcn_cfg();
  cfg.train.epochs = 30; // a few more epochs so the planted signal is learnable
  cfg.train.ensemble_size = 2;

  const auto mean_series = [](const LearnedModel &m) -> f64 {
    if (m.oos_score_series.empty()) {
      return 0.0;
    }
    f64 s = 0.0;
    for (const f64 v : m.oos_score_series) {
      s += v;
    }
    return s / static_cast<f64>(m.oos_score_series.size());
  };

  const SequenceTensor planted = make_seq({14, 6, 3, 2, 2, /*planted=*/true, 37});
  const SequenceTensor noise = make_seq({14, 6, 3, 2, 2, /*planted=*/false, 41});
  const auto rp = fit_tcn(planted, cfg);
  const auto rn = fit_tcn(noise, cfg);
  ASSERT_TRUE(rp.has_value());
  ASSERT_TRUE(rn.has_value());

  const f64 mp = mean_series(*rp);
  const f64 mn = mean_series(*rn);
  EXPECT_GT(mp, 0.0) << "a clean planted signal must yield a positive mean OOS IC";
  // The noise panel must not show MORE skill than the planted one (the gate's
  // honest floor); a light, non-flaky inequality rather than a hard noise<=0.
  EXPECT_LT(mn, mp) << "pure noise must show less OOS skill than the planted signal";
}

// =====================================================================
//  Errors.
// =====================================================================
TEST(LearnTcnGruAlpha, EmptyTensor_ReturnsInvalidArgument) {
  SequenceTensor st; // n_samples == 0
  st.lookback = 3;
  st.n_features = 2;
  st.y.assign(2, {});
  const auto r = fit_tcn(st, tiny_tcn_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnTcnGruAlpha, ZeroFeatures_ReturnsInvalidArgument) {
  SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 43});
  st.n_features = 0; // poison F
  const auto r = fit_gru(st, tiny_gru_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnTcnGruAlpha, ZeroLookback_ReturnsInvalidArgument) {
  SequenceTensor st = make_seq({10, 4, 3, 2, 2, /*planted=*/true, 47});
  st.lookback = 0; // poison L
  const auto r = fit_tcn(st, tiny_tcn_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

} // namespace

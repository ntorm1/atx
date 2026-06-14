// atx::engine::learn — fit_autoencoder_factors (p2 S5-3b) tests.
//
// TDD pins for the AUTOENCODER statistical-factor alpha (Gu-Kelly-Xiu): a trailing-
// fit autoencoder that compresses the F-dim CROSS-SECTIONAL feature matrix (the
// last timestep of each valid window) into K latent factors, deployed encoder-only
// so an OOS feature row maps to its leading latent factor. It is the FACTOR track —
// orthogonal to the TCN/GRU/Attn predictive track.
//
// Load-bearing properties pinned here:
//   - R7 LINEAR-AE -> PCA pin: a centered no-bias linear AE recovers pca(X,K) —
//     (a) its rank-K reconstruction MSE matches PCA's, (b) the encoder's K-subspace
//     aligns with pca().components (projection-matrix / Frobenius tolerance). Raw
//     weights are NOT compared (the linear latent is only determined up to a K×K
//     transform — only the SUBSPACE and the RECONSTRUCTION are invariant).
//   - R2 trailing-fit / OOS-encode: predict_ae on a sample is invariant to data
//     added AFTER that sample's date (the AE is fit on the trailing window).
//   - R1 two-builds: same master_seed -> byte-identical member_states + identical
//     predict_ae on a fixed row.
//   - L1 active: l1=0 vs l1=large -> the fitted member weight L2-norm shrinks.
//   - predict_blended dispatch: a kind==Autoencoder model routes to predict_ae.
//   - errors: n_valid<2 / F=0 / k_factors=0 / k_factors>F -> Err(InvalidArgument).
//
// Suite name is `LearnAutoencoderAlpha`. FIXTURE STRATEGY: construct SequenceTensor
// DIRECTLY with genuine LOW-RANK structure (a few latent factors + small noise) so
// PCA and the AE have real signal to compress.

#include <cmath>   // std::isfinite, std::sqrt
#include <cstring> // std::memcmp
#include <span>
#include <vector>

#include <Eigen/Dense> // Eigen::Index

#include <gtest/gtest.h>

#include "atx/core/error.hpp"  // ErrorCode
#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"  // f64, u8, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX, as_matrix
#include "atx/core/linalg/pca.hpp"    // pca, transform, PcaResult

#include "atx/engine/learn/autoencoder_alpha.hpp" // fit_autoencoder_factors, AeFactorCfg
#include "atx/engine/learn/learned_source.hpp"    // LearnedModel, ModelKind, predict_ae, predict_blended
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor

namespace {

using atx::f64;
using atx::i64;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::engine::learn::AeFactorCfg;
using atx::engine::learn::fit_autoencoder_factors;
using atx::engine::learn::LearnedModel;
using atx::engine::learn::ModelKind;
using atx::engine::learn::predict_ae;
using atx::engine::learn::predict_blended;
using atx::engine::learn::SequenceTensor;
namespace lin = atx::core::linalg;
using MatX = atx::core::linalg::MatX;
using atx::core::linalg::pca;

// ---------------------------------------------------------------------------
//  LowRankCfg — a synthetic SequenceTensor whose LAST-timestep cross-section X has
//  genuine RANK-`rank` structure: each anchor's trailing-step feature vector is a
//  random linear combination of `rank` shared latent loadings, plus small noise.
//  The earlier window steps are filler (the AE only reads the trailing step), so
//  only the last step carries the planted low-rank signal.
// ---------------------------------------------------------------------------
struct LowRankCfg {
  usize n_dates;
  usize n_inst;
  usize L;
  usize F;
  usize rank;  // true latent rank of the trailing-step cross-section
  f64 noise;   // additive Gaussian noise on the trailing step
  u64 seed;
};

SequenceTensor make_lowrank_seq(const LowRankCfg &c) {
  SequenceTensor st;
  st.lookback = c.L;
  st.n_features = c.F;
  st.y.assign(1, {}); // a single trivial horizon (the AE is unsupervised)
  atx::core::Xoshiro256pp rng{c.seed};

  // Fixed loadings B (rank x F): the shared latent basis the trailing cross-section
  // lives in. Drawn once so every anchor's trailing step is a combination of these.
  std::vector<std::vector<f64>> B(c.rank, std::vector<f64>(c.F, 0.0));
  for (usize r = 0; r < c.rank; ++r) {
    for (usize f = 0; f < c.F; ++f) {
      B[r][f] = rng.normal();
    }
  }

  for (usize d = c.L - 1; d < c.n_dates; ++d) {
    for (usize i = 0; i < c.n_inst; ++i) {
      std::vector<f64> window(c.L * c.F, 0.0);
      // Filler for the non-trailing steps (the AE never reads them).
      for (usize l = 0; l + 1 < c.L; ++l) {
        for (usize f = 0; f < c.F; ++f) {
          window[l * c.F + f] = 0.01 * rng.normal();
        }
      }
      // Trailing step: a random rank-`rank` combination of the loadings + noise.
      std::vector<f64> scores(c.rank);
      for (usize r = 0; r < c.rank; ++r) {
        scores[r] = rng.normal();
      }
      for (usize f = 0; f < c.F; ++f) {
        f64 v = 0.0;
        for (usize r = 0; r < c.rank; ++r) {
          v += scores[r] * B[r][f];
        }
        window[(c.L - 1) * c.F + f] = v + c.noise * rng.normal();
      }
      st.y[0].push_back(rng.normal()); // unused by the AE (unsupervised)
      for (const f64 v : window) {
        st.x.push_back(v);
      }
      st.date_of.push_back(d);
      st.inst_of.push_back(i);
      st.sample_valid.push_back(static_cast<u8>(1));
      ++st.n_samples;
    }
  }
  return st;
}

// Pull the F-dim trailing-step design X (n_valid x F) the AE fits on, directly from
// a SequenceTensor — the impl-independent reference for the PCA pin.
MatX design_of(const SequenceTensor &st) {
  const usize L = st.lookback, F = st.n_features;
  MatX X(static_cast<Eigen::Index>(st.n_samples), static_cast<Eigen::Index>(F));
  for (usize s = 0; s < st.n_samples; ++s) {
    const usize base = (s * L + (L - 1)) * F;
    for (usize f = 0; f < F; ++f) {
      X(static_cast<Eigen::Index>(s), static_cast<Eigen::Index>(f)) = st.x[base + f];
    }
  }
  return X;
}

// The trailing-step feature row of one sample (the predict_ae input).
std::vector<f64> feature_row_of(const SequenceTensor &st, usize sample) {
  const usize L = st.lookback, F = st.n_features;
  std::vector<f64> row(F);
  const usize base = (sample * L + (L - 1)) * F;
  for (usize f = 0; f < F; ++f) {
    row[f] = st.x[base + f];
  }
  return row;
}

// Orthogonal projector P = W (WᵀW)⁻¹ Wᵀ onto the column space of W (F x K). The
// K-subspace invariant the linear-AE / PCA pin compares (sign-and-rotation
// tolerant: P depends only on col(W), not the basis).
MatX projector(const MatX &W) {
  const MatX gram = W.transpose() * W;             // K x K
  return W * gram.inverse() * W.transpose();        // F x F
}

// Mean reconstruction error of the centered design Xc onto a K-subspace projector P:
// (1/(n*F)) Σ ||Xc_r - Xc_r·P||². The Eckart-Young invariant both PCA and the
// converged linear AE attain at the optimal K-subspace.
f64 recon_mse(const MatX &Xc, const MatX &P) {
  const MatX resid = Xc - Xc * P; // n x F
  const f64 n = static_cast<f64>(Xc.rows()) * static_cast<f64>(Xc.cols());
  return resid.squaredNorm() / n;
}

// A tiny, fast, deterministic LINEAR-AE config (the PCA pin): no hidden layers, no
// regularization, single member, full-batch, many epochs so it converges to the
// PCA subspace.
AeFactorCfg linear_pin_cfg(usize k) {
  AeFactorCfg cfg;
  cfg.k_factors = k;
  cfg.beta_hidden = {};   // LINEAR AE
  cfg.l1 = 0.0;           // R7 pin: no penalties
  cfg.l2 = 0.0;
  cfg.train.epochs = 4000;
  cfg.train.batch_size = 4096; // full-batch (>= n)
  cfg.train.ckpt_every = 100;
  cfg.train.ensemble_size = 1;
  cfg.train.master_seed = 7;
  return cfg;
}

// A tiny config for the non-pin behavioural tests (R1/R2/L1/dispatch).
AeFactorCfg tiny_cfg() {
  AeFactorCfg cfg;
  cfg.k_factors = 2;
  cfg.beta_hidden = {};
  cfg.l1 = 0.0;
  cfg.l2 = 0.0;
  cfg.train.epochs = 60;
  cfg.train.batch_size = 32;
  cfg.train.ckpt_every = 10;
  cfg.train.ensemble_size = 2;
  cfg.train.master_seed = 4242;
  return cfg;
}

// =====================================================================
//  Happy path — well-formed deployed model.
// =====================================================================
TEST(LearnAutoencoderAlpha, FitAutoencoder_ReturnsWellFormedModel) {
  const SequenceTensor st = make_lowrank_seq({12, 6, 3, 5, 2, 0.05, 11});
  const auto r = fit_autoencoder_factors(st, tiny_cfg());
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const LearnedModel &m = *r;

  EXPECT_EQ(m.kind, ModelKind::Autoencoder);
  EXPECT_EQ(m.n_base_features, st.n_features) << "aug empty => n_base_features == F";
  EXPECT_EQ(m.augmented_dim(), st.n_features);
  EXPECT_EQ(m.feat_mean.size(), st.n_features);
  EXPECT_EQ(m.feat_sd.size(), st.n_features);
  for (const f64 sd : m.feat_sd) {
    EXPECT_DOUBLE_EQ(sd, 1.0) << "centering only — unit sd";
  }
  EXPECT_EQ(m.nn.member_states.size(), 2U); // one state per ensemble member
  EXPECT_EQ(m.trial_count, 2U) << "trial_count == number of distinct fits (the ensemble)";
  // arch_dims = {F, K, n_hidden} with no hidden layers.
  ASSERT_EQ(m.nn.arch_dims.size(), 3U);
  EXPECT_EQ(m.nn.arch_dims[0], st.n_features);
  EXPECT_EQ(m.nn.arch_dims[1], tiny_cfg().k_factors);
  EXPECT_EQ(m.nn.arch_dims[2], 0U);
  // predict_ae returns a finite scalar on a real row.
  const std::vector<f64> row = feature_row_of(st, 0);
  EXPECT_TRUE(std::isfinite(predict_ae(m, std::span<const f64>{row})));
}

// =====================================================================
//  R7 — the LINEAR-AE -> PCA pin (load-bearing). A centered no-bias linear AE
//  recovers pca(X, K): (a) reconstruction MSE matches PCA's rank-K reconstruction,
//  (b) the encoder K-subspace aligns with pca().components.
// =====================================================================
TEST(LearnAutoencoderAlpha, R7_LinearAe_RecoversPcaSubspaceAndReconstruction) {
  const usize K = 2;
  const SequenceTensor st = make_lowrank_seq({16, 8, 3, 5, K, 0.10, 101});
  const MatX X = design_of(st);

  // PCA reference: subspace + rank-K reconstruction of the CENTERED design.
  const auto pr = pca(X, static_cast<i64>(K));
  ASSERT_TRUE(pr.has_value()) << pr.error().to_string();
  const MatX Xc = X.rowwise() - pr->mean.transpose(); // centered (PCA's own mean)
  const MatX P_pca = projector(pr->components);        // F x F projector onto PCA K-subspace
  const f64 mse_pca = recon_mse(Xc, P_pca);

  // Fit the LINEAR AE and pull the encoder weight W (F x K) from the deployed
  // member state. The encoder is Linear(F->K, no bias): the state's leading F*K
  // scalars are W in column-major (in x out) order.
  const auto r = fit_autoencoder_factors(st, linear_pin_cfg(K));
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  const LearnedModel &m = *r;
  ASSERT_EQ(m.nn.member_states.size(), 1U);
  const std::vector<f64> &state = m.nn.member_states[0];
  ASSERT_GE(state.size(), st.n_features * K);
  const MatX W = lin::as_matrix(std::span<const f64>{state.data(), st.n_features * K},
                                static_cast<Eigen::Index>(st.n_features),
                                static_cast<Eigen::Index>(K));
  const MatX P_ae = projector(W);
  const f64 mse_ae = recon_mse(Xc, P_ae);

  // (a) reconstruction MSE matches PCA's rank-K reconstruction (impl-independent).
  EXPECT_NEAR(mse_ae, mse_pca, 1e-4 * (1.0 + mse_pca))
      << "linear-AE rank-K reconstruction must match PCA's (Eckart-Young)";
  // (b) the two K-subspaces coincide: ||P_ae - P_pca||_F ~ 0 (sign/rotation tolerant).
  const f64 dproj = (P_ae - P_pca).norm();
  EXPECT_LT(dproj, 1e-2) << "encoder K-subspace must align with pca() (projector Frobenius)";

  // The mean is the centering pin too: the AE's feat_mean == PCA's mean (both are
  // the column means of X).
  ASSERT_EQ(m.feat_mean.size(), static_cast<usize>(pr->mean.size()));
  for (usize f = 0; f < m.feat_mean.size(); ++f) {
    EXPECT_NEAR(m.feat_mean[f], pr->mean(static_cast<Eigen::Index>(f)), 1e-12)
        << "feat_mean must be the PCA centering";
  }
}

// =====================================================================
//  R1 — two builds with the same master_seed are byte-identical AND predict the
//  same scalar on a fixed row (the determinism proof).
// =====================================================================
TEST(LearnAutoencoderAlpha, R1_TwoBuilds_ByteIdenticalStatesAndPredict) {
  const SequenceTensor st = make_lowrank_seq({12, 6, 3, 5, 2, 0.05, 17});
  const AeFactorCfg cfg = tiny_cfg();

  const auto a = fit_autoencoder_factors(st, cfg);
  const auto b = fit_autoencoder_factors(st, cfg);
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
  const std::vector<f64> row = feature_row_of(st, 0);
  EXPECT_EQ(predict_ae(*a, std::span<const f64>{row}), predict_ae(*b, std::span<const f64>{row}))
      << "predict_ae must be byte-deterministic across builds";
}

// =====================================================================
//  R2 — trailing-fit / OOS-encode: predict_ae on a sample is invariant to data
//  added AFTER that sample's date. We fit on a TRUNCATED window (the deployed model
//  is what ships), then encode a shared sample's feature row drawn from BOTH the
//  truncated and the full panel — the row bytes are identical (trailing windows
//  never read a future date — S5-1) and the encoder is a pure function of the row.
// =====================================================================
TEST(LearnAutoencoderAlpha, R2_TrailingFit_OosEncodeInvariant) {
  const usize L = 3, F = 5, t_cut = 8;
  const SequenceTensor full = make_lowrank_seq({14, 4, L, F, 2, 0.05, 23});
  const SequenceTensor trunc = make_lowrank_seq({t_cut + 1, 4, L, F, 2, 0.05, 23});

  // Sample 0 anchors at date L-1 <= t_cut in BOTH builds, same inst: its trailing
  // feature row is byte-identical (the window never reads a future date).
  ASSERT_EQ(full.date_of[0], trunc.date_of[0]);
  ASSERT_EQ(full.inst_of[0], trunc.inst_of[0]);
  const std::vector<f64> rf = feature_row_of(full, 0);
  const std::vector<f64> rt = feature_row_of(trunc, 0);
  ASSERT_EQ(rf.size(), rt.size());
  for (usize j = 0; j < rf.size(); ++j) {
    ASSERT_EQ(rf[j], rt[j]) << "trailing feature byte differs at j=" << j << " — S5-1 invariant broke";
  }

  // Fit on the TRUNCATED panel (the model that ships), encode the shared row from
  // both byte sources — they must agree exactly (encode is a pure fn of the row).
  const auto r = fit_autoencoder_factors(trunc, tiny_cfg());
  ASSERT_TRUE(r.has_value());
  const f64 pf = predict_ae(*r, std::span<const f64>{rf});
  const f64 pt = predict_ae(*r, std::span<const f64>{rt});
  EXPECT_EQ(pf, pt) << "OOS encode must be invariant to the (identical) row bytes' source";
}

// =====================================================================
//  L1 active (R8): l1=0 vs l1=large -> the fitted member weight L2-norm shrinks.
//  Proves cfg.l1 is wired through fit_autoencoder_factors -> nn::TrainConfig.l1 ->
//  the Trainer's per-step subgradient, not a silently-ignored knob.
// =====================================================================
TEST(LearnAutoencoderAlpha, L1Penalty_ShrinksDeployedWeightNorm) {
  const SequenceTensor st = make_lowrank_seq({16, 8, 3, 5, 2, 0.05, 53});

  const auto weight_sq = [](const LearnedModel &m) -> f64 {
    f64 s = 0.0;
    for (const f64 p : m.nn.member_states[0]) {
      s += p * p;
    }
    return s;
  };

  AeFactorCfg base = tiny_cfg();
  base.train.epochs = 200;       // enough steps for the L1 shrink to bite
  base.train.batch_size = 4096;  // full-batch
  base.train.ensemble_size = 1;  // one member -> a clean single-net comparison
  base.train.master_seed = 909;  // identical init across the two arms

  AeFactorCfg none = base;
  none.l1 = 0.0;
  AeFactorCfg heavy = base;
  heavy.l1 = 0.05; // a large L1 relative to the 0.01 lr

  const auto r0 = fit_autoencoder_factors(st, none);
  const auto rL = fit_autoencoder_factors(st, heavy);
  ASSERT_TRUE(r0.has_value());
  ASSERT_TRUE(rL.has_value());
  ASSERT_FALSE(r0->nn.member_states.empty());
  ASSERT_FALSE(rL->nn.member_states.empty());

  EXPECT_LT(weight_sq(*rL), weight_sq(*r0))
      << "L1 penalty must shrink the deployed weight norm (l1=0.05 vs l1=0)";
}

// =====================================================================
//  predict_blended dispatch: a kind==Autoencoder model routed through
//  predict_blended equals predict_ae (the no-default switch arm works).
// =====================================================================
TEST(LearnAutoencoderAlpha, PredictBlended_DispatchesToPredictAe) {
  const SequenceTensor st = make_lowrank_seq({12, 6, 3, 5, 2, 0.05, 31});
  const auto r = fit_autoencoder_factors(st, tiny_cfg());
  ASSERT_TRUE(r.has_value());
  const std::vector<f64> row = feature_row_of(st, 0);
  EXPECT_EQ(predict_blended(*r, std::span<const f64>{row}), predict_ae(*r, std::span<const f64>{row}))
      << "Autoencoder predict_blended must route to predict_ae";
}

// =====================================================================
//  Errors.
// =====================================================================
TEST(LearnAutoencoderAlpha, EmptyTensor_ReturnsInvalidArgument) {
  SequenceTensor st; // n_samples == 0
  st.lookback = 3;
  st.n_features = 5;
  st.y.assign(1, {});
  const auto r = fit_autoencoder_factors(st, tiny_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnAutoencoderAlpha, OneValidSample_ReturnsInvalidArgument) {
  // A single valid sample: reconstruction variance is undefined (need >= 2).
  SequenceTensor st = make_lowrank_seq({3, 1, 3, 5, 2, 0.05, 59});
  ASSERT_EQ(st.n_samples, 1U); // dates [L-1, n_dates) x 1 inst == 1 anchor
  const auto r = fit_autoencoder_factors(st, tiny_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnAutoencoderAlpha, ZeroFeatures_ReturnsInvalidArgument) {
  SequenceTensor st = make_lowrank_seq({12, 6, 3, 5, 2, 0.05, 61});
  st.n_features = 0; // poison F
  const auto r = fit_autoencoder_factors(st, tiny_cfg());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnAutoencoderAlpha, ZeroKFactors_ReturnsInvalidArgument) {
  const SequenceTensor st = make_lowrank_seq({12, 6, 3, 5, 2, 0.05, 67});
  AeFactorCfg cfg = tiny_cfg();
  cfg.k_factors = 0; // K must be >= 1
  const auto r = fit_autoencoder_factors(st, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(LearnAutoencoderAlpha, KFactorsExceedsFeatures_ReturnsInvalidArgument) {
  const SequenceTensor st = make_lowrank_seq({12, 6, 3, 5, 2, 0.05, 71});
  AeFactorCfg cfg = tiny_cfg();
  cfg.k_factors = st.n_features + 1; // K must be <= F
  const auto r = fit_autoencoder_factors(st, cfg);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

} // namespace

#pragma once

// atx::engine::learn — the AUTOENCODER statistical-factor alpha (p2 S5-3b, the
// Gu-Kelly-Xiu autoencoder factor model).
//
// =====================================================================
//  What this header is
// =====================================================================
//  fit_autoencoder_factors compresses the CROSS-SECTIONAL FEATURE MATRIX into K
//  latent factors with a trailing-fit autoencoder, then deploys the encoder so an
//  OOS feature row maps to its leading latent factor (the scalar alpha). It is the
//  FACTOR track — orthogonal to the TCN/GRU/Attn predictive track (those compress
//  the TEMPORAL L*F window; this compresses the F-dim CURRENT cross-section).
//
//  The design matrix X is the LAST timestep of each valid window: for valid sample
//  s (ascending), X[row, f] = seq.x[(s*L + (L-1))*F + f]. This is the "current
//  cross-section" the factor model compresses (n_valid_samples x F).
//
// =====================================================================
//  The R7 PCA pin (linear/centered case) — load-bearing
// =====================================================================
//  PCA centers X and takes its leading-variance subspace. To make the LINEAR AE
//  recover pca(X, K) EXACTLY:
//    * feat_mean = column means of the train X ; feat_sd = 1 (length F).
//    * train on the CENTERED design Xc = X - mean with NO-BIAS linear encoder /
//      decoder (encoder = Linear(F->K) no activation; decoder = Linear(K->F) no
//      activation).
//  Minimizing ||Xc - Xc·W_enc·W_dec||² over no-bias linear maps yields the SAME
//  rank-K subspace as PCA on X (the Eckart-Young theorem): the reconstruction error
//  matches PCA's rank-K reconstruction, and the encoder's K columns span the same
//  K-subspace as pca().components (up to a K×K transform — only the SUBSPACE and the
//  RECONSTRUCTION are determined, not the raw weights). The test pins both.
//
//  When beta_hidden is NON-empty the AE is nonlinear (Tanh-activated hidden stack)
//  and the layers MAY be biased; the PCA pin only requires the linear/centered case.
//
// =====================================================================
//  Training is UNSUPERVISED reconstruction (R1/R8)
// =====================================================================
//  nn::train(factory, opt, MseLoss, Xc, Xc, Xc, Xc, cfg) — the target IS the input.
//  A GKX seed-ensemble (default 5), fixed epochs + checkpoint-at-best (inherited
//  from the Trainer). The optional L1/L2 knobs ride the Trainer's decoupled penalty
//  (the regularization story, R8). The deployed payload stores the encoder+decoder
//  member states; predict_ae (learned_source.hpp) runs only the ENCODER.
//
// Header = API; the logic (and the only nn/ includes) lives in
// src/learn/autoencoder_alpha.cpp.

#include <vector> // std::vector

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/learn/learned_source.hpp"    // LearnedModel
#include "atx/engine/learn/nn/trainer.hpp"        // nn::TrainConfig
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor

namespace atx::engine::learn {

// ===========================================================================
//  AeFactorCfg — the autoencoder-factor knobs (GKX-style, R6/R8).
//
//  k_factors   : K, the latent factor count. Must be >= 1 and <= F. The leading
//                latent Z[0] is the deployed scalar alpha.
//  beta_hidden : the encoder's hidden widths (the decoder mirrors them). EMPTY =>
//                the LINEAR AE (encoder Linear(F->K) / decoder Linear(K->F), no
//                activation) — the R7 PCA pin. Non-empty => a Tanh-activated MLP AE
//                encoder F -> h0 -> h1 -> ... -> K (and the mirror decoder).
//  l1, l2      : the L1 (LASSO) and L2 (weight-decay) penalties folded into the
//                Trainer objective (R8 — the regularization knobs). The R7 PCA pin
//                uses l1 == 0 AND l2 == 0.
//  train       : the deterministic Trainer budget (fixed epochs + ensemble_size +
//                master_seed — R1/R8). GKX default ensemble_size 5.
// ===========================================================================
struct AeFactorCfg {
  atx::usize k_factors = 4;
  std::vector<atx::usize> beta_hidden{}; // EMPTY => linear AE (the PCA pin); else hidden dims
  atx::f64 l1 = 1e-4, l2 = 0.0;
  nn::TrainConfig train{}; // fixed epochs + ensemble (R1/R8)
};

// ===========================================================================
//  fit_autoencoder_factors — fit the deployed autoencoder statistical-factor alpha.
//
//  Extracts the F-dim design X (the last timestep of each valid sample's window),
//  centers it on the train column means, fits a GKX seed-ensemble autoencoder by
//  unsupervised reconstruction (target == centered input), and stores the encoder+
//  decoder member states + arch ({F, K, beta_hidden}) on m.nn so predict_ae runs
//  the encoder forward. PURE in (seq, cfg).
//
//  Errors (expected failures, travel in the Result):
//    - fewer than 2 valid samples => Err(InvalidArgument)  (reconstruction needs >=2)
//    - seq.n_features == 0         => Err(InvalidArgument)  (F >= 1)
//    - cfg.k_factors == 0          => Err(InvalidArgument)  (K >= 1)
//    - cfg.k_factors > F           => Err(InvalidArgument)  (K <= F)
// ===========================================================================
[[nodiscard]] atx::core::Result<LearnedModel>
fit_autoencoder_factors(const SequenceTensor &seq, const AeFactorCfg &cfg);

} // namespace atx::engine::learn

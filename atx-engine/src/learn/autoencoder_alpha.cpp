#include "atx/engine/learn/autoencoder_alpha.hpp"

#include <memory>     // std::unique_ptr, std::make_unique
#include <span>       // std::span
#include <utility>    // std::move
#include <vector>     // std::vector

#include <Eigen/Dense> // Eigen::Index, MatX

#include "atx/core/error.hpp"  // Result, Ok, Err, ErrorCode
#include "atx/core/random.hpp" // Xoshiro256pp (seeded param init)
#include "atx/core/types.hpp"  // f64, u16, u32, u64, usize

#include "atx/core/linalg/linalg.hpp" // MatX

#include "atx/engine/learn/nn/layers.hpp"    // nn::Linear, nn::Tanh
#include "atx/engine/learn/nn/loss.hpp"      // nn::MseLoss
#include "atx/engine/learn/nn/module.hpp"    // nn::Module, nn::Sequential
#include "atx/engine/learn/nn/optimizer.hpp" // nn::Adam
#include "atx/engine/learn/nn/trainer.hpp"   // nn::train, nn::ModelFactory, nn::TrainConfig

namespace atx::engine::learn {

namespace lin = atx::core::linalg;

// All AE assembly helpers have INTERNAL linkage (anonymous namespace): they are
// TU-local and must not collide with same-named free functions in sibling TUs
// (e.g. tcn_alpha.cpp's detail::seed_init — same name, different file).
namespace {

// ---------------------------------------------------------------------------
//  Casts kept local so every size_t<->Eigen::Index boundary is explicit
//  (/Wconversion clean), mirroring layers.cpp.
// ---------------------------------------------------------------------------
[[nodiscard]] Eigen::Index eidx(atx::usize n) noexcept { return static_cast<Eigen::Index>(n); }

// ---------------------------------------------------------------------------
//  Seed-initialise a freshly built network's params with small normals from a
//  generator seeded by `seed` (the ascending flat-buffer param order). Mirrors
//  tcn_alpha's seed_init exactly (R1): same seed -> byte-identical init.
// ---------------------------------------------------------------------------
void seed_init(nn::Module &net, atx::u64 seed) {
  atx::core::Xoshiro256pp rng{seed};
  for (atx::f64 &p : net.params()) {
    p = 0.05 * rng.normal();
  }
}

// ---------------------------------------------------------------------------
//  Encoder layer stack appended to `seq` (NOT built): F -> [h0 -> h1 -> ...] -> K.
//  LINEAR (hidden empty): a single NO-BIAS Linear(F->K) with NO activation — the
//  centered, no-bias linear map whose subspace == pca(X,K) (R7). NONLINEAR: a
//  biased Linear into each hidden width, Tanh between, a biased Linear to K (no
//  final activation, so the latent is unbounded).
// ---------------------------------------------------------------------------
void add_encoder_layers(nn::Sequential &seq, atx::usize f_dim, atx::usize k,
                        const std::vector<atx::usize> &hidden) {
  if (hidden.empty()) {
    seq.add(std::make_unique<nn::Linear>(f_dim, k, /*bias=*/false));
    return;
  }
  atx::usize prev = f_dim;
  for (const atx::usize h : hidden) {
    seq.add(std::make_unique<nn::Linear>(prev, h, /*bias=*/true));
    seq.add(std::make_unique<nn::Tanh>());
    prev = h;
  }
  seq.add(std::make_unique<nn::Linear>(prev, k, /*bias=*/true));
}

// ---------------------------------------------------------------------------
//  Decoder layer stack appended to `seq` (the mirror of the encoder): K -> [...
//  h1 -> h0 ...] -> F. LINEAR: a single NO-BIAS Linear(K->F), NO activation (so
//  reconstruction == centered input projected through the no-bias linear AE, the
//  R7 pin). NONLINEAR: biased Linears with Tanh between, a biased Linear to F.
// ---------------------------------------------------------------------------
void add_decoder_layers(nn::Sequential &seq, atx::usize f_dim, atx::usize k,
                        const std::vector<atx::usize> &hidden) {
  if (hidden.empty()) {
    seq.add(std::make_unique<nn::Linear>(k, f_dim, /*bias=*/false));
    return;
  }
  atx::usize prev = k;
  for (atx::usize i = hidden.size(); i-- > 0;) { // mirror: descending hidden widths
    seq.add(std::make_unique<nn::Linear>(prev, hidden[i], /*bias=*/true));
    seq.add(std::make_unique<nn::Tanh>());
    prev = hidden[i];
  }
  seq.add(std::make_unique<nn::Linear>(prev, f_dim, /*bias=*/true));
}

// Build an ENCODER-ONLY Sequential (F -> K), built + bound. Used both by predict
// (run only the encoder) and to size the encoder param prefix.
[[nodiscard]] std::unique_ptr<nn::Sequential>
build_encoder(atx::usize f_dim, atx::usize k, const std::vector<atx::usize> &hidden) {
  auto enc = std::make_unique<nn::Sequential>();
  add_encoder_layers(*enc, f_dim, k, hidden);
  enc->build();
  return enc;
}

// ---------------------------------------------------------------------------
//  The autoencoder ModelFactory: Sequential{ encoder_layers..., decoder_layers... }
//  as ONE flat param buffer. Child order is encoder-then-decoder, so the
//  serialized state is [encoder params (ascending), decoder params (ascending)] —
//  predict_ae slices the leading encoder prefix to reload the encoder alone. The
//  built net is a PURE function of its member seed (R1): same seed -> byte-
//  identical init.
// ---------------------------------------------------------------------------
[[nodiscard]] nn::ModelFactory ae_factory(atx::usize f_dim, atx::usize k,
                                          std::vector<atx::usize> hidden) {
  return [f_dim, k, hidden = std::move(hidden)](atx::u64 seed) -> std::unique_ptr<nn::Module> {
    auto seq = std::make_unique<nn::Sequential>();
    add_encoder_layers(*seq, f_dim, k, hidden);
    add_decoder_layers(*seq, f_dim, k, hidden);
    seq->build();
    seed_init(*seq, seed);
    return seq;
  };
}

// ---------------------------------------------------------------------------
//  Extract the F-dim cross-sectional design X from the valid samples' LAST
//  timestep, ascending by sample: X[row, f] = seq.x[(s*L + (L-1))*F + f]. Returns
//  the row count (the number of valid samples copied). x_out is sized here.
// ---------------------------------------------------------------------------
[[nodiscard]] atx::usize extract_design(const SequenceTensor &seq, lin::MatX &x_out) {
  const atx::usize L = seq.lookback;
  const atx::usize F = seq.n_features;
  std::vector<atx::usize> valid;
  valid.reserve(seq.n_samples);
  for (atx::usize s = 0; s < seq.n_samples; ++s) {
    if (seq.sample_valid[s] != 0U) {
      valid.push_back(s);
    }
  }
  x_out.resize(eidx(valid.size()), eidx(F));
  atx::usize row = 0;
  for (const atx::usize s : valid) {
    const atx::usize base = (s * L + (L - 1U)) * F; // last-timestep offset
    for (atx::usize f = 0; f < F; ++f) {
      x_out(eidx(row), eidx(f)) = seq.x[base + f];
    }
    ++row;
  }
  return row;
}

// Column means of X (n x F), an ASCENDING scalar fold per column (R1). Length F.
[[nodiscard]] std::vector<atx::f64> column_means(const lin::MatX &x) {
  const atx::usize n = static_cast<atx::usize>(x.rows());
  const atx::usize F = static_cast<atx::usize>(x.cols());
  std::vector<atx::f64> mean(F, 0.0);
  for (atx::usize f = 0; f < F; ++f) {
    atx::f64 acc = 0.0;
    for (atx::usize r = 0; r < n; ++r) {
      acc += x(eidx(r), eidx(f)); // ascending row fold
    }
    mean[f] = (n == 0U) ? 0.0 : acc / static_cast<atx::f64>(n);
  }
  return mean;
}

// Center X in place by subtracting `mean` (length F) from each row. Ascending.
void center_in_place(lin::MatX &x, const std::vector<atx::f64> &mean) {
  const atx::usize n = static_cast<atx::usize>(x.rows());
  const atx::usize F = static_cast<atx::usize>(x.cols());
  for (atx::usize r = 0; r < n; ++r) {
    for (atx::usize f = 0; f < F; ++f) {
      x(eidx(r), eidx(f)) -= mean[f];
    }
  }
}

// Whether the deployed AE payload carries enough arch integers to rebuild the
// encoder. predict_ae is a PUBLIC path over the POD NnPayload, so a too-short
// arch_dims (e.g. after a partial load-from-disk / IPC) must be caught BEFORE
// indexing — an OOB vector read is UB. Layout: arch_dims = {F, K, n_hidden, h0,
// h1, ...}; arch_params unused. On violation predict_ae returns its 0.0 path.
[[nodiscard]] bool ae_payload_ok(const LearnedModel &m) noexcept {
  if (m.nn.arch_dims.size() < 3U) {
    return false; // need at least {F, K, n_hidden}
  }
  const atx::usize n_hidden = m.nn.arch_dims[2];
  return m.nn.arch_dims.size() == 3U + n_hidden; // exactly the declared hidden widths
}

// Rebuild the {F, K, beta_hidden} triple from a deployed AE payload (the inverse
// of how fit records arch_dims). PRECONDITION: ae_payload_ok(m).
struct AeArch {
  atx::usize f_dim;
  atx::usize k;
  std::vector<atx::usize> hidden;
};
[[nodiscard]] AeArch arch_from_payload(const LearnedModel &m) {
  AeArch a;
  a.f_dim = m.nn.arch_dims[0];
  a.k = m.nn.arch_dims[1];
  const atx::usize n_hidden = m.nn.arch_dims[2];
  a.hidden.reserve(n_hidden);
  for (atx::usize i = 0; i < n_hidden; ++i) {
    a.hidden.push_back(m.nn.arch_dims[3U + i]);
  }
  return a;
}

} // namespace (anonymous)

atx::core::Result<LearnedModel> fit_autoencoder_factors(const SequenceTensor &seq,
                                                        const AeFactorCfg &cfg) {
  if (seq.n_features == 0U || seq.lookback == 0U || seq.n_samples == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_autoencoder_factors: empty / zero-feature / zero-lookback tensor");
  }
  const atx::usize F = seq.n_features;
  const atx::usize K = cfg.k_factors;
  if (K == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_autoencoder_factors: k_factors must be >= 1");
  }
  if (K > F) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_autoencoder_factors: k_factors must not exceed the feature count");
  }

  lin::MatX X;
  const atx::usize n_valid = extract_design(seq, X);
  if (n_valid < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_autoencoder_factors: need at least 2 valid samples");
  }

  // R7 centering: feat_mean = train column means; feat_sd = 1 (no per-feature
  // scaling — PCA does not scale, and the no-bias linear AE on the CENTERED design
  // recovers pca(X,K)). Train the AE on Xc = X - mean (target == input).
  const std::vector<atx::f64> mean = column_means(X);
  center_in_place(X, mean);

  LearnedModel m;
  m.kind = ModelKind::Autoencoder;
  m.n_base_features = static_cast<atx::u32>(F); // aug empty => augmented_dim() == F
  m.feat_mean = mean;
  m.feat_sd.assign(F, 1.0); // unit sd (centering only); never 0 so the column is live
  // A single trivial horizon/blend entry: the AE is a factor extractor, not a
  // per-horizon predictor, so there is no horizon mixture to deploy.
  m.horizons.assign(1, static_cast<atx::u16>(0));
  m.blend_w.assign(1, 1.0);
  // oos_score_series is intentionally LEFT EMPTY for S5-3b: the AE is a factor
  // extractor here, not a deflation-gated predictor; the full OOS gate is S5-4/S5-5.

  // Record the arch so predict_ae rebuilds the encoder identically. Layout:
  // arch_dims = {F, K, n_hidden, h0, h1, ...}; arch_params is unused (kept empty).
  m.nn.lookback = seq.lookback;
  m.nn.n_seq_features = F;
  m.nn.arch_dims.clear();
  m.nn.arch_dims.push_back(F);
  m.nn.arch_dims.push_back(K);
  m.nn.arch_dims.push_back(cfg.beta_hidden.size());
  for (const atx::usize h : cfg.beta_hidden) {
    m.nn.arch_dims.push_back(h);
  }

  // The GKX seed-ensemble, trained by UNSUPERVISED reconstruction (target == the
  // centered input). trial_count = the number of distinct fits = the ensemble size
  // (each member is one fit), so the deflation gate is honest.
  const nn::ModelFactory factory = ae_factory(F, K, cfg.beta_hidden);
  nn::TrainConfig train = cfg.train;
  train.l1 = cfg.l1;
  train.l2 = cfg.l2;
  nn::Adam opt{0.01};
  nn::MseLoss loss;
  const auto states = nn::train(factory, opt, loss, X, X, X, X, train);
  if (!states.has_value()) {
    return atx::core::Err(states.error().code(), "fit_autoencoder_factors: ensemble train failed");
  }
  m.nn.member_states = *states;
  m.trial_count = m.nn.member_states.size();

  return atx::core::Ok(std::move(m));
}

// ===========================================================================
//  predict_ae — the AUTOENCODER statistical-factor inference (declared in
//  learned_source.hpp, defined here so this TU owns the nn/ includes). Centers the
//  F-dim feature row with feat_mean, rebuilds the ENCODER-only Sequential, reloads
//  each member's encoder PREFIX (the leading encoder_param_count scalars of the
//  member state — the train factory laid encoder params first), runs ONLY the
//  encoder to the K latent, and returns the ascending-member mean of Z[0].
// ===========================================================================
atx::f64 predict_ae(const LearnedModel &m, std::span<const atx::f64> feature_row) {
  if (m.nn.member_states.empty() || !ae_payload_ok(m)) {
    return 0.0; // undeployed OR malformed payload -> "no opinion"
  }
  const AeArch arch = arch_from_payload(m);
  const atx::usize F = arch.f_dim;
  if (feature_row.size() != F || m.feat_mean.size() != F) {
    return 0.0; // shape mismatch -> no opinion (release-safe; never index OOB)
  }

  // Center the input row with the trailing-fit feat_mean (the R7 centering applied
  // forward — M2). Ascending fold.
  lin::MatX xc(1, eidx(F));
  for (atx::usize f = 0; f < F; ++f) {
    xc(0, eidx(f)) = feature_row[f] - m.feat_mean[f];
  }

  const std::unique_ptr<nn::Sequential> enc = build_encoder(F, arch.k, arch.hidden);
  const atx::usize enc_params = enc->param_count();

  // Ascending-member mean of the leading latent Z[0] (R1). Reload each member's
  // encoder prefix, run only the encoder, accumulate Z[0].
  atx::f64 acc = 0.0;
  for (const std::vector<atx::f64> &state : m.nn.member_states) {
    if (state.size() < enc_params) {
      return 0.0; // a member state too short to carry the encoder -> no opinion
    }
    enc->state_from(std::span<const atx::f64>{state.data(), enc_params});
    enc->train(false);
    const lin::MatX z = enc->forward(xc); // (1 x K)
    acc += z(0, 0);                        // leading latent factor
  }
  return acc / static_cast<atx::f64>(m.nn.member_states.size());
}

} // namespace atx::engine::learn

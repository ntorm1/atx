#pragma once

// atx::engine::learn — the walk-forward learned SEQUENCE alphas: TCN + GRU-lite
// (p2 S5-2b-ii, §4.3/§0.6).
//
// =====================================================================
//  What this header is
// =====================================================================
//  fit_tcn / fit_gru assemble a deflation-gated, firewall-fit, multi-horizon
//  learned SEQUENCE alpha from a SequenceTensor (the S5-1 (sample x time x
//  feature) windows), MIRRORING fit_linear (linear_alpha.hpp) exactly — only the
//  per-fold "fit" step changes from a linear coefficient solve to "train a
//  deterministic seed-ensemble net on the train windows and predict the test
//  windows". EVERYTHING ELSE is the same pattern: the CPCV date-fold firewall,
//  the genuine out-of-fold IC series, the §0.6 horizon blend, and the
//  oos_deflated_sharpe gate (which is already kind-agnostic — it reads only
//  m.oos_score_series + m.trial_count).
//
//    * Per HORIZON, walk the CPCV DATE-folds (mapped to SAMPLE index sets keyed
//      on seq.date_of): on each fold train a seed-ensemble on the TRAIN samples'
//      windows + horizon-h labels, predict OOS on the TEST samples' windows (the
//      net is causal + trailing, so a test sample is never scored by a fit that
//      saw it — M2), and accumulate the per-sample horizon-0 out-of-fold
//      predictions. Each distinct fold fit bumps the deflation trial_count (§0.3).
//    * oos_score_series = the per-date cross-sectional IC of those horizon-0 OOF
//      predictions (the sample-keyed analog of detail::oof_ic_series), frozen on
//      the model for the gate (M3).
//    * blend_w (§0.6) = normalize(max(oos_IC_h, 0)) across horizons (uniform if
//      every horizon is non-positive).
//    * DEPLOYED model: a SINGLE seed-ensemble refit on ALL valid samples (the full
//      trailing window) against the blend-weighted target Σ_h blend_w[h]·y[h][s].
//      Its serialized member states are stored on m.nn so predict_nn is ONE
//      ascending-member-mean forward (no per-horizon blend at inference — the
//      blend is baked into the deployed target). See tcn_alpha.cpp for the rationale.
//
// =====================================================================
//  Determinism / firewalls (R1/R2/R4)
// =====================================================================
//  R1: every seed flows from cfg.train.master_seed via seed_for / the Trainer;
//  ensemble mean + OOF accumulation are ascending; predict_nn is byte-deterministic
//  (rebuilds the same factory + reloads the same ascending member states).
//  R2: windows are trailing (S5-1) and the layers are causal (seq_layers); the fit
//  trains trailing CPCV folds and predicts OOS, so an earlier prediction cannot
//  change when later dates are added (truncation-invariance is the test).
//  R4: trial_count counts every fold fit so oos_deflated_sharpe deflates honestly.
//
// Header = API; the logic (and the only nn/ includes) lives in src/learn/tcn_alpha.cpp.

#include <vector> // std::vector

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u16, usize

#include "atx/engine/eval/cpcv.hpp"               // eval::CpcvConfig
#include "atx/engine/learn/learned_source.hpp"    // LearnedModel
#include "atx/engine/learn/nn/trainer.hpp"        // nn::TrainConfig
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor

namespace atx::engine::learn {

// ===========================================================================
//  TcnAlphaCfg — the learned-TCN-alpha knobs (tiny by design, R6).
//
//  blocks/kernel/channels: the TCN stack — `blocks` TcnResidualBlocks with
//                          dilations {1,2,4,...}, each over `channels`, kernel
//                          width `kernel`. The first block maps F -> channels.
//  dropout               : the TCN block drop probability (eval = identity).
//  l2                    : the L2 weight-decay folded into the train objective.
//  cpcv                  : the CPCV fold config for the OOS walk.
//  horizons              : the forward-return horizons to fit + blend (§0.6).
//  train                 : the deterministic Trainer budget (fixed epochs +
//                          ensemble_size + master_seed — R1/R8).
// ===========================================================================
struct TcnAlphaCfg {
  atx::usize blocks = 3;
  atx::usize kernel = 3;
  atx::usize channels = 24;
  atx::f64 dropout = 0.1;
  atx::f64 l2 = 1e-4;
  eval::CpcvConfig cpcv{};
  std::vector<atx::u16> horizons{1, 5, 21};
  nn::TrainConfig train{};
};

// ===========================================================================
//  GruAlphaCfg — the learned-GRU-lite-alpha knobs.
//
//  hidden : the GRU cell's hidden width (F -> hidden -> 1). The rest mirrors
//           TcnAlphaCfg.
// ===========================================================================
struct GruAlphaCfg {
  atx::usize hidden = 24;
  atx::f64 dropout = 0.1;
  atx::f64 l2 = 1e-4;
  eval::CpcvConfig cpcv{};
  std::vector<atx::u16> horizons{1, 5, 21};
  nn::TrainConfig train{};
};

// ===========================================================================
//  AttnAlphaCfg — the learned ATTENTION-LITE-alpha knobs (p2 S5-3a).
//
//  d_model : the single attention head's projection width (Q/K/V each F -> d_model);
//            the causal-attention summary at the trailing step feeds a d_model -> 1
//            head. The rest (dropout / l2 / cpcv / horizons / train) mirrors the
//            TCN/GRU configs exactly — only the per-fold network differs.
// ===========================================================================
struct AttnAlphaCfg {
  atx::usize d_model = 24;
  atx::f64 dropout = 0.1;
  atx::f64 l2 = 1e-4;
  eval::CpcvConfig cpcv{};
  std::vector<atx::u16> horizons{1, 5, 21};
  nn::TrainConfig train{};
};

// ===========================================================================
//  fit_tcn / fit_gru / fit_attn — assemble the deployed multi-horizon learned
//  SEQUENCE alpha.
//
//  See the header: per-horizon CPCV OOS fit (trial_count per fold fit), a single
//  deployed seed-ensemble refit on the full trailing window against the
//  blend-weighted target, and §0.6 horizon-blend weights from the OOS IC. PURE in
//  (seq, cfg). The three differ ONLY in the ModelFactory + the stored arch dims;
//  the CPCV / OOF / blend / gate core is shared (fit_seq_alpha in the .cpp).
//
//  Errors (expected failures, travel in the Result):
//    - seq.n_samples == 0  => Err(InvalidArgument)
//    - seq.n_features == 0  => Err(InvalidArgument)
//    - seq.lookback == 0    => Err(InvalidArgument)
// ===========================================================================
[[nodiscard]] atx::core::Result<LearnedModel> fit_tcn(const SequenceTensor &seq,
                                                      const TcnAlphaCfg &cfg);
[[nodiscard]] atx::core::Result<LearnedModel> fit_gru(const SequenceTensor &seq,
                                                      const GruAlphaCfg &cfg);
[[nodiscard]] atx::core::Result<LearnedModel> fit_attn(const SequenceTensor &seq,
                                                       const AttnAlphaCfg &cfg);

} // namespace atx::engine::learn

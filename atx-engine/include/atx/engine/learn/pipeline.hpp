#pragma once

// atx::engine::learn — the THIN end-to-end learned-pipeline harness (S5-7).
//
// =====================================================================
//  What this header is
// =====================================================================
//  A small set of FREE FUNCTIONS that WIRE the already-built learn units
//  (FeatureMatrix -> fit_linear -> fit_gbt -> baum_welch -> fit_stack) into one
//  coherent end-to-end pipeline + a deterministic digest. It is PURE WIRING: every
//  function only constructs the existing cfgs and calls the existing fit_* /
//  oos_* / fit_stack functions, then hashes or compares their DECIDED outputs. It
//  adds NO new model, NO new statistic, and NO new fitting logic — the S5-1..S5-6
//  units own all of that. This header is the integration seam the S5-7 proofs
//  exercise, nothing more.
//
// =====================================================================
//  Determinism (M1) — and the honest `workers` knob
// =====================================================================
//  full_pipeline_digest folds ONLY deterministic DECIDED fields — fitted
//  coefficients, GBT forest node bytes, HMM log-parameters + a forward
//  log-likelihood, the stacking verdict_hash, and each model's frozen out-of-fold
//  series — into one byte hash. There is NO wall-clock, NO filesystem, and NO map
//  iteration on the digest path, so the same FeatureMatrix + same cfg.master_seed
//  gives a byte-identical digest.
//
//  PipelineCfg carries a `workers` knob that is ACCEPTED but read by NO derived cfg
//  — its value reaches no fit at all, so it is a no-op for the digest BY
//  CONSTRUCTION: every S5 unit is single-threaded with order-fixed reductions
//  (RNG-free cyclic CD, a deterministic histogram GBT with first-max tie-breaks,
//  log-space Baum-Welch with fixed-order sums), so there is no parallel
//  non-associative float reduction a worker count could perturb. The digest is
//  therefore IDENTICAL for any worker count — proven by
//  ThreadCountInvariance_DigestUnchanged. This is an HONEST forward seam for a
//  future parallel fold-walk, NOT a fake parallel path.
//
// Header-only; every function is defined inline. The pipeline is a COLD research
// path (one full fit per call), so std::vector / Eigen allocation is fine.

#include <span>    // std::span (the multi-horizon horizon-set view)
#include <vector>  // std::vector (digest buffer, reordered horizon list)

#include <Eigen/Dense> // Eigen::Index (coeff / forest / hmm element access)

#include "atx/core/types.hpp" // f64, u16, u32, u64, usize

#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig
#include "atx/engine/learn/elastic_net.hpp"    // ElasticNetCfg
#include "atx/engine/learn/ensemble.hpp"       // StackingCfg, StackingVerdict, fit_stack
#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix
#include "atx/engine/learn/gbt.hpp"            // GbtCfg, fit_gbt, oos_ic
#include "atx/engine/learn/hmm.hpp"            // Hmm, forward_log, ForwardResult
#include "atx/engine/learn/latent.hpp"         // LatentAugmentation
#include "atx/engine/learn/learned_source.hpp" // LearnedModel, GbtForest/Tree/Node, ModelKind
#include "atx/engine/learn/linear_alpha.hpp"   // LinearAlphaCfg, fit_linear, oos_deflated_sharpe

namespace atx::engine::learn {

// ===========================================================================
//  PipelineCfg — the end-to-end harness knobs.
//
//  master_seed : the determinism root, threaded into every derived cfg (M1).
//  workers     : an ACCEPTED-but-UNUSED knob (read by no derived cfg). The S5 units
//                are single-threaded with order-fixed reductions, so the digest is
//                worker-invariant BY CONSTRUCTION — even more strongly than a no-op:
//                the value reaches no fit at all. A forward seam for a future
//                parallel fold-walk; ThreadCountInvariance proves determinism
//                survives the knob (it does NOT claim a real parallel path exists).
//  cpcv        : the CPCV fold config shared by every fitted model.
//  horizons    : the forward-return horizons (the model / meta Y horizons).
// ===========================================================================
struct PipelineCfg {
  atx::u64 master_seed{0};
  atx::u32 workers{1};
  eval::CpcvConfig cpcv{};
  std::vector<atx::u16> horizons{1};
};

namespace pipeline_detail {

// Build the linear-alpha cfg from the pipeline cfg (no ridge baseline -> a genuine
// L1/L2 elastic-net fit; the shared CPCV / seed / horizons). The `workers` knob is
// not a fit parameter of any S5 unit, so it does not appear here — it cannot
// perturb a single-threaded order-fixed fit (the worker-invariance honesty).
//
// ANTI-SNOOPING L1 (the linear analog of the GBT's OOF dispersion floor): the
// elastic-net penalty is a STRONG L1 (lambda 0.20, alpha 0.80 ⇒ ~0.16 L1 + ~0.02
// L2 in the (1/2n) objective). On a genuinely edge-free panel L1 of this strength
// drives every spurious feature coefficient to EXACTLY zero -> a constant
// prediction -> an exactly-zero per-date IC -> the deflation gate scores dsr <= 0
// (robustly, every seed). A weaker penalty lets the elastic-net latch onto a
// spurious column and produce a coin-flip positive dsr on noise (a known low-N DSR
// leak — see the S5-3 ledger). The penalty is strong enough to zero noise yet far
// below the bar that would shrink a GENUINE edge (a planted 0.9·col0 signal still
// scores dsr 1.0). This is the anti-snooping knob for the LINEAR arm specifically;
// the GBT arm carries its own (the OOF dispersion floor) and the stacking benchmark
// uses its own elastic-net cfg — there is no single global lambda, each arm owns its
// anti-overfit guard.
[[nodiscard]] inline LinearAlphaCfg linear_cfg_of(const PipelineCfg &cfg) {
  LinearAlphaCfg lin;
  lin.en = ElasticNetCfg{/*lambda=*/0.20, /*alpha=*/0.80, /*max_iter=*/2000, /*tol=*/1e-9};
  lin.use_ridge_baseline = false;
  lin.cpcv = cfg.cpcv;
  lin.master_seed = cfg.master_seed;
  lin.horizons = cfg.horizons;
  return lin;
}

// Build the GBT cfg from the pipeline cfg (default tree knobs; the shared CPCV /
// seed / horizons). Same worker-invariance note as linear_cfg_of.
[[nodiscard]] inline GbtCfg gbt_cfg_of(const PipelineCfg &cfg) {
  GbtCfg g;
  g.cpcv = cfg.cpcv;
  g.master_seed = cfg.master_seed;
  g.horizons = cfg.horizons;
  return g;
}

// Build the stacking cfg from the pipeline cfg (GBT nonlinear base; the shared
// CPCV / seed / horizons). The linear benchmark + GBT base inside fit_stack share
// these folds, so the §0.4 gate is same-fold same-metric.
[[nodiscard]] inline StackingCfg stack_cfg_of(const PipelineCfg &cfg) {
  StackingCfg s;
  s.base = StackingCfg::Base::Gbt;
  s.cpcv = cfg.cpcv;
  s.master_seed = cfg.master_seed;
  s.horizons = cfg.horizons;
  return s;
}

// The per-date observable the pipeline fits the HMM on is the SAME marker derivation
// fit_stack uses for its regime split — so the pipeline REUSES the canonical
// `ensemble_detail::regime_observable` (cross-sectional mean of the last feature
// column per date, a (n_dates x 1) MatX, order-fixed forward walk) rather than
// keeping a divergent copy. (Both derivations must stay identical; one definition
// guarantees it.)

// Fold a LearnedModel's DECIDED parameters (M1) into the f64 digest buffer:
// kind, trial_count, blend weights, the frozen out-of-fold series, the linear
// coefficients, and every GBT forest's node bytes. All deterministic from the
// seeded fit; NO clock / map / filesystem input. Order-fixed ascending walks.
void fold_model(std::vector<atx::f64> &buf, const LearnedModel &m);

} // namespace pipeline_detail

// ===========================================================================
//  full_pipeline_digest — the M1 headline: ONE u64 over every fitted model's
//  decided outputs.
//
//  Runs the WHOLE pipeline on `fm` — fit_linear, fit_gbt, baum_welch on the derived
//  marker observable, and fit_stack — and folds every fitted model's DECIDED
//  fields (coeffs / forest node bytes / blend / out-of-fold series / HMM
//  log-parameters + a forward log-likelihood / stacking verdict_hash) into one byte
//  hash. DETERMINISTIC: same fm + same cfg.master_seed -> identical digest, and
//  identical for any cfg.workers (which is read by no fit at all — see the header).
//  PURE in (fm, cfg).
// ===========================================================================
[[nodiscard]] atx::u64 full_pipeline_digest(const FeatureMatrix &fm, const PipelineCfg &cfg);

// ===========================================================================
//  admitted_count — how many learned models (linear, gbt) survive the deflation
//  gate (oos_deflated_sharpe > 0) on `fm`. The M3 anti-snooping headline: a
//  genuinely edge-free panel yields 0; a planted real edge yields > 0, under the
//  SAME cfg / gate. Pure reuse of fit_linear / fit_gbt + oos_deflated_sharpe.
// ===========================================================================
[[nodiscard]] inline atx::usize admitted_count(const FeatureMatrix &fm, const PipelineCfg &cfg) {
  const LatentAugmentation empty_aug;
  const LearnedModel lin = fit_linear(fm, empty_aug, pipeline_detail::linear_cfg_of(cfg));
  const LearnedModel gbt = fit_gbt(fm, empty_aug, pipeline_detail::gbt_cfg_of(cfg));
  atx::usize n = 0;
  if (oos_deflated_sharpe(lin, fm) > 0.0) {
    ++n;
  }
  if (oos_deflated_sharpe(gbt, fm) > 0.0) {
    ++n;
  }
  return n;
}

// ===========================================================================
//  pipeline_admits_stack — does the §0.4 stacking gate admit `meta`? Pure reuse of
//  fit_stack (flat, no regime): a linearly-combinable pool gives the nonlinear base
//  no OOS edge over linear -> false; a genuine interaction pool -> true.
// ===========================================================================
[[nodiscard]] inline bool pipeline_admits_stack(const FeatureMatrix &meta, const PipelineCfg &cfg) {
  return fit_stack(meta, /*regime=*/nullptr, pipeline_detail::stack_cfg_of(cfg)).admitted;
}

// ===========================================================================
//  oos_with_regime / oos_without_regime — the regime-conditional vs flat OOS dsr of
//  the nonlinear stacking base (reuse fit_stack with / without an Hmm*). On a pool
//  whose optimal combination DIFFERS by regime, the regime-conditional fit scores a
//  higher OOS dsr than the flat fit (the spec-exit improvement).
// ===========================================================================
[[nodiscard]] inline atx::f64 oos_with_regime(const FeatureMatrix &meta, const Hmm &regime,
                                              const PipelineCfg &cfg) {
  return fit_stack(meta, &regime, pipeline_detail::stack_cfg_of(cfg)).oos_dsr_nonlinear;
}

[[nodiscard]] inline atx::f64 oos_without_regime(const FeatureMatrix &meta, const PipelineCfg &cfg) {
  return fit_stack(meta, /*regime=*/nullptr, pipeline_detail::stack_cfg_of(cfg)).oos_dsr_nonlinear;
}

namespace pipeline_detail {

// The OOS IC of a single-horizon linear fit of `fm` on horizon `h` (reuse
// fit_linear + oos_ic). The model's frozen out-of-fold series is the horizon-0
// (here: the only horizon) cross-sectional IC, so oos_ic is that horizon's genuine
// OOS information coefficient.
[[nodiscard]] inline atx::f64 single_horizon_ic(const FeatureMatrix &fm, atx::u16 h,
                                                const PipelineCfg &cfg) {
  const LatentAugmentation empty_aug;
  PipelineCfg one_h = cfg;
  one_h.horizons = {h};
  const LearnedModel m = fit_linear(fm, empty_aug, linear_cfg_of(one_h));
  return oos_ic(m, fm);
}

} // namespace pipeline_detail

// ===========================================================================
//  oos_ic_best_single — the BEST single-horizon OOS IC over `horizons`: fit one
//  single-horizon linear model per horizon and take the max OOS IC. Pure reuse of
//  fit_linear + oos_ic.
// ===========================================================================
[[nodiscard]] inline atx::f64 oos_ic_best_single(const FeatureMatrix &fm,
                                                 std::span<const atx::u16> horizons,
                                                 const PipelineCfg &cfg) {
  atx::f64 best = 0.0;
  bool seen = false;
  for (const atx::u16 h : horizons) {
    const atx::f64 ic = pipeline_detail::single_horizon_ic(fm, h, cfg);
    if (!seen || ic > best) {
      best = ic;
      seen = true;
    }
  }
  return best;
}

// ===========================================================================
//  horizon_blend_weights — the §0.6 per-horizon blend weights of ONE multi-horizon
//  linear fit, aligned to the input `horizons` order. The weights are
//  normalize(max(oos_IC_h, 0)): a horizon carrying genuine OOS skill earns positive
//  weight, a no-skill horizon is driven toward zero (and an all-non-positive set
//  falls back to uniform). This is the HONEST, non-tautological §0.6 proof surface —
//  a blend that demonstrably WEIGHTS BY OOS IC.
//
//  NOTE (deferred residual): a full "the blended PREDICTION's OOS IC strictly beats
//  the best single horizon's" proof is NOT expressible by pure wiring today — the
//  frozen oos_ic surface reads only each model's horizon-0 out-of-fold series, so no
//  metric scores a blended prediction. That requires retaining every horizon's OOF
//  predictions plus a blended-OOS-IC metric (new model logic, beyond S5-7's wiring
//  scope) and a decorrelated-horizon fixture; it is recorded as an S5 close residual.
//  This function proves the weighting mechanism (the §0.6 weights are IC-driven, not
//  degenerate), which is what the as-built surface can honestly establish.
// ===========================================================================
[[nodiscard]] inline std::vector<atx::f64>
horizon_blend_weights(const FeatureMatrix &fm, std::span<const atx::u16> horizons,
                      const PipelineCfg &cfg) {
  const LatentAugmentation empty_aug;
  PipelineCfg blend_cfg = cfg;
  blend_cfg.horizons.assign(horizons.begin(), horizons.end());
  // fit_linear sets m.horizons = cfg.horizons and m.blend_w[i] = the §0.6 weight of
  // horizons[i], so the returned weights are aligned to the input order.
  const LearnedModel m = fit_linear(fm, empty_aug, pipeline_detail::linear_cfg_of(blend_cfg));
  return m.blend_w;
}

} // namespace atx::engine::learn

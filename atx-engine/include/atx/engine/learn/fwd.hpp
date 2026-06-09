#pragma once

// atx::engine::learn — learned-signals / ML-combiner layer forward declarations
// (Sprint 5). A lightweight header other engine headers include to NAME the
// learn-layer spine types without pulling in the model machinery (feature
// builder, elastic-net / GBT / HMM kernels, the source adapter) behind them.
//
// =====================================================================
//  Thesis
// =====================================================================
// A learned model is a deterministic, deflation-gated, firewall-fit alpha that
// plugs into the existing pool through the existing seams. The engine learns
// higher-order structure (latent factors, tree interactions, regimes,
// alpha-of-alphas) WITHOUT ever learning to look ahead or to fool its own
// scorer.
//
// =====================================================================
//  The eight invariants (M1–M8) — every coding unit honors these
// =====================================================================
//  M1  Determinism under training. Same data + same seed => byte-identical
//      fitted params AND byte-identical emitted signal. All RNG is
//      Xoshiro256pp seeded from (master_seed, unit, fold, horizon); GBT uses a
//      deterministic histogram + first-max tie-break; elastic-net is RNG-free
//      cyclic coordinate descent; HMM init is seeded; reductions are
//      order-fixed.
//  M2  Fit/apply firewall (no look-ahead). Every fitted object — feature
//      standardization stats, PCA latent basis, linear coeffs, GBT splits, HMM
//      (A,B,pi) + regime posterior, horizon-blend weights — is estimated on a
//      trailing <= t-embargo window and applied forward only. Truncation-
//      invariance is the test.
//  M3  No snooping. Every model and the ensemble trains INSIDE S1 CPCV and
//      reports a DEFLATED Sharpe with N = the ML trial count. A no-edge model
//      is rejected by the DSR gate. The nonlinear combiner is admitted only if
//      it beats the P4 linear combiner OOS-after-deflation.
//  M4  Differential correctness. Every new kernel ships a bounded differential
//      test vs a reference (elastic-net == ridge at alpha=0 + the soft-threshold
//      closed form; GBT == brute-force threshold search on a stump; HMM == a
//      reference impl + the fwd==bwd log-likelihood identity) — never a
//      "returned something" test.
//  M5  Pattern B — no general-purpose primitive in the engine. Elastic-net,
//      GBT, and the HMM forward-backward kernel are atx-core L7 requests,
//      shipped engine-local-then-lifted. Ridge, PCA, linalg, RNG, and
//      cross_section are CONSUMED from atx-core, never re-implemented.
//  M6  A learned model is just an alpha. It emits a cross-section per date via
//      the SAME combine::ISignalSource seam, is scored by compute_metrics, and
//      is admitted through the SAME library::Library::admit(AlphaCandidate,
//      AlphaGate) path (Provenance{expr_source="learned:<spec>", ...}). It does
//      NOT fork the corr-to-pool math.
//  M7  No hot-path allocation; cold-fit may allocate. Training, feature-build,
//      PCA-fit, HMM-fit, and snapshot (all cold) may allocate. The steady-state
//      evaluate() of a fitted LearnedSignalSource reuses pre-sized scratch
//      (zero alloc per date).
//  M8  No survivorship · PIT · NaN-verbatim. Feature build carries delisted
//      instruments with their final bar; out-of-universe / not-yet-knowable
//      cells read NaN; NaN is handled EXPLICITLY (row-dropped from a fit or
//      cross-sectionally imputed — documented per call site), never silently
//      coerced to zero.
//
// =====================================================================
//  Seam map — the APIs this layer consumes (never re-implements)
// =====================================================================
//  From atx-core (consumed directly):
//    core::linalg::{ridge, pca, transform, as_matrix, as_vector, MatX, VecX}
//                                       — Eigen-backed L2 baseline + trailing-fit
//                                         latent features + design-matrix bridges
//    core::{solve, decompose}           — solve_spd / inverse / symmetric_eig
//    core::Xoshiro256pp                 — seeded, reproducible training RNG
//    core::cross_section::{rank, zscore, demean, winsorize}
//    core::rolling::{RollingMean, RollingStd}
//    core::simd::{dot, axpy}            — inner loops
//    core::hash::{hash_bytes}           — M1 fitted-param byte hashing
//  From the engine combine layer:
//    combine::ISignalSource             — the per-date cross-section seam (M6)
//    combine::{AlphaStore, AlphaGate, AlphaMetrics, compute_metrics,
//              AlphaCombiner}           — store / gate / scoring / linear baseline
//  From the engine eval layer (S1):
//    eval::{cpcv_folds, deflated_sharpe, stats_ext}  — CPCV folds + DSR + the
//              engine-local stats (norm_cdf / skewness / excess_kurtosis /
//              mean_std_pop / median) atx-core lacks
//  From the engine alpha layer:
//    alpha::{Panel, Engine, AlphaStreams}  — PIT panel fields + stored streams
//  From the engine library layer (S4):
//    library::{Library, AlphaCandidate, Provenance, admit}  — the single
//              admission path a learned model travels (M6)
//
// =====================================================================
//  Determinism contract
// =====================================================================
//  Same data + same seed => BYTE-IDENTICAL fitted params AND emitted signal.
//  The RNG is always Xoshiro256pp, seeded deterministically from the tuple
//  (master_seed, unit, fold, horizon) — never from wall-clock, address, or
//  thread id. Every RNG-bearing unit ships a "two-builds-equal" test (fitted
//  coefficient/threshold bytes hashed via core::hash::hash_bytes AND the emitted
//  cross-section bit-equal). This is M1 made testable.
//
// =====================================================================
//  The three Pattern-B edges (engine-local-then-lifted, M5)
// =====================================================================
//  1. Elastic-net (L1+L2) coordinate-descent solver  (learn/elastic_net.hpp) —
//     cyclic CD with soft-thresholding, RNG-free; differential vs core::ridge at
//     alpha=0 and the soft-threshold closed form. atx-core L7 request.
//  2. Deterministic histogram gradient-boosted-tree   (learn/gbt.hpp) —
//     fixed-bin histogram split finder, seeded row/feature subsampling, first-max
//     tie-break; differential vs an exact brute-force stump. atx-core L7 request.
//  3. Log-space HMM forward-backward / Baum-Welch      (learn/hmm.hpp) —
//     log-space forward-backward + EM re-estimation, seeded init; differential vs
//     a reference impl + the fwd==bwd log-likelihood identity. atx-core L7 request.
//
// Full definitions live in (added per phase unit):
//   learn/feature_matrix.hpp — FeatureMatrix, FeatureSpec, build_features   (S5-1)
//   learn/learned_source.hpp — LearnedSignalSource                          (S5-3)
//   learn/linear_alpha.hpp   — the fitted LearnedModel (ridge + elastic-net) (S5-3)

namespace atx::engine::learn {

// PIT (date x instrument) x feature tensor + multi-horizon forward-return
// labels. Full definition in learn/feature_matrix.hpp (S5-1).
struct FeatureMatrix;

// Declarative recipe for a FeatureMatrix: raw fields, derived/latent features,
// pool alphas, and forward-return horizons. Full definition in
// learn/feature_matrix.hpp (S5-1).
struct FeatureSpec;

// Adapts any fitted per-horizon learned model to the combine::ISignalSource
// seam (M6): emits a cross-section per date from pre-sized scratch (M7). Full
// definition in learn/learned_source.hpp (S5-3).
class LearnedSignalSource;

// A fitted, firewall-respecting model (coefficients / splits / HMM params) plus
// the metadata needed to apply it forward. Full definition in
// learn/linear_alpha.hpp (S5-3, extended by S5-4/S5-5).
struct LearnedModel;

} // namespace atx::engine::learn

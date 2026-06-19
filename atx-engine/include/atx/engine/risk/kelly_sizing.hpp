#pragma once

// atx::engine::risk — fractional-Kelly conviction-scaled sizing (S10-2).
//
// ===========================================================================
//  What this unit is (RenTech gap G2)
// ===========================================================================
//  Berlekamp rebuilt Medallion (1989) around Kelly sizing of small edges; the
//  realized form is FRACTIONAL Kelly (full Kelly overbets — Samuelson). atx's
//  Garleanu-Pedersen optimizer (risk/garleanu_pedersen.hpp) is "Kelly-inspired"
//  with an implicit quadratic leverage penalty — there is no explicit, fractional,
//  conviction-scaled, covariance-aware leverage layer. This unit adds one.
//
//  Given an expected-alpha vector mu, the factored covariance V (risk/factor_model),
//  and a per-name conviction in [0,1] (S10-1 output), kelly_size computes the
//  full-Kelly target f* = V^{-1} mu via FactorModel::apply_inverse (Woodbury — never
//  materializes the MxM inverse), scales it by a config kelly_fraction (default
//  quarter-Kelly), scales each name by its conviction, and clamps gross/leverage.
//  The result is a TARGET the GP optimizer tracks (its cost-to-go machinery is
//  untouched) — NOT a replacement QP.
//
// ===========================================================================
//  Determinism / ownership
// ===========================================================================
//  NO RNG. The reductions (the V^{-1}mu Woodbury apply, the conviction scale, the
//  gross L1) run in canonical ascending instrument order — same inputs → byte-
//  identical weights. KellyWeights OWNS its VecX (Rule of Zero: the only members are
//  value types, so the compiler-generated copy/move/dtor are correct and sufficient).
//
//  This header carries the POD config/result structs + the kelly_size DECLARATION;
//  the body (the apply-math, the clamp arithmetic) lives in src/risk/kelly_sizing.cpp,
//  compiled into the atx-engine static library so a body edit does not re-parse the
//  factor-model include fan-out in every dependent.

#include "atx/core/linalg/linalg.hpp" // atx::core::linalg::VecX (== Eigen::VectorXd)
#include "atx/core/types.hpp"         // atx::f64

#include "atx/engine/risk/factor_model.hpp" // FactorModel (the factored V; apply_inverse)

namespace atx::engine::risk {

// ===========================================================================
//  KellyConfig — fractional-Kelly knobs. Defaults are the published policy.
// ===========================================================================
struct KellyConfig {
  atx::f64 kelly_fraction = 0.25; // quarter-Kelly: scale full-Kelly target down (full Kelly overbets)
  atx::f64 max_gross = 1.0;       // cap on gross leverage Sum|w|; <= 0 disables the clamp
};

// ===========================================================================
//  KellyWeights — the sizing output. OWNS its VecX (Rule of Zero).
// ===========================================================================
struct KellyWeights {
  atx::core::linalg::VecX weights; // final per-name target weights (length M)
  atx::f64 gross;                  // realized Sum|w| after the clamp
  atx::f64 scale_applied;          // overall gross-clamp scale factor applied (1.0 if clamp not binding)
};

// ===========================================================================
//  kelly_size — fractional-Kelly, conviction-scaled sizing over the FACTORED
//  covariance.
//
//  expected_alpha: mu (length M). cov: the FactorModel V (V = X F X^T + D).
//  conviction: per-name confidence in [0,1] (length M; e.g. the S10-1
//  ConvictionScore::score per alpha mapped to names). cfg: the fractional-Kelly
//  knobs (default quarter-Kelly, gross cap 1.0).
//
//  PRECONDITIONS (fail-closed, ATX_ASSERT in debug): the three vectors and
//  cov.n_instruments() share length M; every expected_alpha[i] / conviction[i] is
//  finite; every conviction[i] in [0,1]; cfg.kelly_fraction >= 0. Pure, no RNG,
//  order-fixed. Returns the per-name target the GP optimizer tracks.
// ===========================================================================
[[nodiscard]] KellyWeights kelly_size(const atx::core::linalg::VecX &expected_alpha,
                                      const FactorModel &cov,
                                      const atx::core::linalg::VecX &conviction,
                                      const KellyConfig &cfg = {});

} // namespace atx::engine::risk

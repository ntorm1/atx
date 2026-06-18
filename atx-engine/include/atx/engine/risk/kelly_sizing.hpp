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
//  and a per-name conviction in [0,1] (S10-1 output), it computes the full-Kelly
//  target f* = V^{-1} mu via FactorModel::apply_inverse (Woodbury — never
//  materializes the MxM inverse), scales it by a config kelly_fraction (default
//  quarter-Kelly), scales each name by its conviction, and clamps gross/leverage.
//  The result is a TARGET the GP optimizer tracks (its cost-to-go machinery is
//  untouched) — not a replacement QP.
//
//  SCAFFOLD (S10-0): forward declarations only; full definitions + kelly_size()
//  land in S10-2 (which adds the <atx/core/types.hpp> + linalg includes the
//  field definitions need).

namespace atx::engine::risk {

// =====================================================================
//  Kelly config + result — forward declarations (S10-2)
// =====================================================================

// Fractional-Kelly knobs: kelly_fraction (e.g. 0.25 = quarter-Kelly), gross /
// leverage clamps. Named struct, defaults are the published policy.
// Full definition in risk/kelly_sizing.hpp (S10-2).
struct KellyConfig;

// Per-name target weights after fractional-Kelly + conviction scaling + clamp,
// plus the realized gross/leverage the clamp produced.
// Full definition in risk/kelly_sizing.hpp (S10-2).
struct KellyWeights;

} // namespace atx::engine::risk

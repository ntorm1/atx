#pragma once

// atx::engine::combine — AlphaMetrics: per-alpha realized-performance summary (P4-1 POD; P4-2 fills it).
//
// ===========================================================================
//  What this header is (and is NOT, yet)
// ===========================================================================
//  AlphaMetrics is the plain POD summary of one alpha's realized performance,
//  computed over its PnL stream (alpha::AlphaStreams::pnl(a)). THIS unit (P4-1)
//  defines ONLY the value type — AlphaStore stores one per AlphaRecord. The
//  computation (`compute_metrics(...)`) is added to THIS SAME header by P4-2;
//  the field set below is the frozen §4 shape it will populate. All fields are
//  f64 and carry NaN where the statistic is undefined (e.g. a degenerate, zero-
//  variance stream has an undefined Sharpe).
//
//  Rule of Zero: a trivial aggregate, copied/moved by value (it is small and
//  owns nothing). No invariants are enforced here — a metrics value is only ever
//  produced by P4-2's compute_metrics, never hand-mutated on a hot path.

#include "atx/core/types.hpp" // atx::f64

namespace atx::engine::combine {

// ===========================================================================
//  AlphaMetrics — realized-performance summary for one alpha (§4 data model).
//
//  All fields f64; NaN where the statistic is undefined. Aggregate-initialized
//  in declaration order. Annualization uses 252 trading periods/year. The
//  fitness formula and its turnover floor follow WorldQuant's published metric
//  (WQ §4.4) — see the per-field notes.
// ===========================================================================
struct AlphaMetrics {
  // Annualized Sharpe: sqrt(252) * mean(pnl) / std(pnl). NaN if std(pnl) == 0.
  atx::f64 sharpe;
  // Mean daily dollar-traded as a fraction of book size (Σ|Δw| per period, mean).
  atx::f64 turnover;
  // Annualized mean periodic return: 252 * mean(pnl).
  atx::f64 returns;
  // Max peak-to-trough drawdown of the cumulative-return curve (fraction, [0,1]).
  atx::f64 drawdown;
  // Return per dollar traded, in bps: returns / max(turnover, eps).
  atx::f64 margin;
  // WQ §4.4 fitness: sqrt(abs(returns) / max(turnover, 0.125)) * sharpe.
  // The 0.125 turnover floor is WorldQuant's published low-turnover guard
  // (prevents a near-zero denominator inflating the fitness of an idle alpha).
  atx::f64 fitness;
  // Mean holding horizon in periods, ~ 1 / turnover.
  atx::f64 holding_days;
};

} // namespace atx::engine::combine

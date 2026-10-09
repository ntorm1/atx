#pragma once

// Shared, output-preserving calibrator primitives for the atx-vol per-slice
// fitters (eSSVI, raw-SVI / SVI-MM, CStar). Everything here is a pure function
// or a compile-time constant that MULTIPLE calibrators already computed
// identically — hoisted to a single source of truth so a value can never drift
// between fitters. Nothing here changes any fitted number: each entry is
// byte-for-byte what the call sites evaluated before.
//
// ── Deliberately NOT unified (do not "finish the job" — it breaks byte
//    identity) ──────────────────────────────────────────────────────────────
//   * Post-fit sigma gates: eSSVI uses a theta-based bound, raw-SVI a
//     closed-form w_min plus a 2.5x source_atm_vol extra gate, SVI-MM / C8 /
//     CStar none. Different math per curve.
//   * Weighted normal-equation assemble + symmetric mirror: different parameter
//     dimensions per calibrator (3-D cube, 4-D, 5-D) — not a shared routine.
//   * Revert-to-seed 1.05x-RMSE guard: a free function in C8 vs an inline block
//     in CStar, keyed on different fields.
//   * Marquardt damped-solve wrappers: different damped-diagonal floors
//     (CStar 1e-18 vs C8 1e-12) — hoisting one onto the other shifts a fit.
//   * SVI's bespoke inline Huber threshold (k = 1.5) differs from the k = 1.345
//     the other robust reweighters share — left as a distinct literal.
//   * C8's LM schedule (c8_calib.cpp) grows lambda by x4 (not x10) and has NO
//     lower clamp — the kLambda* schedule below MUST NOT be routed onto it.

#include <cstdint>

#include "atx/vol/api/fitting/calib.hpp"  // CalibOpts, OptimizationLevel

namespace atx::vol::detail {

// Profile-aware outer-iteration cap (ports `ats_vol_calib_outer_cap`). The
// active optimization level selects a per-level cap; a zero per-level cap falls
// back to the legacy `max_outer_iter` (or 4 when that is also unset). Canonical
// replacement for the textually-divergent-but-behaviorally-identical copies
// that lived in essvi_calib.cpp and svi_calib.cpp — returns the same
// std::uint16_t for every input.
[[nodiscard]] inline std::uint16_t outer_cap(const CalibOpts& opts) noexcept {
  std::uint16_t per_level = 0;
  switch (opts.optimization_level) {
    case OptimizationLevel::QuickMark:
      per_level = opts.max_iter_quick_mark;
      break;
    case OptimizationLevel::Trading:
      per_level = opts.max_iter_trading;
      break;
    case OptimizationLevel::Risk:
      per_level = opts.max_iter_risk;
      break;
    case OptimizationLevel::Reference:
      per_level = opts.max_iter_reference;
      break;
    case OptimizationLevel::ColdFast:
      per_level = opts.max_iter_cold_fast;
      break;
  }
  if (per_level > 0) {
    return per_level;
  }
  // The ternary promotes its uint16_t arms to int; cast back for the return.
  return static_cast<std::uint16_t>((opts.max_outer_iter > 0) ? opts.max_outer_iter
                                                              : 4);
}

// ── Levenberg-Marquardt damping schedule (VALUE-identical across eSSVI,
//    SVI-MM and CStar) ────────────────────────────────────────────────────
// These are the identical damping constants the three LM fitters used — spelled
// as named constants in eSSVI/CStar and as inline literals in SVI-MM. Only the
// exact-value matches are routed here; look-alike floors (e.g. a w_pred
// variance floor of 1e-12, a sumw floor of 1e-15, a half-spread floor of 1e-9,
// the CStar damped-diag floor of 1e-18, or the backtracking line-search
// factor 0.5) are NOT this schedule and stay where they are.
inline constexpr double kLambdaLmInit = 1.0e-3;    // initial LM damping lambda
inline constexpr double kLambdaLmMax = 1.0e8;      // give up when lambda exceeds this
inline constexpr double kLambdaLmMin = 1.0e-12;    // lower clamp on lambda
inline constexpr double kLambdaGrow = 10.0;        // lambda *= grow on a rejected step
inline constexpr double kLambdaShrink = 0.5;       // lambda *= shrink on an accepted step
inline constexpr int kLmTrialCap = 8;              // damped-solve trials per LM step
inline constexpr int kLmInnerDefault = 12;         // inner-iter fallback when opts unset
inline constexpr double kTolParamDefault = 1.0e-9; // param-norm tol fallback when opts unset
inline constexpr double kOuterStallSse = 1.0e-15;  // outer-loop SSE-change stall threshold

}  // namespace atx::vol::detail

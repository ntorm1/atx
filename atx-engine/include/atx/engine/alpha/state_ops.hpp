#pragma once

// atx::engine::alpha — stateful causal-recurrence kernels (P3b-3).
//
// `trade_when` and `hump` carry TRUE cross-date state from the panel's first
// date forward. UNLIKE the windowed Ts* ops (ema/wma seed on the oldest cell of
// a trailing-d window and reset per window), these recur over the ENTIRE history
// with no window: out[t] depends on out[t-1], which depends on out[t-2], … back
// to out[0]. They therefore cannot use the windowed `eval_time_series` path; the
// VM/oracle drive them through a dedicated forward-scan (eval_recurrence) that,
// per instrument j, walks dates t = 0 … D-1 maintaining a single `state[j]`
// value (the prior output) in a pooled buffer.
//
// ===========================================================================
//  CAUSALITY — STRUCTURAL, NOT POLICY (the #1 concern this sprint)
// ===========================================================================
//  Each kernel computes out[t,j] from `prior` (== out[t-1,j], held in state[j])
//  and the operands' cells at the SAME flat index `t*I + j` (date t). There is
//  NO parameter, no index, no API by which a kernel could read state[>t] or an
//  input at a date > t — a forward reference is unrepresentable. The first date
//  is special-cased (t == 0) by `first == true`, which seeds the recurrence
//  without consulting any prior. // SAFETY: the only state read is the caller-
//  supplied `prior` (the value at t-1); the only inputs read are the t-th cells
//  the caller passes by value. See vm.hpp::eval_recurrence / oracle's mirror.
//
//  The VM kernels here and the oracle's reference recurrence (oracle.hpp) are
//  INDEPENDENT re-statements of the SAME scalar policy and branch order; the
//  P3b-3 differential test proves they agree to the bit (NaN == NaN). The
//  policy is PINNED here and restated in oracle.hpp:
//    * trade_when: out = NaN if exit_true; elif trigger_true -> alpha[t];
//      else -> prior. A NaN trigger/exit is mask_true==false (neither enters nor
//      exits) -> holds the prior. The exit branch is checked FIRST.
//    * hump: out = x[t] if |x[t] - prior| > threshold (STRICT >); else prior.
//      The first date is x[0] unconditionally. A NaN difference is never > thr,
//      so a NaN x[t] holds the prior (does NOT poison the carried state).
//
// Header-only; every free function is `inline`. Leaf math is `noexcept`.

#include <cmath>
#include <limits>

#include "atx/core/types.hpp"

namespace atx::engine::alpha::detail {

inline constexpr atx::f64 kStateNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] inline bool state_is_nan(atx::f64 x) noexcept { return std::isnan(x); }

// A mask cell is "true" iff finite and non-zero; NaN is neither (a NaN trigger /
// exit is treated as false). Mirrors oracle.hpp / vm.hpp mask_true exactly.
[[nodiscard]] inline bool state_mask_true(atx::f64 x) noexcept {
  return x != 0.0 && !state_is_nan(x);
}

// trade_when single-cell recurrence at date t for one instrument.
//   `prior` == out[t-1] (ignored when `first`); `trig`/`exit_v` are the mask
//   cells at date t; `alpha` is the F64 signal cell at date t.
// Branch order (exit FIRST) is the pinned policy — the oracle mirrors it.
// SAFETY: reads only `prior` (state[t-1]) and the three date-t operand cells; no
// index into a future date or future state exists in this signature.
[[nodiscard]] inline atx::f64 trade_when_step(atx::f64 prior, atx::f64 trig, atx::f64 exit_v,
                                              atx::f64 alpha, bool first) noexcept {
  if (state_mask_true(exit_v)) {
    return kStateNaN; // close / no position
  }
  if (state_mask_true(trig)) {
    return alpha; // (re)enter with the new signal
  }
  // No exit, no trigger: hold the prior signal. On the first date there is no
  // prior position, so the held value is NaN (flat).
  return first ? kStateNaN : prior;
}

// hump single-cell recurrence at date t for one instrument.
//   `prior` == out[t-1] (ignored when `first`); `x` is the input cell at date t;
//   `threshold` is the scalar suppression band.
// SAFETY: reads only `prior` (state[t-1]) and the date-t input `x`.
[[nodiscard]] inline atx::f64 hump_step(atx::f64 prior, atx::f64 x, atx::f64 threshold,
                                        bool first) noexcept {
  if (first) {
    return x; // out[0] == x[0] unconditionally
  }
  // STRICT >: a change exactly equal to the threshold (and a NaN difference,
  // which is never > threshold) holds the prior — suppressing small turnover.
  return (std::fabs(x - prior) > threshold) ? x : prior;
}

} // namespace atx::engine::alpha::detail

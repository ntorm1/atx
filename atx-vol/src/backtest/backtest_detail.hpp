#pragma once

// Backtest engine internals split out of the public backtest.hpp API surface
// (Task 6, atx-vol API restructure): should_exercise_early is used only by
// backtest.cpp's own TU (apply_early_exercise), never by any other production
// TU.

#include <cmath>

namespace atx::vol::detail {

// Task B3: the exercise/assignment decision rule, factored out as a PURE
// function so it is unit-testable independent of the engine's per-step
// machinery (same rationale as `MarginBreachPolicy`'s placement note in
// backtest.hpp).
//
// `intrinsic` is the lot's current intrinsic value; a non-ITM lot
// (`intrinsic <= 0.0`) is never exercise-optimal and this always returns
// false for one. `extension_value` is the lot's remaining time value (mark -
// intrinsic; must be finite and >= 0 for a sane American mark, else this
// returns false rather than let a solver artifact drive a decision).
// `threshold` is what the extension is compared against:
//
//   * a short call's assignment risk over an ex-date: the discrete forward
//     dividend a long counterparty would capture by exercising before the
//     ex-date (`FinancingConfig::share_dividends`, WS-F F3(b)/A5) -- the
//     caller passes 0 (never firing) when no ex-date falls in this step's
//     window;
//   * a deep-ITM put's exercise opportunity, either side of the book: the
//     interest-carry benefit of collecting the strike now instead of at
//     expiry, `K * (1 - exp(-r*T))` -- the caller computes this from the
//     board's own `r` and the lot's residual `T`, both already read for other
//     per-step arithmetic (A5's financing-rate lookup, `compute_step`'s own
//     `residual_T`).
//
// Exercise/assignment is optimal exactly when the remaining time value is
// LESS than what early action would capture; a non-finite or non-positive
// threshold never fires (there is nothing to capture).
[[nodiscard]] inline bool should_exercise_early(double intrinsic, double extension_value,
                                                double threshold) noexcept {
  return intrinsic > 0.0 && std::isfinite(extension_value) && extension_value >= 0.0 &&
         std::isfinite(threshold) && threshold > 0.0 && extension_value < threshold;
}

} // namespace atx::vol::detail

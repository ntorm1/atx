#pragma once

// atx::engine::book — shaping of a per-name desired target between decisions (platform v8).
//
//   Construction rules that act on the desired target of a book before a trading rule moves
//   the weights toward it. Each is a small row kernel over one decision: the caller owns the
//   rows, the per-name state and the strategy wiring (atx-impl), so a backtest replay and a
//   daily decide path run the same arithmetic.
//
//   apply_hold_band — rank hysteresis (v8 R-4, rule hold-band-v1). A name's desired value moves
//     only when its rank leaves a band around the rank at which that value was set; inside the
//     band the previous value is kept. The state (rank_set, desired_prev) is carried per name
//     across decisions, including decisions at which the name is not eligible, so a name that
//     misses a day is compared with the rank of its last set value when it returns.
//
// Determinism: no RNG, clock or map; names are visited in index order. Same inputs, same outputs.

#include <span>   // std::span
#include <vector> // std::vector

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u8, usize

namespace atx::engine::book {

// Per-name hold-band state: the rank at which the kept desired value was set, and that value.
// A NaN rank_set means unset (the name's next eligible decision sets it). Both vectors hold one
// entry per name, or are both empty (apply_hold_band then sizes them to NaN on first use).
struct HoldBandState {
  std::vector<atx::f64> rank_set;
  std::vector<atx::f64> desired_prev;
};

// What one apply_hold_band pass did over the eligible names.
struct HoldBandCounts {
  atx::usize moved = 0U;     // took the fresh value (first_set included)
  atx::usize kept = 0U;      // kept desired_prev
  atx::usize first_set = 0U; // of `moved`: rank_set was unset
};

// hold-band-v1. For every name with eligible[i] != 0:
//   moved = !isfinite(rank_set[i]) || |rank_now[i] - rank_set[i]| > band
//   moved -> rank_set[i] = rank_now[i] and desired_prev[i] = desired[i] (the fresh value stays);
//   else  -> desired[i] = desired_prev[i].
// A name with eligible[i] == 0 is not touched, its state included. `desired` enters as the fresh
// value and leaves as the shaped one; rank_now may alias desired (a name's inputs are read before
// its output is written).
// Identity: when every state entry was written by this kernel from rows with desired == rank_now
// (so desired_prev == rank_set), band 0 returns the fresh row bit for bit: a kept name has
// |rank_now - rank_set| == 0, i.e. rank_now == rank_set == desired_prev.
// Err InvalidArgument, nothing modified: rows of different widths, a non-empty state of another
// width, band not finite or negative, an eligible name with a non-finite rank_now or desired, or a
// state entry with a finite rank_set and a non-finite desired_prev.
[[nodiscard]] atx::core::Result<HoldBandCounts>
apply_hold_band(std::span<const atx::f64> rank_now, std::span<atx::f64> desired,
                std::span<const atx::u8> eligible, atx::f64 band, HoldBandState &state);

} // namespace atx::engine::book

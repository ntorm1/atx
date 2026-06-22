#pragma once
// stage_discover_detail.hpp — internal helpers for stage_discover.cpp.
//
// Exposed in atx::impl::detail so unit tests can call the real implementation
// (Fix 1, W2 review). Do NOT include this header from production callers other
// than stage_discover.cpp, stage_sweep.cpp, and discover_test.cpp.

#include "atx/core/error.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::detail {

// Apply the W2 capacity universe screen to `panel`.
//
// Builds a derived Panel whose universe mask is:
//   original_universe AND (close > min_price) AND (adv{adv_window} >= min_adv)
//
// Fail-closed: missing 'close' or 'volume' => Err(InvalidArgument).
// When screen is ACTIVE (called only when min_adv>0 || min_price>0):
//   - adv_window out of [1, 65535]  => Err(InvalidArgument) naming the bad value.
//   - post-screen kept-cell count == 0 => Err(InvalidArgument) with diagnostic.
//
// Predicate mirrors single_alpha_capacity_test.cpp::capacity_universe exactly.
[[nodiscard]] atx::core::Result<atx::engine::alpha::Panel>
apply_capacity_screen(const atx::engine::alpha::Panel& panel,
                      atx::f64 min_price, atx::f64 min_adv, long adv_window);

// Build the W4a robust-factor weak/holdout sub-universe Panel (Deliverable 2).
//
// Returns a derived Panel = `panel` with the SAME field columns but its universe
// restricted to a DETERMINISTIC seeded instrument sub-sample: instrument i stays
// in-universe iff it was in the original universe AND it is SELECTED by a SplitMix64
// mix of (master_seed, i) falling under `frac`. Seeded by `master_seed` only (never
// thread/time) -> same seed + frac + panel => same weak universe (seed-stable, F1).
// The candidate's WQ is re-scored on this held-out universe to form the robust factor.
//
// `frac` MUST be in (0, 1). Fail-closed: frac out of range, or a sub-sample that
// retains zero in-universe cells, => Err(InvalidArgument) with a diagnostic. Exposed
// so discover_test.cpp can verify the determinism + masking directly.
[[nodiscard]] atx::core::Result<atx::engine::alpha::Panel>
build_robust_holdout_panel(const atx::engine::alpha::Panel& panel, atx::f64 frac,
                           atx::u64 master_seed);

// Mean in-universe instrument count per date over `panel` (sum of in_universe cells
// over all (date, inst) divided by dates()). 0.0 when the panel has no dates. PURE;
// reads ONLY the universe mask. Used to RECORD the capacity-screen universe size as a
// discovery admission metric (W5). When `panel` is the post-capacity-screen panel this
// equals apply_capacity_screen's stderr `names_per_day` by construction (same mask).
// Exposed so discover_test.cpp can verify the count directly on a hand-built mask.
[[nodiscard]] double mean_names_per_day(const atx::engine::alpha::Panel& panel);

} // namespace atx::impl::detail

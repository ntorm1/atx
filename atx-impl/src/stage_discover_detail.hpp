#pragma once
// stage_discover_detail.hpp — internal helpers for stage_discover.cpp.
//
// Exposed in atx::impl::detail so unit tests can call the real implementation
// (Fix 1, W2 review). Do NOT include this header from production callers other
// than stage_discover.cpp and discover_test.cpp.

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

} // namespace atx::impl::detail

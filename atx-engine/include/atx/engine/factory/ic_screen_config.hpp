#pragma once

#include <array>
#include "atx/core/types.hpp"

namespace atx::engine::factory {

enum class IcScreenRule : atx::u8 { DisabledV1 = 1, ConservativeV2 = 2, EquivalenceV3 = 3 };

// P9 S2 (DSL/IC review §3, §7): the HAC rule of the IC screen's mean-IC standard error, named so
// it can be selected beside eval/hac.hpp's. Frozen integer values.
//   * Bartlett2hV1 (the default, the screen's rule since v1): Bartlett kernel at lag
//     max(2h, newey_west_rule_of_thumb_lag(T)) on the calendar-preserving influence series, with an
//     IID (lag 0) floor on the standard error;
//   * HorizonAwareV3: eval::hac::mean_tstat's TStatRule::HorizonAwareV3 on the same influence
//     series (h > 1: uniform kernel at max(h-1, rule-of-thumb lag), cancellation-guarded, falling
//     back to Bartlett; h == 1: Bartlett at the Newey-West automatic lag), no IID floor. This is
//     the rule the combine path's t-statistics use, so under it the two are comparable.
enum class IcHacRule : atx::u8 { Bartlett2hV1 = 1, HorizonAwareV3 = 2 };

// The screen's coverage gates (a date's paired names against its cached and eligible names, and
// a series' valid dates against its calendar dates and per quarter segment) before P9 S2.
inline constexpr atx::f64 kDefaultIcMinCoverage = 0.8;

struct IcScreenConfig {
  IcScreenRule rule{IcScreenRule::DisabledV1};
  std::array<atx::usize, 4> horizons{{5, 21, 63, 126}};
  atx::usize execution_delay{1};
  atx::usize window_begin{};
  atx::usize window_end{};   // exclusive; zero resolves to panel.dates()
  atx::usize maturity_end{}; // exclusive; zero resolves to window_end
  atx::usize min_names{20};
  atx::usize min_dates{128};
  atx::f64 practical_abs_ic{0.02}; // screening tolerance, NOT an alpha admission gate
  atx::f64 confidence_multiplier{3.5};
  atx::u64 max_cache_bytes{atx::u64{512} * 1024U * 1024U};
  // P9 S2 (DS-4): the coverage fraction every coverage gate of the screen requires, in (0, 1]
  // (was the hard-coded kMinCoverage = .8). The default keeps every result bit.
  atx::f64 min_coverage{kDefaultIcMinCoverage};
  // P9 S2: the mean-IC standard error's HAC rule (default: the screen's own, unchanged).
  IcHacRule hac_rule{IcHacRule::Bartlett2hV1};
};

// Explicit active-application recipe: protect full-window absolute IC >= .002.
// The default constructor retains the legacy DisabledV1/.02 configuration.
[[nodiscard]] IcScreenConfig equivalence_ic_screen_config() noexcept;

} // namespace atx::engine::factory

#pragma once

#include <array>
#include "atx/core/types.hpp"

namespace atx::engine::factory {

enum class IcScreenRule : atx::u8 { DisabledV1 = 1, ConservativeV2 = 2, EquivalenceV3 = 3 };

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
};

// Explicit active-application recipe: protect full-window absolute IC >= .002.
// The default constructor retains the legacy DisabledV1/.02 configuration.
[[nodiscard]] IcScreenConfig equivalence_ic_screen_config() noexcept;

} // namespace atx::engine::factory

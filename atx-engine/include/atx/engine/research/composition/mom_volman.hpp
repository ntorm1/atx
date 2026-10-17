#pragma once
// atx::engine::research::composition -- the registered constants of composition schedule
// mom-volman-v1 (P9 lane AL-COMB, cell P9-M; registration scripts/specs/p9/rules/mom-volman-v1.json,
// DEC-16), the one definition both sides read (P9 lane D3W-PB, review D3W S-2): the C++ fit records
// the price_momentum sleeve (fit.hpp, ruling ALCOMB-7) and the IC runner forms every block's
// volatility, target and scale from it (atx-impl strategy_ic_mom_volman.hpp, which re-exports these
// names and holds the registration text and the block kernel). Block b starts at decision index
// lookback + lag - 1 + step * b, so a role needs at least lookback + lag decisions.
#include <string_view>
#include "atx/core/types.hpp"

namespace atx::engine::research::composition {
inline constexpr std::string_view mom_volman_rule="mom-volman-v1";
inline constexpr std::string_view mom_volman_theme="price_momentum";
inline constexpr atx::usize mom_volman_lookback=126; // BSC's six months of daily returns
inline constexpr atx::usize mom_volman_lag=2;        // f(d) is realised at d + 2
inline constexpr atx::usize mom_volman_step=21;      // one month of decisions
inline constexpr atx::usize mom_volman_min_decisions=mom_volman_lookback+mom_volman_lag;
inline constexpr atx::usize mom_volman_max_decisions=65536;
} // namespace atx::engine::research::composition

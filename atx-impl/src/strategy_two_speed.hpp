#pragma once
// two-speed-v1 (platform v8 Y-5, lane YCOMB; Ruling PM8-5, accepted as distinct from R-3 by PM8-10;
// task-YCOMB-report.md "Y-5" and "Y-5 wiring"). The registration that the fitter, the IC runner and
// the NAV replay share:
//   - the per-theme alpha-decay half-life table (sessions), registered blind; a theme whose
//     half-life is at most engine::book::two_speed_fast_bound (10) is FAST, every other weighted
//     theme SLOW (this is the only copy: the Python wrapper writes {"rule": "two-speed-v1"} and the
//     IC runner reads the table here, Ruling PM8-12);
//   - the IC runner (weights-file block theme_sleeves) splits the standardised blend into the fast
//     and the slow themes' parts and saves them with the fast part's theme-mass share per date
//     (<role>_sleeves.json, pinned in the combined manifest as composition_sleeves);
//   - the NAV replay (nav --two-speed two-speed-v1) forms each sleeve's desired target with the
//     parent's construction, moves a virtual fast sleeve F toward L m_f d_f at theta_f = 1 -
//     2^(-C/5) at the cadence C, lets the book's remainder (current - F) be the slow sleeve moving
//     toward L m_s d_s at the registered theta_s = .05, and trades only the netted change
//     (strategy_target_replay.cpp).
#include <array>
#include <string_view>
#include <utility>
#include "atx/core/types.hpp"
#include "atx/engine/book/two_speed.hpp"

namespace atx::impl::strategy {
inline constexpr std::string_view two_speed_rule = "two-speed-v1";
// Registered blind (task-YCOMB-report.md, Y-5 half-life table): theme -> alpha half-life, sessions;
// merger_arbitrage 126 (half its field's 252-session window; Rulings PM8-14, YP-10). These are the
// rule's registered parameters, not a theme list: the registered themes are the IC runner's theme
// table (P9 lane D1, strategy_ic_rules.hpp: the alpha registry's themes), and a weighted theme
// needs both a row here and a place in that table. A theme registered without a half-life is
// refused only by two-speed-v1, when weighted.
inline constexpr std::array<std::pair<std::string_view, atx::f64>, 13> two_speed_half_lives{{
    {"value", 252.0},          {"profitability_quality", 252.0}, {"investment_issuance", 252.0},
    {"earnings_momentum", 63.0}, {"price_momentum", 126.0},       {"low_risk", 252.0},
    {"short_interest", 63.0},  {"reversal_seasonality", 5.0},    {"options_implied", 21.0},
    {"ownership_flow", 63.0},  {"filing_events", 21.0},          {"price_volume", 5.0},
    {"merger_arbitrage", 126.0}}};
// The registered half-life of `theme` in `out`; false when the theme is not registered.
[[nodiscard]] constexpr bool two_speed_half_life(std::string_view theme, atx::f64& out) noexcept {
  for (const auto& [name, half_life] : two_speed_half_lives)
    if (name == theme) {
      out = half_life;
      return true;
    }
  return false;
}
// A registered half-life is fast at most two_speed_fast_bound sessions.
[[nodiscard]] constexpr bool two_speed_fast(atx::f64 half_life) noexcept {
  return half_life <= atx::engine::book::two_speed_fast_bound;
}
} // namespace atx::impl::strategy

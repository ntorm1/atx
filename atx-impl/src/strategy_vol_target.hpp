#pragma once

// vol-target-v1 (platform v8 Y, lane YCOMB): the risk-managed form of the leverage cell X-10. The
// NAV replay's `--vol-target vol-target-v1` (strategy_nav_v7.cpp) runs the risk target's scaler
// (strategy_risk_target.hpp, Law::vol_target_v1) with the law of engine::book::vol_target.hpp:
//
//   L_t = clip(L x sigma_ref_t / sigma_hat_t, 1, L)
//
// L = the run's --aim-leverage (the cap: X-10's fixed L, 2.0 in the cell); sigma_hat_t = the
// annualised ex-ante volatility of the book's current weights (its DECIDE's, after the session's
// fills) at gross 1 over the whole book, on the atx-risk-v1 row d of the pinned store (the names
// with a risk row), risk-target-v1's forecast; sigma_ref_t = the mean of the book's estimates so
// far, sigma_hat_t included. Estimated at the book's first decision with a forecast, a book and a
// positive variance, then at the first such decision at least 21 sessions after the previous
// estimate; held between; L before the first. Registered constants: the floor 1, the cadence 21,
// the annualisation 252; no flag moves them. aim-partial-v5 only: it moves toward L_t x desired;
// the shared desired target is unchanged.
//
// This file holds the rule's published text and parameter block; the scaler and the series are
// strategy_risk_target's.

#include <string>
#include <nlohmann/json_fwd.hpp>

namespace atx::impl::strategy::vol_target {

inline constexpr const char* rule_id = "vol-target-v1";

// The rule's text (recipe.json, summary.json, holdings manifest, v7_extras.json "vol_target").
[[nodiscard]] std::string declaration();
// {rule, declaration, floor, cap, cadence_sessions, periods_per_year, reference, series}.
[[nodiscard]] nlohmann::json parameters_json();

} // namespace atx::impl::strategy::vol_target

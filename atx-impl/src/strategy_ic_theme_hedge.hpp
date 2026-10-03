#pragma once
// Composition rule theme-cov-hedge-v1 (P9 lane AL-COMB, cell P9-H; registration
// scripts/specs/p9/rules/theme-cov-hedge-v1.json, DEC-15). Hypothesis (Daniel, Mota, Rottke and
// Santos 2020, "The Cross-Section of Risk and Returns", RFS 33(5)): a theme composite, like any
// characteristic-sorted portfolio, carries the theme's priced characteristic and the unpriced
// covariance of its names with the theme's own factor. Neither theme-resid-v1 (themes against each
// other) nor price-risk-v1 (beta, vol, size) hedges a theme's loading on its own factor at a fixed
// characteristic; a hedge leg short that loading should lower the book's volatility at about the
// same gross alpha. Registration (every constant fixed before any read; the house's windows and
// decision-to-return lag):
//   per theme t and decision d, on the theme's standardised composite (ew-theme-std-v1's re-rank
//   R_t, the "score"), engine combine/char_cov_hedge.hpp with lookback 252, lag 2, step 21,
//   min_pairs 126:
//   1. the theme sleeve s_t(d') = sum_i R_t(d', i) r_i(d' + 2) (r the valid guarded session return,
//      strategy_price_exposures.hpp session_interval_returns);
//   2. beta_i of r_i on s_t over the trailing 252 decisions ending d - 2 (>= 126 pairs);
//   3. beta_perp: beta's least-squares residual on [1, R_t(d, .)] (orthogonal to the score);
//      H_t(d) its centred tied rank;
//   4. h_t, the OLS slope of s_t on the hedge leg's return g_t = sum_i H_t r_i over the same kind
//      of window, re-estimated every 21 decisions from decision 253 (0 before, or below 126 pairs);
//   5. the theme adds W_t(d) (R_t - h_t H_t) in place of W_t(d) R_t (W_t(d): its mass in force,
//      a theme_schedule's when one rides too). Before h_t is first estimated the theme adds exactly
//      what ew-theme-std-v1 adds.
// Everything is estimated at apply time from the role's own sessions, so no fitted constant enters
// the weights file: the fitter writes only the spec block (composition_theme_hedge.py). The block
// is parsed by ic_detail::composition_hedge (declared in strategy_ic_rules.hpp); the runner hands
// the composition the role's session returns (ic_detail::attach_theme_hedge) and
// IcComposition::finish applies the hedge (add_theme_hedged). It rides on a theme_standardise rule
// with rerank true and is refused with theme_residualise and theme_sleeves.
#include <span>
#include <string_view>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_ic_composition.hpp"

namespace atx::engine::data { struct StrategyRoleData; }
namespace atx::impl::strategy {
inline constexpr std::string_view theme_hedge_rule = "theme-cov-hedge-v1";
inline constexpr atx::usize theme_hedge_lookback = 252;  // the house's one-year window
inline constexpr atx::usize theme_hedge_lag = 2;         // f(d) is realised at d + 2
inline constexpr atx::usize theme_hedge_step = 21;       // the house's monthly re-estimation
inline constexpr atx::usize theme_hedge_min_pairs = 126; // price-risk-v1's beta minimum (half)

// Working bytes of the hedge beside the composition's planes: the role's session returns (owned by
// the composition from attach to finish), one hedged plane, the presence copy attach_theme_hedge
// builds and the kernel's scratch. InvalidArgument for empty dimensions; OutOfRange on overflow.
[[nodiscard]] atx::core::Result<atx::u64> theme_hedge_working_bytes(atx::usize dates,
                                                                   atx::usize names);

// Step 5 for every theme: `planes` one date-major dates x `names` plane per theme holding the sum
// of its present members' w_k s_k rank_k (NaN = no member present), `mass` W_theme per plane,
// `schedule` the mass blocks in force (empty: W_theme throughout;
// IcComposition::schedule_theme_masses' blocks), `session_returns` dates x names. Per theme in
// index order and per block of the schedule, every cell with a hedged value adds the mass in force
// times that value to `out` (a zero mass adds nothing), the order the ew-theme-std-v1 composition
// adds its re-ranks in. Refuses (InvalidArgument) on mismatched shapes or a non-finite mass;
// OutOfRange if the hedged plane cannot be allocated.
[[nodiscard]] atx::core::Status add_theme_hedged(std::span<const std::vector<atx::f64>> planes,
                                                 std::span<const atx::f64> mass,
                                                 std::span<const IcThemeBlock> schedule,
                                                 std::span<const atx::f64> session_returns,
                                                 atx::usize names, std::span<atx::f64> out);

// The role's session returns (dates x names): row s, name i is the valid adjusted simple return
// over (s - 1, s] by price-risk-v1's guard (session_interval_returns: both sessions present with
// finite positive close and raw_close, |log ratio| <= 1.5 and <= |raw log ratio| + .10), NaN
// otherwise; row 0 is NaN. Needs the role's close and raw_close fields.
[[nodiscard]] atx::core::Result<std::vector<atx::f64>> theme_hedge_session_returns(
    const engine::data::StrategyRoleData& role);
} // namespace atx::impl::strategy

namespace atx::impl::strategy::ic_detail {
struct PinnedWeights;
// The runner's hook (one call after IcComposition::create_from_stages): when the pinned weights run
// theme-cov-hedge-v1, the role's session returns go to composition.set_theme_hedge; otherwise
// nothing is read or changed.
[[nodiscard]] atx::core::Status attach_theme_hedge(const PinnedWeights& pinned,
                                                   const engine::data::StrategyRoleData& role,
                                                   IcComposition& composition);
} // namespace atx::impl::strategy::ic_detail

#pragma once

// Cost model v2 (platform v7 lane L4): name-level market-impact laws as NAV stress
// scenarios, the replayed capacity scenarios, and the cost-aware construction rule
// aim-partial-v6 (literature-v7 R2.2, R2.3, R2.5, R4.1, R4.2). Pure functions and
// value types only; the NAV replay reaches them through strategy_nav_v7.hpp.
//
// Units: a "cost fraction" is dollars of cost per dollar traded; participation x is
// |fill dollars| / ADV dollars of the execution session's liquidity row.

#include <array>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::cost_v2 {

// ---- Kyle-Obizhaeva (2016, Econometrica) invariance, square-root fit -------------------
// impact(x) = (sigma/.02) (W/W*)^(-1/3) [k0 + kI sqrt(x (W/W*)^(2/3) / .01)] bps, with
// W = sigma * ADV dollars (daily "trading activity"), W* = .02 * $40 * 1e6 shares.
// Benchmark stock at 1% of ADV: 2.08 + 12.08 = 14.16 bps.
inline constexpr atx::f64 ko_kappa0_bps = 2.08;
inline constexpr atx::f64 ko_kappa_i_bps = 12.08;
inline constexpr atx::f64 ko_sigma_ref = 0.02;
inline constexpr atx::f64 ko_activity_ref = 0.02 * 40.0 * 1.0e6; // W* = $800,000
inline constexpr atx::f64 ko_participation_ref = 0.01;
// ---- Frazzini-Israel-Moskowitz (2018 WP "Trading Costs") live-trade model, eq. (8) -------
// MI(y) = a + b y + c sqrt(y) bps, y = participation in PERCENT of daily volume.
// b, c: Table VII column (10) (United States); a: set so the model equals the Table XI
// cross-sectional median at 1% of DTV (14.55 bps). MI includes commissions (FIM: "captured
// in our execution prices"), so the FIM scenario charges no separate commission.
inline constexpr atx::f64 fim_b_bps = -0.53;
inline constexpr atx::f64 fim_c_bps = 11.21;
inline constexpr atx::f64 fim_median_1pct_bps = 14.55;
inline constexpr atx::f64 fim_a_bps = fim_median_1pct_bps - fim_b_bps - fim_c_bps; // 3.87

// KO cost fraction at participation x on (daily_vol, adv_dollars). NaN when adv is not
// finite > 0, daily_vol not finite >= 0 or x not finite >= 0; 0 impact at daily_vol 0 (the
// limit of the formula).
[[nodiscard]] atx::f64 ko_cost_fraction(atx::f64 participation, atx::f64 daily_vol,
                                        atx::f64 adv_dollars) noexcept;
// FIM cost fraction at participation x (a fraction; y = 100 x). Floored at 0; NaN when x is
// not finite >= 0.
[[nodiscard]] atx::f64 fim_cost_fraction(atx::f64 participation) noexcept;

enum class ImpactLaw : atx::u8 { KyleObizhaevaV1 = 1, FimLiveV1 = 2 };

// Per-name trade cost under one law, plus a commission, with the replay's participation
// cap (|fill| <= max_participation * ADV). Mirrors engine SqrtImpactCost: a row with no
// usable ADV fills nothing; a complete fill returns the request bit for bit.
class NameImpactCost final : public atx::engine::book::ReplayCostModel {
public:
  // commission_bps finite >= 0; max_participation in (0, inf].
  [[nodiscard]] static atx::core::Result<NameImpactCost> create(ImpactLaw law,
                                                                atx::f64 commission_bps,
                                                                atx::f64 max_participation);
  [[nodiscard]] atx::engine::book::TradeCost cost(
      atx::usize instrument, atx::usize period, atx::f64 trade_dollars,
      const atx::engine::book::LiquidityRow& liquidity) const noexcept override;
  // Full request at the uncapped rate; NaN on an unusable row.
  [[nodiscard]] atx::f64 unrationed_cost(
      atx::usize instrument, atx::usize period, atx::f64 trade_dollars,
      const atx::engine::book::LiquidityRow& liquidity) const noexcept override;
  // Cost per dollar of a fill of |abs_dollars| (commission included), no cap; NaN when unusable.
  [[nodiscard]] atx::f64 cost_fraction(atx::f64 abs_dollars,
                                       const atx::engine::book::LiquidityRow& liquidity) const
      noexcept;
  [[nodiscard]] ImpactLaw law() const noexcept { return law_; }

private:
  NameImpactCost(ImpactLaw law, atx::f64 commission_bps, atx::f64 max_participation) noexcept
      : law_(law), commission_bps_(commission_bps), max_participation_(max_participation) {}
  ImpactLaw law_{ImpactLaw::KyleObizhaevaV1};
  atx::f64 commission_bps_{}, max_participation_{};
};

// ---- Stress scenarios next to S1/S2/S3 (S2 stays primary) ------------------------------
// Reserved trading ids. Each is a copy of the run's primary S2 book (financing, fallback vol,
// stale rule, 1% ADV cap) whose cost model is the named law instead of the sqrt law. They
// are declared SqrtImpactV1 with half_spread 0 and impact_y 0 so the replay's validation and
// linear/impact split apply unchanged (linear = commission); the model itself comes from
// reserved_cost_model, keyed by the id.
inline constexpr std::string_view ko_scenario_id = "modeled-1bn-ko-v1";
inline constexpr std::string_view fim_scenario_id = "modeled-1bn-fim-v1";
[[nodiscard]] NavScenario ko_scenario(const NavScenario& s2);  // commission = S2's (1 bps)
[[nodiscard]] NavScenario fim_scenario(const NavScenario& s2); // commission 0 (inside MI)
// The v2 model of a reserved id: Ok(nullptr) for any other id (the replay's own model then
// applies); InvalidArgument when a reserved id carries another shape (a spec cannot ride a
// reserved id with different parameters).
[[nodiscard]] atx::core::Result<std::unique_ptr<const atx::engine::book::ReplayCostModel>>
reserved_cost_model(const NavScenario& s);
// Recipe label of a reserved id ("ko-invariance-v1" / "fim-live-v1"); nullptr otherwise.
[[nodiscard]] const char* reserved_cost_rule(std::string_view trading_id) noexcept;

// ---- Capacity curve by replay (R4.2) -----------------------------------------------------
// NAV multiples m of the declared initial NAV. The replay is scale invariant in every dollar
// except the cost law and the participation cap, so the S2 book at NAV m x V equals, divided
// by m, the S2 book at V with impact_y' = impact_y * m^delta and max_participation' =
// max_participation / m: for fill F at scale m and F' = F / m, min(|F'|, (p/m) ADV) =
// min(|F|, p ADV) / m and |F'| (L + Y m^d sigma (|F'|/ADV)^d) = cost(F) / m. Returns, SR,
// cost per traded dollar and cap binding are those of the NAV-m book (fixed rate only: the
// per-name rate reads the NAV). m = 1 is S2 bit for bit.
inline constexpr std::array<atx::f64, 5> capacity_multiples{0.5, 1.0, 2.0, 4.0, 8.0};
[[nodiscard]] std::vector<NavScenario> capacity_scenarios(const NavScenario& s2);
// The multiple of a capacity scenario id; NaN for any other id.
[[nodiscard]] atx::f64 capacity_multiple(std::string_view trading_id) noexcept;

// ---- Decision liquidity (the NAV's execution liquidity at a decision session) ------------
// For every member of decision d: raw-dollar ADV over [d-w, d) (absent rows count 0) and the
// sample SD of adjusted simple returns over present, unguarded adjacent pairs in the window
// (NaN below min_pairs). The same arithmetic as strategy_nav_replay.cpp window_liquidity.
// Nonmembers are NaN. Requires in.volume at full geometry.
struct DecisionLiquidity {
  std::vector<atx::f64> adv, sigma;
};
void decision_liquidity(const TargetReplayInput& in, atx::usize d, atx::usize window,
                        atx::usize min_pairs, DecisionLiquidity& out);

// ---- aim-partial-v6 (R2.2 + R2.3) --------------------------------------------------------
// On a rebalance decision, for member i with modelled marginal cost per dollar c_i (primary
// S2 law at the reference trade q = theta * L * NAV / N_d) and c_bar the members' median:
//   target_i = desired_i / (1 + kappa c_i/c_bar), then longs and shorts each rescaled to the
//              desired side gross (net and gross of the aim kept; kappa 0: target == desired)
//   band_i   = (b / N_d) (c_i/c_bar)^(1/3), replacing the uniform dust band
//   theta_t  = theta clip((c_ref / c_bar)^(1/2), lo, hi), c_ref = mean c_bar over the last
//              `reference_decisions` rebalance decisions (today included)
// A member without a usable cost (no ADV) gets target 0 when kappa > 0 and the median band.
// Everything else is aim-partial-v5: aim = L * target, dusted members keep their weight,
// the rest move by theta_t, nonmembers exit (or decay at exit_rate inside dust / N_d).
struct AimV6Params {
  atx::f64 kappa{1.0};
  atx::f64 band_b{0.1};           // default: the run's dust multiple (median band = dust)
  atx::f64 clip_lo{0.5}, clip_hi{1.5};
  atx::f64 band_exponent{1.0 / 3.0}; // fixed by the rule (Janecek-Shreve); tests use 0
  atx::usize reference_decisions{252};
};
[[nodiscard]] atx::core::Status validate_aim_v6(const AimV6Params& p, atx::f64 theta);
// Marginal cost per dollar of the S2 sqrt law at trade size q (dollars):
// (half_spread + commission) 1e-4 + (1 + delta) Y sigma (q/adv)^delta; sigma NaN -> the
// scenario's fallback vol; NaN when adv is not finite > 0 or q not finite >= 0.
[[nodiscard]] atx::f64 marginal_cost_s2(const NavScenario& s2, atx::f64 q, atx::f64 adv,
                                        atx::f64 sigma) noexcept;
// Median of the finite values (mean of the two middle ones for an even count); NaN if none.
[[nodiscard]] atx::f64 finite_median(std::span<const atx::f64> values);
struct AimV6Decision {
  std::vector<atx::f64> target, band; // per name; band -1 = off (nothing dusts)
  atx::f64 theta{}, c_bar{}, c_ref{}, scale_long{1.0}, scale_short{1.0};
  atx::usize costed{}, members{};
};
// Forms the v6 targets/bands/theta of decision d. cost: per-name c_i (NaN unusable); c_ref:
// the reference median (NaN: no rate adjustment). Not a rebalance: target = desired, band
// off, theta = theta (the move then keeps members; only exits trade, as v5).
void form_aim_v6(std::span<const atx::u8> member, std::span<const atx::f64> desired,
                 std::span<const atx::f64> cost, atx::f64 c_bar, atx::f64 c_ref,
                 atx::f64 theta, const AimV6Params& p, bool rebalance, AimV6Decision& out);
// The v6 move of decision d on `current` (updated in place; out accumulates the plan
// fields and banded_names exactly as aim-partial-v5's update_weights). cfg must be the
// aim-partial-v5 base config (theta, dust and exit_rate for nonmembers, aim_leverage).
// With kappa 0, clip [1, 1] and every band equal to dust_multiple / N_d it is v5 bit for bit.
[[nodiscard]] atx::core::Status aim_partial_v6_weights(const TargetReplayInput& in,
                                                       const TargetReplayConfig& cfg,
                                                       atx::usize d, bool rebalance,
                                                       const AimV6Decision& v6,
                                                       std::vector<atx::f64>& current,
                                                       TargetReplayDay& out);

// ---- Transfer coefficient (R2.5, Clarke-de Silva-Thorley 2002) ---------------------------
// TC = corr(alpha_i / sigma_i^2, w_i) over members with finite sigma_i > 0, where alpha_i =
// desired_i sigma_i (Grinold-Kahn alpha = IC sigma z with z the neutralized rank aim; IC
// cancels in a correlation), i.e. alpha_i / sigma_i^2 = desired_i / sigma_i. NaN below 3
// names or with a constant side.
[[nodiscard]] atx::f64 transfer_coefficient(std::span<const atx::u8> member,
                                            std::span<const atx::f64> desired,
                                            std::span<const atx::f64> sigma,
                                            std::span<const atx::f64> weights) noexcept;
} // namespace atx::impl::strategy::cost_v2

#pragma once
// atx::engine::research::composition -- the fit side of the v8 composition rules on a K-P9-4
// factor series (P9 lane D2; migration plan section 1 `composition/`, slice 6; composition review
// finding 3 "the missing core": before D2 every estimate below was computed only by
// atx-impl/tools/fit_composition_weights.py and its composition_*.py modules, and the C++ verified
// the last arithmetic step alone). `atx-research-composition fit` (composition_cli.hpp) drives it.
//
// What is fitted, member by member in the caller's order (the fitter's admitted members in
// admission order), every theme numbered in ascending name order:
//   parent    ew-theme-std-v1 / ew-theme-std-aim-v1: tier re-grades, tier scores (x aim gains),
//             theme_gain_weights (ew_theme_std.hpp); ic-shrink-v1 / ic-shrink-aim-v1: each member's
//             IC = its admission train_mean (admission::mean_sharpe over its live TRAIN decisions,
//             the screen's own function, so the value is admission.csv's bit for bit), then
//             ic_shrink_weights (ic_shrink.hpp); ew-theme-aim-v2 (P9 lane D3W-PB):
//             theme_gain_weights on the members' aim gains (ew_theme_std.hpp), which reads no
//             factor value.
//   erc       theme-erc-v1 on the parent: within-theme shares a_k (the parent's pre-cap weights
//             normalised inside the theme), the TRAIN sleeves r_t(d) = sum_{k in t} a_k s_k f_k(d)
//             and their sample covariance (theme_sleeve_covariance), theme_erc_weights.
//   tsmom     theme-tsmom-v1 on the final weights: sleeves, trailing sums and block masses
//             (theme_tsmom_schedule).
//   resid     theme-resid-v1: the registered order restricted to the weighted themes.
//
// Summation order (pinned; composition review "Float identity" and table row 5): every elementwise
// expression is the Python's, in member order; a sleeve's mean is numpy's pairwise sum / n
// (numpy_pairwise_sum); the covariance's inner products and the trailing sums run sequentially in
// decision order. numpy computes the inner products through BLAS (np.dot), whose order this code
// does not reproduce, so on real data a covariance entry may differ from the Python's in its last
// bits (the fitter's --engine-fit check holds every fitted value to the Python's within
// 1e-12 x max(1, |x|), ruling P12's tolerance); on inputs whose sums are exact (the committed
// fixture) every value is the Python's bit for bit.
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/group_shrink.hpp"
#include "atx/engine/research/composition/ew_theme_std.hpp"
#include "atx/engine/research/composition/ic_shrink.hpp"
#include "atx/engine/research/composition/mom_volman.hpp"
#include "atx/engine/research/composition/theme_erc.hpp"

namespace atx::engine::research::composition {

// The decisions a fit reads: one K-P9-4 factor series and its TRAIN mask.
struct FactorSeriesView {
  atx::usize candidates{};
  atx::usize decisions{};
  std::span<const atx::f64> factors;  // decisions x candidates, decision-major; NaN = flat
  std::span<const atx::i64> sessions; // decisions, strictly increasing
  std::span<const atx::u8> train;     // decisions: 1 when the decision session lies in TRAIN
};

// numpy's sum of a contiguous float64 vector (np.add.reduce: pairwise, blocks of at most 128
// values summed with eight accumulators, halves above), added to 0.0.
[[nodiscard]] atx::f64 numpy_pairwise_sum(std::span<const atx::f64> x) noexcept;

// The signed, zero-filled series s * f(d) of one candidate column over every decision (a flat
// decision 0): the fitter's prior_signs[k] * zero_filled[k].
[[nodiscard]] std::vector<atx::f64> signed_series(const FactorSeriesView& view, atx::usize column,
                                                  atx::i32 sign);

// The admission train_mean of one candidate column (admission::mean_sharpe over the finite TRAIN
// decisions, annualisation 252); nullopt without a live TRAIN decision.
[[nodiscard]] std::optional<atx::f64> train_mean(const FactorSeriesView& view, atx::usize column);

// theme-erc-v1 step 1: each member's pre-cap parent weight divided by its theme's total (member
// order). Err (InvalidArgument) when a theme's total is not > 0 or the shapes differ.
[[nodiscard]] atx::core::Result<std::vector<atx::f64>> within_theme_shares(
    std::span<const atx::f64> uncapped, std::span<const atx::usize> theme, atx::usize themes);

// theme-erc-v1 step 2: the sleeves r_t(d) = sum_{k in t} share_k * x_k(d) over the TRAIN
// decisions (members in input order, x_k = `series[k]`, each over every decision) and their
// sample covariance (themes x themes, row-major, exactly symmetric; divisor n - 1). Err
// (InvalidArgument) below two TRAIN decisions or on mismatched shapes.
struct SleeveCovariance {
  std::vector<atx::f64> covariance;
  atx::usize decisions{}; // the TRAIN decisions n
};
[[nodiscard]] atx::core::Result<SleeveCovariance> theme_sleeve_covariance(
    std::span<const std::vector<atx::f64>> series, std::span<const atx::u8> train,
    std::span<const atx::f64> share, std::span<const atx::usize> theme, atx::usize themes);

// theme-tsmom-v1 steps 1-3 (theme_tsmom.hpp) on the final weights: the weighted themes (weight > 0)
// in ascending index order, their masses W_t, and per block j = lookback + lag - 1 + b * step < D
// its session, the trailing sums T_t(j) and theme_tsmom_masses' masses. The sleeves are 0 outside
// TRAIN. Err (InvalidArgument) on non-finite or negative weights, sessions that do not increase,
// 0 or more than 32 weighted themes, or no block (or more than theme_tsmom_max_blocks).
struct TsmomBlock {
  atx::usize decision_index{};
  atx::i64 from_session{};
  std::vector<atx::f64> trailing,masses; // per weighted theme
  atx::usize off{};                      // themes switched off
};
struct TsmomSchedule {
  std::vector<atx::usize> themes;        // the weighted themes' indices, ascending
  std::vector<atx::f64> parent_mass;     // W_t per weighted theme
  std::vector<TsmomBlock> blocks;
  atx::usize off{};                      // themes switched off over every block
  std::vector<atx::usize> on_count;      // per weighted theme: blocks with T_t > 0
};
[[nodiscard]] atx::core::Result<TsmomSchedule> theme_tsmom_schedule(
    std::span<const std::vector<atx::f64>> series, std::span<const atx::i64> sessions,
    std::span<const atx::u8> train, std::span<const atx::f64> weights,
    std::span<const atx::usize> theme, atx::usize themes);

// theme-resid-v1 rule 1 (composition_resid.theme_order, Ruling PM4-11): `registered` (distinct,
// beginning with theme_resid_frozen_prefix) restricted to the `weighted` themes, in registered
// order. Err (InvalidArgument) on a weighted theme outside it, no weighted theme, or a malformed
// registered list.
inline constexpr std::array<std::string_view,10> theme_resid_frozen_prefix{
    "value",          "profitability_quality", "investment_issuance", "earnings_momentum",
    "price_momentum", "low_risk",              "short_interest",      "reversal_seasonality",
    "options_implied", "ownership_flow"};
[[nodiscard]] atx::core::Result<std::vector<std::string>> theme_resid_order(
    std::span<const std::string> registered, std::span<const std::string> weighted);

// ---- The whole fit ---------------------------------------------------------------------------
struct FitSpecMember {
  std::string id;
  std::string theme;
  std::string tier;          // ew-theme-std rules: the source tier (registry, else the library)
  atx::i32 sign{1};          // the prior sign: +1
  std::optional<atx::f64> gain; // the aim rules: the member's aim gain
};
struct FitSpec {
  // ew-theme-std-v1 | ew-theme-std-aim-v1 | ic-shrink-v1 | ic-shrink-aim-v1 | ew-theme-aim-v2
  std::string composition;
  std::vector<FitSpecMember> members;
  std::vector<std::pair<std::string,atx::f64>> tier_scores; // ew-theme-std rules: tier -> score
  bool theme_erc{};
  bool theme_tsmom{};
  bool theme_resid{};
  bool mom_volman{};         // P9 D3W (ruling ALCOMB-7): record the price_momentum sleeve
  std::vector<std::string> registered_themes; // theme-resid-v1: the registered order
};
struct CompositionFit {
  std::vector<std::string> themes;       // the members' themes, ascending
  std::vector<atx::usize> member_theme;  // per member
  std::vector<atx::usize> column;        // per member: its factor series column
  atx::usize train_decisions{};
  // The parent composition (per member; weights after its cap).
  std::vector<atx::f64> parent_weights,parent_uncapped;
  atx::f64 parent_cap{};
  std::vector<atx::engine::combine::GroupCapPass> parent_passes;
  std::vector<std::string> tiers;        // ew-theme-std: after the re-grades
  std::vector<atx::f64> scores;          // ew-theme-std: tier scores (before the gains)
  std::vector<RegradeApplied> regrades;  // ew-theme-std
  std::vector<atx::f64> ic,shrunk,share; // ic-shrink: per member
  std::vector<atx::f64> theme_mean;      // ic-shrink: per theme, the mean IC
  std::vector<atx::u8> equal_theme;      // ic-shrink: per theme
  std::vector<atx::f64> gains;           // ew-theme-aim-v2: per member, its aim gain (its score)
  std::vector<atx::f64> theme_weight;    // ew-theme-aim-v2: per theme, its capped weights summed
  // theme-erc-v1 (erc_share per member, covariance per theme pair).
  bool erc{};
  std::vector<atx::f64> erc_share,covariance;
  ThemeErcFit erc_fit;
  // theme-tsmom-v1.
  bool tsmom{};
  TsmomSchedule schedule;
  // theme-resid-v1.
  bool resid{};
  std::vector<std::string> resid_order;
  // mom-volman-v1's sleeve (P9 lane D3W, ruling ALCOMB-7): per decision.
  bool volman{};
  std::vector<atx::f64> volman_sleeve;
  std::vector<atx::f64> weights;         // per member: the final weights
};
// mom-volman-v1's sleeve (P9 lane D3W, ruling ALCOMB-7; composition_mom_volman.schedule): theme-
// tsmom-v1's step 1 for the price_momentum theme on the final weights, r(d) = sum over the theme's
// weighted members of (w_k / W) s_k f_k(d) in member order, 0 outside TRAIN, over every decision of
// the series. The IC runner forms every block's volatility, target and scale from it
// (strategy_ic_mom_volman.cpp). The registered parents are ew-theme-std-v1 and ic-shrink-v1, with
// or without theme-erc-v1; the role needs mom_volman_min_decisions (lookback + lag, 126 + 2) to
// mom_volman_max_decisions decisions (mom_volman.hpp: the one definition both sides read).

// The fit of `spec` on `view`, whose candidate columns carry the ids `ids`. Err (InvalidArgument)
// on an unknown composition, a member not in `ids` or listed twice, a sign other than +1, a missing
// or extra aim gain, an ew-theme-aim-v2 gain that is not finite and > 0, theme-erc-v1,
// theme-tsmom-v1, theme-resid-v1 or mom-volman-v1 on ew-theme-aim-v2 (a plain weighted blend: no
// theme block), a tier without a score, theme-resid-v1 beside theme-erc-v1 or theme-tsmom-v1,
// mom-volman-v1 on an aim rule or beside theme-tsmom-v1 or theme-resid-v1, mom-volman-v1 without a
// weighted price_momentum member or on a series outside its decision bounds, or any refusal of the
// rule functions.
[[nodiscard]] atx::core::Result<CompositionFit> fit_composition(const FactorSeriesView& view,
                                                                std::span<const std::string> ids,
                                                                const FitSpec& spec);

// ---- walk-forward-v1 (contract K-P9-8; P9 lane D3W, rulings D3-1 / D3-2) ---------------------
// The same fit refitted on an expanding window. Rows sit at decision indices j = first,
// first + step, ... < D; the window of row j is the TRAIN decisions d with d + lag <= j, and
// `first` is the smallest j whose window holds min_decisions decisions. Each row is
// fit_composition of the spec on its window (the view's TRAIN mask cut at j - lag), so no fitted
// value of row j reads a factor value of a decision after j - lag (f(d) is realised at d + 2: lag
// 2 is K-P9-8's "fitted on decisions <= d - 2"). The IC runner applies row j from the session of
// decision j until the next row's; before the first row no fit exists and the book is flat (ruling
// D3-1). Registered constants of walk-forward-v1: window expanding, lag 2, step 21 (plan 5.2),
// min_decisions 21 (one step).
inline constexpr std::string_view walk_forward_rule="walk-forward-v1";
inline constexpr std::string_view walk_forward_expanding="expanding";
inline constexpr atx::usize walk_forward_lag=2;
inline constexpr atx::usize walk_forward_step=21;
inline constexpr atx::usize walk_forward_min_decisions=21;
inline constexpr atx::usize walk_forward_max_rows=4096;
struct WalkForwardSpec {
  std::string window{walk_forward_expanding};
  atx::usize lag{walk_forward_lag};
  atx::usize step{walk_forward_step};
  atx::usize min_decisions{walk_forward_min_decisions};
};
struct WalkForwardRow {
  atx::usize decision_index{};  // j
  atx::i64 from_session{};      // the session of decision j: the row applies from it
  atx::usize fit_decisions{};   // the window's TRAIN decisions
  atx::usize last_fit_index{};  // the window's last TRAIN decision (<= j - lag)
  atx::i64 last_fit_session{};
  CompositionFit fit;           // the window's fit (fit.weights: per member, the spec's order)
};
// Err (InvalidArgument) on a window other than expanding, lag < 2, step 0, min_decisions < 2,
// theme-tsmom-v1 or theme-resid-v1 in the spec (their own TRAIN-fitted schedule and order),
// ew-theme-aim-v2 (P9 lane D3W-PB: its weights read no factor value, only the spec's whole-TRAIN
// aim gains, so every row would be the full fit), a
// series whose TRAIN decisions never fill a window, more than walk_forward_max_rows rows, or a row
// whose fit refuses (the message names its decision index and session). A row's fit records no
// mom-volman-v1 sleeve (that schedule is fitted on the whole TRAIN window: the full fit's).
[[nodiscard]] atx::core::Result<std::vector<WalkForwardRow>> fit_walk_forward(
    const FactorSeriesView& view, std::span<const std::string> ids, const FitSpec& spec,
    const WalkForwardSpec& wf);
} // namespace atx::engine::research::composition

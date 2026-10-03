#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::parallel { class DetPool; }
namespace atx::impl::strategy {
struct IcCompositionCandidate {
  std::string id, family;
};
struct IcCompositionConfig {
  atx::usize dates{}, instruments{}, decision_begin{}, decision_end{};
  atx::usize cadence{5};
  atx::f64 trade_fraction{0.25};
  atx::u64 max_working_bytes{256ULL << 20};
};
// Rule applied to pinned_themes (see create; ignored without themes).
//   redistribute  fitter ew-theme-v6, rule within-theme-v1.
//   standardise   fitter ew-theme-std-v1 (platform v8 R-1).
//   residualise   standardise, then theme-resid-v1 (platform v8 R-11; theme index = position
//                 in the registered order, strategy_ic_theme_resid.hpp).
enum class IcThemeRule : atx::u8 { redistribute, standardise, residualise };
// One block of a theme-mass schedule (theme-tsmom-v1, platform v8 Y-2; strategy_ic_theme_tsmom.hpp):
// from date `begin` until the next block's begin (the last: to the end) the standardised themes
// enter with `mass` (one finite value >= 0 per theme index) in place of W_theme.
struct IcThemeBlock {
  atx::usize begin{};
  std::vector<atx::f64> mass;
};
struct IcCompositionResult {
  std::vector<atx::f64> signal; // date-major; nonmembers NaN, missing contributions zero (pinned themes: see create)
  std::vector<atx::f64> planned_turnover, contribution_fraction;
  std::vector<atx::f64> planned_gross, planned_net;
  std::vector<atx::usize> eligible_names;
  atx::f64 total_planned_turnover{}, deployment_turnover{};
  atx::usize deployment_date{}; // dates if there was no nonzero deployment
  // two-speed-v1 (platform v8 Y-5; empty unless set_theme_sleeves ran): the fast and the slow
  // themes' parts of `signal` (each: members 0 plus its themes' W_theme x re-rank, nonmembers
  // NaN, as `signal` is built) and per date the fast themes' share of the theme mass in force
  // over the themes with a present member at that date (0 when none has one; Ruling PM8-16 #9).
  std::vector<atx::f64> sleeve_fast, sleeve_slow, sleeve_fast_share;
};
// Conservative owned allocation envelope, including result + scratch and bounded
// candidate strings. Excludes caller Panel, VM, labels and incoming signal.
// `themes` > 0 (pinned themes, at most 32) adds two f64 planes per theme under
// `redistribute` and one under `standardise`; `residualise` adds to that one plane per theme
// the regression scratch, instruments x (8 x themes + 8) B; 0 is the unchanged envelope.
// `sleeves` (two-speed-v1, platform v8 Y-5) adds the two sleeve planes and the share row.
[[nodiscard]] atx::core::Result<atx::u64> ic_composition_working_bytes(
    atx::usize dates, atx::usize instruments, atx::usize candidates, atx::usize themes = 0,
    IcThemeRule rule = IcThemeRule::redistribute, bool sleeves = false);

// Streaming equal-family/equal-within-family centered tied-rank composition.
// create copies membership and candidate metadata. add borrows one signal only
// for that call and requires library order exactly once, including sign==0.
// Per-add sign permits TRAIN evaluate -> orient -> add -> discard in one VM
// pass. Validation supplies those same frozen signs; this helper never fits.
// No return labels or validation evidence enter this helper.
// Optional pinned_weights (library order, finite, >= 0) replace the equal
// family/within-family weights value-for-value: no normalization and the same
// accumulation expression, so pinning the default values reproduces its bits.
// A zero weight contributes nothing; without pinned_themes nothing is ever
// redistributed.
// Optional pinned_themes (library order, one theme index per candidate; needs
// pinned_weights; indices of zero-weight candidates are ignored; each < 32) switch
// on within-theme coverage redistribution (fitter composition ew-theme-v6, rule
// within-theme-v1): per name and date a theme adds
//   W_theme * sum_{k present} w_k s_k r_k / sum_{k present} w_k,
// W_theme = the sum of its pinned weights, so a member missing for that name keeps
// its mass inside its theme and a theme with no present member adds nothing.
// Present = a member name with a finite signal on a date where the candidate ranks
// >= 2 names, added with a nonzero sign. Empty (default): the path above, bit for bit.
// `rule` standardise (fitter ew-theme-std-v1, platform v8 R-1) replaces that
// redistribution with theme standardisation: per date and theme the weighted sum of the
// theme's present signed member ranks, sum_{k present} w_k s_k r_k (a missing member is
// neutral, nothing is redistributed; its order is that of the weighted mean, which only
// divides by the constant W_theme), is re-ranked over the names with at least one present
// member (centred tied rank in [-0.5, 0.5], atx/engine/combine/group_rerank.hpp) and adds
//   W_theme * rerank_theme,
// W_theme = the sum of its pinned weights; a name with no present member of a theme gets
// nothing from it. Every theme so enters with the same dispersion whatever its member
// count. Member ranks are the pinned path's expression; themes fold in index order.
// `rule` residualise (theme-resid-v1, platform v8 R-11) builds the same planes; per date the
// theme at index 0 adds what standardise adds for it, and the theme at index t > 0 adds W_theme
// times the re-ranked least-squares residual of its re-ranked composite on an intercept and the
// re-ranked composites of themes 0..t-1 (add_theme_residualised, strategy_ic_theme_resid.hpp).
class IcComposition {
 public:
  ~IcComposition();
  IcComposition(IcComposition&&) noexcept;
  IcComposition& operator=(IcComposition&&) noexcept;
  IcComposition(const IcComposition&) = delete;
  IcComposition& operator=(const IcComposition&) = delete;
  [[nodiscard]] static atx::core::Result<IcComposition> create(
      const IcCompositionConfig&, std::span<const IcCompositionCandidate>,
      std::span<const atx::u8> decision_member,
      std::span<const atx::f64> pinned_weights = {}, // empty: equal family/within
      std::span<const atx::usize> pinned_themes = {}, // empty: no redistribution
      IcThemeRule rule = IcThemeRule::redistribute);  // what pinned_themes switch on
  // Optional `pool` (borrowed; alive for the call, never invoked from inside one
  // of its jobs): dates split into contiguous bands, each ranked in a per-worker
  // row. Every blend cell and per-date coverage sum is written by exactly one
  // band with the serial expression, and candidates still arrive in library
  // order, so the result bits equal the serial path by construction. Worker rows
  // (16 B/name each) are allocated on the first pooled add.
  [[nodiscard]] atx::core::Status add(atx::usize candidate_index,
                                    std::span<const atx::f64> signal,
                                    int frozen_sign, // -1/+1; 0 neutral (pinned themes: mass stays in theme)
                                    atx::engine::parallel::DetPool* pool = nullptr);
  // Planned target proxy only: tied ranks, dollar-neutral desired gross1, no
  // winsorization. Cadence is anchored to decision_begin; interpolate .25 by
  // default from prior planned weights, with no price drift. Membership exits
  // force zero even off cadence; survivors are NOT renormalized. Consequently
  // planned gross/net are reported, not promised to stay1/0 between rebalances.
  // Initial deployment is included in both daily and total turnover.
  [[nodiscard]] atx::core::Result<IcCompositionResult> finish();
  // theme-tsmom-v1 (platform v8 Y-2): optional, before finish, IcThemeRule::standardise only.
  // Blocks in non-decreasing `begin` (each <= dates); dates before the first block keep W_theme.
  // finish then adds, per date and theme, the mass in force at that date times the same re-rank
  // (a zero mass adds nothing); dates are independent, so blocks repeating W_theme leave the blend
  // bit for bit unchanged. Not called (or empty): finish is unchanged. Refuses (InvalidArgument)
  // under another rule, after finish, or on a malformed block; nothing is kept on refusal.
  [[nodiscard]] atx::core::Status schedule_theme_masses(std::span<const IcThemeBlock> blocks);
  // two-speed-v1 (platform v8 Y-5): optional, before finish, IcThemeRule::standardise only. `fast`:
  // one flag per theme index (1 fast, 0 slow), at least one of each. finish then also adds each
  // theme's W x re-rank (the mass in force, as `signal` gets it) to its sleeve's plane and records
  // the fast themes' mass share per date (over the themes present at the date); `signal` is
  // unchanged bit for bit. Refuses
  // (InvalidArgument) under another rule, after finish or on a malformed flag row; OutOfRange when
  // the planes exceed the working budget. Nothing is kept on refusal.
  [[nodiscard]] atx::core::Status set_theme_sleeves(std::span<const atx::u8> fast);
 private:
  struct Impl;
  explicit IcComposition(std::unique_ptr<Impl>);
  // add() for IcThemeRule::standardise: the member's ranks go to its theme's plane.
  [[nodiscard]] atx::core::Status add_standardised(atx::usize candidate_index, std::span<const atx::f64> signal,
                                                   int frozen_sign, atx::engine::parallel::DetPool* pool);
  std::unique_ptr<Impl> impl_;
};
} // namespace atx::impl::strategy

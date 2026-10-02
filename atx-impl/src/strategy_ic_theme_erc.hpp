#pragma once
// Composition rule theme-erc-v1 (platform v8 expansion X, lane XCOMB; task-XCOMB-report.md).
// Hypothesis: theme shares that give every theme sleeve an equal share of the blend's risk
// (equal risk contribution) combine better than the parent's equal theme shares 1/T.
// Registration (every constant fixed blind, declared before any cell read):
//   1. Members and within-theme shares are the parent's: a_k = the parent rule's pre-cap
//      within-theme share of member k (ew-theme-std-v1: tier score / theme sum; ic-shrink-v1: its
//      shrunk-IC share), sum 1 inside each theme.
//   2. Theme sleeve t's daily return r_t(d) = sum_{k in t} a_k s_k f_k(d) over the parent fit's
//      decisions (the fitter's factor series: f_k the member's neutralised gross-1 rank book,
//      s_k its prior sign, a flat decision 0); C = their sample covariance (divisor n - 1).
//      Fitter only (atx-impl/tools/composition_theme_erc.py); C is recorded in the weights file.
//   3. Theme shares b = the equal risk contribution shares of C (atx/engine/combine/group_erc.hpp:
//      Spinu's problem by cyclical coordinate descent, theme_erc_sweeps sweeps in the recorded
//      theme order); the rule refuses unless max_t |c_t / mean(c) - 1| <= theme_erc_dispersion.
//   4. w_k = a_k * b_theme(k), then the parent's member cap 1/(2T) (excess pro rata to the other
//      themes' uncapped members, to a fixed point; T = themes with a member).
//   5. Standardisation unchanged: the per-date rule is ew-theme-std-v1's (IcThemeRule::
//      standardise); W_theme = sum of the theme's weights carries the new share into the blend.
// The IC runner verifies a weights file of the rule against the inputs its theme_standardise block
// records (strategy_ic_admission.cpp, the theme_standardise rule table).
#include <span>
#include <string_view>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
inline constexpr std::string_view theme_erc_rule="theme-erc-v1";
inline constexpr atx::usize theme_erc_sweeps=10000;           // CCD sweeps, fixed (no early exit)
inline constexpr atx::f64 theme_erc_dispersion=1e-10;         // max |c_t / mean(c) - 1| after the sweeps
inline constexpr atx::f64 theme_erc_share_tolerance=1e-12;    // |sum of a theme's shares - 1|
inline constexpr atx::f64 theme_erc_cap_tolerance=1e-12;      // relative (composition_rules.CAP_TOLERANCE)
inline constexpr atx::f64 theme_erc_weight_tolerance=1e-12;   // runner check: |pinned - rule| per weight
struct ThemeErcFit {
  std::vector<atx::f64> theme_share,contribution; // per theme (the covariance's order)
  std::vector<atx::f64> weights;                  // per member, input order, after the cap
  atx::f64 dispersion{};                          // max_t |c_t / mean(c) - 1|
  atx::f64 cap{};                                 // 1 / (2T)
  atx::usize cap_passes{};                        // capping passes run (0: no member above the cap)
};
// Members in input order: share[k] finite >= 0, theme[k] < themes, every theme with at least one
// member and its shares summing to 1 within theme_erc_share_tolerance; 1..256 members, 1..32
// themes; covariance: themes x themes, row-major, in the theme index order. Err
// (InvalidArgument, "theme-erc-v1: ...") on other inputs, on a covariance group_erc_shares
// refuses, on a dispersion above theme_erc_dispersion, or on an infeasible cap.
[[nodiscard]] atx::core::Result<ThemeErcFit> theme_erc_weights(std::span<const atx::f64> share,
                                                               std::span<const atx::usize> theme,
                                                               atx::usize themes,
                                                               std::span<const atx::f64> covariance);
} // namespace atx::impl::strategy

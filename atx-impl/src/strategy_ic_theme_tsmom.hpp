#pragma once
// Composition rule theme-tsmom-v1 (platform v8 Y, lane YCOMB; task-YCOMB-report.md rule Y-2).
// Hypothesis (Ehsani and Linnainmaa 2022, "Factor Momentum and the Momentum Factor", Journal of
// Finance 77(3); Gupta and Kelly 2019): a factor's own trailing one-year return predicts its next
// month; factors after a losing year earn about zero, those after a winning year earn the premium.
// Applied walk-forward to the blend's theme sleeves: at each block start a theme whose sleeve lost
// money over the trailing year gets mass 0 and its mass goes pro rata to the other themes; nothing
// is fitted on the scored window (each block reads only the sign of each sleeve's trailing sum, every
// term of which is realised at least `theme_tsmom_lag` sessions before the block's first decision).
// Registration (every constant fixed blind from the paper's 12-month formation and monthly holding,
// declared before any cell read):
//   1. Sleeve r_t(d) = sum_{k in t} (w_k / W_t) s_k f_k(d) over the parent fit's role decisions, in
//      decision order (the fitter's factor series: f_k(d) the member's neutralised gross-1 rank book
//      formed at d and realised at d + 2; s_k its sign; a flat decision 0), w the parent's final
//      weights, W_t = sum_{k in t} w_k. Fitter only (atx-impl/tools/composition_theme_tsmom.py).
//   2. Block starts at decision index j_b = lookback + lag - 1 + b * step (b = 0, 1, ...; j_b inside
//      the role); trailing sum T_t(j) = sum over d = j - lag - lookback + 1 .. j - lag of r_t(d).
//      The weights file's theme_schedule block records each block's first session and T per theme.
//   3. Masses (theme_tsmom_masses, re-applied here by the IC runner to the recorded T): g_t = 1 if
//      T_t > 0 else 0; if every g is 1 or every g is 0, the parent's W_t verbatim; else
//      W'_t = g_t * W_t * (S / K), S = sum_u W_u, K = sum_u g_u W_u (sums in the block's theme order).
//      Before the first block (the first lookback + lag - 1 decisions), the parent's W_t.
//   4. The per-date standardisation is ew-theme-std-v1's (IcThemeRule::standardise), unchanged; a
//      date uses the masses of the block in force at its session (the last block whose first session
//      is not after it), so a scored role after the last block keeps the last block's masses.
// The schedule rides on any theme_standardise rule with rerank true (the fitter writes it on its
// parents: theme-erc-v1, ew-theme-std-v1, ic-shrink-v1); it is refused with theme_residualise.
#include <span>
#include <string_view>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
inline constexpr std::string_view theme_tsmom_rule="theme-tsmom-v1";
inline constexpr atx::usize theme_tsmom_lookback=252;  // formation: 12 months of decisions
inline constexpr atx::usize theme_tsmom_lag=3;         // f(d) is realised at d + 2; one session of slack
inline constexpr atx::usize theme_tsmom_step=21;       // holding: one month of decisions
inline constexpr atx::usize theme_tsmom_max_blocks=4096;
// Step 3 on one block, in the block's theme order: `parent` the parent's masses W_u (finite, > 0),
// `trailing` the recorded T_u (finite); writes `out` (same length) and returns how many themes the
// block switches off (0 when every g is 1 or every g is 0: `out` is then `parent` verbatim). Err
// (InvalidArgument, "theme-tsmom-v1: ...") on other inputs, before any write.
[[nodiscard]] atx::core::Result<atx::usize> theme_tsmom_masses(std::span<const atx::f64> parent,
                                                               std::span<const atx::f64> trailing,
                                                               std::span<atx::f64> out);
} // namespace atx::impl::strategy

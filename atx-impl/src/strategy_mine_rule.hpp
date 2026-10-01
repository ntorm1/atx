#pragma once
// atx::impl::strategy — admission rule `mined-v1` of the mining verb (platform v8 H-3; declared in
// the v8 sprint plan, task H-3, used only under owner decision OD-7):
//
//   "promotion needs marginal IC HAC t at or above the Bonferroni value for the campaign's
//    effective trial count (3.5 at 100, 4.1 at 1,000, 4.6 at 10,000); sign frozen from the
//    discover window; one confirm read at HAC t 2.0 with Benjamini-Yekutieli p at or below .10;
//    abs(rho) at most .70 to every member; all mined members share one theme."
//
// As coded: the Bonferroni value is -norm_ppf(.05 / (2 N)) with N the campaign's --budget, fixed
// before the search and covering every trial it can evaluate (pre-registration rule 10, Ruling
// E-32a; never the realised or the registry's count). The discover statistic is f2 = the sign
// of the discover h 21 rank IC times the marginal IC HAC t (research_ic_fitness.hpp); the
// shortlist is every evaluated trial with f2 / F at or above the value (F the label-overlap
// factor below), in f2 order. The rho check is greedy in that order against every pool member
// and every earlier kept candidate (mean daily correlation of centred ranks on the discover
// decision rows). The confirm read is the marginal IC HAC t on the confirm window, oriented by
// the discover sign, and counts only on its full window (mined_confirm_defined; otherwise t is
// NaN, unconfirmed); it is read as t / F: one-sided p = Phi(-t / F), Benjamini-Yekutieli over
// the candidates that pass the rho check, confirmed iff t / F >= 2 and the adjusted p <= .10.
// The theme is `mined`.
//
// Ruling E-32a (review MINE-6): the label-overlap factor F. Both reads are Bartlett lag-21 HAC
// t's of a daily series on h 21 labels (neighbouring rows share 20 of 21 returns), which lag 21
// under-corrects for a persistent signal, so every hurdle of the rule applies to t / F. F is
// derived by simulation under the null in atx-impl/tools/mine_overlap_factor.py (seed 20260930;
// 30 names; i.i.d. N(0, 1) returns; a fully persistent signal, the worst case; the verb's own t,
// summarize_rank_ic at lag 21): the larger of the confirm gate's ratio on 200 label rows (the
// |t| quantile at 1 - 2 Phi(-2) over 2: 1.5422) and the discover tail's on 504 rows (the 99.8%
// |t| quantile over z(.999): 1.3931), rounded up to two decimals. Those windows are the rule's
// floors (kMinedMinConfirmRows, kMinedMinDiscoverRows): a longer window needs a smaller factor.
// The Bonferroni value lies beyond the 99.8% level once N exceeds 25 (z 3.09), where F is an
// extrapolation of the simulated tail (the MINE-FIX report gives a deeper one-off check).
//
// Ruling PM4-13 (binds under OD-7): F is checked only to N = kMinedMaxBudget (a one-off 200,000-
// draw null at the 504-row floor: ratio 1.46 at the 99.95% level, 1.51 at 99.99%, nominal near
// N = 1,000), so a --budget above it is refused until F is re-derived at the campaign's own
// Bonferroni level (v9). The ceiling is in the recipe and campaign.json's hurdle beside F.
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"

namespace atx::impl::strategy {

inline constexpr std::string_view kMinedRule = "mined-v1";
inline constexpr std::string_view kMinedTheme = "mined";
inline constexpr atx::f64 kMinedFamilyAlpha = 0.05;
inline constexpr atx::f64 kMinedConfirmT = 2.0;
inline constexpr atx::f64 kMinedConfirmBy = 0.10;
inline constexpr atx::f64 kMinedMaxAbsRho = 0.70;
// Review MINE-2: the fewest mature h 21 label rows a confirm read is made on (registered with the
// rule; the 2023 confirm window of research-window-v2 has about 228). The verb refuses a shorter
// confirm window before any search.
inline constexpr atx::usize kMinedMinConfirmRows = 200;
// Review MINE-6: the fewest mature h 21 label rows of a discover window (two years of sessions;
// the overlap factor's discover derivation). The verb refuses a shorter one before any search.
inline constexpr atx::usize kMinedMinDiscoverRows = 504;
// Ruling E-32a (review MINE-6): the label-overlap factor every hurdle of mined-v1 divides t by.
inline constexpr atx::f64 kMinedOverlapFactor = 1.55;
// Ruling PM4-13: the largest --budget kMinedOverlapFactor is validated to; the verb refuses a
// larger one before any payload, and backtest_integrity.campaign_line refuses its ledger line.
inline constexpr atx::u64 kMinedMaxBudget = 1000;

// t / kMinedOverlapFactor (NaN stays NaN): the scale every hurdle of mined-v1 is read on.
[[nodiscard]] atx::f64 mined_overlap_corrected(atx::f64 t) noexcept;

// Review MINE-2: a confirm read counts only on its full window -- the h 21 rank IC defined (and
// on at least kMinedMinConfirmRows label rows) and the marginal IC defined on every one of the
// window's `label_rows` -- so its HAC t is never read off a few overlapping label rows.
[[nodiscard]] bool mined_confirm_defined(bool ic_defined, atx::usize ic_dates,
                                         atx::usize marginal_dates,
                                         atx::usize label_rows) noexcept;

// -norm_ppf(.05 / (2 N)): 3.48 at 100, 4.06 at 1,000, 4.56 at 10,000. NaN when N is 0.
[[nodiscard]] atx::f64 mined_hurdle(atx::u64 trials) noexcept;

// An evaluated trial as the rule reads it: f2 = discover sign x marginal IC HAC t.
struct MinedRead {
  atx::u64 canon_hash{};
  atx::f64 f2 = std::numeric_limits<atx::f64>::quiet_NaN();
};

// Indices of `reads` with a finite f2 whose overlap-corrected value (mined_overlap_corrected) is
// at or above `hurdle`, by f2 descending then canonical hash ascending, at most `cap`.
[[nodiscard]] std::vector<atx::usize> mined_shortlist(std::span<const MinedRead> reads,
                                                      atx::f64 hurdle, atx::usize cap);

inline constexpr atx::usize kMinedNoRow = std::numeric_limits<atx::usize>::max();

// The largest |rho| a candidate met (NaN: no defined pair) and the row it met it on.
struct MinedRho {
  bool pass{};
  atx::f64 max_abs = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::usize against{kMinedNoRow};
};

// Rows [0, pool_rows) of `rho` are the pool members, row pool_rows + k is shortlist candidate k.
// Candidate k passes iff |rho| <= .70 to every member and every earlier passing candidate; an
// undefined pair (no date with enough jointly ranked names) does not block.
[[nodiscard]] std::vector<MinedRho>
mined_rho_select(const atx::engine::combine::PairwiseRowCorrelation &rho, atx::usize pool_rows,
                 atx::usize candidates);

struct MinedConfirm {
  atx::f64 t = std::numeric_limits<atx::f64>::quiet_NaN();           // oriented HAC t
  atx::f64 t_corrected = std::numeric_limits<atx::f64>::quiet_NaN(); // t / kMinedOverlapFactor
  atx::f64 p = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 p_by = std::numeric_limits<atx::f64>::quiet_NaN();
  bool confirmed{};
};

// One confirm read per candidate (oriented HAC t; NaN when undefined, then p = 1), read as
// t / F: one-sided p = Phi(-t / F), Benjamini-Yekutieli adjusted across the candidates;
// confirmed iff t / F >= 2 and the adjusted p <= .10.
[[nodiscard]] std::vector<MinedConfirm> mined_confirm(std::span<const atx::f64> oriented_t);

} // namespace atx::impl::strategy

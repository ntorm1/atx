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
// factor of N's budget band, below), in f2 order. The rho check is greedy in that order against
// every pool member and every earlier kept candidate (mean daily correlation of centred ranks on
// the discover decision rows). The confirm read is the marginal IC HAC t on the confirm window,
// oriented by the discover sign, and counts only on its full window (mined_confirm_defined;
// otherwise t is NaN, unconfirmed); it is read as t / Fc, Fc the confirm factor of the m reads
// that reach it: one-sided p = Phi(-t / Fc), Benjamini-Yekutieli over the m reads, confirmed iff
// t / Fc >= 2 and the adjusted p <= .10. The theme is `mined`.
//
// Ruling E-32a (review MINE-6): label overlap. Both reads are Bartlett lag-21 HAC t's of a daily
// series on h 21 labels (neighbouring rows share 20 of 21 returns), which lag 21 under-corrects
// for a persistent signal, so every hurdle of the rule applies to t over a factor. Lane MINE-STAT
// derives the factors at the rule's own levels in atx-impl/tools/mine_overlap_factor.py (seed
// 20261001; 30 names; i.i.d. N(0, 1) returns; a fully persistent signal, the worst case; the
// verb's own t, summarize_rank_ic at lag 21; importance-sampled tails with bootstrap standard
// errors; each factor the upper two-standard-error bound of a quantile ratio, rounded up to two
// decimals):
//   kMinedOverlapBands  F(N) = Q(t, 1 - .05 / (2 N)) / z(N) on the 504-row discover floor, so the
//                       per-trial rate of f2 / F >= z(N) is the Bonferroni .05 / N under the
//                       null; a budget band reads the factor of its top (the ratio grows into the
//                       tail, so the top covers the band).
//   kMinedConfirmBands  Fc(m) = the larger of the confirm gate's ratio (level Phi(-2)) and the
//                       ratio at Benjamini-Yekutieli's first step-up level .10 / (m H(m)), H the
//                       harmonic number, on the 200-row confirm floor: the BY proof needs the p's
//                       valid at every step-up level and k = 1 is the deepest. A band of m reads
//                       the factor of its top.
// The windows are the rule's floors (kMinedMinConfirmRows, kMinedMinDiscoverRows): a longer
// window needs a smaller factor (on the whole of TRAIN, 984 label rows, the discover ratios are
// lower at every budget), so the floors' tables hold for every admitted window.
//
// Ruling PM4-13 (binds under OD-7), lifted by the table: a --budget above kMinedMaxBudget, the
// overlap table's last top, is refused before any payload. The tables are in the recipe; the
// factors a campaign used are in campaign.json (hurdle and promotions).
#include <array>
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

// One band of a factor table: a count above the previous band's top and at most `top` reads
// `factor`.
struct MinedFactorBand {
  atx::u64 top{};
  atx::f64 factor{};
};
// Lane MINE-STAT: the discover hurdle's label-overlap factor by --budget band.
inline constexpr std::array<MinedFactorBand, 3> kMinedOverlapBands{
    {{100U, 1.47}, {1000U, 1.54}, {10000U, 1.63}}};
// Lane MINE-STAT: the confirm read's factor by band of m, the reads that reach the confirm.
inline constexpr std::array<MinedFactorBand, 3> kMinedConfirmBands{
    {{16U, 1.77}, {64U, 1.96}, {256U, 2.15}}};
// Ruling PM4-13, lifted by the table: the largest --budget the overlap table covers; the verb
// refuses a larger one before any payload, and backtest_integrity.campaign_line refuses its line.
inline constexpr atx::u64 kMinedMaxBudget = 10000;
static_assert(kMinedMaxBudget == kMinedOverlapBands.back().top,
              "the budget ceiling is the overlap table's last top");

// The overlap factor of --budget `budget`'s band; NaN when the budget is 0 or above
// kMinedMaxBudget (no hurdle reads a factor the table does not cover).
[[nodiscard]] atx::f64 mined_overlap_factor(atx::u64 budget) noexcept;
// The confirm factor of `reads` confirm reads; NaN when 0 or above the last band's top.
[[nodiscard]] atx::f64 mined_confirm_factor(atx::usize reads) noexcept;
// t / factor (NaN stays NaN): the scale every hurdle of mined-v1 is read on.
[[nodiscard]] atx::f64 mined_overlap_corrected(atx::f64 t, atx::f64 factor) noexcept;

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

// Indices of `reads` with a finite f2 whose overlap-corrected value f2 / `factor` is at or above
// `hurdle`, by f2 descending then canonical hash ascending, at most `cap`.
[[nodiscard]] std::vector<atx::usize> mined_shortlist(std::span<const MinedRead> reads,
                                                      atx::f64 hurdle, atx::f64 factor,
                                                      atx::usize cap);

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
  atx::f64 factor = std::numeric_limits<atx::f64>::quiet_NaN();      // Fc of the m reads
  atx::f64 t_corrected = std::numeric_limits<atx::f64>::quiet_NaN(); // t / Fc
  atx::f64 p = std::numeric_limits<atx::f64>::quiet_NaN();
  atx::f64 p_by = std::numeric_limits<atx::f64>::quiet_NaN();
  bool confirmed{};
};

// One confirm read per candidate (oriented HAC t; NaN when undefined, then p = 1), the m =
// oriented_t.size() reads taken as t / Fc, Fc = mined_confirm_factor(m): one-sided p =
// Phi(-t / Fc), Benjamini-Yekutieli adjusted across the m reads; confirmed iff t / Fc >= 2 and
// the adjusted p <= .10. Above the confirm table's last top Fc is NaN and nothing confirms.
[[nodiscard]] std::vector<MinedConfirm> mined_confirm(std::span<const atx::f64> oriented_t);

} // namespace atx::impl::strategy

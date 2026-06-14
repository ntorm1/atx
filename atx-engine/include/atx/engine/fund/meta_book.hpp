#pragma once

// atx::engine::fund — MetaBook: the two-pass meta-book DRIVER (P2-S2-5). The CAPSTONE
// that ties the four S2 units (Sleeve, MetaAllocator, cross_sleeve_risk, netting) into
// one fund: it runs N sleeves independently (net-after-optimize), sets a TRAILING
// cross-sleeve risk budget, nets the per-sleeve books into ONE fund book per period, and
// reports the fund with Euler-EXACT attribution-by-sleeve.
//
// ===========================================================================
//  What this unit is — the orchestration, NOT new arithmetic
// ===========================================================================
//  MetaBook COMPOSES the S2 stack; it re-implements none of it:
//    * PASS 1 (§0.7, net-after-optimize): every Sleeve::run is an INDEPENDENT, causal S1
//      MultiHorizonOptimizer walk — each sleeve is blind to the others.
//    * PASS 2 (the fund overlay): for each period s ascending, build the TRAILING
//      sleeve-return covariance Ω from realized sleeve P&L STRICTLY BEFORE s (R2),
//      MetaAllocator → per-sleeve capital weights c[s], net_fund_book → the consolidated
//      fund book + crossing benefit. The netting at s is a same-timestamp aggregation of
//      the period-s sleeve target books (all known at s).
//  Then the report: Euler attribution-by-sleeve (return / risk / crossing, each SUMMING
//  to the fund total), the fund Sharpe via combine::compute_metrics, and the Meucci
//  effective-bets diversification gauge over the final Ω, c.
//
// ===========================================================================
//  S2-5 DESIGN RESOLUTION — the realized-P&L source (plan §4.5 left this a TODO)
// ===========================================================================
//  The trailing covariance Ω and the fund report both need realized sleeve / fund P&L,
//  which requires per-instrument RETURNS the plan's §4.5 pseudocode only gestured at
//  (`/*pnl via model_at(period) returns*/`). AS-BUILT RESOLUTION (implemented here):
//  run() takes an ADDITIONAL callback
//      returns_at(period) -> std::span<const f64>   (length M)
//  giving the realized per-instrument SIMPLE return earned OVER that period. From it:
//    * Realized sleeve P&L:  r_s[p] = Σ_i sleeve_results[s].books[p][i] · returns_at(p)[i]
//      — sleeve s's realized return holding its period-p book through period p (order-
//      fixed ascending i).
//    * Fund P&L (report):   r_fund[p] = Σ_i fund_book[p][i] · returns_at(p)[i]
//                                     = Σ_s c[p][s]·r_s[p]   (linear — the attribution key).
//  returns_at does NOT affect the realized BOOKS (those come from pass-1 Sleeve::run), so
//  the R7 boundary pin holds regardless of what returns_at supplies.
//
// ===========================================================================
//  CAUSALITY — the central S2 trap (R2, no-look-ahead)
// ===========================================================================
//  The capital weights c[s] at period s are allocated from the TRAILING window of sleeve
//  P&L { r_s[p] : max(0, s − risk_lookback) ≤ p < s } — STRICTLY periods p < s. At s == 0
//  the window is EMPTY ⇒ Ω is 0×0 ⇒ the MetaAllocator degenerate/empty fallback fires
//  (§0.8; equal / inverse-cap). c[s] therefore depends only on P&L realized BEFORE s:
//  unambiguously causal. Truncating the schedule after period t leaves every fund book at
//  p ≤ t byte-identical (the trailing budget read no future) — the R2 integration gate.
//
// ===========================================================================
//  THE BOUNDARY PIN (R7) — the load-bearing regression
// ===========================================================================
//  ONE sleeve, a MetaAllocatorConfig that yields c == [1.0] every period (single sleeve +
//  fractional_kelly = 1, target_vol = 0, max_gross ≥ 1, a large capacity_gross ⇒ c = [1];
//  the s == 0 empty-Ω fallback also gives 1) and one-sleeve netting (net == gross, W =
//  1·w_0 = w_0, no crossing) ⇒ the driver's fund_books[s] is BYTE-IDENTICAL to that
//  sleeve's MultiHorizonResult.books[s] (and to a standalone MultiHorizonOptimizer::run
//  over the same fixture). Asserted element-wise via std::bit_cast<u64> so signed zeros
//  match. This is the single most important test of the unit: the overlay adds NO
//  arithmetic to the realized book in the degenerate case.
//
// ===========================================================================
//  Determinism (R1) / allocation
// ===========================================================================
//  Every reduction is ascending-index order-fixed; no RNG, no clock, no std::unordered_*.
//  The Meucci PCA uses Eigen's SelfAdjointEigenSolver (deterministic). This is a COLD
//  driver (rebalance cadence, not a hot tick path) — the per-period Ω build, the trailing
//  P&L slices and the report vectors allocate freely; that is documented and accepted.

#include <functional> // std::function (the run() callbacks)
#include <span>       // std::span (returns_at / book spans)
#include <vector>     // std::vector (per-period schedules + report rows)

#include "atx/core/error.hpp" // Result, ErrorCode
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics, compute_metrics (the ONE Sharpe)
#include "atx/engine/fund/meta_allocator.hpp" // MetaAllocatorConfig, CapitalWeights
#include "atx/engine/fund/sleeve.hpp"         // Sleeve (the pass-1 unit)
#include "atx/engine/risk/factor_model.hpp"   // risk::FactorModel (the shared V, model_at return)
#include "atx/engine/risk/multi_horizon.hpp"  // risk::MultiHorizonResult, HorizonSources
#include "atx/engine/risk/multi_period.hpp"   // risk::RebalanceSchedule, book::CostInputs

namespace atx::engine::fund {

// ===========================================================================
//  MetaBookConfig — the driver knobs (pure configuration).
// ===========================================================================
struct MetaBookConfig {
  MetaAllocatorConfig alloc;     // the S2-2 allocator config (method / Kelly / caps / gross)
  atx::usize risk_lookback = 60; // TRAILING window length for Ω / sleeve vols (R2; periods p < s)
};

// ===========================================================================
//  SleeveAttribution — the by-sleeve decomposition (R4; each vector SUMS to the fund).
// ===========================================================================
struct SleeveAttribution {
  // Σ_p c[p][s]·r_s[p] ; Σ_s == R_fund = Σ_p r_fund[p] EXACTLY (linear / order-fixed, R4).
  std::vector<atx::f64> return_contrib;
  // Euler RC_s from a REPRESENTATIVE Ω,c (the full-sample Ω over all r_s + final c); the
  // S2-3 identity Σ_s RC_s == sqrt(cᵀΩc) holds exactly (R4). DOCUMENTED: representative
  // Ω = sleeve_return_cov over the WHOLE realized P&L panel; c = the final period's c.
  std::vector<atx::f64> risk_contrib;
  // Total crossing benefit (Σ_p crossing_benefit_bps) allocated pro-rata by the gross
  // volume sleeve s contributed to trading; Σ_s == the total crossing benefit (R4).
  std::vector<atx::f64> crossing_credit;
};

// ===========================================================================
//  FundReport — the per-period fund diagnostics + summary metrics.
// ===========================================================================
struct FundReport {
  // Cumulative equity = Π_p (1 + r_fund[p]) per period (compounded; r_fund[0] included —
  // it is a real period-0 return, NOT the combine structural zero). DOCUMENTED choice:
  // compounded product, not cumsum, so it reads as a growth curve.
  std::vector<atx::f64> equity_curve;
  std::vector<atx::f64> gross_leverage;       // Σ_i |fund_book[p][i]| per period
  std::vector<atx::f64> net_exposure;         // Σ_i fund_book[p][i] per period
  std::vector<atx::f64> turnover_net;          // per period (from NetResult, R3)
  std::vector<atx::f64> turnover_gross;        // per period (turnover_net ≤ turnover_gross, R3)
  std::vector<atx::f64> crossing_benefit_bps;  // per period (≥ 0, R3)
  // The ONE fund Sharpe: compute_metrics over r_fund + the flattened fund_book schedule.
  // DOCUMENTED: compute_metrics treats index 0 as a STRUCTURAL zero (excluded from the
  // moments) — that is the combine convention; the fund Sharpe is over r_fund[1..T).
  combine::AlphaMetrics fund_metrics{};
  // Meucci N_Ent effective bets over the FINAL Ω, c (A5 diversification gate); 0 when Ω
  // is empty/degenerate.
  atx::f64 effective_bets = 0.0;
  SleeveAttribution attribution{};
};

// ===========================================================================
//  MetaBookResult — the full driver output.
// ===========================================================================
struct MetaBookResult {
  // The netted fund book per period (the REALIZED schedule; the R7-pinned quantity).
  std::vector<std::vector<atx::f64>> fund_books;
  // c per period (TRAILING-allocated; c[s] from P&L strictly before s — causal, R2).
  std::vector<CapitalWeights> capital;
  // Pass-1 per-sleeve realized books (each an independent S1 MultiHorizonOptimizer walk).
  std::vector<risk::MultiHorizonResult> sleeve_results;
  FundReport report;
};

// ===========================================================================
//  MetaBook — the two-pass meta-book driver (§4.5).
// ===========================================================================
class MetaBook {
public:
  MetaBookConfig cfg;
  std::vector<Sleeve> sleeves;

  // Run the two-pass driver over the schedule. PASS 1 walks each sleeve independently
  // (net-after-optimize, causal); PASS 2 builds the trailing Ω (periods p < s, R2),
  // allocates capital, nets the period-s sleeve books into the fund book, and accrues the
  // report. See the header block for the full contract, the returns_at resolution, the R2
  // causality and the R7 boundary pin.
  //
  //  sched       : the ascending as-of period indices (PIT).
  //  sources_at  : (sleeve j, period p) → that sleeve's HorizonSources at p (PASS 1).
  //  model_at    : period p → the SHARED FactorModel (one V every sleeve sees).
  //  returns_at  : period p → the realized per-instrument simple return at p (length M;
  //                the S2-5 resolution — drives Ω and the report, NOT the books).
  //  cost        : the ONE calibrated cost model the sleeves + netting share.
  //
  //  Boundary validation ⇒ Err(InvalidArgument): sleeves non-empty; sources_at / model_at
  //  / returns_at callbacks present. An empty schedule ⇒ Ok with empty result (degenerate,
  //  NOT an error). A sleeve / allocator / netting sub-call error PROPAGATES via ATX_TRY.
  [[nodiscard]] atx::core::Result<MetaBookResult>
  run(const risk::RebalanceSchedule &sched,
      const std::function<risk::HorizonSources(atx::usize sleeve, atx::usize period)> &sources_at,
      const std::function<const risk::FactorModel &(atx::usize period)> &model_at,
      const std::function<std::span<const atx::f64>(atx::usize period)> &returns_at,
      const book::CostInputs &cost) const;
};

} // namespace atx::engine::fund

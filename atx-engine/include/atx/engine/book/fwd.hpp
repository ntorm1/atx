#pragma once

// atx::engine::book — operating-book model forward declarations (Sprint 7).
//
// The book layer turns the v2 alpha factory into a live, self-managing book:
//
//   schedule → multi-period optimize → monitor decay → recycle dead alphas
//   as risk factors → allocate (Kelly / fractional-Kelly) → report
//
// Namespace split:
//   atx::engine::risk — extensions to the existing risk spine introduced by
//       S7: `risk::MultiPeriodOptimizer` (turnover-aware multi-horizon QP)
//       and the dead-alpha → orthogonal risk factor extraction FREE FUNCTIONS
//       (`extract_dead_factors` / `augment_factor_model`, operating on the
//       `FactorComponents` / `DeadAlphaFactors` carriers).  Full definitions
//       live in risk/multi_period.hpp and risk/dead_factor.hpp respectively.
//
//   atx::engine::book — new layer introduced by S7: decay monitoring, cost
//       scalar adapter, Kelly allocation, and the top-level pipeline/report.
//       Full definitions will live in book/decay_monitor.hpp,
//       book/allocator.hpp, and book/pipeline.hpp.
//
// book::CostInputs rationale:
//   The merged S6 `cost::` API (cost_aware_knobs, capacity_point,
//   should_trade) exposes richer parameter sets that require calibrated
//   ImpactCfg handles.  `book::CostInputs` is a clean scalar-adapter seam
//   that the multi-period optimizer and allocator consume: it distils the
//   calibrated cost layer down to {kappa, rt_cost_bps, capacity_aum,
//   fitness_cost_floor} so optimizer code never takes a direct
//   cost::CalibratedCost dependency.  This is not a fallback; the merged
//   cost:: API is ALWAYS used upstream; CostInputs is the clean hand-off.
//
// dsr == psr note (D6):
//   eval::deflated_sharpe sets dsr == psr as-built.  Any decay gate that
//   compares `dsr < dsr_admit` is therefore a PSR (probabilistic Sharpe
//   ratio) comparison; callers should document it as such.
//
// A lightweight header that names the book and risk-extension types without
// pulling in their full definitions or any of the underlying QP, Eigen
// eigensolver, or decay-detection machinery.

namespace atx::engine::risk {

// =====================================================================
//  Multi-period optimizer extension (S7-1)
// =====================================================================

// Defines the rebalancing calendar: period length, number of look-ahead
// horizons, and turnover budget per period.
// Full definition in risk/multi_period.hpp (S7-1).
struct RebalanceSchedule;

// Parameters for the multi-period turnover-penalized QP: horizon count,
// risk-aversion schedule, and per-horizon turnover limits.
// Full definition in risk/multi_period.hpp (S7-1).
struct MultiPeriodConfig;

// Output of a multi-period solve: per-horizon optimal weight trajectories,
// aggregate expected return, and achieved turnover sequence.
// Full definition in risk/multi_period.hpp (S7-1).
struct MultiPeriodResult;

// Solves a sequence of single-period QPs linked by turnover penalties across
// multiple look-ahead horizons; ships on the as-built
// risk::PortfolioOptimizer projected/proximal loop (true OSQP-style ADMM is
// the recorded atx-core L7 lift).
// Full definition in risk/multi_period.hpp (S7-1).
class MultiPeriodOptimizer;

// =====================================================================
//  Dead-alpha risk factor extraction (S7-3)
// =====================================================================

// The estimated (X, F, D, fit_end) carrier FactorModelBuilder::build_components
// emits and the dead-factor augmentation hstacks/blockdiags into an augmented
// FactorModel.  Defined in risk/factor_model.hpp (returned by build_components).
struct FactorComponents;

// The dead-alpha risk factors extracted from the holdings-overlap eigenspectrum:
// sign-fixed eigenvector loadings, kept eigenvalues (the dead factor variances),
// and the eRank-truncated factor count k_dead.  S7-3 uses FREE FUNCTIONS
// (extract_dead_factors / augment_factor_model) — not a class — built on
// Eigen::SelfAdjointEigenSolver with a fixed largest-|component|-positive sign
// convention for bit-reproducibility.  Full definition in risk/dead_factor.hpp.
struct DeadAlphaFactors;

} // namespace atx::engine::risk

namespace atx::engine::book {

// =====================================================================
//  Cost scalar-adapter seam (S7-1)
// =====================================================================

// Distilled scalar inputs the optimizer and allocator consume, produced by
// upstream cost:: calls (cost_aware_knobs, capacity_point).  Isolates
// optimizer code from direct cost::CalibratedCost dependencies.
// Full definition in book/pipeline.hpp (S7-5).
struct CostInputs;

// =====================================================================
//  Admitted-alpha baseline snapshot (S7-2)
// =====================================================================

// Snapshot of an admitted alpha's performance baseline at the time it clears
// the decay gate: DSR, IC, and trailing return series used as the reference
// for Page-Hinkley change detection.
// Full definition in book/decay_monitor.hpp (S7-2).
struct AdmittedBaseline;

// =====================================================================
//  Alpha-decay monitoring (S7-2)
// =====================================================================

// Cumulative state for the Page-Hinkley sequential change-point test applied
// to a single alpha's rolling IC series.
// Full definition in book/decay_monitor.hpp (S7-2).
struct PageHinkleyState;

// Configuration for the decay gate: DSR admission threshold (dsr_admit; note
// dsr == psr as-built per D6), Page-Hinkley detection threshold, and the
// minimum observation window before a verdict is issued.
// Full definition in book/decay_monitor.hpp (S7-2).
struct DecayConfig;

// Outcome of a decay evaluation: HEALTHY, SUSPECT, or DECAYED, together with
// the current Page-Hinkley statistic and the number of observations seen.
// Full definition in book/decay_monitor.hpp (S7-2).
struct DecayVerdict;

// Stateless per-alpha decay evaluator: ingests a new IC observation, updates
// PageHinkleyState, and returns a DecayVerdict.
// Full definition in book/decay_monitor.hpp (S7-2).
class DecayMonitor;

// Orchestrates DecayMonitor across the admitted alpha universe; decides which
// alphas to demote to dead status and forwards them to the dead-factor extraction
// (extract_dead_factors / augment_factor_model).
// Full definition in book/decay_monitor.hpp (S7-2).
class DecayController;

// =====================================================================
//  Kelly / fractional-Kelly allocator (S7-4)
// =====================================================================

// Parameters governing capital allocation: Kelly fraction f*, leverage cap,
// and per-alpha concentration limit.
// Full definition in book/allocator.hpp (S7-4).
struct AllocationConfig;

// =====================================================================
//  Top-level pipeline and report (S7-5)
// =====================================================================

// Structured output of one BookPipeline rebalance cycle: optimal weights,
// allocation fractions, decay verdicts, effective breadth (eRank), and
// round-trip cost estimate.  eRank = (Σλ)²/Σλ² shipped engine-local.
// Full definition in book/pipeline.hpp (S7-5).
struct BookReport;

// Orchestrates the full operating-book loop:
//   RebalanceSchedule → MultiPeriodOptimizer → DecayController
//   → extract_dead_factors / augment_factor_model → Kelly allocator → BookReport.
// Full definition in book/pipeline.hpp (S7-5).
class BookPipeline;

} // namespace atx::engine::book

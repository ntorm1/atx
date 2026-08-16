#pragma once

// End-to-end adapter from one projection-backed BacktestDb series to the
// point-in-time research validator and immutable ResearchDb trial catalog.
//
// The adapter deliberately executes a close-derived signal on the NEXT stored
// observation and labels its outcome over the following interval:
//
//   signal row i -> execute at i+1 -> outcome PnL from i+1 to i+2
//
// It therefore never turns a diagnostic recorded at a modeled close into a
// same-close fill. Strategy construction/risk/intent generation remains in
// strategy_pipeline.hpp; this module owns research orchestration and lineage.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "storage/backtest_db.hpp"
#include "atx/vol/api/backtest/dispersion.hpp"
#include "storage/research_db.hpp"
#include "atx/vol/api/backtest/research_validation.hpp"
#include "atx/vol/api/backtest/vol_edge.hpp" // VolEdgeConfig (vrp-backtest subcommand)
#include "backtest/strategy_pipeline.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

inline constexpr std::uint64_t kQuantPipelineTrialSchemaSalt =
    0x41545851504C0001ULL; // "ATXQPL", trial payload revision 1

struct BacktestSignalResearchSpec {
  std::string signal_name;
  // Fixed, strictly positive capital known before every outcome. A future
  // extension may accept a point-in-time capital series; it must not infer one
  // from future NAV.
  double lagged_capital{1.0};
  ResearchWalkForwardSpec validation{};
  std::vector<ResearchSignalCandidate> candidates;
  std::size_t newey_west_lag{0};
  ResearchTrialFamily family{};
};

struct BacktestSignalResearchResult {
  std::vector<ResearchObservation> observations;
  ResearchValidationPlan validation;
  ResearchMiningResult mining;
};

// Convert an indexed BacktestDb series into strict next-observation research
// rows, build the purged/embargoed plan, and select only on stitched OOS
// evidence. The exact immutable BacktestDb partition identity is attached to
// every observation.
[[nodiscard]] Result<BacktestSignalResearchResult>
mine_backtest_signal_series(const BacktestSeriesInfo &info, const BacktestSeriesData &series,
                            const BacktestSignalResearchSpec &spec);

struct ResearchTrialPublishSpec {
  std::string logical_id;
  // Empty means the logical Trial head must not exist. An update supplies the
  // exact current artifact ID, preserving ResearchDb compare-and-swap.
  std::string expected_head_id;
};

// Publish the complete selected-trial summary, OOS return path and fold audit as
// one immutable ResearchDb Trial artifact that depends on the exact BacktestDb
// partition. This operation records evidence only; it cannot send an order.
[[nodiscard]] Result<ResearchArtifactInfo> publish_backtest_signal_trial(
    ResearchDb &db, const BacktestSeriesInfo &info, const BacktestSignalResearchSpec &research_spec,
    const BacktestSignalResearchResult &result, const ResearchTrialPublishSpec &publish_spec);

// ---------------------------------------------------------------------------
// Strategy target -> risk -> scenario -> implementation intent
// ---------------------------------------------------------------------------

// Recover stable symbol metadata for an existing DispersionBook. Authoritative
// quantities and contracts remain those produced by build_dispersion_book; this
// adapter does not reimplement dispersion sizing.
[[nodiscard]] Result<std::vector<NamedPosition>>
dispersion_named_positions(const DispersionBook &book);

struct StrategyImplementationSpec {
  std::uint64_t strategy_fingerprint{0};
  std::int64_t decision_ts_ns{0};
  ScalarRiskLimits risk_limits{};
  std::vector<ConditionalComponentScenario> scenarios;
  std::vector<HedgeTarget> hedge_targets;
  AlgoParameters algo{};
  IntentDisposition disposition{IntentDisposition::ResearchOnly};
};

struct StrategyImplementationPlan {
  std::vector<NamedPosition> unconstrained_target;
  std::vector<NamedPosition> risk_adjusted_target;
  RiskVector unconstrained_risk{};
  std::vector<ConditionalComponentScenarioResult> scenario_results;
  ScalarRiskOverlayResult risk_overlay{};
  BasketOrderIntent intent{};
};

// Compose the broker-neutral implementation stages for either a dispersion
// target or a long/short volatility target. risk_inputs and scenario_inputs
// must be one-for-one with target by stable position ID and must carry the same
// caller-supplied Greek snapshot from the canonical PortfolioPricer path. At
// least one scenario is mandatory: the implementation boundary fails closed
// rather than producing an intent without scenario evidence.
[[nodiscard]] Result<StrategyImplementationPlan> build_strategy_implementation_plan(
    std::span<const NamedPosition> target, std::span<const NamedPosition> current,
    std::span<const PositionRiskInput> risk_inputs,
    std::span<const ScenarioRiskInput> scenario_inputs, const StrategyImplementationSpec &spec);

// ---------------------------------------------------------------------------
// vrp-backtest subcommand (2026-08-15 vrp-ml sprint, lane vrp-book)
// ---------------------------------------------------------------------------
//
// Library-shaped seam behind `atx-vol-quant-research vrp-backtest`: a frozen
// vrp_signal_v1 TSV + one or more SurfaceDb roots + a VolEdgeConfig drive the
// EXISTING strategy-aware `run_backtest` with a `VolEdgeStrategy`, and the
// result is written as one report TSV (NAV, the engine's Greek P&L
// attribution columns, the collected-vs-repriced theo-edge ledger read of
// those columns, turnover and costs, plus the strategy's signal columns).

struct VrpBacktestSpec {
  std::string signal_path;                   // frozen vrp_signal_v1 TSV (must exist)
  std::vector<std::string> surface_db_roots; // >= 1 SurfaceDb roots, disjoint dates
  std::string report_path;                   // output TSV
  std::string date_lo{};                     // optional inclusive window (ISO dates)
  std::string date_hi{};
  VolEdgeConfig config{};
};

struct VrpBacktestSummary {
  std::size_t n_rows{0};
  double final_nav{0.0};
  double total_cost{0.0};
  std::string report_path{};
  // F3/F4 hardening attribution (also per-row in the report's cumulative
  // `vol_edge_held_steps` / `vol_edge_roll_closed` signal columns): rebalance
  // ticks held on a missing signal date, per-name fail-soft skips, and
  // fail-safe expiry-guard roll-closes. The gate's SP100 rerun reads these to
  // quantify how often the hardened paths actually fired.
  std::uint64_t n_held_steps{0};
  std::uint64_t n_skipped_names{0};
  std::uint64_t n_roll_closes{0};
};

// Parse + validate the subcommand's arguments. FAIL-CLOSED VALIDATION: a
// missing/unreadable --signal file, no --surface-db root, an empty --report,
// an unknown flag, a bad numeric, or an invalid VolEdgeConfig are all errors
// here — nothing is deferred to the run to fail later with less context.
[[nodiscard]] Result<VrpBacktestSpec> parse_vrp_backtest_args(std::span<const std::string> args);

// Execute the spec end to end: load the signal, enumerate every root's
// partitions into one ascending clock (duplicate dates across roots are an
// error), run the vol-edge book, write the report TSV. No engine contract is
// modified — this is composition over run_backtest/VolEdgeStrategy only.
[[nodiscard]] Result<VrpBacktestSummary> run_vrp_backtest(const VrpBacktestSpec &spec);

} // namespace atx::vol

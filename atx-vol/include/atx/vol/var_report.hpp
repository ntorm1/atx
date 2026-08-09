#pragma once

// Engine-level VaR scenario reporting and per-underlier attribution.
//
// HistoricalVarResult (var.hpp) carries the per-scenario replay output but no
// presentation logic: before this header, the only exporter was
// var_bench.cpp's bench-local TSV writer, and there was no aggregation of
// leg-level P&L by underlier anywhere (feature-gaps.md findings 6, 7). This
// header lifts both into the engine so any caller of run_historical_var can
// get the same per-scenario report var_bench produced, plus a new
// per-underlier rollup that the bench never had.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/var.hpp"

namespace atx::vol {

// Book-construction accounting the engine cannot know (how many source lots
// were excluded before the book ever reached PreparedVarPortfolio, and why).
// Supplied by the caller and echoed verbatim on every TSV row, exactly as
// var_bench's TerminalVarFixture counters were before this task.
struct VarExclusionSummary {
  std::size_t source_option_lots{0};
  std::size_t coverage_excluded_option_lots{0};
  std::size_t delta_boundary_excluded_option_lots{0};
  std::size_t replay_excluded_option_lots{0};
  std::size_t stock_hedges{0};
};

// Writes one independent historical observation per row using
// std::setprecision(17), sourced from result.frames / result.leg_frames. It
// intentionally does not emit cumulative scenario P&L: the rows are
// alternative VaR observations, not successive returns of a traded strategy.
//
// The plan's sketch of this function is `(out, result, exclusions)`.
// HistoricalVarResult alone cannot name the "max_abs_leg_*" leg -- VarLegFrame
// carries only a uid, and the underlier name / reference-leg definitional
// fields (target delta, target dollar delta, log-moneyness) live on
// VarReferenceLeg, which HistoricalVarResult does not retain. This signature
// therefore adds `reference_legs` (additive vs. the plan sketch; see
// task-4-report.md for the rationale) -- pass
// PreparedVarPortfolio::reference_legs() in the same position order used to
// build result.leg_frames (position i of every scenario's leg block
// corresponds to reference_legs[i]).
//
// When result.leg_frames is empty (VarRunConfig::retain_leg_frames was
// false), reference_legs is ignored and every max_abs_leg_* column is an
// empty string. When result.leg_frames is non-empty, reference_legs MUST have
// exactly result.n_legs entries, AND -- since a matching count alone does not
// prove matching identity -- every result.leg_frames[i].uid must equal
// reference_legs[i % result.n_legs].uid; either violation returns an error
// Status rather than silently naming a leg from a reordered or unrelated
// reference_legs span.
[[nodiscard]] Status write_var_scenario_tsv(std::ostream &out, const HistoricalVarResult &result,
                                            const VarExclusionSummary &exclusions,
                                            std::span<const VarReferenceLeg> reference_legs = {});

struct VarUnderlierAttribution {
  std::string underlier{};
  double total_pnl{0.0};
  double worst_scenario_pnl{0.0};
  std::int64_t worst_scenario_base_ts_ns{0};

  [[nodiscard]] bool operator==(const VarUnderlierAttribution &) const = default;
};

// Sums Ok-status leg P&L per underlier across every scenario and returns one
// row per underlier, sorted ascending by total_pnl (the worst underlier
// first). worst_scenario_pnl / worst_scenario_base_ts_ns identify that
// underlier's single most negative leg-level transition (ties keep the
// earliest scenario, scanned in result.frames order).
//
// Requires result.leg_frames to be populated (VarRunConfig::retain_leg_frames)
// -- a non-empty result with empty leg_frames returns an error Status (a
// result with zero scenarios returns an empty vector regardless). Like
// write_var_scenario_tsv, HistoricalVarResult alone cannot name a uid, so
// `reference_legs` (additive vs. the plan's `(result)`-only sketch; see
// task-4-report.md) must have exactly result.n_legs entries in the same
// position order used to build result.leg_frames, AND every
// result.leg_frames[i].uid must equal reference_legs[i % result.n_legs].uid
// -- a size match alone does not prove identity. Either violation returns an
// error Status rather than silently attributing P&L to the wrong underlier.
[[nodiscard]] Result<std::vector<VarUnderlierAttribution>>
attribute_by_underlier(const HistoricalVarResult &result,
                       std::span<const VarReferenceLeg> reference_legs);

} // namespace atx::vol

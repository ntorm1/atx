// YTD historical VaR throughput for the terminal book produced by the real
// SP100 dispersion-strangle strategy. The fixture is opt-in through
// ATX_SP100_SURFACE_DB and is built once, outside benchmark timing:
//
//   SurfaceDb -> overlapping dispersion backtest -> terminal checkpoint
//             -> delta/TTE VarPosition book -> YTD historical VaR replay.
//
// The overlapping lifecycle enters a new 3M, 25-delta dispersion strangle each
// session and holds every cohort to expiry. The terminal backtest starts one
// tenor before the reference date: earlier cohorts are expired by construction,
// and the daily delta-to-zero hedge is recomputed from the current open book.
// YTD scenarios retain only original adjacent-session pairs; missing SPY dates
// are never bridged into multi-session returns.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/api/backtest/backtest.hpp"
#include "atx/vol/api/marketdata/corpus.hpp"
#include "pricing/pricing_executor.hpp"
#include "atx/vol/api/backtest/dispersion.hpp"
#include "storage/dispersion_surface_db.hpp"
#include "atx/vol/research/dispersion_backtest.hpp"
#include "atx/vol/api/backtest/strategy.hpp"
#include "atx/vol/api/storage/surface_db.hpp"
#include "atx/vol/api/marketdata/universe.hpp"
#include "analytics/var.hpp"
#include "analytics/var_report.hpp"

#include "bench_util.hpp"

namespace {

using atx::vol::ArchiveBacking;
using atx::vol::BacktestCheckpoint;
using atx::vol::Clock;
using atx::vol::CorpusEntry;
using atx::vol::CorpusFitStatus;
using atx::vol::CorpusManifest;
using atx::vol::DispersionBacktestConfig;
using atx::vol::DispersionStrategy;
using atx::vol::DispersionUniverse;
using atx::vol::HedgeSharePosition;
using atx::vol::historical_var_statistics;
using atx::vol::HistoricalVarResult;
using atx::vol::Lot;
using atx::vol::MarketSnapshot;
using atx::vol::OptionDeltaSolvePolicy;
using atx::vol::PreparedVarPortfolio;
using atx::vol::pricing_executor;
using atx::vol::ProjectedMaturitySpec;
using atx::vol::QueryExecution;
using atx::vol::QueryPricingTier;
using atx::vol::StrikeRule;
using atx::vol::SurfaceDb;
using atx::vol::UnpricedLotPolicy;
using atx::vol::VarBaseMarkSource;
using atx::vol::VarEvaluationConfig;
using atx::vol::VarExclusionSummary;
using atx::vol::VarLegFrame;
using atx::vol::VarOptionPosition;
using atx::vol::VarPosition;
using atx::vol::VarReferenceLeg;
using atx::vol::VarScenario;
using atx::vol::VarScenarioFrame;
using atx::vol::VarScenarioStatus;
using atx::vol::VarStockPosition;
using atx::vol::write_var_scenario_tsv;
using atx::vol::bench::apply_common;
using atx::vol::bench::dump_counters;

constexpr std::string_view kHistoryBegin = "2026-01-02";
// May 4 + 91 calendar days is after the July 31 reference timestamp. Starting
// here retains every cohort that can still be open at the terminal step while
// avoiding expired cohorts whose exact settlement partition lacks SPY.
constexpr std::string_view kTerminalBuildBegin = "2026-05-04";
constexpr std::string_view kDateEnd = "2026-07-31";
constexpr double kTenorDays = 91.0;
constexpr double kTargetAbsDelta = 0.25;
constexpr double kDeltaBoundary = 1.0e-5;
constexpr double kReplayDeltaTolerance = 1.0e-7;
constexpr double kMaxAggregateRelativeValueError = 1.0e-9;
constexpr double kMaxAggregateRelativeDeltaError = 1.0e-9;
constexpr double kGrossIndexVega = 10'000.0;
constexpr std::size_t kMinNames = 60u;
// Confidence used for the terminal-output distribution summary. This is not a
// VaR backtest: the threshold and observations come from the same sample.
constexpr double kValidationConfidence = 0.99;

[[nodiscard]] std::string environment_value(const char *name) {
#if defined(_MSC_VER)
  char *raw = nullptr;
  std::size_t size = 0u;
  if (::_dupenv_s(&raw, &size, name) != 0 || raw == nullptr) {
    return {};
  }
  std::string value{raw};
  std::free(raw);
  return value;
#else
  const char *raw = std::getenv(name);
  return raw == nullptr ? std::string{} : std::string{raw};
#endif
}

struct TerminalVarFixture {
  std::string error{};
  BacktestCheckpoint terminal{};
  std::vector<VarPosition> positions{};
  std::vector<MarketSnapshot> snapshots{};
  std::vector<VarScenario> scenarios{};
  std::vector<std::string> base_dates{};
  std::vector<std::string> shifted_dates{};
  std::optional<PreparedVarPortfolio> prepared{};
  std::size_t terminal_option_lots{0u};
  std::size_t covered_option_lots{0u};
  std::size_t excluded_option_lots{0u};
  std::size_t hedge_positions{0u};
  std::size_t underliers{0u};
  std::size_t history_dates{0u};
  std::size_t missing_index_dates{0u};
  std::size_t skipped_gap_transitions{0u};
  std::size_t terminal_steps{0u};
  std::size_t excluded_delta_boundary_lots{0u};
  std::size_t excluded_replay_option_lots{0u};
  double max_abs_pnl_error{0.0};
  double max_relative_value_error{0.0};
  double max_relative_delta_error{0.0};
};

struct IndexCompleteTimeline {
  std::string error{};
  std::optional<Clock> clock{};
  std::vector<std::size_t> source_ordinals{};
};

void set_error(TerminalVarFixture &fixture, std::string message) {
  if (fixture.error.empty()) {
    fixture.error = std::move(message);
  }
}

void add_symbol(std::unordered_map<std::uint32_t, std::string> &symbols, std::string_view symbol) {
  const std::string canonical = atx::vol::canonical_symbol(symbol);
  symbols.emplace(atx::vol::uid_for_symbol(canonical), canonical);
}

[[nodiscard]] bool write_pnl_trace(const TerminalVarFixture &fixture,
                                   std::span<const VarScenarioFrame> frames,
                                   std::span<const VarLegFrame> leg_frames,
                                   std::span<const VarReferenceLeg> reference_legs,
                                   std::string_view output_path, std::string &error) {
  if (frames.size() != fixture.base_dates.size() || frames.size() != fixture.shifted_dates.size()) {
    error = "YTD SP100 P&L trace date/frame cardinality mismatch";
    return false;
  }
  if (reference_legs.size() != fixture.positions.size() ||
      frames.size() > std::numeric_limits<std::size_t>::max() / reference_legs.size() ||
      leg_frames.size() != frames.size() * reference_legs.size()) {
    error = "YTD SP100 P&L trace leg cardinality mismatch";
    return false;
  }
  const std::filesystem::path path{std::string{output_path}};
  if (path.has_parent_path()) {
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
      error = "cannot create YTD SP100 P&L trace directory";
      return false;
    }
  }
  std::ofstream output{path, std::ios::out | std::ios::trunc};
  if (!output) {
    error = "cannot open YTD SP100 P&L trace output";
    return false;
  }

  // The scenario-level economics (frames) come from the fast/aggregate replay
  // route; the per-leg breakdown (leg_frames) comes from the separately
  // replayed retained-leg oracle -- this mixed provenance is deliberate and
  // predates the var_report.hpp extraction (see the two replay_into calls in
  // prepare_replayable_portfolio). HistoricalVarResult is just a plain
  // struct, so assembling one from these two already-validated sources for
  // the sole purpose of formatting is safe: it does not claim they came from
  // a single run_historical_var call.
  HistoricalVarResult result;
  result.base_dates.assign(fixture.base_dates.begin(), fixture.base_dates.end());
  result.shifted_dates.assign(fixture.shifted_dates.begin(), fixture.shifted_dates.end());
  result.frames.assign(frames.begin(), frames.end());
  result.leg_frames.assign(leg_frames.begin(), leg_frames.end());
  result.n_legs = reference_legs.size();

  VarExclusionSummary exclusions;
  exclusions.source_option_lots = fixture.terminal_option_lots;
  exclusions.coverage_excluded_option_lots = fixture.excluded_option_lots;
  exclusions.delta_boundary_excluded_option_lots = fixture.excluded_delta_boundary_lots;
  exclusions.replay_excluded_option_lots = fixture.excluded_replay_option_lots;
  exclusions.stock_hedges = fixture.hedge_positions;

  const atx::vol::Status status =
      write_var_scenario_tsv(output, result, exclusions, reference_legs);
  if (!status) {
    error = status.error().to_string();
    return false;
  }
  if (!output) {
    error = "cannot write YTD SP100 P&L trace output";
    return false;
  }
  return true;
}

[[nodiscard]] bool write_failure_trace(const TerminalVarFixture &fixture,
                                       std::span<const VarLegFrame> leg_frames,
                                       std::span<const VarReferenceLeg> reference_legs,
                                       std::string_view output_path, std::string &error) {
  if (reference_legs.size() != fixture.positions.size() ||
      fixture.scenarios.size() > std::numeric_limits<std::size_t>::max() / reference_legs.size() ||
      leg_frames.size() != fixture.scenarios.size() * reference_legs.size()) {
    error = "YTD SP100 failure trace leg cardinality mismatch";
    return false;
  }
  const std::filesystem::path path{std::string{output_path}};
  if (path.has_parent_path()) {
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
      error = "cannot create YTD SP100 failure trace directory";
      return false;
    }
  }
  std::ofstream output{path, std::ios::out | std::ios::trunc};
  if (!output) {
    error = "cannot open YTD SP100 failure trace output";
    return false;
  }
  output << "base_date\tshifted_date\tposition_index\tstatus\tunderlier\treference_units\t"
            "reference_delta\ttarget_abs_delta\ttarget_dollar_delta\tlog_moneyness\n";
  output << std::setprecision(17);
  for (std::size_t scenario = 0u; scenario < fixture.scenarios.size(); ++scenario) {
    const std::size_t offset = scenario * reference_legs.size();
    for (std::size_t position = 0u; position < reference_legs.size(); ++position) {
      const VarLegFrame &frame = leg_frames[offset + position];
      if (frame.status == atx::vol::VarLegStatus::Ok) {
        continue;
      }
      const VarReferenceLeg &reference = reference_legs[position];
      output << fixture.base_dates[scenario] << '\t' << fixture.shifted_dates[scenario] << '\t'
             << position << '\t' << atx::vol::to_string(frame.status) << '\t' << reference.underlier
             << '\t' << reference.reference_units << '\t' << reference.reference_delta << '\t'
             << reference.target_abs_delta << '\t' << reference.target_dollar_delta << '\t'
             << reference.log_moneyness << '\n';
    }
  }
  if (!output) {
    error = "cannot write YTD SP100 failure trace output";
    return false;
  }
  return true;
}

// In-sample distribution summary only. Kupiec/Christoffersen are deliberately
// not run here: comparing observations with a quantile estimated from those
// same observations mechanically controls the breach count and is not an
// out-of-sample VaR backtest.
void print_distribution_summary(std::span<const VarScenarioFrame> frames) {
  const auto risk = historical_var_statistics(frames, kValidationConfidence);
  if (!risk) {
    std::cerr << "distribution: skipped (" << risk.error().to_string() << ")\n";
    return;
  }
  std::cerr << "distribution: in_sample=true confidence=" << kValidationConfidence
            << " var=" << risk->value_at_risk << " es=" << risk->expected_shortfall
            << " n_scenarios=" << risk->n_scenarios << " note=not_an_out_of_sample_var_backtest\n";
}

[[nodiscard]] IndexCompleteTimeline index_complete_timeline(const Clock &history,
                                                            std::uint32_t index_uid) {
  IndexCompleteTimeline result;
  CorpusManifest manifest;
  const std::span<const atx::vol::SnapshotRef> refs = history.refs();
  manifest.dates.reserve(refs.size());
  manifest.entries.reserve(refs.size());
  result.source_ordinals.reserve(refs.size());
  for (std::size_t ordinal = 0u; ordinal < refs.size(); ++ordinal) {
    const atx::vol::SnapshotRef &ref = refs[ordinal];
    const std::span<const std::uint32_t> requested{&index_uid, 1u};
    auto snapshot = MarketSnapshot::load(ref.archive_path, QueryPricingTier::LegacyCompatible,
                                         requested, ArchiveBacking::Sealed);
    if (!snapshot) {
      result.error = snapshot.error().to_string();
      return result;
    }
    if (snapshot->find(index_uid) == nullptr) {
      continue;
    }
    manifest.dates.push_back(ref.date);
    CorpusEntry entry;
    entry.date = ref.date;
    entry.symbol = "SPY";
    entry.status = CorpusFitStatus::Ok;
    entry.archive_path = ref.archive_path;
    manifest.entries.push_back(std::move(entry));
    result.source_ordinals.push_back(ordinal);
  }
  manifest.n_boards = static_cast<std::uint32_t>(manifest.entries.size());
  manifest.n_ok = manifest.n_boards;
  auto clock = Clock::from_manifest(manifest);
  if (!clock) {
    result.error = clock.error().to_string();
    return result;
  }
  result.clock.emplace(std::move(*clock));
  return result;
}

[[nodiscard]] std::unordered_map<std::uint32_t, std::string>
symbol_map(const DispersionUniverse &universe) {
  std::unordered_map<std::uint32_t, std::string> symbols;
  symbols.reserve(universe.names.size() + 1u);
  add_symbol(symbols, universe.index.symbol);
  for (const atx::vol::DispersionMember &member : universe.names) {
    add_symbol(symbols, member.symbol);
  }
  return symbols;
}

[[nodiscard]] std::vector<std::uint32_t> terminal_uids(const BacktestCheckpoint &checkpoint) {
  std::vector<std::uint32_t> uids;
  uids.reserve(checkpoint.portfolio.lots.size() + checkpoint.hedge_shares.size());
  for (const Lot &lot : checkpoint.portfolio.lots) {
    uids.push_back(lot.contract.uid);
  }
  for (const HedgeSharePosition &hedge : checkpoint.hedge_shares) {
    uids.push_back(hedge.uid);
  }
  std::sort(uids.begin(), uids.end());
  uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
  return uids;
}

[[nodiscard]] std::unordered_set<std::uint32_t>
fully_covered_uids(std::span<const std::uint32_t> uids, std::span<const MarketSnapshot> snapshots) {
  std::unordered_set<std::uint32_t> covered;
  covered.reserve(uids.size());
  for (const std::uint32_t uid : uids) {
    const bool present =
        std::all_of(snapshots.begin(), snapshots.end(), [uid](const MarketSnapshot &snapshot) {
          return snapshot.find(uid) != nullptr;
        });
    if (present) {
      covered.insert(uid);
    }
  }
  return covered;
}

[[nodiscard]] bool
append_option_position(const Lot &lot, const MarketSnapshot &reference,
                       const std::unordered_map<std::uint32_t, std::string> &symbols,
                       std::vector<VarPosition> &positions, bool &appended, std::string &error) {
  appended = false;
  const auto symbol = symbols.find(lot.contract.uid);
  const atx::vol::SurfaceRef surface = reference.find(lot.contract.uid);
  const double time_to_expiry =
      static_cast<double>(lot.expiry_ts_ns - reference.ts_ns()) / atx::vol::kNsPerYear;
  if (symbol == symbols.end() || surface == nullptr ||
      !(std::isfinite(time_to_expiry) && time_to_expiry > 0.0)) {
    error = "terminal SP100 option cannot be resolved on the reference snapshot";
    return false;
  }
  // The terminal position's delta is its VaR moneyness coordinate, so resolve
  // it through the same cold-reference route used for historical restriking.
  const auto delta = surface.delta(lot.contract.K, time_to_expiry, lot.contract.side,
                                   QueryExecution::ColdReference);
  if (!delta || !std::isfinite(*delta)) {
    error = delta ? "terminal SP100 option has a non-finite reference delta"
                  : delta.error().to_string();
    return false;
  }
  const double raw_abs_delta = std::fabs(*delta);
  if (raw_abs_delta <= kDeltaBoundary || raw_abs_delta >= 1.0 - kDeltaBoundary) {
    return true;
  }
  VarOptionPosition option;
  option.underlier = symbol->second;
  option.time_to_expiry = ProjectedMaturitySpec::years(time_to_expiry);
  option.target_abs_delta = raw_abs_delta;
  option.side = lot.contract.side;
  option.quantity = lot.qty;
  option.multiplier = lot.multiplier;
  positions.emplace_back(std::move(option));
  appended = true;
  return true;
}

// ATX_VAR_BENCH_BASE_MARK_SOURCE=harvested selects
// VarBaseMarkSource::HarvestedFromSolver for this fixture's validation replays
// AND for the timed cases; anything else (including unset) keeps the dedicated
// base Price pass. An env knob rather than a registered case per source: the two
// are compared by running the SAME filter twice and differencing the counters,
// so doubling the registered case count would only lengthen every unrelated
// bench run. Wiring it into the fixture as well as the timed loop is the point:
// prepare_replayable_portfolio's aggregate-vs-cold-retained-oracle gate is the
// SP100-scale parity check the plan requires before any default flip, and it
// would be worthless if it always validated the source the timed cases did not
// run.
[[nodiscard]] VarBaseMarkSource base_mark_source_from_environment() {
  return environment_value("ATX_VAR_BENCH_BASE_MARK_SOURCE") == "harvested"
             ? VarBaseMarkSource::HarvestedFromSolver
             : VarBaseMarkSource::DedicatedPricePass;
}

[[nodiscard]] bool prepare_replayable_portfolio(TerminalVarFixture &fixture,
                                                const MarketSnapshot &reference) {
  VarEvaluationConfig evaluation;
  evaluation.n_threads = 0u;
  evaluation.delta_tolerance = kReplayDeltaTolerance;
  evaluation.projection_execution = QueryExecution::ColdReference;
  evaluation.valuation_execution = QueryExecution::ColdReference;
  evaluation.base_mark_source = base_mark_source_from_environment();
  auto candidate = PreparedVarPortfolio::create(fixture.positions, reference.set(), evaluation);
  if (!candidate) {
    set_error(fixture, candidate.error().to_string());
    return false;
  }

  std::vector<VarScenarioFrame> frames(fixture.scenarios.size());
  std::vector<atx::vol::VarLegFrame> legs(fixture.scenarios.size() * candidate->size());
  const auto replay = candidate->replay_into(fixture.scenarios, frames, legs, evaluation);
  if (!replay) {
    set_error(fixture, replay.error().to_string());
    return false;
  }
  const std::string failure_trace_path = environment_value("ATX_VAR_FAILURE_TSV");
  if (!failure_trace_path.empty() &&
      !write_failure_trace(fixture, legs, candidate->reference_legs(), failure_trace_path,
                           fixture.error)) {
    return false;
  }
  std::vector<std::uint8_t> failed(candidate->size(), std::uint8_t{0});
  for (std::size_t scenario = 0u; scenario < fixture.scenarios.size(); ++scenario) {
    const std::size_t offset = scenario * candidate->size();
    for (std::size_t position = 0u; position < candidate->size(); ++position) {
      if (legs[offset + position].status != atx::vol::VarLegStatus::Ok) {
        failed[position] = 1u;
      }
    }
  }
  if (std::find(failed.begin(), failed.end(), std::uint8_t{1}) != failed.end()) {
    std::vector<VarPosition> filtered;
    filtered.reserve(fixture.positions.size());
    for (std::size_t position = 0u; position < fixture.positions.size(); ++position) {
      if (failed[position] == 0u) {
        filtered.push_back(std::move(fixture.positions[position]));
      } else if (std::holds_alternative<VarOptionPosition>(fixture.positions[position])) {
        ++fixture.excluded_replay_option_lots;
      }
    }
    fixture.positions = std::move(filtered);
    fixture.covered_option_lots -= fixture.excluded_replay_option_lots;
    if (fixture.covered_option_lots < 1'000u) {
      set_error(fixture, "replayable YTD SP100 book has fewer than one thousand option lots");
      return false;
    }
    candidate = PreparedVarPortfolio::create(fixture.positions, reference.set(), evaluation);
    if (!candidate) {
      set_error(fixture, candidate.error().to_string());
      return false;
    }
    // The retained-leg oracle must be replayed on the REBUILT book, not
    // recombined from the superset book's leg frames: under the
    // CrossSectionalColdConfirm default, any row failing group resolution
    // downgrades the WHOLE scenario (every leg, every group) to the scalar
    // FastScreenColdConfirm route, so the per-leg strikes/fingerprints depend
    // on which rows share the book. Excluding the failed rows flips those
    // scenarios back onto the cross-sectional route; stale superset frames
    // would disagree with the same-book aggregate replay by admitted
    // strike-corridor slack (structurally, on definition_fingerprint).
    legs.assign(fixture.scenarios.size() * candidate->size(), VarLegFrame{});
    frames.assign(fixture.scenarios.size(), VarScenarioFrame{});
    const auto filtered_replay =
        candidate->replay_into(fixture.scenarios, frames, legs, evaluation);
    if (!filtered_replay) {
      set_error(fixture, filtered_replay.error().to_string());
      return false;
    }
  }
  if (!std::all_of(frames.begin(), frames.end(), [](const VarScenarioFrame &frame) {
        return frame.status == VarScenarioStatus::Ok;
      })) {
    set_error(fixture, "filtered YTD SP100 portfolio still has an incomplete scenario");
    return false;
  }

  // Production-scale economic invariant: every successful historical leg is
  // the same signed holding as the terminal reference portfolio. This catches
  // the former dollar-delta rebasing defect even when aggregate P&L identities
  // and fast-vs-retained parity remain perfectly green.
  const std::span<const VarReferenceLeg> reference_legs = candidate->reference_legs();
  for (std::size_t scenario = 0u; scenario < fixture.scenarios.size(); ++scenario) {
    const std::size_t offset = scenario * candidate->size();
    for (std::size_t position = 0u; position < candidate->size(); ++position) {
      if (legs[offset + position].status == atx::vol::VarLegStatus::Ok &&
          legs[offset + position].units != reference_legs[position].reference_units) {
        set_error(fixture, "historical replay resized a terminal portfolio holding");
        return false;
      }
    }
  }

  std::vector<VarScenarioFrame> aggregate_frames(fixture.scenarios.size());
  const auto aggregate_replay =
      candidate->replay_into(fixture.scenarios, aggregate_frames, {}, evaluation);
  if (!aggregate_replay) {
    set_error(fixture, aggregate_replay.error().to_string());
    return false;
  }
  for (std::size_t scenario = 0u; scenario < frames.size(); ++scenario) {
    const VarScenarioFrame &oracle = frames[scenario];
    const VarScenarioFrame &aggregate = aggregate_frames[scenario];
    if (aggregate.status != VarScenarioStatus::Ok || aggregate.n_ok != oracle.n_ok ||
        aggregate.n_failed != oracle.n_failed ||
        aggregate.definition_fingerprint != oracle.definition_fingerprint) {
      set_error(fixture, "aggregate YTD SP100 replay disagrees with the cold retained-leg oracle");
      return false;
    }
    const double value_scale =
        std::max({1.0, std::fabs(oracle.base_value), std::fabs(oracle.shifted_value)});
    fixture.max_abs_pnl_error =
        std::max(fixture.max_abs_pnl_error, std::fabs(aggregate.pnl - oracle.pnl));
    fixture.max_relative_value_error =
        std::max({fixture.max_relative_value_error,
                  std::fabs(aggregate.base_value - oracle.base_value) / value_scale,
                  std::fabs(aggregate.shifted_value - oracle.shifted_value) / value_scale,
                  std::fabs(aggregate.pnl - oracle.pnl) / value_scale});
    fixture.max_relative_delta_error = std::max(
        fixture.max_relative_delta_error, std::fabs(aggregate.dollar_delta - oracle.dollar_delta) /
                                              std::max(1.0, std::fabs(oracle.dollar_delta)));
  }
  if (fixture.max_relative_value_error > kMaxAggregateRelativeValueError ||
      fixture.max_relative_delta_error > kMaxAggregateRelativeDeltaError) {
    set_error(fixture, "aggregate YTD SP100 replay exceeds the cold economic parity tolerance");
    return false;
  }
  print_distribution_summary(aggregate_frames);
  const std::string pnl_trace_path = environment_value("ATX_VAR_PNL_TSV");
  if (!pnl_trace_path.empty() &&
      !write_pnl_trace(fixture, aggregate_frames, legs, candidate->reference_legs(), pnl_trace_path,
                       fixture.error)) {
    return false;
  }
  fixture.prepared.emplace(std::move(*candidate));
  return true;
}

[[nodiscard]] TerminalVarFixture build_terminal_fixture() {
  TerminalVarFixture fixture;
  const std::string root = environment_value("ATX_SP100_SURFACE_DB");
  if (root.empty()) {
    set_error(fixture, "ATX_SP100_SURFACE_DB is not set");
    return fixture;
  }
  auto db = SurfaceDb::open(root);
  if (!db) {
    set_error(fixture, db.error().to_string());
    return fixture;
  }
  auto full_clock = Clock::from_surface_db(*db);
  if (!full_clock) {
    set_error(fixture, full_clock.error().to_string());
    return fixture;
  }
  auto history = full_clock->between(kHistoryBegin, kDateEnd);
  if (!history || history->size() < 2u) {
    set_error(fixture, history ? "YTD SP100 benchmark needs at least two dates"
                               : history.error().to_string());
    return fixture;
  }
  auto universe = atx::vol::universe_from_surface_db(*db, "SPY");
  if (!universe) {
    set_error(fixture, universe.error().to_string());
    return fixture;
  }
  const std::unordered_map<std::uint32_t, std::string> symbols = symbol_map(*universe);
  IndexCompleteTimeline timeline =
      index_complete_timeline(*history, atx::vol::uid_for_symbol(universe->index.symbol));
  if (!timeline.clock) {
    set_error(fixture, std::move(timeline.error));
    return fixture;
  }
  fixture.history_dates = timeline.clock->size();
  fixture.missing_index_dates = history->size() - timeline.clock->size();
  auto terminal_window = timeline.clock->between(kTerminalBuildBegin, kDateEnd);
  if (!terminal_window || terminal_window->size() < 2u) {
    set_error(fixture, terminal_window ? "terminal SP100 build needs at least two dates"
                                       : terminal_window.error().to_string());
    return fixture;
  }
  fixture.terminal_steps = terminal_window->size();

  DispersionBacktestConfig dispersion =
      atx::vol::make_dispersion_ladder_config(kTenorDays, kGrossIndexVega, kMinNames);
  dispersion.strike.rule = StrikeRule::DeltaStrangle;
  dispersion.strike.target_abs_delta = kTargetAbsDelta;
  dispersion.delta_band = 0.0;
  dispersion.run.unpriced = UnpricedLotPolicy::ExcludeAndReport;
  dispersion.run.price.n_threads = 0u;
  dispersion.run.prefetch_depth = 2u;
  DispersionStrategy strategy = atx::vol::make_dispersion_backtest_strategy(*universe, dispersion);
  auto terminal = atx::vol::run_backtest_incremental(*terminal_window, strategy, dispersion.run);
  if (!terminal) {
    set_error(fixture, terminal.error().to_string());
    return fixture;
  }
  fixture.terminal = std::move(terminal->checkpoint);
  fixture.terminal_option_lots = fixture.terminal.portfolio.lots.size();
  if (fixture.terminal_option_lots < 1'000u) {
    set_error(fixture, "terminal SP100 dispersion book did not reach one thousand option lots");
    return fixture;
  }

  const std::vector<std::uint32_t> uids = terminal_uids(fixture.terminal);
  fixture.snapshots.reserve(timeline.clock->size());
  for (const atx::vol::SnapshotRef &ref : timeline.clock->refs()) {
    auto snapshot = MarketSnapshot::load(ref.archive_path, QueryPricingTier::RepresentativeFast,
                                         uids, ArchiveBacking::Sealed);
    if (!snapshot) {
      set_error(fixture, snapshot.error().to_string());
      return fixture;
    }
    fixture.snapshots.push_back(std::move(*snapshot));
  }
  const std::unordered_set<std::uint32_t> covered = fully_covered_uids(uids, fixture.snapshots);
  const MarketSnapshot &reference = fixture.snapshots.back();
  fixture.positions.reserve(fixture.terminal_option_lots + fixture.hedge_positions);
  for (const Lot &lot : fixture.terminal.portfolio.lots) {
    if (!covered.contains(lot.contract.uid)) {
      ++fixture.excluded_option_lots;
      continue;
    }
    bool appended = false;
    if (!append_option_position(lot, reference, symbols, fixture.positions, appended,
                                fixture.error)) {
      return fixture;
    }
    if (appended) {
      ++fixture.covered_option_lots;
    } else {
      ++fixture.excluded_delta_boundary_lots;
    }
  }
  for (const HedgeSharePosition &hedge : fixture.terminal.hedge_shares) {
    if (hedge.shares == 0.0 || !covered.contains(hedge.uid)) {
      continue;
    }
    const auto symbol = symbols.find(hedge.uid);
    if (symbol == symbols.end()) {
      set_error(fixture, "terminal SP100 hedge has no symbol mapping");
      return fixture;
    }
    fixture.positions.emplace_back(VarStockPosition{symbol->second, hedge.shares});
    ++fixture.hedge_positions;
  }
  if (fixture.covered_option_lots < 1'000u) {
    set_error(fixture,
              "historically complete terminal SP100 book has fewer than one thousand lots");
    return fixture;
  }

  fixture.scenarios.reserve(fixture.snapshots.size() - 1u);
  fixture.base_dates.reserve(fixture.snapshots.size() - 1u);
  fixture.shifted_dates.reserve(fixture.snapshots.size() - 1u);
  for (std::size_t index = 0u; index + 1u < fixture.snapshots.size(); ++index) {
    if (timeline.source_ordinals[index] + 1u != timeline.source_ordinals[index + 1u]) {
      ++fixture.skipped_gap_transitions;
      continue;
    }
    const MarketSnapshot &base = fixture.snapshots[index];
    const MarketSnapshot &shifted = fixture.snapshots[index + 1u];
    fixture.scenarios.push_back(
        VarScenario{base.ts_ns(), &base.set(), shifted.ts_ns(), &shifted.set()});
    fixture.base_dates.push_back(timeline.clock->refs()[index].date);
    fixture.shifted_dates.push_back(timeline.clock->refs()[index + 1u].date);
  }
  if (!prepare_replayable_portfolio(fixture, reference)) {
    return fixture;
  }

  std::unordered_set<std::uint32_t> position_uids;
  position_uids.reserve(fixture.positions.size());
  for (const VarPosition &position : fixture.positions) {
    const std::string &symbol =
        std::visit([](const auto &leg) -> const std::string & { return leg.underlier; }, position);
    position_uids.insert(atx::vol::uid_for_symbol(symbol));
  }
  fixture.underliers = position_uids.size();
  return fixture;
}

[[nodiscard]] const TerminalVarFixture &terminal_fixture() {
  static const TerminalVarFixture fixture = build_terminal_fixture();
  return fixture;
}

// [perf] F4: the pool's Auto size honours the ATX_VOL_FIT_WORKERS env cap (a
// knob shared with the fitter, `pricing_executor.hpp`'s file header), so a
// "tN" row can silently run fewer than N workers with no trace in the JSON --
// inflating any wall-clock-per-worker metric a reader derives from `threads`
// (the REQUESTED count) without knowing the pool shrank. Mirrors var.cpp's
// own resolved_worker_count exactly (same clamp: 0 requests the full pool,
// an explicit request clamps down to pool capacity and to n_scenarios) so
// this counter reports precisely what replay_into's run_dynamic dispatch
// actually resolved `threads` to for this fixture. Any metric a reader
// derives from this JSON that divides by worker count should divide by
// THIS, not by `threads`.
[[nodiscard]] unsigned resolved_worker_count(std::size_t n_scenarios, unsigned requested_threads) {
  if (n_scenarios == 0u) {
    return 0u;
  }
  const unsigned capacity = pricing_executor().size() + 1u;
  unsigned resolved = requested_threads == 0u ? capacity : std::min(requested_threads, capacity);
  if (resolved == 0u) {
    resolved = 1u;
  }
  if (static_cast<std::size_t>(resolved) > n_scenarios) {
    resolved = static_cast<unsigned>(n_scenarios);
  }
  return resolved;
}

void emit_throughput(benchmark::State &state, const TerminalVarFixture &fixture, unsigned threads) {
  const double iterations = static_cast<double>(state.iterations());
  const double scenarios = static_cast<double>(fixture.scenarios.size());
  const double positions = static_cast<double>(fixture.positions.size());
  state.counters["scenarios_per_s"] =
      benchmark::Counter(iterations * scenarios, benchmark::Counter::kIsRate);
  state.counters["leg_scenarios_per_s"] =
      benchmark::Counter(iterations * scenarios * positions, benchmark::Counter::kIsRate);
  state.counters["terminal_option_lots"] = static_cast<double>(fixture.terminal_option_lots);
  state.counters["var_option_lots"] = static_cast<double>(fixture.covered_option_lots);
  state.counters["excluded_option_lots"] = static_cast<double>(fixture.excluded_option_lots);
  state.counters["hedge_positions"] = static_cast<double>(fixture.hedge_positions);
  state.counters["underliers"] = static_cast<double>(fixture.underliers);
  state.counters["n_scenarios"] = scenarios;
  state.counters["history_dates"] = static_cast<double>(fixture.history_dates);
  state.counters["missing_index_dates"] = static_cast<double>(fixture.missing_index_dates);
  state.counters["skipped_gap_transitions"] = static_cast<double>(fixture.skipped_gap_transitions);
  state.counters["terminal_steps"] = static_cast<double>(fixture.terminal_steps);
  state.counters["excluded_delta_boundary_lots"] =
      static_cast<double>(fixture.excluded_delta_boundary_lots);
  state.counters["excluded_replay_option_lots"] =
      static_cast<double>(fixture.excluded_replay_option_lots);
  state.counters["max_abs_pnl_error"] = fixture.max_abs_pnl_error;
  state.counters["max_relative_value_error"] = fixture.max_relative_value_error;
  state.counters["max_relative_delta_error"] = fixture.max_relative_delta_error;
  state.counters["threads"] = static_cast<double>(threads);
  // Additive (F4): the actual pool size replay_into's dispatch resolved
  // `threads` to. Equal to `threads` on a healthy run; a citable-table
  // reader should treat resolved != threads rows as run under a shrunk pool
  // (ATX_VOL_FIT_WORKERS or a low-core host), not as this fixture's true tN
  // throughput.
  state.counters["resolved_workers"] =
      static_cast<double>(resolved_worker_count(fixture.scenarios.size(), threads));
}

void run_terminal_var(benchmark::State &state, unsigned threads,
                      OptionDeltaSolvePolicy solve_policy) {
  const TerminalVarFixture &fixture = terminal_fixture();
  if (!fixture.error.empty()) {
    state.SkipWithError(fixture.error.c_str());
    return;
  }
  std::vector<VarScenarioFrame> frames(fixture.scenarios.size());
  VarEvaluationConfig config;
  config.n_threads = threads;
  config.delta_tolerance = kReplayDeltaTolerance;
  config.projection_execution = QueryExecution::ColdReference;
  config.valuation_execution = QueryExecution::ColdReference;
  config.projection_solve_policy = solve_policy;
  config.base_mark_source = base_mark_source_from_environment();

  for (auto _ : state) {
    const auto status = fixture.prepared->replay_into(fixture.scenarios, frames, {}, config);
    if (!status) {
      state.SkipWithError(status.error().to_string().c_str());
      break;
    }
    benchmark::DoNotOptimize(frames.data());
    benchmark::ClobberMemory();
  }
  emit_throughput(state, fixture, threads);
  state.counters["harvested_base_marks"] =
      config.base_mark_source == VarBaseMarkSource::HarvestedFromSolver ? 1.0 : 0.0;
  // cnt_* columns for ONE untimed replay, so keep/revert decisions on this
  // fixture have a wall-free, host-load-immune basis. Compiled out entirely
  // (and emits no columns) unless the build set ATX_VOL_COUNTERS.
  dump_counters(state, [&] {
    const auto status = fixture.prepared->replay_into(fixture.scenarios, frames, {}, config);
    benchmark::DoNotOptimize(frames.data());
    (void)status;
  });
}

void register_all() {
  const bool single_shot = environment_value("ATX_VAR_BENCH_SINGLE_SHOT") == "1";
  for (const auto &[route, solve_policy] :
       {std::pair{"direct_cold", OptionDeltaSolvePolicy::Direct},
        std::pair{"screened_cold", OptionDeltaSolvePolicy::FastScreenColdConfirm},
        std::pair{"cross_cold", OptionDeltaSolvePolicy::CrossSectionalColdConfirm}}) {
    for (const unsigned threads : {1u, 4u, 8u, 16u}) {
      const std::string name = "var/prepared/sp100_dispersion_terminal/ytd/thousands/" +
                               std::string{route} + "/t" + std::to_string(threads);
      benchmark::internal::Benchmark *registered =
          benchmark::RegisterBenchmark(name, [threads, solve_policy](benchmark::State &state) {
            run_terminal_var(state, threads, solve_policy);
          });
      if (single_shot) {
        registered->Iterations(1)->Repetitions(1)->ReportAggregatesOnly(true);
      } else {
        registered = apply_common(registered);
      }
      registered->Unit(benchmark::kMillisecond)->UseRealTime();
    }
  }
}

const bool kRegistered = [] {
  register_all();
  return true;
}();

} // namespace

#pragma once

// run_diagnostics — PhaseTimer (a steady-clock phase accumulator) plus the
// `diagnostics` RunArchive section encoder. The timer is lifted VERBATIM from
// the instrumented dispersion subcommands (examples/spy_dispersion_backtest.cpp),
// so the binary result container measures phases exactly as the TSV path does.
// The encoder mirrors write_diagnostics (the TSV writer that owns this output
// today): one row per timed phase — (subcommand, phase, wall_ms, count) in
// kDiagnosticsCols registry order — followed by a `total` row carrying the
// caller's total_count.

#include "atx/vol/run_archive.hpp" // RaSectionData (encoder output)

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atx::vol {

// Local phase-timing helper for the instrumented subcommands. Accumulates named
// phases (wall time plus a unit count) in a fixed, pre-declared order.
// steady_clock only; always on, since the overhead is negligible next to the
// surface solves it measures. (Lifted verbatim from
// examples/spy_dispersion_backtest.cpp.)
class PhaseTimer {
public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;

  struct Phase {
    std::string name;
    Duration elapsed{Duration::zero()};
    std::uint64_t count{0u};
  };

  // Pre-declaring the phase order fixes the output row order regardless of when
  // each region is first timed. Some phases accumulate across a loop, and one
  // (archive_load) is timed inside a region also charged to another phase, so
  // first-seen order would not match the intended reading order otherwise.
  explicit PhaseTimer(std::initializer_list<std::string_view> order) {
    for (std::string_view name : order) {
      phases_.push_back(Phase{std::string(name), Duration::zero(), 0u});
    }
  }

  // Same contract, from a contiguous range — lets a caller publish its phase
  // order as a named constant (see kBuildSchedulePhases / kRunBacktestPhases
  // below) instead of an inline braced list.
  explicit PhaseTimer(std::span<const std::string_view> order) {
    phases_.reserve(order.size());
    for (std::string_view name : order) {
      phases_.push_back(Phase{std::string(name), Duration::zero(), 0u});
    }
  }

  static Clock::time_point now() { return Clock::now(); }

  // Charge (now - start) wall time and `count` units to `phase` (created if it
  // was not pre-declared). Repeated calls to the same phase accumulate.
  void add(std::string_view phase, Clock::time_point start, std::uint64_t count = 0u) {
    const Duration elapsed = Clock::now() - start;
    for (Phase &entry : phases_) {
      if (entry.name == phase) {
        entry.elapsed += elapsed;
        entry.count += count;
        return;
      }
    }
    phases_.push_back(Phase{std::string(phase), elapsed, count});
  }

  const std::vector<Phase> &phases() const { return phases_; }

private:
  std::vector<Phase> phases_;
};

// ── Published phase order for the instrumented dispersion subcommands ────────
// These live here, not at the two PhaseTimer construction sites in
// examples/spy_dispersion_backtest.cpp, because the `diagnostics` ROW SET is a
// consumed interface (the Python report layer reads phases by name) and a phase
// list buried in an example binary is untestable: the example is its own
// executable target and no test links it. Publishing the order as a library
// constant makes the row set assertable from atx-vol-tests, so a phase
// rename/drop is caught by the suite instead of by a reader of a report.
//
// `build_schedule`'s `selection` / `quote_join` rows are charged by the library
// builder (build_listed_dispersion_schedule) through the timer the subcommand
// passes in; every other row is charged by the subcommand itself.
// `definitions_parse` is split OUT of `setup_read` in both subcommands: the
// definitions read is the single largest item in either setup and was invisible
// while blended with the run-spec / universe / manifest / clock / OCC-ESS reads.
// The two are charged as disjoint segments and sum to the old `setup_read`.
inline constexpr std::array<std::string_view, 5> kBuildSchedulePhases{
    "setup_read", "definitions_parse", "selection", "quote_join", "write_outputs"};

// The old aggregate `reconciliation` phase fused three different costs — the
// per-date snapshot loads, the per-date OPRA quote joins, and the reconcile
// itself — into one number, so none of them could be attributed or optimised.
// It is REPLACED by `snapshot_load` / `quote_join` / `reconcile`, which
// partition the same span. There is deliberately no `reconciliation` row left:
// keeping it would double count against the encoder's total.
inline constexpr std::array<std::string_view, 7> kRunBacktestPhases{
    "setup_read",    "definitions_parse", "engine_run",   "snapshot_load",
    "quote_join",    "reconcile",         "write_outputs"};

// `diagnostics` SubTable encoder: one row per timed phase in the timer's
// pre-declared order — (subcommand, phase name, wall_ms, count) — followed by a
// trailing `total` row whose `count` is `total_count` and whose `wall_ms` is the
// sum of the phase wall times (the encoder signature receives no independent
// command-level total, so the phase sum is the total row's wall_ms). Columns are
// emitted in kDiagnosticsCols registry order; every synthesized array lives in
// the returned section's type-erased `storage`, so the section owns its own
// backing (the Task 5 encoder convention — no borrowed source to outlive).
[[nodiscard]] RaSectionData encode_diagnostics_section(const PhaseTimer &timer,
                                                       std::string_view subcommand,
                                                       std::uint64_t total_count);

} // namespace atx::vol

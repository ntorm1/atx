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

#include <chrono>
#include <cstdint>
#include <initializer_list>
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

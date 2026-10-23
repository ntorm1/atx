#pragma once

// Benchmark clock contract of atx-vol-quote-bench (lane B0, 2026-10-22; frozen
// definitions, units and receipt fields: docs/benchmark-clocks.md).
//
// Four pieces, each usable without the CLI:
//   * summarize()      - min / median / p95 / p99 / max of one clock over reps;
//   * PhaseClock       - EXCLUSIVE stage attribution of one cold-calibration span:
//                        consecutive laps partition the span (no gap, no overlap), and
//                        a fit lap is split into the stages its fit_timing buckets and
//                        the de-Am stage clock measured, the remainder charged to the
//                        lap's residual stage;
//   * FitWork          - the fit-route work counters summed over chains;
//   * receipts         - build info, affinity / topology, the in-process cold reset.
//
// Clock source: std::chrono::steady_clock (monotonic; QueryPerformanceCounter on
// Windows). Every duration is in seconds unless a field name says otherwise.
// Threading: a PhaseClock / FitWork is owned by the one driver thread that runs the
// fits; nothing here is shared.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace atx::vol {
struct DeAmStageClock;
namespace counters::fit_timing {
struct FitTimingCounters;
} // namespace counters::fit_timing
} // namespace atx::vol

namespace atx::vol::bench {

using Json = nlohmann::json;
using BenchClock = std::chrono::steady_clock;

[[nodiscard]] inline double seconds_between(BenchClock::time_point from,
                                            BenchClock::time_point to) noexcept {
  return std::chrono::duration<double>(to - from).count();
}

// Order statistics of one clock over its reps. Quantiles interpolate linearly between
// order statistics (the quote-bench quantile() rule): p in [0, 1] sits at position
// p * (n - 1) of the sorted values. n == 0 leaves every statistic NaN.
struct ClockSummary {
  std::size_t n{0};
  double min{0.0};
  double median{0.0};
  double p95{0.0};
  double p99{0.0};
  double max{0.0};
};

[[nodiscard]] ClockSummary summarize(std::span<const double> values);
[[nodiscard]] Json summary_json(const ClockSummary &summary);

// The exclusive stages of the cold-calibration clock, in the order of the plan's
// latency budget (deep review 5.6), plus the two fit residuals the budget folds into
// its rows. docs/benchmark-clocks.md maps every fit_timing bucket to one stage.
enum class Stage : unsigned {
  Normalization = 0,     // snapshot classification, grouping, chain markets, routing
  FamilyBuildValidation, // family build + validation (no family on the route: not_built)
  Inverses,              // identifiable bid / ask / mid roots (deam - endpoints - exceptions)
  Endpoints,             // band-edge prices (kIvMin / kIvMax) and their accurate re-prices
  CurveRepair,           // curve solve and channel projection (fit_timing solve + project)
  FinalGrid,             // certification grid, certificate / repairs, calendar validation
  ReferenceExceptions,   // inversions that left the primary route (fallbacks)
  Publication,           // row publication and per-run bookkeeping
  FitOther,              // fit_timing other: chain preparation, containment, carry
  SurfaceOther,          // surface-fit wall charged to no chain (term prior, scheduling)
  Count_
};

inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count_);
inline constexpr std::array<const char *, kStageCount> kStageNames = {
    "normalization", "family_build_validation", "inverses", "endpoints", "curve_repair",
    "final_grid_validation", "reference_exceptions", "publication", "fit_other",
    "surface_other"};

// One measured sub-span of a fit lap: `seconds` of the lap belong to `stage`.
struct StagePart {
  Stage stage{Stage::FitOther};
  double seconds{0.0};
};

// Exclusive lap clock over one span. Unarmed (the default) every call is a branch: no
// clock read, nothing recorded. Armed: start() opens the span; lap(s, t) charges
// [previous mark, t] to s; lap_split(t, parts, residual) charges the parts of that
// interval first, each clamped to what is left of it, and the remainder to `residual`.
// A clamp means the parts claimed more than the interval held (overlapping workers, or
// rounding); the excess is NOT charged and is reported by conflict_seconds(). So the
// stages always partition [start, last mark] exactly, up to floating-point rounding.
class PhaseClock {
public:
  void arm() noexcept { armed_ = true; }
  [[nodiscard]] bool armed() const noexcept { return armed_; }
  void start(BenchClock::time_point t) noexcept;
  void lap(Stage stage, BenchClock::time_point t) noexcept;
  void lap(Stage stage) noexcept;
  void lap_split(BenchClock::time_point t, std::span<const StagePart> parts,
                 Stage residual) noexcept;
  [[nodiscard]] double seconds(Stage stage) const noexcept;
  [[nodiscard]] double total() const noexcept;
  // Records over-attribution measured outside a lap (fit_parts' clamped excess).
  void add_conflict(double seconds) noexcept;
  [[nodiscard]] double conflict_seconds() const noexcept { return conflict_; }
  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] BenchClock::time_point last_mark() const noexcept { return mark_; }

private:
  [[nodiscard]] double take(BenchClock::time_point t) noexcept;
  std::array<double, kStageCount> seconds_{};
  double conflict_{0.0};
  BenchClock::time_point mark_{};
  bool armed_{false};
  bool started_{false};
};

// The fit-route work of a run, summed over every timed chain (fit_timing counters)
// and the de-Am stage clock. Event counts are exact on the fitting thread; AL work a
// fit hands to other threads is not seen (attribution runs use one fit worker).
struct FitWork {
  std::uint64_t chains{0};
  std::uint64_t wall_us{0};
  std::array<std::uint64_t, 7> bucket_us{}; // fit_timing kBucketNames order
  std::array<std::uint64_t, 7> al_by_bucket{};
  std::uint64_t al_price_calls{0};
  std::uint64_t al_greeks_calls{0};
  std::uint64_t iv_newton_iters{0};
  std::uint64_t certification_passes{0};
  std::uint64_t channel_solves{0};
  std::uint64_t jacobian_evals{0};
  std::uint64_t rung_attempts{0};
  std::uint64_t calendar_pairs{0};
  std::uint64_t projections{0};
  std::uint64_t repair_rung_visits{0};
  std::uint64_t deam_inversions{0};
  std::uint64_t deam_inversion_al{0};
  std::uint64_t deam_premium{0};
  std::uint64_t deam_premium_fallbacks{0};
  std::array<std::uint64_t, 8> deam_fallback_by_reason{};
  std::array<std::uint64_t, 6> deam_al_by_reason{};
  std::uint64_t deam_confirms{0};
  std::uint64_t deam_confirms_skipped{0};
  std::uint64_t deam_ambiguous_edges{0};
  std::uint64_t deam_seed_routed{0};
};

void add_fit_work(FitWork &work, const counters::fit_timing::FitTimingCounters &chain) noexcept;

// The stage parts of a fit lap: fit_timing buckets of the lap's chains (`delta`) with
// the de-Am stage clock's endpoint / exception time of the same lap carved out of the
// deam bucket (both in nanoseconds; clamped at zero, the excess returned in `excess`).
[[nodiscard]] std::array<StagePart, 6> fit_parts(const FitWork &delta,
                                                 std::uint64_t endpoint_ns,
                                                 std::uint64_t exception_ns,
                                                 double &excess) noexcept;

// Component-wise after - before (saturating) of two cumulative FitWork totals.
[[nodiscard]] FitWork fit_work_delta(const FitWork &after, const FitWork &before) noexcept;

[[nodiscard]] Json fit_work_json(const FitWork &work);
[[nodiscard]] Json stage_clock_json(const DeAmStageClock &clock);

// Exclusive stage table of one span: every stage with its seconds (null and status
// not_built for the family stage when `family_built` is false), their sum, the
// independently measured cold clock, and the unattributed residual (cold - sum).
[[nodiscard]] Json stages_json(const PhaseClock &clock, double cold_calibration_s,
                               bool family_built);

// Compiler, flags, ISA and math-mode facts of THIS binary (embedded at compile time:
// ATX_VOL_BENCH_CXX_FLAGS / ATX_VOL_BENCH_BUILD_TYPE from CMake, predefined macros).
// fast_math is true when any fast-math / finite-math predefined macro is present.
[[nodiscard]] Json build_info_json();

// In-process cold reset: drops every market-dependent memo a fit on THIS thread can
// read across fits (the de-Am IV thread memo, the process-wide carry memo). The
// per-fit rung memo is a member of each surface fit, so it never crosses fits. The
// receipt lists what was cleared, the entry counts before, and the state that is
// deliberately NOT reset (market-independent tables, allocation scratch) and why.
[[nodiscard]] Json cold_reset();

// Entries of the calling thread's de-Am IV memo (deam_iv_memo_size()).
[[nodiscard]] std::size_t deam_memo_entries() noexcept;

// Per-logical-CPU busy time sample (Windows NtQuerySystemInformation, best effort;
// empty when unavailable). busy = kernel + user - idle, in 100-ns units.
struct CpuSample {
  std::vector<std::uint64_t> busy;
  std::vector<std::uint64_t> total;
};
[[nodiscard]] CpuSample sample_cpus();

// Pins the process and the calling thread to logical CPU `cpu` (group 0). Throws
// std::runtime_error when the CPU is absent from the system mask or a call fails.
// The fit worker is the calling thread when ATX_VOL_FIT_WORKERS resolves to 1.
void pin_to_logical_cpu(unsigned cpu);

// Whether logical CPU `cpu` is on a performance core (the highest EfficiencyClass
// of a heterogeneous package); nullopt when the topology is unavailable.
[[nodiscard]] std::optional<bool> is_performance_cpu(unsigned cpu);

// The affinity / topology / power receipt. `requested` is the --affinity CPU (nullopt:
// unpinned run); `before` / `after` bracket the measured window for the busy shares.
[[nodiscard]] Json affinity_json(std::optional<unsigned> requested, bool require_pcore,
                                 const CpuSample &before, const CpuSample &after);

// Process launch -> now (GetProcessTimes creation time vs the precise system time;
// NaN when unavailable). Best effort: creation is stamped by the kernel at launch.
[[nodiscard]] double seconds_since_process_creation() noexcept;

} // namespace atx::vol::bench

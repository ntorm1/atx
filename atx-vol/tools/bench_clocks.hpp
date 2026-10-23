#pragma once

// Benchmark clock contract of atx-vol-quote-bench (lane B0, 2026-10-22; frozen
// definitions, units and receipt fields: docs/benchmark-clocks.md).
//
// Four pieces, each usable without the CLI:
//   * summarize()      - min / median / p95 / p99 / max of one clock over reps;
//   * PhaseClock       - EXCLUSIVE stage attribution of one cold-calibration span:
//                        harness laps charge the span between two marks to one stage,
//                        and a fit lap charges only what the fit's fit_timing buckets
//                        and the de-Am stage clock measured; the rest of a fit lap is
//                        charged to NO stage, so cold - sum(stages) (unattributed_s)
//                        is the time no timer measured, not zero by construction;
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
  FitOther,              // fit_timing other bucket only: chain preparation, containment, carry
  Count_
};

inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count_);
inline constexpr std::array<const char *, kStageCount> kStageNames = {
    "normalization", "family_build_validation", "inverses", "endpoints", "curve_repair",
    "final_grid_validation", "reference_exceptions", "publication", "fit_other"};

// One measured sub-span of a fit lap: `seconds` of the lap belong to `stage`.
struct StagePart {
  Stage stage{Stage::FitOther};
  double seconds{0.0};
};

// Exclusive lap clock over one span. Unarmed (the default) every call is a branch: no
// clock read, nothing recorded. Armed: start() opens the span; lap(s, t) charges
// [previous mark, t] to s (a harness stage whose boundaries are the marks);
// lap_split(t, parts) charges the MEASURED parts of [previous mark, t], each clamped to
// what is left of it, and nothing else: the rest of the interval is time no timer
// claimed and goes to unclaimed_seconds(), never to a stage; skip(t) moves the mark and
// adds the interval to unclaimed_seconds() (an untimed region). A clamp means the parts
// claimed more than the interval held (overlapping workers, or rounding); the excess is
// NOT charged and is reported by conflict_seconds(). The stage sum is therefore the
// measured time only, and cold - sum (stages_json unattributed_s) is what no stage
// measured: it is zero only when every timer covered its interval.
class PhaseClock {
public:
  void arm() noexcept { armed_ = true; }
  [[nodiscard]] bool armed() const noexcept { return armed_; }
  void start(BenchClock::time_point t) noexcept;
  void lap(Stage stage, BenchClock::time_point t) noexcept;
  void lap(Stage stage) noexcept;
  void lap_split(BenchClock::time_point t, std::span<const StagePart> parts) noexcept;
  void skip(BenchClock::time_point t) noexcept;
  [[nodiscard]] double seconds(Stage stage) const noexcept;
  [[nodiscard]] double total() const noexcept;
  // Lap time no stage claimed (split-lap remainders and skipped regions).
  [[nodiscard]] double unclaimed_seconds() const noexcept { return unclaimed_; }
  // Records over-attribution measured outside a lap (fit_parts' clamped excess).
  void add_conflict(double seconds) noexcept;
  [[nodiscard]] double conflict_seconds() const noexcept { return conflict_; }
  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] BenchClock::time_point last_mark() const noexcept { return mark_; }

private:
  [[nodiscard]] double take(BenchClock::time_point t) noexcept;
  std::array<double, kStageCount> seconds_{};
  double conflict_{0.0};
  double unclaimed_{0.0};
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
  std::array<std::uint64_t, 7> deam_al_by_reason{};
  std::uint64_t deam_confirms{0};
  std::uint64_t deam_confirms_skipped{0};
  std::uint64_t deam_ambiguous_edges{0};
  std::uint64_t deam_seed_routed{0};
  // Sigma / (sigma, q) families (graded.family): builds and the wall of their node
  // solves and probes (inside the deam bucket), iterate prices on a family, Premium
  // attempts a family refused, certification-grid prices on a family / on Fast instead.
  std::uint64_t deam_family_builds{0};
  std::uint64_t deam_family_build_us{0};
  std::uint64_t deam_family_prices{0};
  std::uint64_t deam_family_refused{0};
  std::uint64_t deam_family_grid_prices{0};
  std::uint64_t deam_family_grid_cold{0};
  // K3 typed acceptance and the K4 native fit route (fit_timing counters of the same names;
  // zero off those routes): roots by acceptance kind / reason, requests by the native lane's
  // InverseStatus and by the route's IvRouteOutcome, lanes started from a carry, reference
  // requests / evaluations / budget refusals, band edges decided by bound / priced, family
  // sides the adaptive rule asked for, and the largest per-point family grid bound (USD).
  std::array<std::uint64_t, 5> deam_accept_by_kind{};
  std::array<std::uint64_t, 9> deam_accept_by_reason{};
  std::array<std::uint64_t, 11> deam_native_status{};
  std::array<std::uint64_t, 11> deam_route_outcome{};
  std::uint64_t deam_native_carried{0};
  std::uint64_t deam_reference_requests{0};
  std::uint64_t deam_reference_evaluations{0};
  std::uint64_t deam_reference_budget_refused{0};
  std::uint64_t deam_edges_bound_decided{0};
  std::uint64_t deam_edges_priced{0};
  std::uint64_t deam_family_sides_requested{0};
  std::array<std::uint64_t, 7> deam_family_refused_by_reason{}; // K4b: by DeAmFamilyRefusal
  double deam_family_grid_max_bound_usd{0.0};
  // K4b (r6): the inverse sub-clocks summed over chains (ns; fit_timing counters of the same
  // names): every inversion request's wall, the lane / family-accept / confirm / leg /
  // reference parts inside it (IvPremiumSubClock order) and the chains' band-edge wall.
  std::uint64_t deam_sub_request_ns{0};
  std::array<std::uint64_t, 5> deam_sub_ns{};
  std::uint64_t deam_sub_endpoint_ns{0};
  // K4b (r6): the batched native route's counters (fit_timing counters of the same names).
  std::uint64_t deam_sub_prefill_ns{0};
  std::uint64_t deam_batch_calls{0};
  std::uint64_t deam_batch_lanes{0};
  std::uint64_t deam_batch_avx2_calls{0};
  std::uint64_t deam_batch_lane_hits{0};
  std::uint64_t deam_batch_lane_misses{0};
  std::uint64_t deam_batch_confirms{0};
  std::uint64_t deam_batch_confirm_hits{0};
  std::uint64_t deam_reference_wall_refused{0};
  std::uint64_t deam_sub_grid_family_ns{0};
  std::uint64_t deam_sub_grid_kernel_ns{0};
  std::uint64_t deam_lane_vegas{0};
};

void add_fit_work(FitWork &work, const counters::fit_timing::FitTimingCounters &chain) noexcept;

// The stage parts of a fit lap: fit_timing buckets of the lap's chains (`delta`) with
// the family build wall (`delta.deam_family_build_us`) and the de-Am stage clock's
// endpoint / exception time of the same lap carved out of the deam bucket, in that
// order (each clamped to what is left of the bucket, the excess returned in `excess`).
[[nodiscard]] std::array<StagePart, 7> fit_parts(const FitWork &delta,
                                                 std::uint64_t endpoint_ns,
                                                 std::uint64_t exception_ns,
                                                 double &excess) noexcept;

// Component-wise after - before (saturating) of two cumulative FitWork totals.
[[nodiscard]] FitWork fit_work_delta(const FitWork &after, const FitWork &before) noexcept;

[[nodiscard]] Json fit_work_json(const FitWork &work);
// The family counters of a run (bench.counters.families): measured by fit_timing, so
// they are real counts (zero when the route built none); the fields the fit does not
// count are `unavailable`.
[[nodiscard]] Json families_json(const FitWork &work);
[[nodiscard]] Json stage_clock_json(const DeAmStageClock &clock);

// Exclusive stage table of one span: every stage with its seconds (null and status
// not_built for the family stage when `family_built` is false), their sum, the
// independently measured cold clock (its own start / end stamps), unattributed_s =
// cold - sum (time no stage measured), the clock's unclaimed lap time that explains it,
// span_check_s = cold - sum - unclaimed (zero when the laps span the cold clock), and
// reconciles_within_1pct = |unattributed_s| <= 1% of the cold clock.
[[nodiscard]] Json stages_json(const PhaseClock &clock, double cold_calibration_s,
                               bool family_built);

// Compiler, flags, ISA and math-mode facts of THIS binary (embedded at compile time:
// ATX_VOL_BENCH_CXX_FLAGS / ATX_VOL_BENCH_BUILD_TYPE from CMake, predefined macros).
// fast_math is true when any fast-math / finite-math predefined macro is present.
[[nodiscard]] Json build_info_json();

// Scope guard for the de-Am stage clock (deam_stage_clock_arm): arms `clock` (null
// disarms) on the calling thread for the guard's lifetime and restores the previously
// armed clock on every exit, an exception included.
class StageClockArm {
public:
  explicit StageClockArm(DeAmStageClock *clock) noexcept;
  ~StageClockArm();
  StageClockArm(const StageClockArm &) = delete;
  StageClockArm &operator=(const StageClockArm &) = delete;

private:
  DeAmStageClock *previous_;
};

// Population of one BRACKETED row of the IV comparator (iv_comparator_main.cpp;
// docs/benchmark-clocks.md section 8). `error` = |served_iv - bracketed_iv| and
// `uncertainty` = the reference's fine-scheme uncertainty at the root, both in absolute
// vol. Resolved (uncertainty finite and <= threshold): pass / fail by error <= / >
// threshold. Otherwise a decidable fail when the error clears the threshold by more than
// kDecidableFailMargin uncertainties (error > threshold + 3 * uncertainty: even a
// reference off by three times its own estimated error cannot make the row pass), else
// unresolved. A non-finite error or uncertainty (no reference vega) is unresolved.
inline constexpr double kDecidableFailMargin = 3.0;
enum class IvPopulation { ResolvedPass, ResolvedFail, DecidableFail, ReferenceUnresolved };
[[nodiscard]] IvPopulation classify_iv_row(double error, double uncertainty,
                                           double threshold) noexcept;
[[nodiscard]] const char *iv_population_name(IvPopulation population) noexcept;

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

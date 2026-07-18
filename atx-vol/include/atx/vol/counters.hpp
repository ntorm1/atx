#pragma once

// ── ATX_VOL_COUNTERS — opt-in, compile-time algorithm counters (P0.2) ─────
//
// A zero-overhead instrumentation facility for the American pricer, correction
// cache, and portfolio pricer hot paths. It answers "how many boundary solves /
// Newton sweeps / cache hits / quadrature evaluations did that price cost?" —
// the algorithmic work a wall-clock benchmark cannot see.
//
// ## Compile-time gate
//
// Everything is behind the `ATX_VOL_COUNTERS` compile definition (a new CMake
// option, default OFF). When the definition is ABSENT:
//
//   * `ATX_VOL_COUNT(...)` / `ATX_VOL_COUNT_N(...)` expand to `((void)0)` — no
//     atomic op, no load/store, no branch. The preprocessed pricing TU is
//     byte-for-byte the code it was before instrumentation (verified in the
//     report by diffing the -E output).
//   * `counters_enabled()` is `constexpr false`, so `if constexpr` blocks that
//     read counters vanish.
//   * `snapshot()` returns a Snapshot with `enabled == false` — the disabled
//     sentinel a caller (or the zero-cost unit test) checks.
//   * No exact-counter global mutable state is defined. The independent
//     lightweight sampler below remains enabled in production builds.
//
// When the definition is PRESENT the counters are a header-only array of
// `std::atomic<uint64_t>` (inline variables, C++20) incremented with relaxed
// ordering — safe under the portfolio pricer's worker fan-out, and cheap enough
// that the ON build is still representative (it is a diagnostic build, not the
// baseline build).
//
// ## Usage
//
//   #include "atx/vol/counters.hpp"
//   ATX_VOL_COUNT(BoundarySolves);          // += 1
//   ATX_VOL_COUNT_N(FrameBytes, n_bytes);   // += n
//   const auto snap = atx::vol::counters::snapshot();
//   if (snap.enabled) { use snap.get(Counter::BoundarySolves); }

#include <atomic>
#include <cstdint>
#include <limits>

#if defined(ATX_VOL_COUNTERS)
#include <array>
#endif

namespace atx::vol::counters {

// The counted events. Keep in sync with kNames below (1:1, same order). New
// counters append before Count_ so existing indices are stable.
enum class Counter : unsigned {
  // Andersen-Lake boundary kernel
  BoundarySolves = 0,  // cold boundary seeds (al_seed_boundary from a fresh solve)
  JacobiNewtonSweeps,  // damped Jacobi-Newton boundary sweeps
  FixedPointSweeps,    // naive fixed-point boundary sweeps
  EarlyResidualExits,  // sweeps short-circuited by the residual < tol test
  PremiumQuadEvals,    // early-exercise premium quadrature node evaluations
  NormCdfCalls,        // norm_cdf calls inside the boundary/premium kernel
  LogCalls,            // std::log calls inside the boundary/premium kernel
  ExpCalls,            // std::exp calls inside the boundary/premium kernel
  ScalarFallbackLanes, // scalar (non-SIMD) fallback lanes (0 today; P3 will fill)
  // Correction cache (hot-path American price)
  CacheHits,           // american_price_cached served from a populated cache
  CacheOutOfBoxClamps, // cache query clamped to a box edge
  CacheColdFallbacks,  // cached path fell back to the cold ALO solve
  // Portfolio pricer
  FrameAllocations, // output-frame column vector allocations
  FrameBytes,       // output-frame bytes touched
  WorkerLaunches,   // pricing-pool worker threads ACTUALLY CREATED (P1.4: once,
                    // at the pool's first use; 0 on every steady-state reprice —
                    // repurposed from the old per-call nt-1 jthread launch count)
  // Correction cache 3D Chebyshev kernel (appended last so all prior indices are
  // stable, per the "append before Count_" rule above)
  ClenshawSweeps, // 3D Clenshaw sweeps (one per cheb_clenshaw3d[_partial] call)
  // Portfolio pricer — retained-substrate accounting
  PreparedBuilds, // PreparedPortfolio::create calls from the pricer (0 on a warm reuse)
  // Pricing executor (P1.4) — persistent pool dispatch accounting
  PoolDispatches,            // run_blocks/run_ranges/run_dynamic pool wakes (0 inline)
  ResolvedPriceWrapperCalls, // exact wrapper entries reached from PricedSurface
  ResolvedPriceWrapperLanes, // lanes submitted to those wrapper entries
  AmericanAvxPackDispatches, // complete packs actually dispatched to AVX2
  // Lower bound within this wrapper only: excludes opaque AVX internal patches
  // and scalar American calls made elsewhere in the library.
  AmericanWrapperKnownScalarLanes,
  // Actual PricedSurface routes after full-bundle dominance, not mask requests.
  SurfaceScalarPriceRoutes, // evaluate_resolved only; resolved batch counted separately
  SurfaceDeltaRoutes,
  SurfaceVegaRoutes,
  SurfaceFullGreekRoutes,
  // Correction cache derivative-coefficient transform (T16b)
  ChebDiffCoefs, // cheb_diff_coefs calls (build-time C_k precompute + query-time T/sigma partials)
  // Adjacent valuation reuse: unique-contract base-Greek lanes skipped by P&L.
  BaseGreekReuseLanes,
  // Full-Greek producer/consumer handoff: accepted unique lanes and candidate
  // seeds rejected for missing/mismatched/conflicting provenance.
  FullGreekSeedReuseLanes,
  FullGreekSeedRejectedCandidates,
  // Retained Andersen-Lake state. Counts owning State allocations only; reset
  // and each residual price must leave this unchanged.
  AloStateAllocations,
  Count_
};

inline constexpr unsigned kCount = static_cast<unsigned>(Counter::Count_);

// Stable machine-readable names (used as the benchmark JSON counter keys).
inline constexpr const char *kNames[kCount] = {
    "cnt_boundary_solves",
    "cnt_jacobi_newton_sweeps",
    "cnt_fixed_point_sweeps",
    "cnt_early_residual_exits",
    "cnt_premium_quad_evals",
    "cnt_norm_cdf_calls",
    "cnt_log_calls",
    "cnt_exp_calls",
    "cnt_scalar_fallback_lanes",
    "cnt_cache_hits",
    "cnt_cache_oob_clamps",
    "cnt_cache_cold_fallbacks",
    "cnt_frame_allocations",
    "cnt_frame_bytes",
    "cnt_worker_launches",
    "cnt_clenshaw_sweeps",
    "cnt_prepared_builds",
    "cnt_pool_dispatches",
    "cnt_resolved_price_wrapper_calls",
    "cnt_resolved_price_wrapper_lanes",
    "cnt_american_avx_pack_dispatches",
    "cnt_american_wrapper_known_scalar_lanes",
    "cnt_surface_scalar_price_routes",
    "cnt_surface_delta_routes",
    "cnt_surface_vega_routes",
    "cnt_surface_full_greek_routes",
    "cnt_cheb_diff_coefs",
    "cnt_base_greek_reuse_lanes",
    "cnt_full_greek_seed_reuse_lanes",
    "cnt_full_greek_seed_rejected_candidates",
    "cnt_alo_state_allocations",
};

// A point-in-time copy of every counter. `enabled == false` is the sentinel a
// caller reads from an OFF build (or the zero-cost test asserts).
struct Snapshot {
  bool enabled = false;
  std::uint64_t values[kCount] = {};

  [[nodiscard]] std::uint64_t get(Counter c) const noexcept {
    return values[static_cast<unsigned>(c)];
  }
};

// constexpr build-mode probe — usable in `if constexpr`.
[[nodiscard]] constexpr bool counters_enabled() noexcept {
#if defined(ATX_VOL_COUNTERS)
  return true;
#else
  return false;
#endif
}

#if defined(ATX_VOL_COUNTERS)

namespace detail {
// Header-only global storage (C++20 inline variable): value-initialized to zero,
// one shared instance across all TUs. Relaxed atomics — the counts are a diagnostic
// aggregate, not an ordering primitive.
inline std::array<std::atomic<std::uint64_t>, kCount> g_counters{};
} // namespace detail

inline void add(Counter c, std::uint64_t n = 1) noexcept {
  detail::g_counters[static_cast<unsigned>(c)].fetch_add(n, std::memory_order_relaxed);
}

[[nodiscard]] inline Snapshot snapshot() noexcept {
  Snapshot s;
  s.enabled = true;
  for (unsigned i = 0; i < kCount; ++i) {
    s.values[i] = detail::g_counters[i].load(std::memory_order_relaxed);
  }
  return s;
}

inline void reset() noexcept {
  for (unsigned i = 0; i < kCount; ++i) {
    detail::g_counters[i].store(0, std::memory_order_relaxed);
  }
}

// += 1 / += n. Non-empty statements so `if (x) ATX_VOL_COUNT(y);` parses.
#define ATX_VOL_COUNT(counter) ::atx::vol::counters::add(::atx::vol::counters::Counter::counter)
#define ATX_VOL_COUNT_N(counter, n)                                                                \
  ::atx::vol::counters::add(::atx::vol::counters::Counter::counter, static_cast<std::uint64_t>(n))

#else // !ATX_VOL_COUNTERS — the default. Zero footprint.

// Disabled sentinel: enabled == false, all zero. No global state referenced.
[[nodiscard]] inline Snapshot snapshot() noexcept { return Snapshot{}; }
inline void reset() noexcept {}

// CAUTION: the OFF expansion below drops `n` entirely (never evaluated) — that
// is required for zero-cost, but it means a future ATX_VOL_COUNT_N call site
// whose `n` argument has a side effect (e.g. `ATX_VOL_COUNT_N(X, ++foo)`) would
// silently lose that side effect in the OFF build. Every call site today
// (FrameBytes, WorkerLaunches, ...) passes a side-effect-free expression, so
// this is currently safe; keep it that way, or gate the side effect separately.
#define ATX_VOL_COUNT(counter) ((void)0)
#define ATX_VOL_COUNT_N(counter, n) ((void)0)

#endif // ATX_VOL_COUNTERS

// Always-on production telemetry. Unlike the exact diagnostic counters above,
// this plane samples one in every kSamplePeriod root operations. Query outcomes
// are mutually exclusive, and American-IV kernel work is accumulated in TLS and
// published with four relaxed atomic additions only for a sampled inversion.
// It therefore exposes useful rate/work estimates without putting an atomic on
// every pricing event.
namespace lightweight {

inline constexpr std::uint32_t kSamplePeriod = 64u;
static_assert((kSamplePeriod & (kSamplePeriod - 1u)) == 0u,
              "lightweight counter sample period must be a power of two");

struct Snapshot {
  std::uint32_t sample_period{kSamplePeriod};
  std::uint64_t representative_hit_samples{0u};
  std::uint64_t other_cache_hit_samples{0u};
  std::uint64_t cold_fallback_samples{0u};
  std::uint64_t american_iv_samples{0u};
  std::uint64_t residual_evaluations_in_sampled_iv{0u};
  std::uint64_t boundary_solves_in_sampled_iv{0u};
  std::uint64_t exp_calls_in_sampled_iv{0u};

  [[nodiscard]] std::uint64_t cache_hit_samples() const noexcept {
    return representative_hit_samples + other_cache_hit_samples;
  }

  [[nodiscard]] std::uint64_t query_attempt_samples() const noexcept {
    return cache_hit_samples() + cold_fallback_samples;
  }

private:
  [[nodiscard]] std::uint64_t estimate(std::uint64_t samples) const noexcept {
    const std::uint64_t period = sample_period;
    if (period != 0u && samples > std::numeric_limits<std::uint64_t>::max() / period) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return samples * period;
  }

  [[nodiscard]] static double ratio(std::uint64_t numerator, std::uint64_t denominator) noexcept {
    return denominator == 0u ? 0.0
                             : static_cast<double>(numerator) / static_cast<double>(denominator);
  }

public:
  [[nodiscard]] std::uint64_t estimated_query_attempts() const noexcept {
    return estimate(query_attempt_samples());
  }

  [[nodiscard]] std::uint64_t estimated_cache_hits() const noexcept {
    return estimate(cache_hit_samples());
  }

  [[nodiscard]] std::uint64_t estimated_cold_fallbacks() const noexcept {
    return estimate(cold_fallback_samples);
  }

  [[nodiscard]] std::uint64_t estimated_american_iv_inversions() const noexcept {
    return estimate(american_iv_samples);
  }

  [[nodiscard]] std::uint64_t estimated_boundary_solves() const noexcept {
    return estimate(boundary_solves_in_sampled_iv);
  }

  [[nodiscard]] std::uint64_t estimated_residual_evaluations() const noexcept {
    return estimate(residual_evaluations_in_sampled_iv);
  }

  [[nodiscard]] std::uint64_t estimated_exp_calls() const noexcept {
    return estimate(exp_calls_in_sampled_iv);
  }

  [[nodiscard]] double cache_hit_rate() const noexcept {
    return ratio(cache_hit_samples(), query_attempt_samples());
  }

  [[nodiscard]] double representative_hit_rate() const noexcept {
    return ratio(representative_hit_samples, query_attempt_samples());
  }

  [[nodiscard]] double cold_fallback_rate() const noexcept {
    return ratio(cold_fallback_samples, query_attempt_samples());
  }

  [[nodiscard]] double boundary_solves_per_inversion() const noexcept {
    return ratio(boundary_solves_in_sampled_iv, american_iv_samples);
  }

  [[nodiscard]] double residual_evaluations_per_inversion() const noexcept {
    return ratio(residual_evaluations_in_sampled_iv, american_iv_samples);
  }

  [[nodiscard]] double exp_calls_per_inversion() const noexcept {
    return ratio(exp_calls_in_sampled_iv, american_iv_samples);
  }
};

namespace detail {

struct GlobalCounters {
  std::atomic<std::uint64_t> representative_hits{0u};
  std::atomic<std::uint64_t> other_cache_hits{0u};
  std::atomic<std::uint64_t> cold_fallbacks{0u};
  std::atomic<std::uint64_t> american_iv{0u};
  std::atomic<std::uint64_t> residual_evaluations{0u};
  std::atomic<std::uint64_t> boundary_solves{0u};
  std::atomic<std::uint64_t> exp_calls{0u};
};

struct InversionAccumulator {
  std::uint64_t residual_evaluations{0u};
  std::uint64_t boundary_solves{0u};
  std::uint64_t exp_calls{0u};
};

struct SamplerState {
  std::uint32_t position{0u};
  std::uint32_t target{0u};
  std::uint64_t random{0x9e3779b97f4a7c15ULL};
};

// R-20: a monotonic nonce, bumped once per constructed ThreadState, so every
// thread seeds its samplers from a distinct value. With a fixed shared seed AND
// target==0, every thread's sampler was phase-locked to the same within-block
// positions and always observed the FIRST event of block 0 (and of every short
// run) — a systematic bias in *which* operations the DoD counters describe.
inline std::atomic<std::uint64_t> g_sampler_seed_nonce{0u};

// splitmix64 finalizer over (base ^ nonce): a well-mixed, thread-distinct seed.
[[nodiscard]] inline std::uint64_t mix_thread_seed(std::uint64_t base) noexcept {
  const std::uint64_t nonce = g_sampler_seed_nonce.fetch_add(1u, std::memory_order_relaxed);
  std::uint64_t z = base ^ (0x9e3779b97f4a7c15ULL * (nonce + 1u));
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

// Advance the xorshift64 stream and pick the next within-block target position.
// Exactly one event per kSamplePeriod block is still selected; only the phase
// moves, so a periodic pricing pattern cannot stay phase-locked to the sampler.
inline void roll_sample_target(SamplerState &state) noexcept {
  state.random ^= state.random << 13u;
  state.random ^= state.random >> 7u;
  state.random ^= state.random << 17u;
  state.target = static_cast<std::uint32_t>(state.random) & (kSamplePeriod - 1u);
}

struct ThreadState {
  SamplerState query{};
  SamplerState american_iv{};
  InversionAccumulator *active_inversion{nullptr};
  // R-21: nesting depth of AmericanIvSample scopes on this thread. Only the
  // OUTERMOST inversion (depth 0 -> 1) is eligible to be sampled as a root;
  // deeper scopes are always part of the outer operation, whether or not the
  // outer was itself sampled. (The prior `active_inversion==nullptr` gate
  // conflated "not nested" with "outer not sampled", so a nested inversion under
  // an unsampled outer re-sampled itself as a spurious extra root.)
  std::uint32_t inversion_depth{0u};

  ThreadState() noexcept {
    // R-20: distinct per-thread seed for each independent sampler stream...
    query.random = mix_thread_seed(0x9e3779b97f4a7c15ULL);
    american_iv.random = mix_thread_seed(0xd1b54a32d192ed03ULL);
    // ...and roll the target BEFORE first use so block 0's sampled position is
    // randomized too (otherwise target==0 always samples the very first event).
    roll_sample_target(query);
    roll_sample_target(american_iv);
  }
};

inline GlobalCounters g_counters{};
inline thread_local ThreadState t_state{};

[[nodiscard]] inline bool take_sample(SamplerState &state) noexcept {
  const bool sampled = state.position == state.target;
  ++state.position;
  if (state.position == kSamplePeriod) {
    state.position = 0u;
    roll_sample_target(state);
  }
  return sampled;
}

[[nodiscard]] inline std::uint64_t subtract_or_restart(std::uint64_t before,
                                                       std::uint64_t after) noexcept {
  return after >= before ? after - before : after;
}

} // namespace detail

// One configured fast-cache attempt. If no hit is recorded before destruction,
// the sampled operation is classified as a cold fallback. Copy/move are denied
// so one attempt can publish at most one outcome.
class QuerySample final {
public:
  explicit QuerySample(bool eligible) noexcept
      : sampled_(eligible && detail::take_sample(detail::t_state.query)) {}

  QuerySample(const QuerySample &) = delete;
  QuerySample &operator=(const QuerySample &) = delete;
  QuerySample(QuerySample &&) = delete;
  QuerySample &operator=(QuerySample &&) = delete;

  ~QuerySample() noexcept {
    if (sampled_ && !completed_) {
      // SAFETY: telemetry is an unordered aggregate; it does not synchronize
      // application data, so relaxed ordering is sufficient.
      detail::g_counters.cold_fallbacks.fetch_add(1u, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] bool sampled() const noexcept { return sampled_; }

  void record_cache_hit(bool representative) noexcept {
    if (!sampled_ || completed_) {
      return;
    }
    std::atomic<std::uint64_t> &counter = representative ? detail::g_counters.representative_hits
                                                         : detail::g_counters.other_cache_hits;
    // SAFETY: see the destructor; no ordering relationship is consumed.
    counter.fetch_add(1u, std::memory_order_relaxed);
    completed_ = true;
  }

private:
  bool sampled_{false};
  bool completed_{false};
};

// Samples a complete American-IV inversion, including the boundary/exp work
// performed by its residual evaluations. Nested scopes contribute to the outer
// sampled inversion rather than publishing a second root operation.
class AmericanIvSample final {
public:
  AmericanIvSample() noexcept : previous_(detail::t_state.active_inversion) {
    // R-21: only the OUTERMOST inversion is a root; a nested scope never takes
    // its own sample (regardless of whether the outer was sampled), so a
    // nested-under-unsampled inversion can no longer be miscounted as a second
    // root — its kernel work flows to `active_inversion` (the outer accumulator
    // when the outer was sampled, else nullptr = correctly uncounted).
    const bool is_root = detail::t_state.inversion_depth == 0u;
    ++detail::t_state.inversion_depth;
    if (is_root && detail::take_sample(detail::t_state.american_iv)) {
      sampled_ = true;
      detail::t_state.active_inversion = &accumulator_;
    }
  }

  AmericanIvSample(const AmericanIvSample &) = delete;
  AmericanIvSample &operator=(const AmericanIvSample &) = delete;
  AmericanIvSample(AmericanIvSample &&) = delete;
  AmericanIvSample &operator=(AmericanIvSample &&) = delete;

  ~AmericanIvSample() noexcept {
    // Unwind the nesting depth for every scope (sampled or not), symmetric with
    // the constructor's increment.
    --detail::t_state.inversion_depth;
    if (!sampled_) {
      return;
    }
    detail::t_state.active_inversion = previous_;
    // SAFETY: these atomics publish diagnostic aggregates only. The TLS scope
    // owns accumulator_ until all additions complete.
    detail::g_counters.american_iv.fetch_add(1u, std::memory_order_relaxed);
    detail::g_counters.residual_evaluations.fetch_add(accumulator_.residual_evaluations,
                                                      std::memory_order_relaxed);
    detail::g_counters.boundary_solves.fetch_add(accumulator_.boundary_solves,
                                                 std::memory_order_relaxed);
    detail::g_counters.exp_calls.fetch_add(accumulator_.exp_calls, std::memory_order_relaxed);
  }

private:
  detail::InversionAccumulator accumulator_{};
  detail::InversionAccumulator *previous_{nullptr};
  bool sampled_{false};
};

inline void record_residual_evaluation() noexcept {
  detail::InversionAccumulator *const active = detail::t_state.active_inversion;
  if (active != nullptr) {
    ++active->residual_evaluations;
  }
}

inline void record_boundary_solves(std::uint64_t count = 1u) noexcept {
  detail::InversionAccumulator *const active = detail::t_state.active_inversion;
  if (active != nullptr) {
    active->boundary_solves += count;
  }
}

inline void record_exp_calls(std::uint64_t count) noexcept {
  detail::InversionAccumulator *const active = detail::t_state.active_inversion;
  if (active != nullptr) {
    active->exp_calls += count;
  }
}

[[nodiscard]] inline Snapshot snapshot() noexcept {
  Snapshot result;
  // SAFETY: a snapshot is a best-effort diagnostic view. Route attempts remain
  // algebraically coherent because they are derived from exclusive outcomes.
  result.representative_hit_samples =
      detail::g_counters.representative_hits.load(std::memory_order_relaxed);
  result.other_cache_hit_samples =
      detail::g_counters.other_cache_hits.load(std::memory_order_relaxed);
  result.cold_fallback_samples = detail::g_counters.cold_fallbacks.load(std::memory_order_relaxed);
  result.american_iv_samples = detail::g_counters.american_iv.load(std::memory_order_relaxed);
  result.residual_evaluations_in_sampled_iv =
      detail::g_counters.residual_evaluations.load(std::memory_order_relaxed);
  result.boundary_solves_in_sampled_iv =
      detail::g_counters.boundary_solves.load(std::memory_order_relaxed);
  result.exp_calls_in_sampled_iv = detail::g_counters.exp_calls.load(std::memory_order_relaxed);
  return result;
}

[[nodiscard]] inline Snapshot delta(const Snapshot &before, const Snapshot &after) noexcept {
  Snapshot result;
  result.sample_period = after.sample_period;
  result.representative_hit_samples = detail::subtract_or_restart(before.representative_hit_samples,
                                                                  after.representative_hit_samples);
  result.other_cache_hit_samples =
      detail::subtract_or_restart(before.other_cache_hit_samples, after.other_cache_hit_samples);
  result.cold_fallback_samples =
      detail::subtract_or_restart(before.cold_fallback_samples, after.cold_fallback_samples);
  result.american_iv_samples =
      detail::subtract_or_restart(before.american_iv_samples, after.american_iv_samples);
  result.residual_evaluations_in_sampled_iv = detail::subtract_or_restart(
      before.residual_evaluations_in_sampled_iv, after.residual_evaluations_in_sampled_iv);
  result.boundary_solves_in_sampled_iv = detail::subtract_or_restart(
      before.boundary_solves_in_sampled_iv, after.boundary_solves_in_sampled_iv);
  result.exp_calls_in_sampled_iv =
      detail::subtract_or_restart(before.exp_calls_in_sampled_iv, after.exp_calls_in_sampled_iv);
  return result;
}

// Test/measurement seam. Precondition: no concurrent producer and no active
// AmericanIvSample on the calling thread.
inline void reset() noexcept {
  detail::g_counters.representative_hits.store(0u, std::memory_order_relaxed);
  detail::g_counters.other_cache_hits.store(0u, std::memory_order_relaxed);
  detail::g_counters.cold_fallbacks.store(0u, std::memory_order_relaxed);
  detail::g_counters.american_iv.store(0u, std::memory_order_relaxed);
  detail::g_counters.residual_evaluations.store(0u, std::memory_order_relaxed);
  detail::g_counters.boundary_solves.store(0u, std::memory_order_relaxed);
  detail::g_counters.exp_calls.store(0u, std::memory_order_relaxed);
  detail::t_state = detail::ThreadState{};
}

} // namespace lightweight

} // namespace atx::vol::counters

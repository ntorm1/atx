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
//   #include "atx/vol/detail/counters.hpp"
//   ATX_VOL_COUNT(BoundarySolves);          // += 1
//   ATX_VOL_COUNT_N(FrameBytes, n_bytes);   // += n
//   const auto snap = atx::vol::counters::snapshot();
//   if (snap.enabled) { use snap.get(Counter::BoundarySolves); }

#include <array>  // solve-ledger per-thread block (always on) + gated exact counters
#include <atomic>
#include <chrono>
#include <cstddef> // std::size_t -- every_name_present's array-bound parameter
#include <cstdint>
#include <limits>
#include <mutex>  // solve ledger registry lock (register/scrape only; never the hot path)
#include <vector> // solve-ledger per-step trace sink

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
  // COMPLETE 4-lane packs handed to the AVX2 boundary driver — and nothing else.
  // Bumped at BOTH dispatch sites in american_batch.cpp: `american_price_batch`
  // (by m/4, the driver's own complete-pack count) and
  // `american_price_batch_resolved` (one per full pack, as it fills them).
  // What it deliberately does NOT count, so no gate mis-reads it (REVWSA finding 6):
  //   * the n % 4 TAIL. Both entries flush a short pack scalar and the AVX2 driver
  //     prices its own tail scalar, so a workload of < 4 kernel lanes dispatches
  //     nothing and reads 0 while being perfectly healthy.
  //   * lanes patched back to scalar INSIDE a dispatched pack (all-ineligible packs,
  //     non-finite results). The pack was still dispatched; this counts dispatches,
  //     not lanes. NOTHING in this codebase gives that lane view — not a counter, and
  //     not a route output either (REVA7TIDY; an earlier revision of this block named
  //     the route outputs as the substitute, which they are not):
  //       - AmericanWrapperKnownScalarLanes is bumped ONLY inside
  //         american_price_batch_resolved, so it does not exist at
  //         american_price_batch at all, and at either entry it cannot see lanes the
  //         AVX2 driver patched to scalar inside a pack it had already dispatched.
  //       - PriceBatchOutput::route[] and ResolvedAmericanPriceBatchRequest::
  //         pack_dispatch[] are per-LANE spans but report the containing PACK's
  //         dispatch, not the driver's per-lane patch — american_batch.cpp assigns
  //         `out.route[i] = pack_route` uniformly to every packed lane, and both
  //         members' own docs disclaim exactly this blindness
  //         (american_batch.hpp, ResolvedAmericanPriceBatchRequest::pack_dispatch and
  //         PriceBatchOutput::route).
  //       - The two are not even equivalent to each other on the NON-FINITE half:
  //         at the resolved entry a non-finite packed lane is re-run through
  //         scalar_lane, which sets pack_dispatch[i] = Scalar, so pack_dispatch[]
  //         DOES catch that one case; at american_price_batch the same lane keeps
  //         route[i] = Avx2 and only its LaneStatus becomes Unsupported.
  //     So: the in-pack scalar patch is unobservable at either entry, and the
  //     non-finite patch is observable only at the resolved entry, only through
  //     pack_dispatch[]. Anything needing the true lane view has to add a counter.
  //   * AVX2 dispatched from ANYWHERE outside american_batch.cpp's two entries. The
  //     bumps live in the two CALLERS, not inside simd::american_put_boundary_batch,
  //     so every OTHER caller of that same function dispatches AVX2 packs and bumps
  //     nothing: bench/american_shootout_bench.cpp's run_boundary_batch (which backs
  //     the registered american/boundary_batch/avx2 and .../avx2_qlfast rows) and the
  //     direct call sites in simd_american_test.cpp, simd_vector_math_test.cpp and
  //     american_batch_test.cpp. Likewise AVX2 reached by a different route entirely
  //     (e.g. the laned Greeks kernel in american_greeks_avx2.cpp).
  // Therefore: NON-ZERO proves complete packs went to AVX2; ZERO does NOT prove the
  // pack path is dead. It is not a general "the AVX2 path was taken" observable and
  // must not be re-used as one without a lane-count check alongside it.
  AmericanAvxPackDispatches,
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
  // Calls and lanes submitted by the shared PricedSurface/PricedSurfaceView
  // analytic-Greek accumulator to a side-specific batch entry point. These are
  // deliberately separate from SurfaceFullGreekRoutes (bundles) and from
  // AmericanAvxPackDispatches (the resolved-price wrapper only).
  SurfaceGreekBatchDispatches,
  SurfaceGreekBatchLanes,
  // Successful construction of a ConvexDense/SplineVol concrete curve from a
  // mapped PricedSurfaceView record. Counts derived-state materializations, not
  // queries or the lightweight per-slice synchronization slots.
  SurfaceViewHeavyMaterializations,
  // ConvexDense's numerical-wing fallback table. The table is intentionally
  // absent on ordinary queries; these counters make accidental eager work and
  // rare fallback coverage observable without timing-sensitive tests.
  ConvexDenseWingAnchorBuilds,
  ConvexDenseWingAnchorIvEvaluations,
  ConvexDenseWingFallbackEntries,
  // Strict convex-dense admission-recovery rung (pricer_fitter.cpp): fired only
  // after the fallback ladder is exhausted with a pure-geometry rejection.
  // Rounds counts every strict refit attempted; Admitted counts only those whose
  // refit passed independent admission. A clean/admitted fit never bumps either.
  RiskStrictRecoveryRounds,
  RiskStrictRecoveryAdmitted,
  Count_
};

inline constexpr unsigned kCount = static_cast<unsigned>(Counter::Count_);

// Task F-5. Both name tables in this header are C arrays sized by their enum's
// own `Count_`, and C++ aggregate initialization SILENTLY value-initializes
// every element the initializer list omits. A new enumerator added without its
// name string therefore yields a `nullptr` entry -- no compiler diagnostic
// fires, and `std::size(kNames) == kCount` would be vacuously true because the
// BOUND is what is short-initialized, not the declaration. The nullptr then
// reaches `py::str` (python/src/bindings/backtest.cpp) and `printf("%s")`
// (tools/surface_db_build_main.cpp) unchecked. The "keep in sync (1:1, same
// order)" comment above each table was, until now, enforced by nothing.
//
// Stated once and asserted at both tables. The `1:1, same order` half is still
// convention -- this catches a MISSING name, never a misordered one.
template <std::size_t N>
[[nodiscard]] constexpr bool every_name_present(const char *const (&names)[N]) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (names[i] == nullptr) {
      return false;
    }
  }
  return true;
}

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
    "cnt_surface_greek_batch_dispatches",
    "cnt_surface_greek_batch_lanes",
    "cnt_surface_view_heavy_materializations",
    "cnt_convex_dense_wing_anchor_builds",
    "cnt_convex_dense_wing_anchor_iv_evaluations",
    "cnt_convex_dense_wing_fallback_entries",
    "cnt_risk_strict_recovery_rounds",
    "cnt_risk_strict_recovery_admitted",
};

static_assert(every_name_present(kNames),
              "counters::kNames is short: a Counter enumerator was added without appending "
              "its name string, so kNames now holds a nullptr that reaches the bench JSON "
              "writer unchecked. Append the name, in the enum's own order.");

// A point-in-time copy of every counter. `enabled == false` is the sentinel a
// caller reads from an OFF build (or the zero-cost test asserts).
struct Snapshot {
  bool enabled = false;
  std::uint64_t values[kCount] = {};

  [[nodiscard]] std::uint64_t get(Counter c) const noexcept {
    return values[static_cast<unsigned>(c)];
  }
};

// ── Build-configuration tag: the ODR guard for this plane (plan 5.2) ─────────
//
// EVERYTHING BELOW, DOWN TO THE MATCHING `}`, HAS A DEFINITION THAT DEPENDS ON
// `ATX_VOL_COUNTERS` — `counters_enabled()` returns a different constant, and
// `snapshot()` / `reset()` either touch `g_counters` or do not. Those are inline
// entities with linkage, so before this tag a TU compiled WITHOUT the definition
// and a library compiled WITH it put two different definitions of the same
// entity into one program. That is an ODR violation (IFNDR): the linker keeps
// one arbitrarily and the loser's callers silently get the wrong plane, with no
// diagnostic anywhere.
//
// Naming the configuration in the namespace makes the two views declare
// DIFFERENT entities, so the violation cannot be formed. The namespace is
// `inline`, so every existing spelling — `atx::vol::counters::snapshot()`, and
// the `if constexpr (counters_enabled())` blocks across src/, bench/ and the
// test suite — resolves unchanged; nothing outside this header moves.
//
// The mismatch is also made LOUD rather than merely well-defined:
// `detail::assert_build_configuration_matches()` is declared here, defined in
// src/instrumentation_abi.cpp (compiled with the LIBRARY's view), and called
// from `snapshot()`/`reset()` in BOTH configurations. A TU that disagrees with
// the library references `counters_off::detail::assert_build_...` while the
// library defines `counters_on::detail::assert_build_...`, so the link fails
// with a symbol name that says exactly what is wrong. It is never called from
// `add()` — that is the hot path.
//
// What this deliberately does NOT do is share ONE counter plane across a
// mismatched pair. That would need out-of-line storage, which needs
// `__declspec(dllimport)` on the consumer side for a DATA symbol, which needs
// the `ATX_VOL_API` export macro this release does not have (see the shared-libs
// policy in the root CMakeLists). Under the static-only distribution policy the
// mismatched pair does not need to work — it needs to be impossible to build,
// which is what the guard achieves.
//
// SCOPE: only this plane is tagged. `lightweight`, `timing` and `ledger` below
// are compiled identically in every configuration and are deliberately left
// untagged — tagging them would split the ALWAYS-ON solve ledger into two planes
// whenever the tag differed, which is the opposite of what they are for.
#if defined(ATX_VOL_COUNTERS)
#define ATX_VOL_COUNTERS_ABI_TAG counters_on
#else
#define ATX_VOL_COUNTERS_ABI_TAG counters_off
#endif

inline namespace ATX_VOL_COUNTERS_ABI_TAG {

namespace detail {
// Link-time half of the guard above. Defined once, in the library, under the
// tag the LIBRARY was compiled with. Body is empty on purpose: the symbol's
// existence is the whole assertion.
void assert_build_configuration_matches() noexcept;
} // namespace detail

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
  detail::assert_build_configuration_matches();
  Snapshot s;
  s.enabled = true;
  for (unsigned i = 0; i < kCount; ++i) {
    s.values[i] = detail::g_counters[i].load(std::memory_order_relaxed);
  }
  return s;
}

inline void reset() noexcept {
  detail::assert_build_configuration_matches();
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
// The guard call is the ONE library dependency this configuration takes, and it
// is on the cold query path only — `ATX_VOL_COUNT*` below still expands to
// nothing, so the shipping build is unchanged instruction-for-instruction.
[[nodiscard]] inline Snapshot snapshot() noexcept {
  detail::assert_build_configuration_matches();
  return Snapshot{};
}
inline void reset() noexcept { detail::assert_build_configuration_matches(); }

// CAUTION: the OFF expansion below drops `n` entirely (never evaluated) — that
// is required for zero-cost, but it means a future ATX_VOL_COUNT_N call site
// whose `n` argument has a side effect (e.g. `ATX_VOL_COUNT_N(X, ++foo)`) would
// silently lose that side effect in the OFF build. Every call site today
// (FrameBytes, WorkerLaunches, ...) passes a side-effect-free expression, so
// this is currently safe; keep it that way, or gate the side effect separately.
#define ATX_VOL_COUNT(counter) ((void)0)
#define ATX_VOL_COUNT_N(counter, n) ((void)0)

#endif // ATX_VOL_COUNTERS

} // inline namespace ATX_VOL_COUNTERS_ABI_TAG (counters_on / counters_off)

// The tag itself is an implementation detail of THIS header and is not part of
// the macro surface below — it is expanded above and undefined here so it cannot
// collide in a consumer or be used to smuggle the untagged spelling back in.
#undef ATX_VOL_COUNTERS_ABI_TAG

// ── The macros this header leaves defined, in EVERY configuration ────────────
//
//   ATX_VOL_COUNT(counter)      += 1
//   ATX_VOL_COUNT_N(counter, n) += n
//
// That is the complete set (verified by preprocessing the header with and
// without ATX_VOL_COUNTERS and diffing `-dM` against a baseline TU including the
// same standard headers). Both are ATX_VOL_-prefixed and both exist in both
// configurations, so a consumer's preprocessor environment does not change
// shape when the option is flipped — only what the two macros expand to does.

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

// ── Harness-side pipeline stage attribution (M3, backtest hot-path sprint) ────
//
// A zero-dependency (std::chrono only) named-stage wall-time accumulator + RAII
// scoped timer for attributing an end-to-end pipeline wall to its fit / serialize /
// deserialize / price stages. UNLIKE the exact `ATX_VOL_COUNT*` counters above it is
// ALWAYS compiled in (it carries no global mutable state and touches nothing on the
// pricing hot path) — it is a measurement facility a bench or an instrumented
// pipeline can adopt to time each stage BOUNDARY from the outside, without
// instrumenting the stage's own TU.
//
// Ownership/threading: a `StageAccumulator` is single-thread-owned by convention —
// the thread that drives the four stages of one board owns the accumulator it feeds.
// (Stages may themselves fan out internally; this times the wall the driver observes.)
//
//   using namespace atx::vol::counters::timing;
//   StageAccumulator acc;
//   { ScopedStageTimer t(acc, Stage::Fit);         fitter.fit(chain); }
//   { ScopedStageTimer t(acc, Stage::Serialize);   bytes = serialize(surface); }
//   { ScopedStageTimer t(acc, Stage::Deserialize); board = deserialize(bytes); }
//   { ScopedStageTimer t(acc, Stage::Price);       price_grid(board); }
//   const double deser_frac = acc.fraction(Stage::Deserialize);
namespace timing {

enum class Stage : unsigned {
  Fit = 0,     // fit a board (OptionChain -> FittedSurface)
  Serialize,   // fit output -> on-disk bytes (snapshot + write_surface_archive_v2)
  Deserialize, // bytes -> ready-to-price surfaces (open + reconstruct)
  Price,       // price + greeks over the deserialized surfaces
  Count_
};

inline constexpr unsigned kStageCount = static_cast<unsigned>(Stage::Count_);
inline constexpr const char *kStageNames[kStageCount] = {"fit", "serialize", "deserialize",
                                                         "price"};

// Accumulates elapsed wall (nanoseconds) per stage across many timed scopes.
class StageAccumulator {
public:
  void add_ns(Stage stage, double ns) noexcept { ns_[static_cast<unsigned>(stage)] += ns; }

  [[nodiscard]] double ns(Stage stage) const noexcept { return ns_[static_cast<unsigned>(stage)]; }
  [[nodiscard]] double ms(Stage stage) const noexcept { return ns(stage) * 1.0e-6; }

  [[nodiscard]] double total_ns() const noexcept {
    double total = 0.0;
    for (unsigned i = 0; i < kStageCount; ++i) {
      total += ns_[i];
    }
    return total;
  }

  // Fraction of the summed stage wall attributable to `stage` (0 when nothing timed).
  [[nodiscard]] double fraction(Stage stage) const noexcept {
    const double total = total_ns();
    return total > 0.0 ? ns(stage) / total : 0.0;
  }

  void reset() noexcept {
    for (unsigned i = 0; i < kStageCount; ++i) {
      ns_[i] = 0.0;
    }
  }

private:
  double ns_[kStageCount] = {};
};

// RAII: charges its lifetime's elapsed wall to `stage` on the given accumulator.
// steady_clock is monotonic (immune to wall-clock adjustments) — the right source
// for an elapsed-duration measurement.
class ScopedStageTimer {
public:
  ScopedStageTimer(StageAccumulator &acc, Stage stage) noexcept
      : acc_(acc), stage_(stage), start_(std::chrono::steady_clock::now()) {}

  ScopedStageTimer(const ScopedStageTimer &) = delete;
  ScopedStageTimer &operator=(const ScopedStageTimer &) = delete;
  ScopedStageTimer(ScopedStageTimer &&) = delete;
  ScopedStageTimer &operator=(ScopedStageTimer &&) = delete;

  ~ScopedStageTimer() noexcept {
    const auto end = std::chrono::steady_clock::now();
    acc_.add_ns(stage_, std::chrono::duration<double, std::nano>(end - start_).count());
  }

private:
  StageAccumulator &acc_;
  Stage stage_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace timing

// ── Solve ledger (WS-V V1) — always-on, per-thread, merged-at-read ────────────
//
// The exact `ATX_VOL_COUNT*` counters above are the DIAGNOSTIC plane: shared
// atomics gated OFF by default (they are only compiled in a special counters
// build). The sprint's fewer-solves gates must instead be assertable on the
// SHIPPING `rel`/`rel-avx2` builds — so the solve ledger is a SECOND, always-on
// plane with a contention-free discipline:
//
//   * Each thread owns a `Block` of plain per-counter cells (relaxed atomics used
//     only to keep the concurrent scrape data-race-free — on x86 a relaxed
//     load/store is a plain mov, so a `bump` is load+add+store with NO lock prefix
//     and NO cross-thread cache-line sharing). A counter that perturbs the timing
//     it measures is a bug; this one writes only the calling thread's own line.
//   * Blocks self-register in a global intrusive list at thread start and fold
//     their tally into `g_retired` at thread exit, so a worker that dies mid-run
//     never loses counts. The registry mutex is taken ONLY on register / unregister
//     / snapshot / reset — never on `bump`.
//   * `snapshot()` merges retired + every live block into a plain-value `Counts`.
//     A backtest step or a fit board reads deltas of these snapshots; a `StepTrace`
//     records one delta per step for the per-step gate.
//
// Counter meanings (a "solve-equiv" = one AL boundary solve; analytic greeks
// bundle = 5, FD bundle = 7, a Marks price = 1, adjoint bundle = 1 taped solve):
//   al_boundary_solves — every al_seed_boundary cold seed (the gate metric)
//   al_premium_evals   — early-exercise premium quadrature node evaluations
//   greeks_bundles{fd,analytic,adjoint} — full-Greek bundles by route
//   iv_newton_iters    — American-IV inversion Newton iterations
namespace ledger {

enum class Solve : unsigned {
  AlBoundarySolves = 0,  // al_seed_boundary cold seeds (mirrors Counter::BoundarySolves, always-on)
  AlPremiumEvals,        // early-exercise premium quadrature evaluations
  GreeksBundlesFd,       // full-Greek bundles via the FD stencil route (~7 boundary solves each)
  GreeksBundlesAnalytic, // full-Greek bundles via the analytic AL route (~5 boundary solves each)
  GreeksBundlesAdjoint,  // full-Greek bundles via the adjoint route (1 taped AL solve each)
  IvNewtonIters,         // American-IV inversion Newton iterations
  // WS-L L2 (append-only per PM narrow license; verify workstream complete — this
  // adds an enum entry + name, changes nothing existing). A duplicate mark
  // COMPUTATION: a per-(unique-contract, base-date) mark the backtest re-solves
  // although that exact mark was already computed this step-cycle and is available
  // in the per-step mark memo. The L2 settlement-mark memo drives this to 0;
  // tapped ONLY from loop-owned backtest.cpp.
  DuplicateMarkSolves,
  // WS-A A3 (GR-P2-3, append-only). A cached-jet SERVE (american_price_cached /
  // american_greeks_first_order) whose query risk-free rate has drifted from the
  // fixed-carry correction cache's baked rate by more than the C2 stale-gate
  // (25 bps) — an intraday-rate-move or stale-market cache served through a
  // representative-carry cache, silently mixing old-carry early-exercise
  // sensitivities into fresh-carry Black-76 legs. RATE-ONLY: the per-tenor q_eff
  // drift from the mid-expiry representative carry is a legitimate in-fit artifact
  // (american.cpp american_price_cached note — an assert on baked_q at 25 bps
  // aborted the suite), so it is deliberately not counted. In-fit de-Am queries at
  // the session rate == baked rate, so this stays 0 through a normal fit/serve.
  CacheCarryDrift,
  // Task P-6 (GK-P book memo, append-only). One bump per actual variance-strip
  // quadrature (`var_swap_fair_strike`'s templated body, the ONE place every
  // dispatch path -- price_var_swap, deriv_greeks' FD/analytic bump table, the
  // vol-swap/capped-swap best-effort diagnostics -- resolves K_var), counted
  // AFTER its own cheap validation guards (T>0, reserved fields, vol_of_vol,
  // wing_clamp) so a call rejected before ever touching the grid is not
  // counted as an evaluation. This is what `price_deriv_book`'s book-level
  // shared-strip memo measures itself against: a book of L VarSwap rows over
  // K distinct (uid,T) groups must bump this O(K), not O(L) -- PV, every
  // market greek, AND theta/theta_carry/theta_zero_fixing/charm's own T-dt
  // roll are all resolved from the SAME per-group shared block (see
  // `deriv_ref_bridge.hpp`'s `VarSwapSharedBlock`); only each row's own cheap
  // aged-blend/discount/strike-offset combine (no strip work) runs per row.
  VarSwapStripEvals,
  // Task F-2 (append-only, mirrors VarSwapStripEvals's own precedent above --
  // a SEPARATE counter, not a shared one, because P-6's book-memo O(K)-not-
  // O(L) gate reads VarSwapStripEvals specifically and a gamma-swap eval
  // folded into that same counter would silently corrupt what that gate
  // measures). One bump per actual gamma-weighted strip quadrature
  // (`strip_fair_value_core`'s shared body, `DerivKind::GammaSwap` branch),
  // counted after the same cheap validation guards VarSwapStripEvals is.
  // GammaSwap has no book-memo participation (`var_swap_memo_eligible`,
  // deriv_book.cpp, whitelists VarSwap only), so this is O(L) for a book of L
  // gamma-swap rows -- expected, and not itself a gate this task adds.
  GammaSwapStripEvals,
  // Task F-3 (append-only), for the SAME reason GammaSwapStripEvals is
  // separate: P-6's book-memo O(K)-not-O(L) gate reads VarSwapStripEvals
  // specifically, and a corridor-strip eval folded into it would corrupt what
  // that gate measures. One bump per actual corridor-strip quadrature
  // (`strip_fair_value_core`'s shared body, `DerivKind::CorridorVarSwap`
  // branch), counted after the same cheap validation guards the other two are.
  // CorridorVarSwap has no book-memo participation (`var_swap_memo_eligible`,
  // deriv_book.cpp, whitelists VarSwap only), so this is O(L) for a book of L
  // corridor rows -- expected, and pinned by
  // `DerivBook.CorridorVarSwapNeverUsesTheVarSwapMemo`.
  CorridorVarSwapStripEvals,
  Count_
};

inline constexpr unsigned kCount = static_cast<unsigned>(Solve::Count_);

// Stable machine-readable names (bench JSON keys). `sl_` distinguishes the always-on
// solve ledger from the gated `cnt_` exact counters.
inline constexpr const char *kNames[kCount] = {
    "sl_al_boundary_solves",    "sl_al_premium_evals",       "sl_greeks_fd",
    "sl_greeks_analytic",       "sl_greeks_adjoint",         "sl_iv_newton_iters",
    "sl_duplicate_mark_solves", "sl_cache_carry_drift",      "sl_var_swap_strip_evals",
    "sl_gamma_swap_strip_evals", "sl_corridor_var_swap_strip_evals",
};

static_assert(every_name_present(kNames),
              "ledger::kNames is short: a Solve enumerator was added without appending its "
              "name string, so kNames now holds a nullptr -- which reaches py::str "
              "(python/src/bindings/backtest.cpp:58) and printf(\"%s\") "
              "(tools/surface_db_build_main.cpp:588) unchecked. Append the name, in the "
              "enum's own order, and add it to SOLVE_LEDGER_KEYS "
              "(python/tests/test_run_sp100_strangle_backtest.py) too.");

// A merged, point-in-time copy. Plain values (not atomics) so it is trivially
// copyable, subtractable, and cheap to store per step.
struct Counts {
  std::uint64_t v[kCount] = {};

  [[nodiscard]] std::uint64_t get(Solve s) const noexcept { return v[static_cast<unsigned>(s)]; }

  // Saturating per-counter difference (after - before). Saturation guards a scrape
  // that straddles a reset(); a well-ordered delta never underflows.
  [[nodiscard]] Counts operator-(const Counts &before) const noexcept {
    Counts d;
    for (unsigned i = 0; i < kCount; ++i) {
      d.v[i] = v[i] >= before.v[i] ? v[i] - before.v[i] : 0u;
    }
    return d;
  }
};

namespace detail {

// One thread's tally. `v` is value-initialized to zero (C++20 atomic value-init is
// zero); only the owning thread writes it, so there is no contention.
struct Block {
  std::array<std::atomic<std::uint64_t>, kCount> v{};
  Block *next{nullptr};
};

inline std::mutex g_mutex;               // guards g_head + g_retired (register/scrape only)
inline Block *g_head{nullptr};           // intrusive list of live per-thread blocks
inline std::array<std::uint64_t, kCount> g_retired{}; // exited threads' folded tallies

// RAII owner of a thread's Block: links on construction, folds+unlinks on exit so
// counts survive a thread's death (thread-pool workers persist, but a mid-run join
// must not drop its tally).
struct Registrar {
  Block block;

  Registrar() noexcept {
    std::lock_guard<std::mutex> lk(g_mutex);
    block.next = g_head;
    g_head = &block;
  }

  ~Registrar() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (unsigned i = 0; i < kCount; ++i) {
      g_retired[i] += block.v[i].load(std::memory_order_relaxed);
    }
    Block **p = &g_head;
    while (*p != nullptr && *p != &block) {
      p = &(*p)->next;
    }
    if (*p == &block) {
      *p = block.next;
    }
  }

  Registrar(const Registrar &) = delete;
  Registrar &operator=(const Registrar &) = delete;
};

inline thread_local Registrar t_reg;

// Per-step trace sink armed on the calling (driver) thread. thread_local so a
// StepScope on the producer path is a single TLS-null check when unarmed.
inline thread_local std::vector<Counts> *t_step_sink{nullptr};

} // namespace detail

// Hot path: the calling thread bumps its OWN block. Relaxed load+store (a plain mov
// on x86) — no lock prefix, no shared cache line, no timing perturbation.
inline void bump(Solve s, std::uint64_t n = 1) noexcept {
  std::atomic<std::uint64_t> &cell = detail::t_reg.block.v[static_cast<unsigned>(s)];
  cell.store(cell.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// Merge retired + every live block into a plain-value Counts. Takes the registry
// lock; call at a step/board boundary, never inside the priced fan-out.
[[nodiscard]] inline Counts snapshot() noexcept {
  Counts c;
  std::lock_guard<std::mutex> lk(detail::g_mutex);
  for (unsigned i = 0; i < kCount; ++i) {
    c.v[i] = detail::g_retired[i];
  }
  for (detail::Block *b = detail::g_head; b != nullptr; b = b->next) {
    for (unsigned i = 0; i < kCount; ++i) {
      c.v[i] += b->v[i].load(std::memory_order_relaxed);
    }
  }
  return c;
}

// Test/measurement seam. Precondition: no concurrent producer (quiescent host).
inline void reset() noexcept {
  std::lock_guard<std::mutex> lk(detail::g_mutex);
  for (unsigned i = 0; i < kCount; ++i) {
    detail::g_retired[i] = 0u;
  }
  for (detail::Block *b = detail::g_head; b != nullptr; b = b->next) {
    for (unsigned i = 0; i < kCount; ++i) {
      b->v[i].store(0u, std::memory_order_relaxed);
    }
  }
}

// Test/bench seam: arm a per-step trace on the calling thread. While alive, a
// producer's StepScope records one Counts delta per step into steps(). RAII;
// nesting restores the prior sink. Off by default => zero producer cost.
class StepTrace {
public:
  StepTrace() noexcept : prev_(detail::t_step_sink) { detail::t_step_sink = &steps_; }
  ~StepTrace() noexcept { detail::t_step_sink = prev_; }
  StepTrace(const StepTrace &) = delete;
  StepTrace &operator=(const StepTrace &) = delete;

  [[nodiscard]] const std::vector<Counts> &steps() const noexcept { return steps_; }
  [[nodiscard]] std::size_t size() const noexcept { return steps_.size(); }

private:
  std::vector<Counts> steps_;
  std::vector<Counts> *prev_{nullptr};
};

// Producer-side per-step scope (the backtest loop wraps each step in one). When a
// StepTrace is armed on this thread it records (snapshot_after - snapshot_before)
// as one step delta; otherwise it is two TLS-null checks and nothing else.
class StepScope {
public:
  StepScope() noexcept {
    if (detail::t_step_sink != nullptr) {
      armed_ = true;
      before_ = snapshot();
    }
  }
  ~StepScope() noexcept {
    if (armed_ && detail::t_step_sink != nullptr) {
      detail::t_step_sink->push_back(snapshot() - before_);
    }
  }
  StepScope(const StepScope &) = delete;
  StepScope &operator=(const StepScope &) = delete;

private:
  Counts before_{};
  bool armed_{false};
};

} // namespace ledger

} // namespace atx::vol::counters

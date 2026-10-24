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
// (The runtime-armed per-chain fit timing plane, `fit_timing`, is at the end of
// this file; it is not behind this gate.)
//
// ## Usage
//
//   #include "fitting/counters.hpp"
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

// The CALLING thread's own tally of `s` (no lock, no merge): a relaxed load of the
// thread's block. For per-thread deltas around work that stays on this thread
// (fit_timing below); a fit that fans work out to other threads is not seen.
[[nodiscard]] inline std::uint64_t this_thread(Solve s) noexcept {
  return detail::t_reg.block.v[static_cast<unsigned>(s)].load(std::memory_order_relaxed);
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

// ── Per-chain fit timing (L1 fit profile, 2026-10-18) — opt-in, slot-local ────
//
// Attributes one expiry chain's quote-slice fit wall and work to its stages:
//   deam     — American IV inversions of the quotes (point admission mid IVs, channel
//              bid/ask/mid IVs) and the channel floor/cap prices;
//   solve    — the curve solve (point fit_slice_curve, channel native/family solve);
//   project  — the channel projection (project_in_channel);
//   price    — the certification grid reprice (two American prices per strike);
//   certify  — the AmericanPriceSlice certificate, its repairs and row re-implication;
//   calendar — calendar pair checks and calendar repairs;
//   other    — everything else charged to the chain (preparation, containment, ...).
// Buckets are EXCLUSIVE, not inclusive: a scope opened inside another charges its own
// time and the outer bucket resumes on exit, so the seven buckets partition the
// chain's wall. One cross-cut sits beside them: repair_rung_us, the INCLUSIVE wall of
// the surface fitter's repair rungs (RepairRungScope), which overlaps the buckets.
//
// Zero cost when off. This is the solve ledger's StepTrace/StepScope discipline, not
// the compile-time gate (the tool must stay the shipping build): nothing is armed
// by default, and a producer scope or count() is one thread_local pointer load and
// a branch — no clock read, no atomic, no allocation, no change to any value the
// fit computes. A surface fitter arms its per-expiry recorders only when its CALLER
// armed a SurfaceTrace on the calling thread (quote-bench --timing-detail).
//
// Threading: one ChainRecorder per expiry, a per-expiry slot owned by the surface
// fitter and written only by the thread fitting that expiry while a ChainScope for
// it is open on that thread (t_chain names the slot). There is no shared mutable
// accumulator, so a fan-out over expiries stays race-free. AL work is read from the
// calling thread's own solve-ledger block, so work a fit hands to OTHER threads is
// not counted (attribution runs use fit_workers = 1).
namespace fit_timing {

enum class Bucket : unsigned { Other = 0, Deam, Solve, Project, Price, Certify, Calendar, Count_ };

inline constexpr unsigned kBucketCount = static_cast<unsigned>(Bucket::Count_);
inline constexpr const char *kBucketNames[kBucketCount] = {
    "other", "deam", "solve", "project", "price", "certify", "calendar"};

static_assert(every_name_present(kBucketNames),
              "fit_timing::kBucketNames is short: append the new Bucket's name in enum order");

// Non-overlapping repaired-IV work, excluding batch preparation (reported separately).
enum class ReimplyStage : unsigned {
  Native = 0, Search, Legacy, Nonmonotone, Plateau, PriceCheck, PriceCandidate, PriceSeed, Count_
};
inline constexpr unsigned kReimplyStageCount = static_cast<unsigned>(ReimplyStage::Count_);
inline constexpr const char *kReimplyStageNames[kReimplyStageCount] = {
    "native", "search", "legacy", "nonmonotone", "plateau", "price_check", "price_candidate",
    "price_seed"};
static_assert(every_name_present(kReimplyStageNames));

// One chain's counters (the interface later lanes read by name).
struct FitTimingCounters {
  std::uint32_t al_price_calls{0};       // AL boundary solves on the fitting thread (every AL
                                         // price, including those inside American IV inversions)
  std::uint32_t al_greeks_calls{0};      // AL full-Greek bundles (any route) on that thread
  std::uint32_t certification_passes{0}; // certify_and_serve runs (each a full American reprice)
  std::uint32_t channel_solves{0};       // channel objective solves started (native or family)
  std::uint32_t jacobian_evals{0};       // channel solver iterations of the solves that returned
                                         // (native: Newton steps; family: refits of the family)
  std::uint32_t rung_attempts{0};        // per-expiry fits the surface fitter ran for the chain
  std::uint32_t calendar_pairs{0};       // check_american_calendar pairs charged to the chain
  std::uint32_t projections{0};          // project_in_channel calls
  std::uint32_t iv_newton_iters{0};      // American IV inversion iterations (solve ledger)
  // Repaired/query prices accelerated on the fit's retained native family. A fallback
  // runs the original typed Search route; root acceptance still requires the K3 bracket.
  std::uint32_t reimply_native_attempts{0};
  std::uint32_t reimply_native_roots{0};
  std::uint32_t reimply_native_fallbacks{0};
  std::uint32_t reimply_native_fast_confirms{0};
  double reimply_native_max_bound_iv{0.0};
  std::uint32_t reimply_cache_hits{0};
  std::uint32_t reimply_uncached{0};
  std::uint32_t reimply_no_context{0};
  std::uint32_t reimply_outside_q{0};
  std::uint32_t reimply_outside_window{0};
  std::uint32_t reimply_search_nonmonotone{0};
  std::uint32_t reimply_plateau_searches{0};
  std::uint32_t reimply_envelope_covered{0};
  std::uint32_t reimply_price_family_checks{0};
  std::uint32_t reimply_price_enclosure_hits{0};
  std::uint32_t reimply_price_cold_checks{0};
  std::uint32_t reimply_price_rejections{0};
  // Historical aggregate names: the candidate chain may now begin with Screen.
  std::uint32_t reimply_fast_attempts{0};
  std::uint32_t reimply_fast_roots{0};
  // Fast-only proposal outcomes, after any preceding Screen refusal.
  std::uint32_t reimply_proposal_stops{0};
  std::uint32_t reimply_proposal_roots{0};
  std::uint32_t reimply_proposal_retries{0};
  std::uint32_t reimply_screen_attempts{0};
  std::uint32_t reimply_screen_stops{0};
  std::uint32_t reimply_screen_roots{0};
  std::uint32_t reimply_seed_attempts{0};
  std::uint32_t reimply_seed_roots{0};
  // Explicit monetary-policy candidates: never counted as strict K3 roots.
  std::uint32_t reimply_economic_attempts{0};
  std::uint32_t reimply_economic_roots{0};
  std::uint64_t reimply_economic_ns{0};
  // Prepared full cold proofs after economic-envelope refusal; roots are prepared,
  // hits are consumed. These are separate from later ordinary seed fallback work.
  std::uint32_t reimply_cold_seed_attempts{0};
  std::uint32_t reimply_cold_seed_roots{0};
  std::uint32_t reimply_cold_seed_hits{0};
  std::uint32_t reimply_cold_seed_floor_preparations{0};
  std::uint32_t reimply_cold_seed_floor_hits{0};
  std::uint64_t reimply_cold_seed_ns{0};
  // RootAcceptanceKind / RootAcceptanceReason, exactly as deam_accept_by_* below.
  std::array<std::uint32_t, 5> reimply_accept_by_kind{};
  std::array<std::uint32_t, 9> reimply_accept_by_reason{};
  std::array<std::uint32_t, kReimplyStageCount> reimply_stage_calls{};
  std::array<std::uint64_t, kReimplyStageCount> reimply_stage_al{};
  std::array<std::uint64_t, kReimplyStageCount> reimply_stage_iv{};
  std::array<std::uint64_t, kReimplyStageCount> reimply_stage_ns{};
  // Repaired publication's prepared work and actual consumption. Sub-clocks cover
  // preparation only; native acceptance can still do scalar fallback/confirm work.
  std::uint32_t reimply_batch_calls{0};
  std::uint32_t reimply_batch_lanes{0};
  std::uint32_t reimply_batch_avx2_calls{0};
  std::uint32_t reimply_batch_hits{0};
  std::uint32_t reimply_batch_confirm_hits{0};
  std::uint32_t reimply_batch_family_prices{0}; // subset of prepared family prices
  std::uint32_t reimply_batch_family_retry_prices{0}; // subset spent by scalar retry
  std::uint32_t deam_batch_family_prices{0}; // initial native prepared family batch
  std::uint32_t deam_batch_family_retry_prices{0}; // initial scalar enclosure retries
  std::uint64_t reimply_batch_lane_ns{0};
  std::uint64_t reimply_batch_family_ns{0};
  std::uint64_t reimply_batch_confirm_ns{0};
  std::uint64_t wall_us{0};              // the sum of the seven exclusive buckets below
  std::uint64_t deam_us{0};
  std::uint64_t solve_us{0};
  std::uint64_t project_us{0};
  std::uint64_t price_us{0};
  std::uint64_t certify_us{0};
  std::uint64_t calendar_us{0};
  std::uint64_t other_us{0};
  std::array<std::uint32_t, kBucketCount> al_by_bucket{}; // al_price_calls split by Bucket
  // Cross-cut, NOT a bucket: the surface fitter's repair rungs (repair_rung /
  // ladder_repair_rung) INCLUSIVE of every bucket their refits charge; not in wall_us.
  std::uint32_t repair_rung_visits{0}; // repair-rung visits of the chain
  std::uint64_t repair_rung_us{0};     // their inclusive wall
  // De-Am IV cache inversions (lane L2e; VolChannel only, zero otherwise): count, the AL
  // boundary solves inside them (fallbacks included) and the most one inversion spent;
  // the Premium attempts, their fallbacks to Search by IvPremiumFallback reason
  // (index = enum value) and the largest last-solve price residual (USD) of an accepted
  // Premium root.
  std::uint32_t deam_inversions{0};
  std::uint32_t deam_inversion_al{0};
  std::uint32_t deam_inversion_al_max{0};
  std::uint32_t deam_premium{0};
  std::uint32_t deam_premium_fallbacks{0};
  std::array<std::uint32_t, 8> deam_premium_fallback_by_reason{};
  double deam_premium_max_residual_usd{0.0};
  // Opt-in audit (arm_deam_audit): each Premium inversion is ALSO run on Search and the
  // pair compared; |sigma_premium - sigma_search| histogram by decade (bin 0: exactly
  // 0; bin b in 1..16: [1e-(17-b), 1e-(16-b)), bin 1 open below, bin 16 open above).
  std::uint32_t deam_audit_pairs{0};
  std::uint32_t deam_audit_status_mismatch{0}; // one Ok and the other not
  double deam_audit_max_dsigma{0.0};
  std::array<std::uint32_t, 17> deam_audit_dsigma_decades{};
  // Pairs that differ: each answer's Newton distance |P_am(sigma) - target| / slope on
  // the accurate map (slope = the map's chord between the two answers; two extra
  // solves per differing pair), the largest of each, and how many pairs have Search
  // farther from the accurate root than Premium.
  double deam_audit_max_newton_premium{0.0};
  double deam_audit_max_newton_search{0.0};
  std::uint32_t deam_audit_search_farther{0};
  // Seed-tier wiring (SPY1s Phase 1, 2026-10-20; QuoteSliceDeamProfile::Screen): the
  // cache's AL boundary solves by reason (index = DeamReason; the brief's four plus the
  // band gate's chord solve and the Search route's solves, fallbacks included), the
  // confirms done and skipped, the ambiguous band edges re-priced accurately, the
  // inversions routed to accurate iterates by the seed tier's non-monotone gate, and the
  // largest gate bound (eps_A + eps_r) / v_min (vol) of a skipped root.
  std::array<std::uint32_t, 7> deam_al_by_reason{};
  std::uint32_t deam_confirms{0}; // confirming SOLVES (= deam_al_by_reason confirm), not roots
  std::uint32_t deam_confirms_skipped{0};
  std::uint32_t deam_ambiguous_edges{0};
  std::uint32_t deam_seed_routed{0};
  double deam_gate_max_bound_vol{0.0};
  // --deam-audit, skipped confirms only: pairs and the largest accurate-map Newton
  // distance of a root accepted without a confirm (the band gate's or, lane K3, a family
  // envelope enclosure's).
  std::uint32_t deam_audit_skipped_pairs{0};
  double deam_audit_max_newton_skipped{0.0};
  // Lane K3 (pricing/american_family_error.hpp): the Premium roots by typed acceptance kind
  // (index = RootAcceptanceKind; None = no family-route root and no skip) and reason (index
  // = RootAcceptanceReason), and, under --deam-audit, every served typed root re-solved by
  // bisection on the accurate map to 1e-9: roots audited, references not bracketed,
  // |served - reference| > kIvRootRequirement (violations), and the largest distance (vol)
  // of a Skipped and of a Confirmed root. Per root: ATX_VOL_FAMILY_ROOT_AUDIT.
  std::array<std::uint32_t, 5> deam_accept_by_kind{};
  std::array<std::uint32_t, 9> deam_accept_by_reason{};
  std::uint32_t deam_audit_bracket_roots{0};
  std::uint32_t deam_audit_bracket_unresolved{0};
  std::uint32_t deam_audit_bracket_violations{0};
  double deam_audit_bracket_max_skipped{0.0};
  double deam_audit_bracket_max_confirmed{0.0};
  // Sigma-family iterates (SPY1s Phase 2; QuoteSliceDeamFamily): families built and the
  // wall of their node solves (NOT in al_price_calls' reason split: built outside any
  // inversion), iterates priced on a family, Premium attempts a family refused.
  std::uint32_t deam_family_builds{0};
  std::uint64_t deam_family_build_us{0};
  std::uint32_t deam_family_prices{0};
  std::uint32_t deam_family_refused{0};
  // grid_kernel Family (step 5): certification-grid prices served by a (sigma, q) family,
  // and those priced on Fast instead (no family of the side, a refusal, a bound over the
  // node's allowance).
  std::uint32_t deam_family_grid_prices{0};
  std::uint32_t deam_family_grid_cold{0};
  // The native fit route (lane K4; deam_native_route, zero otherwise; review-K2 m8: no
  // native non-acceptance collapses into deam_family_refused). Per request: the family-native
  // lane's typed outcome (index = iv_batch::InverseStatus; Inactive = no lane ran) and the
  // route's typed outcome (index = IvRouteOutcome), the lanes started from a retained carry,
  // the bounded reference route's requests, kernel evaluations and budget refusals. Per band:
  // edges decided by validated bounds or priced. Per cache: family sides the adaptive rule
  // asked for, and the certification-grid prices served on a family under its per-point
  // bound (in deam_family_grid_prices) with the largest such bound (USD).
  std::array<std::uint32_t, 11> deam_native_status{};
  std::array<std::uint32_t, 11> deam_route_outcome{};
  std::uint32_t deam_native_carried{0};
  std::uint32_t deam_reference_requests{0};
  std::uint32_t deam_reference_evaluations{0};
  std::uint32_t deam_reference_budget_refused{0};
  std::uint32_t deam_edges_bound_decided{0};
  std::uint32_t deam_edges_priced{0};
  std::uint32_t deam_family_sides_requested{0};
  // K4b (review-K4 m8): requested family sides that built none, by DeAmFamilyRefusal
  // (deam_iv_cache.hpp); the sum is deam_family_sides_requested less the sides built.
  std::array<std::uint32_t, 7> deam_family_refused_by_reason{};
  double deam_family_grid_max_bound_usd{0.0};
  // K4b (r6): the inverse stage's sub-clocks of the chain (nanoseconds; zero when untimed):
  // the wall of every de-Am inversion request and, inside them, the family-native lane, the
  // K3 acceptance's family prices, the confirm solves, the seed-leg route less its confirms
  // and the bounded reference route (index = IvPremiumSubClock, american_iv_premium.hpp),
  // and the band-edge wall of the chain's bands (the de-Am stage clock's endpoints).
  std::uint64_t deam_sub_request_ns{0};
  std::array<std::uint64_t, 5> deam_sub_ns{};
  std::uint64_t deam_sub_endpoint_ns{0};
  // K4b (r6): the batched native route (deam_native_batch; zero otherwise). The wall of the
  // chain's prefill (its lane batches, enclosures and confirm batches are ALSO charged to the
  // matching deam_sub_ns parts; the requests that consume them are not); invert_many calls
  // and the lanes they inverted (on AVX2: calls that ran the kernel); requests that consumed
  // a precomputed lane and requests that ran their own one-lane call (no precomputed lane
  // with their exact inputs); first confirm prices evaluated in a batch and the requests that
  // consumed one; reference requests refused by the chain's wall budget (also counted in
  // deam_reference_budget_refused).
  std::uint64_t deam_sub_prefill_ns{0};
  std::uint32_t deam_batch_calls{0};
  std::uint32_t deam_batch_lanes{0};
  std::uint32_t deam_batch_avx2_calls{0};
  std::uint32_t deam_batch_lane_hits{0};
  std::uint32_t deam_batch_lane_misses{0};
  std::uint32_t deam_batch_confirms{0};
  std::uint32_t deam_batch_confirm_hits{0};
  std::uint32_t deam_reference_wall_refused{0};
  // K4b (r6): the certification grid's repricing sub-clocks of the chain (ns; zero when
  // untimed): nodes priced on a family under the per-point bound (and the bound itself), and
  // nodes priced on an Andersen-Lake kernel (Fast or the fit's own).
  std::uint64_t deam_sub_grid_family_ns{0};
  std::uint64_t deam_sub_grid_kernel_ns{0};
  // K4b (r6): native roots whose acceptance placed its confirm with the lane's fused vega
  // instead of a family chord (the lane-vega hook; no covering envelope, or a refused chord).
  std::uint32_t deam_lane_vegas{0};
};

// Reason of an AL boundary solve inside the de-Am cache (FitTimingCounters::
// deam_al_by_reason): the kIvMin / kIvMax band-edge prices, the inversion iterates (seed
// tier or accurate), the confirming solve, an ambiguous edge re-priced accurately, the
// band gate's chord solve, the Search route's solves (plain Search inversions and
// Premium fallbacks), and (K4, appended) the native fit route's bounded reference route.
enum class DeamReason : unsigned {
  RangeEdge = 0,
  Iterate,
  Confirm,
  AmbiguousEdge,
  Gate,
  Search,
  Reference,
  Count_
};
inline constexpr unsigned kDeamReasonCount = static_cast<unsigned>(DeamReason::Count_);
inline constexpr const char *kDeamReasonNames[kDeamReasonCount] = {
    "range_edge", "iterate", "confirm", "ambiguous_edge", "gate", "search", "reference"};
static_assert(decltype(FitTimingCounters::deam_al_by_reason){}.size() == kDeamReasonCount,
              "deam_al_by_reason is sized by DeamReason");

class ChainRecorder;
class SurfaceTrace;

namespace detail {
inline thread_local ChainRecorder *t_chain{nullptr};   // the chain this thread is fitting
inline thread_local SurfaceTrace *t_surface{nullptr};  // armed by a fit_quote_surface caller

[[nodiscard]] inline std::uint32_t saturate(std::uint64_t n) noexcept {
  return n > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                       : static_cast<std::uint32_t>(n);
}
} // namespace detail

// One expiry's recorder. Event counts accumulate in `counts`; wall and AL work are
// charged to the active bucket at every bucket or chain switch.
class ChainRecorder {
public:
  FitTimingCounters counts{};

  // Starts charging (the chain becomes current on this thread).
  void resume() noexcept {
    mark_ = Clock::now();
    al_mark_ = ledger::this_thread(ledger::Solve::AlBoundarySolves);
    iv_mark_ = ledger::this_thread(ledger::Solve::IvNewtonIters);
    greeks_mark_ = greeks_now();
  }

  // Charges the elapsed interval to the active bucket and stops charging.
  void pause() noexcept { charge(); }

  // Adds one repair-rung visit's inclusive wall (RepairRungScope; not a bucket).
  void charge_repair_rung(std::chrono::steady_clock::duration elapsed) noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    repair_ns_ += ns > 0 ? static_cast<std::uint64_t>(ns) : 0u;
  }

  // Charges the elapsed interval to the active bucket, then makes `b` active.
  [[nodiscard]] Bucket switch_to(Bucket b) noexcept {
    charge();
    const Bucket previous = active_;
    active_ = b;
    return previous;
  }

  // The counters with wall (microseconds, rounded per bucket) and AL work filled in.
  [[nodiscard]] FitTimingCounters result() const noexcept {
    FitTimingCounters out = counts;
    std::array<std::uint64_t, kBucketCount> us{};
    std::uint64_t al_total = 0;
    out.wall_us = 0;
    for (unsigned b = 0; b < kBucketCount; ++b) {
      us[b] = (ns_[b] + 500u) / 1000u;
      out.wall_us += us[b];
      out.al_by_bucket[b] = detail::saturate(al_[b]);
      al_total += al_[b];
    }
    out.other_us = us[static_cast<unsigned>(Bucket::Other)];
    out.deam_us = us[static_cast<unsigned>(Bucket::Deam)];
    out.solve_us = us[static_cast<unsigned>(Bucket::Solve)];
    out.project_us = us[static_cast<unsigned>(Bucket::Project)];
    out.price_us = us[static_cast<unsigned>(Bucket::Price)];
    out.certify_us = us[static_cast<unsigned>(Bucket::Certify)];
    out.calendar_us = us[static_cast<unsigned>(Bucket::Calendar)];
    out.al_price_calls = detail::saturate(al_total);
    out.iv_newton_iters = detail::saturate(iv_iters_);
    out.al_greeks_calls = detail::saturate(greeks_);
    out.repair_rung_us = (repair_ns_ + 500u) / 1000u;
    return out;
  }

private:
  using Clock = std::chrono::steady_clock;

  [[nodiscard]] static std::uint64_t greeks_now() noexcept {
    return ledger::this_thread(ledger::Solve::GreeksBundlesFd) +
           ledger::this_thread(ledger::Solve::GreeksBundlesAnalytic) +
           ledger::this_thread(ledger::Solve::GreeksBundlesAdjoint);
  }

  void charge() noexcept {
    const auto now = Clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark_).count();
    ns_[static_cast<unsigned>(active_)] += elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0u;
    mark_ = now;
    const std::uint64_t al = ledger::this_thread(ledger::Solve::AlBoundarySolves);
    const std::uint64_t iv = ledger::this_thread(ledger::Solve::IvNewtonIters);
    const std::uint64_t greeks = greeks_now();
    // Saturating deltas: a ledger::reset() (test seam) between two marks reads as 0.
    const auto delta = [](std::uint64_t after, std::uint64_t before) noexcept {
      return after >= before ? after - before : std::uint64_t{0};
    };
    al_[static_cast<unsigned>(active_)] += delta(al, al_mark_);
    iv_iters_ += delta(iv, iv_mark_);
    greeks_ += delta(greeks, greeks_mark_);
    al_mark_ = al;
    iv_mark_ = iv;
    greeks_mark_ = greeks;
  }

  std::array<std::uint64_t, kBucketCount> ns_{};
  std::array<std::uint64_t, kBucketCount> al_{};
  std::uint64_t iv_iters_{0};
  std::uint64_t greeks_{0};
  std::uint64_t repair_ns_{0};
  Bucket active_{Bucket::Other};
  Clock::time_point mark_{};
  std::uint64_t al_mark_{0};
  std::uint64_t iv_mark_{0};
  std::uint64_t greeks_mark_{0};
};

// Makes `rec` the chain this thread is fitting for the scope's lifetime and charges
// the scope's wall to it. Null `rec` (timing off) is one branch. Re-entering the
// chain that is already current only counts (the outer scope keeps timing it); a
// different current chain is paused and resumed around this one.
class ChainScope {
public:
  explicit ChainScope(ChainRecorder *rec, std::uint32_t rungs = 0) noexcept : slot_(rec) {
    if (rec == nullptr) return;
    rec->counts.rung_attempts += rungs;
    prev_ = detail::t_chain;
    if (prev_ == rec) return;
    if (prev_ != nullptr) prev_->pause();
    detail::t_chain = rec;
    rec->resume();
    owner_ = true;
  }

  ~ChainScope() {
    if (!owner_) return;
    slot_->pause();
    detail::t_chain = prev_;
    if (prev_ != nullptr) prev_->resume();
  }

  ChainScope(const ChainScope &) = delete;
  ChainScope &operator=(const ChainScope &) = delete;
  ChainScope(ChainScope &&) = delete;
  ChainScope &operator=(ChainScope &&) = delete;

  // One more per-expiry fit attempt charged to the chain.
  void rung() noexcept {
    if (slot_ != nullptr) ++slot_->counts.rung_attempts;
  }

private:
  ChainRecorder *slot_;
  ChainRecorder *prev_{nullptr};
  bool owner_{false};
};

// Charges its lifetime to bucket `b` of the chain current on this thread (if any).
class BucketScope {
public:
  explicit BucketScope(Bucket b) noexcept : rec_(detail::t_chain) {
    if (rec_ != nullptr) prev_ = rec_->switch_to(b);
  }

  ~BucketScope() {
    if (rec_ != nullptr) static_cast<void>(rec_->switch_to(prev_));
  }

  BucketScope(const BucketScope &) = delete;
  BucketScope &operator=(const BucketScope &) = delete;
  BucketScope(BucketScope &&) = delete;
  BucketScope &operator=(BucketScope &&) = delete;

private:
  ChainRecorder *rec_;
  Bucket prev_{Bucket::Other};
};

// Cross-cut timer, not a bucket: one repair-rung visit of the chain current on this
// thread, INCLUSIVE of the buckets its refits charge (so repair_rung_us overlaps them
// and is not part of wall_us). Open it inside the visit's ChainScope. No chain current
// (timing off): one branch, no clock read.
class RepairRungScope {
public:
  RepairRungScope() noexcept : rec_(detail::t_chain) {
    if (rec_ == nullptr) return;
    ++rec_->counts.repair_rung_visits;
    start_ = std::chrono::steady_clock::now();
  }

  ~RepairRungScope() {
    if (rec_ != nullptr) rec_->charge_repair_rung(std::chrono::steady_clock::now() - start_);
  }

  RepairRungScope(const RepairRungScope &) = delete;
  RepairRungScope &operator=(const RepairRungScope &) = delete;
  RepairRungScope(RepairRungScope &&) = delete;
  RepairRungScope &operator=(RepairRungScope &&) = delete;

private:
  ChainRecorder *rec_;
  std::chrono::steady_clock::time_point start_{};
};

// Adds `n` to one event count of the chain current on this thread (if any).
inline void count(std::uint32_t FitTimingCounters::*field, std::uint32_t n = 1) noexcept {
  if (ChainRecorder *rec = detail::t_chain) rec->counts.*field += n;
}

// The counters of the chain current on this thread, or null (timing off).
[[nodiscard]] inline FitTimingCounters *current_counts() noexcept {
  ChainRecorder *rec = detail::t_chain;
  return rec != nullptr ? &rec->counts : nullptr;
}

// Process-wide opt-in for the de-Am Premium audit (quote-bench --deam-audit): set once
// before any fit starts and only read by the fitting threads (relaxed: a plain flag,
// no data is published through it).
namespace detail {
inline std::atomic<bool> g_deam_audit{false};
} // namespace detail
inline void arm_deam_audit(bool on) noexcept {
  detail::g_deam_audit.store(on, std::memory_order_relaxed);
}
[[nodiscard]] inline bool deam_audit_armed() noexcept {
  return detail::g_deam_audit.load(std::memory_order_relaxed);
}

// Armed by a caller on its own thread around fit_quote_surface: the fitter records
// one FitTimingCounters per expiry (input order) into `chains` when it returns Ok.
// RAII; nesting restores the prior trace.
class SurfaceTrace {
public:
  SurfaceTrace() noexcept : prev_(detail::t_surface) { detail::t_surface = this; }
  ~SurfaceTrace() { detail::t_surface = prev_; }

  SurfaceTrace(const SurfaceTrace &) = delete;
  SurfaceTrace &operator=(const SurfaceTrace &) = delete;
  SurfaceTrace(SurfaceTrace &&) = delete;
  SurfaceTrace &operator=(SurfaceTrace &&) = delete;

  std::vector<FitTimingCounters> chains;

private:
  SurfaceTrace *prev_;
};

// The trace armed on the calling thread, or null (timing off).
[[nodiscard]] inline SurfaceTrace *armed_surface() noexcept { return detail::t_surface; }

} // namespace fit_timing

} // namespace atx::vol::counters

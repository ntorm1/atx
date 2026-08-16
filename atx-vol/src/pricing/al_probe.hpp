#pragma once

// ── Env-gated Andersen-Lake hot-path probe (Perf Phase 2b, step-1) ───────────
//
// WHY THIS EXISTS. The always-on solve ledger (counters.hpp, `namespace ledger`)
// counts EVENTS: `sl_al_boundary_solves`, `sl_al_premium_evals`,
// `sl_iv_newton_iters`. Counts alone were read in Phase 1 §8.1 as
// "~34 premium evaluations per boundary solve, therefore the premium quadrature
// is the cost" — but the two counters are not commensurate: a premium eval is ONE
// quadrature node (1 log + 2 norm_cdf), whereas a boundary solve is a whole
// Barone-Adesi-Whaley seed plus n_boundary × n_quad_fp × n_sweeps kernel node
// evaluations (american.cpp's own comment calls that loop "the dominant cold
// cost"). Comparing 34 : 1 across those units cannot decide where the CPU goes.
//
// So this plane measures TIME, not events, on the shipping Release binary, and it
// additionally records the normalized state each cold boundary solve is queried at
// so the reuse/surrogate question can be answered from data.
//
// DISCIPLINE (mirrors the solve ledger's):
//   * OFF unless `ATX_VOL_AL_PROBE` is set — one `extern const bool` load and a
//     not-taken branch per instrumented scope when off. Never allocates, never
//     locks, and touches no shared cache line on the hot path when off.
//   * ON: each thread accumulates into its OWN block (plain adds, no atomics on
//     the hot path beyond the relaxed store the concurrent scrape needs). Blocks
//     self-register and fold into a retired tally at thread exit, exactly like the
//     ledger, so a fit worker that dies never loses its tally.
//   * `ATX_VOL_AL_PROBE` is a mode string: 'z' (or any value) enables ZONE timing;
//     's' additionally enables the per-solve normalized-state trace, which is
//     written to `ATX_VOL_AL_PROBE_OUT` at dump time. The two modes are separable
//     because the state trace's vector growth would perturb the very zone it sits
//     inside — never measure both at once.
//
// Cycle source is `__rdtsc()`: ~25 cycles, invariant-TSC on every host this ships
// to, and it is a *ratio* between zones that is being measured, so TSC-vs-core-clock
// drift on a P/E-core hybrid does not affect the conclusion. `zone_overhead_cycles()`
// reports the measured cost of an empty scope so a caller can net it out.
//
// Library-private: src/-only, NOT installed, no stability guarantee. This is a
// measurement facility, not an API.

#include <cstdint>
#include <cstdio>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

namespace atx::vol::alprobe {

// Timed zones. Nesting is INCLUSIVE and deliberate: BoardFit contains everything,
// AlPriceFromBoundary contains AlPremiumQuad, SigmaInterpBuild contains
// AlBoundarySolveCold. Read shares against BoardFit, and read children against
// their parent — that is what separates "the premium quadrature" from "the
// boundary solve" from "everything that is not Andersen-Lake at all".
enum class Zone : unsigned {
  BoardFit = 0,          // corpus_board_fit.cpp fit_board — the denominator
  AlBoundarySolveCold,   // amer::al_solve_put_boundary (BAW seed + JN/FP sweeps)
  AlBoundarySolveWarm,   // amer::al_solve_put_boundary_warm
  AlPremiumQuad,         // amer::al_put_premium — the de-Am premium quadrature
  AlPriceFromBoundary,   // al_put_price_from_boundary[_cached] (euro + premium + clamp)
  AmericanIv,            // american_implied_vol_impl — one scalar inversion
  SigmaInterpBuild,      // SigmaBoundaryInterp::build — the n_sigma shared solves
  SliceSigma,            // slice_sigma_impl — a whole shared-boundary slice reprice
  AloPricerPrice,        // AloPricer::price(sigma) — the retained per-residual AL price
                         // the IV inversion actually runs (its own inline seed+sweep
                         // loop, NOT al_solve_put_boundary — which is why the two
                         // boundary-solve counts disagree)
  // Entry-path-independent boundary-solve primitives: every AL solve, from any of
  // the four entry paths, is exactly one AlSeedBaw plus n_iter_jn AlSweepJn plus
  // n_iter_fp AlSweepFp plus the geometry binds. Summing these attributes the
  // boundary cost without having to enumerate callers.
  AlSeedBaw,             // al_seed_boundary — the Barone-Adesi-Whaley cold seed
  AlSweepJn,             // al_jacobi_newton_sweep — one Jacobi-Newton sweep
  AlSweepFp,             // al_fixed_point_sweep — one fixed-point sweep
  AlBindGeoStatic,       // al_bind_geometry_static — sigma-invariant geometry
  AlBindGeoSigma,        // al_bind_geometry_sigma — the sigma-dependent rebind
  // Same three primitives, split off for n_boundary != 7 — i.e. the ACCURATE
  // {12,24,48} scheme the de-Am AUDIT / certification plane runs through
  // `std::nullopt` opts. A {12,24} sweep is 11x24 = 264 kernel node evaluations
  // against a {7,16} sweep's 6x16 = 96, so the audit plane's share of the cost is
  // several times its share of the solve COUNT, and a fast-tier scheme change does
  // not touch it. Separating them is what makes the achievable speedup computable.
  AlSeedBawAcc,
  AlSweepJnAcc,
  AlSweepFpAcc,
  // The COLD reference polish at the tail of american_implied_vol_impl: up to two
  // full `american_price` re-solves per inversion, run to lock the returned iv to
  // andersen_lake(iv) == price. It nests inside AmericanIv but NOT inside
  // AloPricerPrice, so no existing zone separates it from the bracket-and-Newton
  // search. They are the two halves of an inversion's cost and the optimizations
  // available to each are completely different, so they need separate meters.
  IvColdPolish,
  Count_
};

inline constexpr unsigned kZoneCount = static_cast<unsigned>(Zone::Count_);
extern const char *const kZoneNames[kZoneCount];

// Named events on the shared-boundary de-Am engagement path. These answer "does
// the existing σ-interpolant lane actually engage on production boards", which no
// existing counter reports outside a per-board diagnostics struct.
enum class Event : unsigned {
  SharedSideConsidered = 0,  // prepare_shared_boundary_side entered
  SharedSideGuardSkip,       // bailed on a width/T/rate/σ-box guard (no interpolant built)
  SharedSideBuildFail,       // interp.build() refused (non-American corner / node solve failed)
  SharedSideCertified,       // the side's lanes were certified and kept
  SharedSideRejected,        // built + solved, then invalidated (whole side back to scalar)
  SharedRowsLaned,           // rows served by the interpolant
  SharedRowsFallback,        // rows that fell back to the per-row scalar inverter
  // Guard-skip REASONS. SharedSideGuardSkip alone cannot be acted on: the four
  // conditions it fuses have opposite remedies. A narrow side is a threshold
  // question (does 9 boundary solves amortise over n rows?); a non-positive
  // internal rate is a REGIME question (McDonald-Schroder maps the call side's
  // rate to q, and classify_regime calls rate <= 0 European — those rows never
  // solve a boundary at all, so "engaging" them would replace a Black-76
  // evaluation with nine AL solves). The `*Rows` companions carry the row counts,
  // which is what decides whether a reason is worth any work.
  SharedSideSkipNarrow,      // side_rows < kSharedMinSideRows
  SharedSideSkipShortT,      // T < kSharedMinT
  SharedSideSkipRate,        // internal_rate <= 0 — the non-American (European) regime
  SharedSideSkipBox,         // degenerate / too-wide sigma box
  SharedRowsSkipNarrow,
  SharedRowsSkipShortT,
  SharedRowsSkipRate,
  SharedRowsSkipBox,
  // Three-tier exercise-ladder routing (perf/exercise-ladder). One bump per
  // de-Am row, in the tier the router assigned it, plus the reasons a row was
  // refused the cheap tiers. `LadderRefusedDiv` is the dividend-proximity
  // pocket and is counted separately from the ordinary budget refusal because
  // the two have different remedies: a budget refusal is a threshold question,
  // a dividend refusal is a data question (this seam sees no ex-div calendar).
  LadderTier0,
  LadderTier1,
  LadderTier2,
  LadderRefusedGuard,  // vega / |k| / T / anchor / method precondition
  LadderRefusedDiv,    // call-side dividend-proximity pocket
  LadderRefusedBudget, // estimated premium over the Tier-1 budget
  LadderEscalatedByMargin, // cleared a budget outright but not by the margin
  Count_
};

inline constexpr unsigned kEventCount = static_cast<unsigned>(Event::Count_);
extern const char *const kEventNames[kEventCount];

namespace detail {
// Dynamically initialized from the environment at TU init; every hot-path read is
// a plain load of a const bool. `g_states` implies `g_on`.
extern const bool g_on;
extern const bool g_states;
} // namespace detail

[[nodiscard]] inline bool enabled() noexcept { return detail::g_on; }
[[nodiscard]] inline bool states_enabled() noexcept { return detail::g_states; }

// Hot path: the calling thread adds to its own block.
void add(Zone z, std::uint64_t cycles) noexcept;
void bump(Event e, std::uint64_t n = 1) noexcept;

// One cold boundary solve's query state, recorded only in state mode. `nb` is the
// scheme's n_boundary, which separates the {7,16} populate-tier solves from the
// {12,24} ACCURATE certification/oracle solves in the same run.
void record_boundary_state(double T, double sigma, double r, double q, double K,
                           unsigned nb) noexcept;

// Measured cost of an empty Scope on this host, so a reader can net the
// instrumentation out of a zone with a very high call count.
[[nodiscard]] std::uint64_t zone_overhead_cycles() noexcept;

// Merge every live + retired block and print the zone table, the event table and
// the state-trace summary to `out`. In state mode also writes the raw trace to
// `ATX_VOL_AL_PROBE_OUT` (little-endian f64 records: T, sigma, r, q, K, nb).
// No-op when the probe is off.
void dump(std::FILE *out) noexcept;

// RAII zone timer. Off: one const-bool load + a not-taken branch on entry and exit.
class Scope {
public:
  explicit Scope(Zone z) noexcept : z_(z) {
    if (detail::g_on) {
      t0_ = __rdtsc();
    }
  }
  ~Scope() noexcept {
    if (detail::g_on) {
      add(z_, __rdtsc() - t0_);
    }
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  Scope(Scope &&) = delete;
  Scope &operator=(Scope &&) = delete;

private:
  Zone z_;
  std::uint64_t t0_{0};
};

} // namespace atx::vol::alprobe

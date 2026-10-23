#pragma once

// ── Andersen-Lake exercise region: one classification, opt-in exact plateau ──────
//
// L3k monotone (2026-10-21) + C1 exercise-region semantics (2026-10-22).
//
// Tier-B (outside the frozen vol.hpp umbrella): opt-in Andersen-Lake entry points. They
// live in their own header rather than in american.hpp so that the frozen Tier-A header —
// and, through it, most of the library — is untouched by this opt-in addition.
//
// Inside its own exercise region — put: S <= B(T), the boundary the price solves at
// tau = T; call: the McDonald-Schroder internal put (spot K, strike S, rate q, yield r) in
// ITS region, i.e. S >= K^2 / B_put(T; strike K, rate q, yield r) by strike homogeneity,
// the test exercise_boundary() documents — the kernel prints euro + premium floored at
// intrinsic. There the premium quadrature lands above intrinsic by an amount that is not
// monotone in sigma, so P(sigma) dips on long-dated deep-ITM contracts and an inversion can
// find more than one root (LEDGER 2026-10-19 trap; sprints/2026-10-18-L1c-al-nonmonotone-
// sprint.md section 2). The exact American value in the region is the intrinsic.
//
// MEASURED IN-REGION ERROR of the kernel floor (KernelFloor - exact intrinsic, over
// K * max(1, T)^2), reference-backed on the AlMonotone grid (C1, 2026-10-22): accurate and
// Fast profile 5.0e-7, al_fast_opts 9.6e-7, al_bulk_opts 3.64e-6; the reference is the
// converged Andersen-Lake scheme (32 nodes, quadrature 64/64, 16 sweeps, tol 1e-14) and a
// Broadie-Detemple binomial lattice, both of which put every such cell >= 5% inside the
// exercise region at exactly the intrinsic. Under Intrinsic that error is zero.
//
// SUPPORTED DOMAIN (what classify_exercise_region returns, in the order it decides):
//   ExpiryLimit       T <= kAlExpiryLimitT: the payoff (intrinsic) — exact in every regime.
//   ZeroVolLimit      sigma <= kAlZeroVolSigma: the andersen_lake closed form
//                     max(df * forward intrinsic, spot intrinsic). KNOWN GAP (documented,
//                     not changed here): the exact sigma = 0 value is the supremum over
//                     exercise dates, whose interior stationary point the closed form
//                     misses (put q > r > 0 with S > K r / q, and the mirrored call; LEDGER
//                     2026-08-23 "zero-vol American bound"). No fit iterates there
//                     (kIvMin = 0.005).
//   European          the carry regime where early exercise is never optimal (put
//                     r <= 0 && q >= r; call q <= 0 && r >= q): Black-76.
//   UnsupportedRegime the double-continuation regime (put q < r < 0 / call r < q < 0):
//                     the option has TWO exercise boundaries and the single-boundary
//                     Andersen-Lake scheme cannot represent them. The module has no
//                     two-boundary route, so no price is served: andersen_lake returns
//                     NotImplemented and andersen_lake_classified a typed
//                     UnsupportedRegime with a NaN price.
//   ExercisePlateau   American regime, spot on the exercise side of B(T): the exact
//                     value is the intrinsic and dP/dsigma == 0.
//   Continuation      American regime, spot strictly on the continuation side.

#include <cstdint>
#include <limits>
#include <optional>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp" // AlOpts, AlExpiryGeometry, andersen_lake

namespace atx::vol {

// The price rule inside the exercise region.
enum class AlExerciseRegion : std::uint8_t {
  KernelFloor, // euro + premium floored at intrinsic, euro and 0 — every other entry point
  Intrinsic,   // intrinsic, exactly, when the spot is on the exercise side of B(T)
};

// The andersen_lake degenerate thresholds, named. The dispatch in american.cpp and
// classify_exercise_region decide with exactly these values.
inline constexpr double kAlExpiryLimitT = 1.0e-12;
inline constexpr double kAlZeroVolSigma = 1.0e-8;

// Where a contract sits for the Andersen-Lake routes (supported domain above).
enum class AlExerciseClass : std::uint8_t {
  Continuation,
  ExercisePlateau,
  European,
  ExpiryLimit,
  ZeroVolLimit,
  UnsupportedRegime,
};

[[nodiscard]] const char *to_string(AlExerciseClass value) noexcept;

// THE exercise classification. Every Andersen-Lake price site — the scalar uncached
// site, the scalar cached site and the laned AVX2 kernel — calls this one function with
// the boundary value it prices against, so the sites cannot disagree on a contract they
// see with the same boundary bits (AmericanExerciseRegion.ThreeSitesAgree... pins it).
//
// @param S,K,T,r,q,sigma  the contract (validated as andersen_lake validates them)
// @param is_call          side; a call is classified through its McDonald-Schroder
//                         internal put (spot K, strike S, rate q, yield r)
// @param boundary         B(tau = T) of the INTERNAL PUT the kernel solved — for a put
//                         the boundary of strike K, for a call the boundary of strike S
//                         at rate q / yield r — in that put's spot units. Read only in the
//                         American regime; NaN there classifies Continuation (a NaN never
//                         compares inside).
// The put-coordinate test is `spot <= boundary` (spot = S for a put, K for a call):
// a pure comparison, no arithmetic, so it is exact on every ISA.
[[nodiscard]] AlExerciseClass classify_exercise_region(double S, double K, double T, double r,
                                                       double q, double sigma, bool is_call,
                                                       double boundary) noexcept;

// andersen_lake with an explicit exercise-region rule. KernelFloor is BIT-IDENTICAL to the
// 8-argument / geometry entry points of american.hpp (same validation, errors, closed forms
// and regimes). Intrinsic solves the same boundary and, when the spot is in its exercise
// region, returns the intrinsic (one subtraction: no euro leg, no premium quadrature);
// everywhere else it is bit-identical to KernelFloor. Under Intrinsic P(sigma) is flat at
// intrinsic up to the exercise edge and P_Intrinsic <= P_KernelFloor everywhere, so the
// in-region roots of an inversion disappear. Tests: AlMonotone.*
// (tests/american_monotone_test.cpp), AmericanExerciseRegion.* (tests/american_region_test.cpp);
// design note sprints/2026-10-22-C1-exercise-region-sprint.md.
[[nodiscard]] Result<double> andersen_lake(double S, double K, double T, double sigma, double r,
                                           double q, Side side,
                                           const std::optional<AlOpts> &opts,
                                           AlExerciseRegion region);

// The geometry overload under an explicit rule: bit-identical to the fresh overload above at
// (g.T(), g.r(), g.q(), opts-of-g). InvalidArgument when `g` is not prepared.
[[nodiscard]] Result<double> andersen_lake(const AlExpiryGeometry &g, double S, double K,
                                           double sigma, Side side, AlExerciseRegion region);

// A price together with the classification the route used — the hook the family and
// inversion routes consume (C1 deliverable 6).
struct AlRegionPrice {
  // The price; quiet NaN exactly when `cls` is UnsupportedRegime (nothing is served).
  double price = std::numeric_limits<double>::quiet_NaN();
  // B(tau = T) of the internal put the kernel solved (put: strike K; call: strike S, rate
  // q, yield r), in that put's spot units; NaN when no boundary was solved (every class
  // but Continuation / ExercisePlateau).
  double boundary = std::numeric_limits<double>::quiet_NaN();
  AlExerciseClass cls = AlExerciseClass::UnsupportedRegime;

  [[nodiscard]] constexpr bool priced() const noexcept {
    return cls != AlExerciseClass::UnsupportedRegime;
  }
};

// andersen_lake(S, K, T, sigma, r, q, side, opts, rule) plus its classification.
// `price` is BIT-IDENTICAL to that overload whenever it returns a value; `cls` equals
// classify_exercise_region(S, K, T, r, q, sigma, side == Call, boundary).
// @return the AlRegionPrice, or the same Error andersen_lake returns — except in the
//         double-continuation regime (UnsupportedRegime above), where andersen_lake's
//         NotImplemented becomes a typed value { NaN, NaN, UnsupportedRegime } so a
//         caller can route it without parsing an error message. InvalidArgument,
//         Internal and the boundary-solve NotImplemented errors pass through unchanged.
[[nodiscard]] Result<AlRegionPrice>
andersen_lake_classified(double S, double K, double T, double sigma, double r, double q,
                         Side side, const std::optional<AlOpts> &opts, AlExerciseRegion rule);

// The geometry overload: bit-identical to andersen_lake(g, S, K, sigma, side, rule) in
// price, classified as above. InvalidArgument when `g` is not prepared.
[[nodiscard]] Result<AlRegionPrice> andersen_lake_classified(const AlExpiryGeometry &g,
                                                             double S, double K, double sigma,
                                                             Side side, AlExerciseRegion rule);

} // namespace atx::vol

#pragma once

// ── L3k monotone (2026-10-21): exact exercise-region evaluation, opt-in ──────
//
// Tier-B (outside the frozen vol.hpp umbrella): an opt-in Andersen-Lake entry point. It
// lives in its own header rather than in american.hpp so that the frozen Tier-A header —
// and, through it, most of the library — is untouched by this opt-in addition.
//
// Inside its own exercise region — put: S <= B(T), the boundary the price solves at
// tau = T; call: the McDonald-Schroder internal put (spot K, strike S, rate q, yield r) in
// ITS region, i.e. S >= K^2 / B_put(T; strike K, rate q, yield r) by strike homogeneity,
// the test exercise_boundary() documents — the kernel prints euro + premium floored at
// intrinsic. There the premium quadrature lands above intrinsic by an amount that is not
// monotone in sigma (up to ~1e-6 * K * max(1, T)^2 on the accurate preset), so P(sigma)
// dips on long-dated deep-ITM contracts and an inversion can find more than one root
// (LEDGER 2026-10-19 trap; sprints/2026-10-18-L1c-al-nonmonotone-sprint.md section 2). The
// exact American value in the region is the intrinsic.

#include <cstdint>
#include <optional>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp" // AlOpts, AlExpiryGeometry, andersen_lake

namespace atx::vol {

// The price rule inside the exercise region.
enum class AlExerciseRegion : std::uint8_t {
  KernelFloor, // euro + premium floored at intrinsic, euro and 0 — every other entry point
  Intrinsic,   // intrinsic, exactly, when the spot is on the exercise side of B(T)
};

// andersen_lake with an explicit exercise-region rule. KernelFloor is BIT-IDENTICAL to the
// 8-argument / geometry entry points of american.hpp (same validation, errors, closed forms
// and regimes). Intrinsic solves the same boundary and, when the spot is in its exercise
// region, returns the intrinsic (one subtraction: no euro leg, no premium quadrature);
// everywhere else it is bit-identical to KernelFloor. Under Intrinsic P(sigma) is flat at
// intrinsic up to the exercise edge and P_Intrinsic <= P_KernelFloor everywhere, so the
// in-region roots of an inversion disappear. Tests: AlMonotone.*
// (tests/american_monotone_test.cpp); measured bound and map: sprints/2026-10-20-L3k-
// monotone-sprint.md.
[[nodiscard]] Result<double> andersen_lake(double S, double K, double T, double sigma, double r,
                                           double q, Side side,
                                           const std::optional<AlOpts> &opts,
                                           AlExerciseRegion region);

// The geometry overload under an explicit rule: bit-identical to the fresh overload above at
// (g.T(), g.r(), g.q(), opts-of-g). InvalidArgument when `g` is not prepared.
[[nodiscard]] Result<double> andersen_lake(const AlExpiryGeometry &g, double S, double K,
                                           double sigma, Side side, AlExerciseRegion region);

} // namespace atx::vol

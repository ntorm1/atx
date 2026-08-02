#pragma once

// Strict convex-dense recovery: the producer-side bridge between the admission
// oracle's sampling contract (RiskSurfaceValidationConfig) and the ConvexDense
// calendar-repair override (ConvexRepairSpec). Root cause (2026-08 SPY
// backfill, 181/1890 cells): the repair loop's fixed lattice and 1e-7
// acceptance are strictly looser than the oracle's grid and 1e-8 tolerance, so
// a fit could pass repair yet die at admission by a sub-vol-tick margin at a k
// repair never sampled. These helpers are pure so that contract is
// unit-testable without a fitter.

#include <vector>

#include "atx/vol/detail/risk_surface_validation.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol::detail {

// Recovery applies only when the candidate died on geometry the strict refit
// can actually cure: at least one of Butterfly/Calendar set and nothing else
// outside {Butterfly, Calendar, CarryGap}. CarryGap alone publishes Degraded
// without help; InvalidDomain / InsufficientData / TimedOut / Wing and the
// rest need different medicine, and a refit under those masks would only burn
// the rejection budget.
[[nodiscard]] bool should_attempt_strict_recovery(ValidationFailure failures) noexcept;

// The oracle's calendar band/grid verbatim, acceptance pinned to 0.1x the
// oracle's tolerance so post-repair QP roundoff cannot straddle the gate.
[[nodiscard]] ConvexRepairSpec make_strict_repair_spec(const RiskSurfaceValidationConfig &config);

// Exact-QP-node promotions for one recovery round, from the digest's reported
// first violations: the calendar k verbatim (repair's own grid scan handles
// the rest of the calendar band); the butterfly k plus its two straddling
// uniform strike-grid neighbors on each side — the finite-difference stencil
// the oracle measured the kink on, which is what forces the QP to be convex
// across the intrinsic-bound seam. Sorted, deduplicated, non-finite dropped.
[[nodiscard]] std::vector<double> strict_promotion_ks(const ValidationDigest &digest,
                                                      const RiskSurfaceValidationConfig &config);

} // namespace atx::vol::detail

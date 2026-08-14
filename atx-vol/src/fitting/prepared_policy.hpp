#pragma once

// Leaf definition of the family-neutral fitting-preparation policy selector.
//
// This enum lives in its own tiny header so that structs which must NAME an
// enumerator in a default member initializer — `SurfaceParityInputs`
// (surface_parity.hpp), `SessionInputs` (session.hpp), and `PricerConfig`
// (pricer_fitter.hpp) — can depend on the COMPLETE enum definition without
// including prepared_fitting.hpp. prepared_fitting.hpp forward-declares
// `SurfaceParityInputs`, so pulling it into surface_parity.hpp would form an
// include cycle; a default member initializer naming `::Configured` needs the
// full definition (a forward-declared scoped enum is not enough). Both
// prepared_fitting.hpp and the three consumer headers include THIS header, so
// there is a single source of truth for the enum's underlying type and values.

#include <cstdint>

namespace atx::vol {

// Preparation policy for a fitted slice's observation set. `Configured` applies
// `CalibOpts` through the shared calibration builder (build_observations_european),
// which is always cold-reference audited internally. `LegacyEssviCompatibility`
// reproduces the historical eSSVI cold driver's permissive quote predicate and
// direct de-Americanization; it optionally audits fitted inversions when the
// caller sets `audit_fit_inversions`. The two policies are intentionally NOT
// semantically equivalent (see prepared_fitting.hpp).
enum class PreparedObservationPolicy : std::uint8_t {
  Configured = 0,
  LegacyEssviCompatibility,
};

// ── Explicit preparation-policy decision (W2-B) ─────────────────────────────
//
// Preparation strictness used to be a CONSEQUENCE of curve-family choice: the
// eSSVI driver prepared under the permissive predicate, every other family got
// the full `CalibOpts` cascade, and nothing chose. Because the auto-router sends
// illiquid small caps to SVI, the thinnest boards drew the harshest filter and
// starved below `kMinPreparedFitRows`.
//
// The types below make the decision an INPUT. A caller states what it wants
// (`PreparationPolicyRequest`, every field defaulting to `Auto` = "the
// historical default for this lane"), names the driver the fit will run on
// (`PreparationLane`), and `resolve_preparation_policy` returns exactly what the
// driver WILL do (`ResolvedPreparationPolicy`) — never what was asked for. That
// separation is the point: the resolved value is inspectable and testable
// without running a fit, and a rescue the requested lane cannot execute resolves
// to `false` instead of being silently promised.

// Which cold fit driver the decision is being resolved for. The historical
// preparation defaults differ per DRIVER, and that — not the curve family — is
// the coupling this type makes explicit.
enum class PreparationLane : std::uint8_t {
  // `run_surface_parity` (surface_parity.cpp). Prepares under the permissive
  // eSSVI cold-driver predicate by default and implements NO per-slice rescue:
  // it can emit only 6 of the 10 `ExpiryFitOutcome` values and in particular can
  // never report `FittedLegacyPrep` or `FittedFallbackCurve`.
  EssviDriver = 0,
  // `fit_curve_surface` (curve_fit.cpp) — every non-eSSVI family (ConvexDense,
  // Svi, SplineVol, C8, LinearVariance). Prepares under `Configured` by default
  // and implements both per-slice rescues.
  PolymorphicDriver = 1,
};

// Requested preparation strictness, independent of curve family.
enum class PrepStrictness : std::uint8_t {
  Auto = 0,   // the requested lane's historical default (byte-compatible)
  Configured, // full CalibOpts cascade (kill-mask, spread/IV/vega filters)
  Permissive, // the LegacyEssviCompatibility predicate (strike>0 + quote valid)
};

// Tri-state request for one thin-slice rescue lane. `Auto` is the shipped
// default for the resolved lane; `On`/`Off` are explicit operator choices.
enum class ThinSliceRescue : std::uint8_t {
  Auto = 0,
  Off,
  On,
};

// How aggressively the polymorphic driver applies the Legacy-prep rescue.
//
// The distinction is load-bearing, and MEASURED rather than assumed. Rescuing
// every starved slice changes what a board SERVES: a slice recovered under the
// permissive predicate carries quotes the configured cascade rejected (wide,
// one-sided, kill-mask-flagged), so it can drag a board's worst-slice quality
// under an admission floor and fail a board that previously passed on its
// fitted slices alone. On the 240-name lqbench OPRA snapshot that never
// happened (0 status changes; 13 boards were promoted off their eSSVI fallback
// onto their own primary family). On the AAPL/GOOGL/NVDA populate lane, which
// admits far more strictly, it failed 2 of 3 boards that fit before. Coverage
// recovery must not be able to cost a healthy board its surface, so the DEFAULT
// is the last-resort form and the aggressive form is an explicit request.
enum class LegacyPrepRescueMode : std::uint8_t {
  // Never re-prepare.
  Off = 0,
  // Second pass, run ONLY when the primary preparation leaves the board with no
  // fittable slice at all. It can turn a total refusal into a fit and can do
  // nothing else: a board that already produces one slice is untouched, bits
  // included, and pays no extra preparation.
  BoardStarvedOnly,
  // Per-slice — every starved slice is re-prepared, even on a board that
  // already has fittable slices. Maximum coverage, and the only form that can
  // move a healthy board's served quality.
  EverySlice,
};

struct PreparationPolicyRequest {
  PrepStrictness strictness{PrepStrictness::Auto};
  // Re-prepare a slice that starved the primary policy under the permissive
  // predicate (`SurfaceParityInputs::per_slice_legacy_prep_fallback` /
  // `::board_starved_legacy_prep_fallback`).
  // Auto => `BoardStarvedOnly` wherever it can run and can matter: the
  // polymorphic driver with a `Configured` primary. `On` => `EverySlice`.
  ThinSliceRescue legacy_prep_rescue{ThinSliceRescue::Auto};
  // Serve a >=2-node LinearVariance slice when the primary family's FIT fails
  // (`CalibOpts::per_slice_linear_fallback`).
  // Auto => OFF, deliberately. It is not a preparation rescue at all — it fires
  // after the fit, so it cannot recover a prep-starved slice — and it silently
  // downgrades the served family to a piecewise-linear variance curve that a
  // two-node population is enough to publish, while its union-grid calendar
  // floor absorbs (and un-counts) the calendar refusals
  // `n_slice_calendar_unsupported` exists to surface. Recovery that changes what
  // is SERVED stays an explicit opt-in.
  ThinSliceRescue linear_fallback{ThinSliceRescue::Auto};
};

// What the driver will actually do. Produced only by
// `resolve_preparation_policy`; never assembled by hand at a call site.
struct ResolvedPreparationPolicy {
  PreparedObservationPolicy primary{PreparedObservationPolicy::Configured};
  LegacyPrepRescueMode legacy_prep_rescue{LegacyPrepRescueMode::Off};
  bool linear_fallback{false};
};

// Pure, total resolution of `request` against `lane`. No I/O, no allocation,
// usable in a constant expression, and the single source of truth for every
// "what does Auto mean here" question.
//
// Contract:
//   * `primary` never depends on the curve family, only on the explicit
//     strictness (with `Auto` reproducing each lane's historical default).
//   * `legacy_prep_rescue` is non-`Off` only where the lane implements it AND it
//     can change anything (the primary must be strict — re-preparing a
//     permissive slice under the permissive predicate is a no-op).
//   * `linear_fallback` is true only where the lane implements it and only when
//     explicitly requested `On`.
[[nodiscard]] constexpr ResolvedPreparationPolicy
resolve_preparation_policy(const PreparationPolicyRequest &request, PreparationLane lane) noexcept {
  ResolvedPreparationPolicy out{};
  switch (request.strictness) {
  case PrepStrictness::Auto:
    out.primary = (lane == PreparationLane::EssviDriver)
                      ? PreparedObservationPolicy::LegacyEssviCompatibility
                      : PreparedObservationPolicy::Configured;
    break;
  case PrepStrictness::Configured:
    out.primary = PreparedObservationPolicy::Configured;
    break;
  case PrepStrictness::Permissive:
    out.primary = PreparedObservationPolicy::LegacyEssviCompatibility;
    break;
  }

  const bool lane_has_rescues = lane == PreparationLane::PolymorphicDriver;
  const bool rescue_can_matter =
      lane_has_rescues && out.primary == PreparedObservationPolicy::Configured;
  switch (request.legacy_prep_rescue) {
  case ThinSliceRescue::Auto:
    out.legacy_prep_rescue =
        rescue_can_matter ? LegacyPrepRescueMode::BoardStarvedOnly : LegacyPrepRescueMode::Off;
    break;
  case ThinSliceRescue::On:
    out.legacy_prep_rescue =
        rescue_can_matter ? LegacyPrepRescueMode::EverySlice : LegacyPrepRescueMode::Off;
    break;
  case ThinSliceRescue::Off:
    out.legacy_prep_rescue = LegacyPrepRescueMode::Off;
    break;
  }

  switch (request.linear_fallback) {
  case ThinSliceRescue::On:
    out.linear_fallback = lane_has_rescues;
    break;
  case ThinSliceRescue::Auto:
  case ThinSliceRescue::Off:
    out.linear_fallback = false;
    break;
  }
  return out;
}

} // namespace atx::vol

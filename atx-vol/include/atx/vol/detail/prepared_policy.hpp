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

} // namespace atx::vol

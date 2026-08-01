#pragma once

// SurfaceRef -> vol-derivative pricing bridge (Task 9, DerivBook).
//
// WHY THIS HEADER EXISTS. `deriv_price` / `deriv_greeks` are TEMPLATES on the
// surface type, and their bodies live in `src/derivatives.cpp` (only
// EssviSurface / SviSurface are explicitly instantiated — see the `extern
// template` block in derivatives.hpp). Pricing a book against a
// `SurfaceSet` therefore needs a THIRD surface adapter — one that presents a
// `SurfaceRef` (absolute-strike `iv(K,T)`) through the log-moneyness
// `iv(k_log,T)` contract the strip templates require — and that adapter must be
// instantiated in a translation unit that can see the template bodies.
//
// Rather than export the adapter type (and with it the whole strip-template
// machinery) into a header, the two entry points below are DECLARED here and
// DEFINED in `src/derivatives.cpp` beside the templates, exactly as the
// PricedSurface-native E6 overloads are. All surface-adapter machinery stays in
// that one TU; `deriv_book.cpp` sees only these two ordinary functions.
//
// CARRY. Each call derives a single-tenor `CurveSet` from the handle itself:
// `spot = pricing().S`, ONE forward pillar `(T, forward_at(T))`, and a flat
// zero curve at `rate_at(T)`. `SurfaceRef` exposes no fitted-pillar list (no
// `context()`), so the E6 `carry_from` fitted-range gate is unavailable here;
// the handle's own economic forward/rate extrapolation is used instead and the
// caller owns that choice. See the definition for the exact contract.
//
// Thread-safety: both are stateless pure functions of a borrowed surface, safe
// to call concurrently from any number of threads (the CurveSet they build is
// function-local).

#include "atx/vol/derivatives.hpp" // DerivContract/DerivConfig/DerivQuote/DerivGreeks/bumps
#include "atx/vol/types.hpp"       // Result

namespace atx::vol {

// portfolio_pricer.hpp. Only ever taken by const reference here, so the
// two-pointer handle's definition is the includer's business, not this
// header's.
class SurfaceRef;

namespace detail {

// Mark a vol-derivative contract against a borrowed surface.
//
// @pre `ref.valid()` — a null handle returns InvalidArgument rather than
//      dereferencing (the book layer resolves the uid first and reports a
//      missing surface as ModelUnavailable, so this is defence in depth).
// @return the same error contract as the templated `deriv_price`.
[[nodiscard]] Result<DerivQuote>
deriv_price_on_ref(const SurfaceRef &ref, const DerivContract &contract, const DerivConfig &cfg);

// Finite-difference greeks for the same contract, differentiating exactly the
// `deriv_price_on_ref` above. Same precondition and error contract as the
// templated `deriv_greeks`.
[[nodiscard]] Result<DerivGreeks> deriv_greeks_on_ref(const SurfaceRef &ref,
                                                      const DerivContract &contract,
                                                      const DerivConfig &cfg,
                                                      const DerivGreekBumps &bumps);

} // namespace detail
} // namespace atx::vol

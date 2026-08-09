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
// CARRY. Each call derives a `CurveSet` from the handle itself: `spot =
// pricing().S`, a forward pillar at `(T, forward_at(T))` — plus one at the
// theta-roll tenor `(T - dt, forward_at(T - dt))` when the caller is about to
// roll, so the rolled repricing reads the surface's own forward at its own
// residual tenor — and a zero curve flat in RATE at `rate_at(T)`.
//
// NO FITTED-RANGE GATE is applied, unlike the E6 `carry_from`. That is a CHOICE,
// not a limitation: an owned handle could reach `owned()->context()`, but a
// view-backed `PricedSurfaceView` exposes no pillar list, so gating would make
// the two `SurfaceRef` forms behave DIFFERENTLY on the same surface. Uniform
// behaviour wins; the tenor-hygiene obligation it hands the caller is documented
// on the public API in deriv_book.hpp. See the definition in
// `src/derivatives.cpp` for the exact contract.
//
// Thread-safety: both are stateless pure functions of a borrowed surface, safe
// to call concurrently from any number of threads (the CurveSet they build is
// function-local).

#include <optional>

#include "atx/vol/derivatives.hpp" // DerivContract/DerivConfig/DerivQuote/DerivGreeks/bumps
#include "atx/vol/types.hpp"       // Result

namespace atx::vol {

// portfolio_pricer.hpp. Only ever taken by const reference here, so the
// two-pointer handle's definition is the includer's business, not this
// header's.
class SurfaceRef;

namespace detail {

// Task P-6 (GK-P book memo). Per-(uid, T) shared state for pricing MANY
// VarSwap `DerivPosition`s against the SAME surface and tenor: PV and every
// strip-affine greek read nothing surface/tenor-dependent that differs
// across those rows (see `resolve_var_swap_strip_raw`'s doc, derivatives.cpp)
// -- this struct is that shared state, resolved lazily (by
// `deriv_price_var_swap_on_ref_shared` / `deriv_greeks_var_swap_on_ref_shared`
// below) the first time a row in the group needs it, and reused bit-for-bit
// by every subsequent row.
//
// Every field is a plain value/`Result<DerivQuote>` (no SurfaceT template
// parameter) precisely so `deriv_book.cpp`'s book-level memo map can hold
// this type directly -- the SurfaceT-templated machinery that BUILDS it stays
// inside derivatives.cpp, exactly like `SurfaceRefStripView` itself.
//
// Default-constructed = "nothing resolved yet"; a caller default-constructs
// one PER (uid, T, kind-class) GROUP and passes the SAME instance to every
// row in that group. NOT thread-safe to share across threads pricing
// different rows of the SAME group concurrently (mutated in place by the
// `ensure_*` builders) -- `price_deriv_book`'s loop is serial (see
// deriv_book.hpp's own "Determinism / threading" section), so this is not a
// new constraint.
struct VarSwapSharedBlock {
  // T-side: the group's own maturity.
  double df_at_T = 0.0;
  bool df_fallback_at_T = false;
  bool center_resolved = false;
  Result<DerivQuote> center_raw = atx::core::Ok(DerivQuote{});  // meaningful iff center_resolved

  // Market-greek + T-dt roll sub-block: both are needed by exactly the same
  // condition (a NOT-fully-aged row asking for greeks), so both resolve
  // together, lazily, the first time any row in the group asks.
  bool greeks_resolved = false;
  DerivConfig cfg_pinned{};
  Result<DerivQuote> pinned_center_raw = atx::core::Ok(DerivQuote{});
  bool have_analytic = false;
  double a_delta = 0.0, a_gamma = 0.0, a_vega = 0.0, a_vanna = 0.0, a_volga = 0.0;
  bool have_second_order = false;
  Result<DerivQuote> s_up_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> s_dn_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> v_up_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> v_dn_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> sv_pp_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> sv_pm_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> sv_mp_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> sv_mm_raw = atx::core::Ok(DerivQuote{});

  bool can_roll = false;
  double t_minus_dt = 0.0;
  double df_at_Tdt = 0.0;
  bool df_fallback_at_Tdt = false;
  Result<DerivQuote> c_tdt_raw = atx::core::Ok(DerivQuote{});
  bool have_tdt_spot_bumps = false;
  Result<DerivQuote> s_up_tdt_raw = atx::core::Ok(DerivQuote{});
  Result<DerivQuote> s_dn_tdt_raw = atx::core::Ok(DerivQuote{});
};

// Marks-only shared-block entry point. Bit-identical to `deriv_price_on_ref`
// on an in-scope VarSwap contract (`cfg.discrete_correction_mode == None` --
// NOT re-checked here, the caller's responsibility, see
// `resolve_var_swap_strip_raw`'s own doc) -- reuses/extends `block` instead
// of resolving the strip fresh.
//
// @pre `ref.valid()`.
[[nodiscard]] Result<DerivQuote>
deriv_price_var_swap_on_ref_shared(const SurfaceRef &ref, const DerivContract &contract,
                                   const DerivConfig &cfg, VarSwapSharedBlock &block,
                                   std::optional<double> surface_certified_wing_band = std::nullopt);

// Full-greeks shared-block entry point. Bit-identical to
// `deriv_greeks_on_ref` on an in-scope VarSwap contract, same scope/caveat as
// above.
[[nodiscard]] Result<DerivGreeks>
deriv_greeks_var_swap_on_ref_shared(const SurfaceRef &ref, const DerivContract &contract,
                                    const DerivConfig &cfg, const DerivGreekBumps &bumps,
                                    VarSwapSharedBlock &block,
                                    std::optional<double> surface_certified_wing_band = std::nullopt);

// Mark a vol-derivative contract against a borrowed surface.
//
// `surface_certified_wing_band`: FIT-C7 (Task C-6) -- same contract as the
// PricedSurface-native `var_swap_fair_strike`'s parameter of the same name
// (derivatives.hpp): a caller who knows `ref`'s own build quality mode should
// resolve `atx::vol::certified_wing_half_band(mode)` (surface_policy.hpp) and
// pass it here so the strip trusts the surface exactly where that mode's fit
// pipeline certified it, rather than the mode-blind default `std::nullopt`
// preserves.
//
// @pre `ref.valid()` — a null handle returns InvalidArgument rather than
//      dereferencing (the book layer resolves the uid first and reports a
//      missing surface as ModelUnavailable, so this is defence in depth).
// @return the same error contract as the templated `deriv_price`.
[[nodiscard]] Result<DerivQuote>
deriv_price_on_ref(const SurfaceRef &ref, const DerivContract &contract, const DerivConfig &cfg,
                   std::optional<double> surface_certified_wing_band = std::nullopt);

// Finite-difference greeks for the same contract, differentiating exactly the
// `deriv_price_on_ref` above. Same precondition and error contract as the
// templated `deriv_greeks`.
[[nodiscard]] Result<DerivGreeks>
deriv_greeks_on_ref(const SurfaceRef &ref, const DerivContract &contract, const DerivConfig &cfg,
                    const DerivGreekBumps &bumps,
                    std::optional<double> surface_certified_wing_band = std::nullopt);

} // namespace detail
} // namespace atx::vol

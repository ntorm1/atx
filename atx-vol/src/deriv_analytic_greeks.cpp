#include "deriv_analytic_greeks.hpp"

#include <cmath>

#include "atx/core/math.hpp"

namespace atx::vol::detail {

// Deliberately duplicates (rather than composes) `black76_value_and_vega` +
// `black76_aux`'s d1/d2 resolution: volga needs d1 AND d2 alongside vega, and
// calling the two existing public entries separately would recompute
// sqrt(T)/sigma*sqrt(T)/ln(F/K)/d1 a second time for no benefit, PLUS force
// this file to reconcile two independently-defined degenerate-branch
// conventions (T <= 0 / sigma <= 0) instead of one. Fusing vega and volga
// into a single pass mirrors `black76.hpp`'s own `Black76ValueVega` fusion
// precedent (price + vega, one d1 resolution) one level deeper. The vega
// FORMULA itself is copied verbatim from `black76_value_and_vega`
// (black76.cpp) -- same operations in the same order -- so the two agree to
// the bit for identical inputs; this is exercised indirectly by every
// analytic-vs-FD parity case in `deriv_greeks_test.cpp` (`AnalyticGreeks.*`),
// since a drifted vega would blow the FD parity gate immediately.
B76VegaVolga black76_vega_volga(double F, double K, double T, double sigma, double df) noexcept {
  // Mirrors `price_node`'s (derivatives.cpp) "bad node contributes 0"
  // convention exactly, extended to F/K (both are always > 0 by construction
  // here -- F resolved from the curve, K = F*exp(x) -- but the check is
  // cheap and keeps this function correct standing alone, not merely correct
  // as called).
  if (!(F > 0.0) || !(K > 0.0) || T <= 0.0 || !(sigma > 0.0) || !std::isfinite(sigma)) {
    return B76VegaVolga{0.0, 0.0};
  }
  const double sqrt_t = std::sqrt(T);
  const double v = sigma * sqrt_t;
  const double inv_v = 1.0 / v;
  const double ln_fk = std::log(F / K);
  const double d1 = (ln_fk + 0.5 * v * v) * inv_v;
  const double d2 = d1 - v;
  // phi(d1) = (1/sqrt(2*pi)) * exp(-d1^2/2); vega = F*df*phi(d1)*sqrt(T) --
  // identical formula to `black76_value_and_vega` (black76.cpp).
  const double pdf_d1 = atx::core::inv_sqrt_2pi<double> * std::exp(-0.5 * d1 * d1);
  const double vega = F * df * pdf_d1 * sqrt_t;
  // d(vega)/dsigma = vega * d1 * d2 / sigma -- the standard Black-Scholes/76
  // identity; see deriv_analytic_greeks.hpp's "VOLGA" derivation for the
  // from-scratch re-derivation (d(d1)/dsigma = -d2/sigma).
  const double volga = vega * d1 * d2 / sigma;
  return B76VegaVolga{vega, volga};
}

}  // namespace atx::vol::detail

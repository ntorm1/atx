#include "atx/vol/api/fitting/surface_overlay.hpp"

#include <cmath>

namespace atx::vol {

// `log1p(h)`, not `log(1+h)`: for the 1e-4 relative bumps `DerivGreekBumps`
// defaults to, `1.0 + h` has already rounded away most of the significand that
// the difference quotient then divides by.
//
// Out-of-line, so the sign lives in exactly one object file rather than being
// re-derived at each of the stencil's call sites. It is called once per bump,
// never per surface read.
double sticky_k_shift(StickyMode mode, double spot_rel) noexcept {
  return mode == StickyMode::StickyMoneyness ? 0.0 : std::log1p(spot_rel);
}

}  // namespace atx::vol

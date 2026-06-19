// atx::engine::risk — fractional-Kelly conviction-scaled sizing (S10-2).
//
// The heavy body of kelly_size() lives here so the public header carries only the
// POD structs + the declaration. See risk/kelly_sizing.hpp for the full contract;
// this file only realizes the FIXED-ORDER algorithm:
//   1. full-Kelly target  f* = V^{-1} mu   (Woodbury apply_inverse — never an MxM inverse)
//   2. fractional scale    f  = kelly_fraction * f*
//   3. conviction scale    w_i = conviction[i] * f_i   (zero-conviction => exactly 0)
//   4. gross clamp         if Sum|w| > max_gross > 0: w *= max_gross / Sum|w|
// Pure, no RNG, order-fixed (ascending instrument i throughout).

#include "atx/engine/risk/kelly_sizing.hpp"

#include <cmath> // std::isfinite (debug finite guards), std::fabs (gross L1)
#include <cstddef> // std::size_t (span length cast)
#include <span>    // std::span (apply_inverse views over the Eigen buffers)

#include "atx/core/macro.hpp" // ATX_ASSERT (fail-closed preconditions)
#include "atx/core/types.hpp" // atx::f64, atx::usize

namespace atx::engine::risk {

KellyWeights kelly_size(const atx::core::linalg::VecX &expected_alpha, const FactorModel &cov,
                        const atx::core::linalg::VecX &conviction, const KellyConfig &cfg) {
  const atx::usize m = cov.n_instruments();

  // Fail closed: a length or domain violation would silently mis-size every name.
  // The death test relies on the finite guard aborting. Debug-only (ATX_ASSERT) —
  // the caller (S10 combine layer) owns the validated upstream contract; this is a
  // loud trip-wire, not a release-path branch.
  ATX_ASSERT(static_cast<atx::usize>(expected_alpha.size()) == m);
  ATX_ASSERT(static_cast<atx::usize>(conviction.size()) == m);
  ATX_ASSERT(cfg.kelly_fraction >= 0.0);
  for (atx::usize i = 0U; i < m; ++i) {
    ATX_ASSERT(std::isfinite(expected_alpha[static_cast<Eigen::Index>(i)]));
    const atx::f64 c = conviction[static_cast<Eigen::Index>(i)];
    ATX_ASSERT(std::isfinite(c));
    ATX_ASSERT(c >= 0.0 && c <= 1.0);
  }

  // (1) full-Kelly target f* = V^{-1} mu via the cached Woodbury inverse. Spans view
  // the Eigen buffers directly (no copy); apply_inverse computes the PURE V^{-1}·in,
  // O(MK + K^3), and NEVER materializes the MxM inverse (the whole point of the
  // factored model). `out` is a fresh M-vector we then scale in place.
  atx::core::linalg::VecX w(static_cast<Eigen::Index>(m));
  cov.apply_inverse(
      std::span<const atx::f64>(expected_alpha.data(), static_cast<std::size_t>(expected_alpha.size())),
      std::span<atx::f64>(w.data(), static_cast<std::size_t>(w.size())));

  // (2) fractional scale + (3) per-name conviction scale, fused in one ascending
  // pass. f = kelly_fraction * f*, then w_i = conviction[i] * f_i. A zero-conviction
  // name multiplies to EXACTLY 0.0 (no floating drift — 0.0 * x is exactly 0 for
  // finite x), so it carries no target weight; a full-conviction name keeps its
  // fractional-Kelly size.
  atx::f64 gross = 0.0;
  for (atx::usize i = 0U; i < m; ++i) {
    const Eigen::Index ei = static_cast<Eigen::Index>(i);
    const atx::f64 wi = conviction[ei] * (cfg.kelly_fraction * w[ei]);
    w[ei] = wi;
    gross += std::fabs(wi); // Sum|w| accumulated in the same fixed order (R1)
  }

  // (4) gross clamp: if enabled (max_gross > 0) and binding (gross exceeds the cap),
  // scale the WHOLE vector by max_gross / gross so realized Sum|w| == max_gross. The
  // uniform scale preserves every name's relative tilt (it is a leverage cap, not a
  // re-optimization). scale_applied records the factor for report attribution; 1.0
  // when the clamp is disabled or slack.
  atx::f64 scale_applied = 1.0;
  if (cfg.max_gross > 0.0 && gross > cfg.max_gross) {
    scale_applied = cfg.max_gross / gross;
    for (atx::usize i = 0U; i < m; ++i) {
      w[static_cast<Eigen::Index>(i)] *= scale_applied;
    }
    gross = cfg.max_gross; // set to the cap exactly (avoids re-summing rounding noise)
  }

  return KellyWeights{std::move(w), gross, scale_applied};
}

} // namespace atx::engine::risk

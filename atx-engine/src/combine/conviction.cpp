// atx::engine::combine — conviction score implementation (S10-1).
//
// The heavy body of conviction() lives here so the public header drags neither
// <cmath> nor the eval-result definitions into every includer. See
// combine/conviction.hpp for the full contract; this file only realizes it.

#include "atx/engine/combine/conviction.hpp"

#include <cmath> // std::isfinite (debug finite guard)

#include "atx/core/macro.hpp" // ATX_ASSERT (fail-closed precondition guards)
#include "atx/core/types.hpp" // atx::f64

#include "atx/engine/eval/deflated_sharpe.hpp" // eval::DsrResult (the `dsr` field)
#include "atx/engine/eval/pbo.hpp"             // eval::PboResult (the `pbo` field)

namespace atx::engine::combine {

namespace {

// clamp `x` into [lo, hi]. Branch form (not std::clamp) so a NaN — already
// trapped by the debug finite guard in conviction() — would surface as `lo`
// rather than UB in a release build that skipped the assert. lo <= hi by callsite.
[[nodiscard]] atx::f64 clamp(atx::f64 x, atx::f64 lo, atx::f64 hi) noexcept {
  if (x < lo) {
    return lo;
  }
  if (x > hi) {
    return hi;
  }
  return x;
}

// Map the step-3 explainability verdict to its size multiplier. Exhaustive
// switch with NO default: a future ExplainFlag enumerator forces a compile error
// here rather than silently inheriting some fall-through multiplier.
[[nodiscard]] atx::f64 explain_multiplier(ExplainFlag explain, const ConvictionConfig &cfg) noexcept {
  switch (explain) {
  case ExplainFlag::Explained:
    return 1.0; // known mechanism — full size
  case ExplainFlag::PartlyExplained:
    return cfg.partly_explained_mult; // partial mechanism — modest haircut
  case ExplainFlag::HeadScratcher:
    return cfg.head_scratcher_discount; // no mechanism — trade at reduced size
  }
  // Unreachable for a valid (in-range) enumerator; an out-of-range cast value
  // would fall here. Abort rather than return an unprincipled multiplier.
  ATX_UNREACHABLE();
}

} // namespace

ConvictionScore conviction(const eval::DsrResult &dsr, const eval::PboResult &pbo,
                           atx::f64 oos_is_ratio, ExplainFlag explain,
                           const ConvictionConfig &cfg) noexcept {
  // Fail closed: a NaN/inf input would poison every downstream size. Trap it in
  // debug (the death test relies on this) instead of propagating a silent NaN.
  ATX_ASSERT(std::isfinite(dsr.dsr));
  ATX_ASSERT(std::isfinite(pbo.pbo));
  ATX_ASSERT(std::isfinite(oos_is_ratio));
  // The weights must form a convex combination for `base` to stay in [0,1].
  // Tolerance (not exact equality): callers that REWEIGHT — e.g. the combine
  // stage drops the PBO term and renormalizes the remaining two by dividing by
  // their sum — produce a triple that sums to 1.0 only within FP rounding. An
  // exact-equality assert would abort a Debug run on any such near-1.0 triple
  // (including future default-weight changes). ±1e-9 still traps a GROSS
  // violation (e.g. sum 1.5), so a death test on a grossly-invalid config is
  // unaffected. Changes no computed value (release: still a no-op under NDEBUG).
  ATX_ASSERT(std::fabs((cfg.w_dsr + cfg.w_pbo + cfg.w_stability) - 1.0) < 1e-9);

  // Three components, each clamped into [0,1] before weighting:
  //   * DSR is already a probability — clamp only guards a degenerate input.
  //   * (1 - PBO): higher = less overfit ⇒ more conviction.
  //   * stability: OOS/IS ratio, capped at 1 (OOS no better-than-IS earns no
  //     bonus) and floored at 0 (an OOS breakdown earns no credit).
  const atx::f64 dsr_term = clamp(dsr.dsr, 0.0, 1.0);
  const atx::f64 pbo_term = clamp(1.0 - pbo.pbo, 0.0, 1.0);
  const atx::f64 stability_term = clamp(oos_is_ratio, 0.0, 1.0);

  // Order-fixed reduction (dsr, then pbo, then stability) so the sum is
  // byte-identical run to run. base ∈ [0,1] since the weights sum to 1.
  const atx::f64 base =
      cfg.w_dsr * dsr_term + cfg.w_pbo * pbo_term + cfg.w_stability * stability_term;

  // Explainability modulates SIZE, not membership (RenTech G1): multiply, never
  // veto. explain_mult ∈ [0,1] keeps score ∈ [0,1].
  const atx::f64 explain_mult = explain_multiplier(explain, cfg);
  const atx::f64 score = base * explain_mult;

  return ConvictionScore{score, dsr_term, pbo_term, stability_term, explain_mult};
}

} // namespace atx::engine::combine

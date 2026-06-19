// atx::engine::combine — crowding / capacity-aware de-correlation impl (S10-4).
//
// The body lives here so the public header drags neither the pairwise-correlation
// kernel nor <algorithm> into every includer. See combine/crowding.hpp for the full
// contract (the de-correlation form, the WHY of its shape, and the exact passthrough
// guard); this file only realizes it.

#include "atx/engine/combine/crowding.hpp"

#include <algorithm> // std::clamp (capacity scale)
#include <cmath>     // std::abs, std::isfinite (precondition checks, |corr|)
#include <span>      // std::span
#include <vector>    // std::vector

#include "atx/core/macro.hpp" // ATX_ASSERT (programmer-error preconditions)
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/combine/correlation.hpp" // pairwise_complete_corr (the shared §3.3 kernel)
#include "atx/engine/combine/store.hpp"       // AlphaStore, AlphaId

namespace atx::engine::combine {

namespace {

// The window sub-span of alpha `id`'s PnL: pnl(id).subspan(fit_begin, T). The SAME
// [fit_begin, fit_end) the weights were fit on, so the crowding correlations are
// coherent with the fit. `id` is < pool.size() <= u32-max (AlphaStore mints only u32
// ids — store.hpp), so the usize->u32 narrowing provably cannot truncate.
[[nodiscard]] std::span<const atx::f64>
window_span(const AlphaStore &pool, atx::usize id, atx::usize fit_begin, atx::usize t) {
  return pool.pnl(AlphaId{static_cast<atx::u32>(id)}).subspan(fit_begin, t);
}

} // namespace

std::vector<atx::f64> decorrelate_weights(std::span<const atx::f64> weights, const AlphaStore &pool,
                                          atx::usize fit_begin, atx::usize fit_end,
                                          std::span<const atx::f64> capacity,
                                          const CrowdingConfig &cfg) {
  const atx::usize n = pool.size();
  // Programmer-error preconditions (abort in debug). A misshaped input or an
  // out-of-range window is a caller bug, not a recoverable runtime condition.
  ATX_ASSERT(weights.size() == n);
  ATX_ASSERT(capacity.size() == n);
  ATX_ASSERT(fit_begin < fit_end && fit_end <= pool.n_periods());
  ATX_ASSERT(cfg.corr_penalty >= 0.0); // a negative penalty would AMPLIFY crowding
  for (atx::usize i = 0U; i < n; ++i) {
    ATX_ASSERT(std::isfinite(weights[i]));  // a NaN/inf weight would poison the output
    ATX_ASSERT(std::isfinite(capacity[i])); // and a NaN capacity its cap_scale
  }

  const atx::usize t = fit_end - fit_begin;
  const bool cap_enabled = cfg.capacity_floor > 0.0;

  std::vector<atx::f64> out(n, 0.0);
  for (atx::usize i = 0U; i < n; ++i) {
    // crowding_i = Σ_{j != i, ascending} |corr(win_i, win_j)| — the TOTAL redundancy
    // of alpha i. Order-fixed (ascending j) so the sum is byte-reproducible. N == 1
    // leaves crowding_i = 0 (the inner loop never finds a j != i).
    atx::f64 crowding = 0.0;
    const std::span<const atx::f64> win_i = window_span(pool, i, fit_begin, t);
    for (atx::usize j = 0U; j < n; ++j) {
      if (j == i) {
        continue;
      }
      const std::span<const atx::f64> win_j = window_span(pool, j, fit_begin, t);
      const atx::f64 c = pairwise_complete_corr(win_i, win_j);
      crowding += (c < 0.0) ? -c : c; // |corr| (magnitude: a copy and an anti-copy both crowd)
    }

    // cap_scale_i: fade in linearly from 0 to the floor, then hold at 1. Disabled
    // (floor <= 0) ⇒ exactly 1, so it cannot perturb the passthrough guard.
    const atx::f64 cap_scale =
        cap_enabled ? std::clamp(capacity[i] / cfg.capacity_floor, 0.0, 1.0) : 1.0;

    // out_i = cap_scale_i * w_i / (1 + penalty·crowding_i). With penalty == 0 AND
    // cap disabled this is exactly w_i (divisor 1, scale 1) — the passthrough rail.
    const atx::f64 denom = 1.0 + cfg.corr_penalty * crowding;
    out[i] = cap_scale * weights[i] / denom;
  }
  // Intentionally NOT renormalized: the absolute shrink of a crowded pair must stay
  // observable to the caller (it renormalizes downstream if it wants fixed gross).
  return out;
}

} // namespace atx::engine::combine

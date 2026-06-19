#pragma once

// atx::engine::combine — RegimeCombiner: regime-conditioned signal combination
// (S10-3, RenTech gap G3 — "one model integrating sub-models for different market
// conditions").
//
// ===========================================================================
//  What this unit is (RenTech gap G3)
// ===========================================================================
//  AlphaCombiner (combine/combiner.hpp) fits ONE static blend over the whole
//  history. RenTech ran sub-models tuned to distinct market conditions and routed
//  between them. The learn HMM (learn/hmm.hpp) already produces a point-in-time
//  regime posterior P(regime | data ≤ t) but was unwired. THIS unit closes the
//  loop: a RegimeCombiner holds ONE AlphaCombiner combo per regime (each fit on
//  that regime's masked sub-history) and, at eval time, BLENDS those per-regime
//  weight vectors by the HMM posterior — so the combined book tilts toward the
//  combo trained on whatever regime data ≤ t says we are most likely in.
//
//  The HMM dependency lives only at the CALL SITE (the caller passes regime
//  labels for the fit and a posterior for the blend); this unit does NOT depend on
//  learn/hmm.hpp. It depends only on AlphaCombiner / AlphaStore (combine spine).
//
// ===========================================================================
//  The byte-identical single-regime fallback — the critical guard
// ===========================================================================
//  With n_regimes == 1 (all labels equal) the masked sub-pool over the FULL
//  [fit_begin, fit_end) window holds the SAME PnL rows AlphaCombiner::fit reads
//  directly, so the single fitted combo is BYTE-IDENTICAL to
//  AlphaCombiner{cfg}.fit(pool, fit_begin, fit_end). Routing through regimes must
//  never perturb the legacy static-combo path — this is pinned by a degenerate
//  guard test (EXPECT_EQ element-wise on fit_begin == 0).
//
// ===========================================================================
//  Point-in-time / no-look-ahead
// ===========================================================================
//  The fit reads ONLY PnL rows in [fit_begin, fit_end) (AlphaCombiner's §3.1
//  firewall, inherited through the sub-pool). blend() introduces NO look-ahead of
//  its own: it is a pure linear combination of already-fit weight vectors by a
//  caller-supplied posterior. When that posterior comes from learn's
//  regime_posterior_at (which reads only obs rows ≤ t, truncation-invariant), the
//  whole path is point-in-time. A probability-vector posterior (≥0, Σ=1) makes the
//  blend a CONVEX combination of the per-regime weights.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG, no allocation on the blend hot path beyond the output vector. Every
//  reduction is order-fixed: the masked sub-pool is built in ascending period
//  order, the per-regime fit is AlphaCombiner's own fixed order, and blend() sums
//  ascending regime then ascending alpha. Same inputs -> byte-identical output.

#include <span>   // std::span (the posterior view)
#include <vector> // std::vector (per-regime combos / the blended weights)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode, ATX_TRY
#include "atx/core/types.hpp" // f64, u32, usize

#include "atx/engine/combine/combiner.hpp" // AlphaCombiner, Combination, CombinerConfig
#include "atx/engine/combine/store.hpp"    // AlphaStore, AlphaId

namespace atx::engine::combine {

// ===========================================================================
//  RegimeCombiner — one linear combo per regime + a PIT posterior blend.
//
//  per_regime[r] is the AlphaCombiner combo fit on regime r's masked sub-history;
//  index == regime id. INVARIANT (established by fit_regime_combiner): per_regime
//  is non-empty and every per_regime[r].weights has the SAME length (== the pool's
//  alpha count), so blend() is well-defined. A trivial aggregate (Rule of Zero);
//  owns its combos by value.
// ===========================================================================
struct RegimeCombiner {
  std::vector<Combination> per_regime; // index = regime id; equal weights.size() across r

  // Blend the per-regime weight vectors by a point-in-time regime posterior:
  //   out[i] = Σ_r posterior[r] * per_regime[r].weights[i]
  // A probability-vector posterior (≥0, Σ=1) ⇒ out is a CONVEX combination of the
  // per-regime weights. PIT: `posterior` is supplied by the caller from data ≤ t
  // (e.g. learn::regime_posterior_at); blend introduces no look-ahead of its own.
  // PURE, order-fixed (ascending regime r, then ascending alpha i).
  //
  // PRECONDITIONS (ATX_ASSERT — programmer error, abort in debug): per_regime
  // non-empty; all per_regime[r].weights share one length; posterior.size() ==
  // per_regime.size(); every posterior[r] finite. These are caller contracts, not
  // runtime conditions, so they assert rather than return Err.
  [[nodiscard]] std::vector<atx::f64> blend(std::span<const atx::f64> posterior) const;
};

// ===========================================================================
//  fit_regime_combiner — fit one combo per regime over masked sub-histories.
//
//  For each regime r, COMPACT every alpha's PnL down to the periods (in
//  [fit_begin, fit_end)) labelled r, then fit AlphaCombiner over that contiguous
//  sub-history. `regime_labels[t]` ∈ [0, n_regimes) is period t's regime; its
//  length MUST equal pool.n_periods(). Only periods in [fit_begin, fit_end) are
//  used. A regime with < 2 periods in the window cannot satisfy AlphaCombiner's
//  T >= 2 requirement, so its slot FALLS BACK to the global combo over the full
//  window (documented in the .cpp).
//
//  n_regimes == 1 (all labels equal) ⇒ the single combo is BYTE-IDENTICAL to
//  AlphaCombiner{cfg}.fit(pool, fit_begin, fit_end) — the critical fallback guard.
//
//  FAIL-CLOSED (runtime conditions -> Err, never abort): empty pool;
//  regime_labels.size() != pool.n_periods(); n_regimes == 0; any label >=
//  n_regimes; or an out-of-range window (NOT fit_begin < fit_end <=
//  pool.n_periods()). Any AlphaCombiner::fit Err is propagated.
[[nodiscard]] atx::core::Result<RegimeCombiner>
fit_regime_combiner(const AlphaStore &pool, std::span<const atx::u32> regime_labels,
                    atx::usize n_regimes, atx::usize fit_begin, atx::usize fit_end,
                    const CombinerConfig &cfg = {});

} // namespace atx::engine::combine

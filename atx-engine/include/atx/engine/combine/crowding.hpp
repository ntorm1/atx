#pragma once

// atx::engine::combine — crowding / capacity-aware de-correlation (S10-4, RenTech
// gap G4 — "breadth only helps if the bets are INDEPENDENT").
//
// ===========================================================================
//  What this unit is (RenTech gap G4)
// ===========================================================================
//  The gate (combine/gate.hpp) screens crowding at DISCOVERY (it rejects a
//  candidate whose |corr-to-pool| exceeds a threshold), and the combiner
//  (combine/combiner.hpp) fits ONE blend assuming the admitted pool is already
//  diversified. But two admitted alphas can still be mutually correlated below the
//  gate threshold, and breadth (S10-5: IR = IC·√N) only pays off if the N bets are
//  INDEPENDENT — n correlated copies of one edge are worth ≈ one bet, not n.
//
//  THIS unit closes that gap at the COMBINE step: it takes a fitted weight vector
//  and shrinks each weight by how redundant (mutually correlated) its alpha is with
//  the rest of the pool, and by how capacity-limited the name is. The result is a
//  combined book where a crowded cluster contributes about ONE signal's worth of
//  weight instead of summing its members, and a name with little remaining capacity
//  is scaled down before it is ever traded at size.
//
// ===========================================================================
//  The de-correlation form (and WHY it has this exact shape)
// ===========================================================================
//    crowding_i  = Σ_{j != i} |pairwise_complete_corr(pnl_i[win], pnl_j[win])|
//    cap_scale_i = (cfg.capacity_floor <= 0) ? 1.0
//                                            : clamp(capacity[i]/cfg.capacity_floor, 0, 1)
//    out_i       = cap_scale_i * weights[i] / (1 + cfg.corr_penalty * crowding_i)
//
//  crowding_i is the TOTAL redundancy of alpha i — the sum of its |correlation|
//  magnitudes to every OTHER pool member (a magnitude, so a perfect copy and a
//  perfect anti-correlate are equally "crowded": both carry no independent bet).
//  The 1/(1+penalty·crowding) shrink is calibrated so that n PERFECTLY-correlated
//  copies (each |corr| = 1 to the other n−1) get crowding_i = n−1, hence at
//  corr_penalty = 1: out_i = w_i/(1+(n−1)) = w_i/n. The n copies TOGETHER then
//  contribute n·(w/n) = w — about one signal's weight, not n×. Uncorrelated alphas
//  (crowding ≈ 0) are left essentially unchanged. corr_penalty scales the strength;
//  corr_penalty == 0 disables the de-correlation entirely.
//
//  cap_scale_i linearly fades a name in as its remaining capacity rises from 0 to
//  the floor, then holds at 1 (>= floor ⇒ full size). capacity[i] == 0 ⇒ out_i == 0
//  (a fully-consumed name is dropped from the book). capacity_floor <= 0 disables
//  capacity scaling (every cap_scale_i == 1).
//
//  Capacity is CALLER-SUPPLIED per name: the caller computes remaining capacity
//  upstream from cost/capacity.hpp (ADV / participation curves). Wiring a full
//  ExecutionSimulator into the combiner is out of scope for this unit, which keeps
//  it pure, deterministic, and self-contained.
//
// ===========================================================================
//  The exact passthrough guard — the critical no-op rail
// ===========================================================================
//  corr_penalty == 0 AND capacity_floor <= 0 ⇒ out == weights ELEMENT-WISE EXACT
//  (no arithmetic touches the weight: cap_scale_i == 1 and the 1/(1+0) divisor is
//  exactly 1). A caller that disables both knobs gets the fitted weights back
//  bit-for-bit, so de-correlation is strictly opt-in and never silently perturbs
//  the legacy combine path. This is pinned by a passthrough EXPECT_EQ test.
//
// ===========================================================================
//  Determinism / no renormalization
// ===========================================================================
//  Pure, NO RNG, order-fixed: crowding_i sums j over ascending pool ids, and the
//  output is built in ascending alpha order — same inputs ⇒ byte-identical output.
//  The result is intentionally NOT renormalized: the ABSOLUTE shrink of a crowded
//  pair must remain observable to the caller (renormalizing would hide it by
//  rescaling the surviving weights back up). The caller renormalizes downstream if
//  it wants a fixed gross exposure.

#include <span>   // std::span (weight / capacity views)
#include <vector> // std::vector (the de-correlated output)

#include "atx/core/types.hpp" // atx::f64, atx::usize

#include "atx/engine/combine/store.hpp" // AlphaStore, AlphaId

namespace atx::engine::combine {

// ===========================================================================
//  CrowdingConfig — the two de-correlation knobs (§4). Both default to "off-ish":
//  corr_penalty = 1 is the calibrated "n copies collapse to one signal" strength,
//  and capacity_floor = 0 DISABLES capacity scaling (opt-in by passing a positive
//  floor). A trivial aggregate, copied by value.
// ===========================================================================
struct CrowdingConfig {
  // De-correlation shrink strength on the crowding redundancy. 0 disables the
  // de-correlation (the 1/(1+0·crowding) divisor is exactly 1 ⇒ exact passthrough).
  atx::f64 corr_penalty = 1.0;
  // Per-name remaining-capacity floor. <= 0 DISABLES capacity scaling (every
  // cap_scale == 1). Positive ⇒ a name fades linearly from 0 to the floor.
  atx::f64 capacity_floor = 0.0;
};

// ===========================================================================
//  decorrelate_weights — shrink crowded + capacity-limited weights (S10-4).
//
//  Down-weight each fitted blend weight by (a) how mutually correlated its alpha is
//  with the rest of the pool and (b) how little remaining capacity the name has:
//    crowding_i  = Σ_{j != i} |pairwise_complete_corr(win_i, win_j)|
//    cap_scale_i = (cfg.capacity_floor <= 0) ? 1 : clamp(capacity[i]/floor, 0, 1)
//    out_i       = cap_scale_i * weights[i] / (1 + cfg.corr_penalty * crowding_i)
//  Correlations are taken over the window sub-span [fit_begin, fit_end) of each
//  alpha's PnL: win_k = pool.pnl(AlphaId{k}).subspan(fit_begin, fit_end-fit_begin)
//  — the SAME window the weights were fit on. The result is NOT renormalized (the
//  absolute shrink of a crowded pair stays observable). N == 1 ⇒ crowding_0 == 0.
//
//  EXACT PASSTHROUGH GUARD: corr_penalty == 0 AND capacity_floor <= 0 ⇒ out ==
//  weights element-wise EXACT (no arithmetic perturbs the weight).
//
//  PRECONDITIONS (ATX_ASSERT — programmer error, abort in debug; these are caller
//  contracts, not recoverable runtime conditions): weights.size() == pool.size() ==
//  capacity.size(); fit_begin < fit_end <= pool.n_periods(); every weights[i] and
//  capacity[i] is finite; cfg.corr_penalty >= 0.
//
//  PURE, no RNG, order-fixed (crowding sums ascending j; output built ascending i).
//  COLD path (allocates the output and crowding scratch — runs once per re-fit, not
//  on the apply hot path). Returns the de-correlated weights (length N == pool.size()).
[[nodiscard]] std::vector<atx::f64>
decorrelate_weights(std::span<const atx::f64> weights, const AlphaStore &pool, atx::usize fit_begin,
                    atx::usize fit_end, std::span<const atx::f64> capacity,
                    const CrowdingConfig &cfg);

} // namespace atx::engine::combine

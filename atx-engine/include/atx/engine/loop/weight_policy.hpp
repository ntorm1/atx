#pragma once

// atx::engine::loop — WeightPolicy: signal -> target portfolio + trade list (P2-4).
//
// ===========================================================================
//  What this unit does
// ===========================================================================
//  WeightPolicy turns a cross-sectional alpha signal (one f64 score per live
//  instrument, NaN == "no opinion") into (1) a vector of target portfolio
//  weights and (2) a trade list that drives the current book toward those
//  targets. It is the bridge between the strategy seam (ISignalSource, P2-3) and
//  execution (OrderPayload -> ExecutionSimulator, P2-6): the loop calls
//  to_target_weights() each rebalance, then reconcile() to emit the orders.
//
// ===========================================================================
//  The pipeline (to_target_weights) — Alpha101 `scale` of a neutralized signal
// ===========================================================================
//  Default policy is the canonical long/short construction (P4-8 inserts the two
//  optional [bracketed] neutralization stages; both default OFF):
//
//    winsorize (clamp tail outliers)  ->  transform (rank | zscore)
//      ->  [GROUP-neutralize (per-group demean, if industry_neutral)]
//      ->  dollar-neutralize (subtract mean, Sigma w=0)
//      ->  [TRUNCATE (clip-renorm cap |w_i|, if truncation>0)]
//      ->  gross-normalize (divide by Sigma|w|, times gross_leverage, Sigma|w|=L)
//
//  WHY each step:
//    * winsorize: clamp every live score into the quantile band
//      [winsorize_limit, 1 - winsorize_limit] BEFORE transforming. This caps the
//      influence of a single extreme outlier so it cannot dominate the
//      downstream standardization. It is near-no-op for Rank (rank already
//      ignores magnitude — only order matters), but MATERIAL for ZScore, which
//      has no built-in outlier clamping: one wild value would inflate the mean
//      and std and squash every other name toward zero. Winsorizing first is why
//      ZScore stays robust. nearest-rank quantile band (snaps to observed order
//      statistics — see atx::core::stats::winsorize for the convention).
//    * transform: rank or z-score puts every instrument on a common, outlier-
//      robust cross-sectional scale (a raw alpha can have arbitrary units /
//      heavy tails). Rank is the WorldQuant-style default (monotone, bounded,
//      tie-robust); z-score keeps relative magnitudes.
//    * dollar-neutral: subtracting the cross-sectional mean makes Sigma w = 0 so
//      the long book funds the short book — the portfolio carries no net market
//      (beta-ish) exposure, isolating the alpha's cross-sectional bet.
//    * gross-normalize: dividing by Sigma|w| then scaling by gross_leverage fixes
//      the deployed gross capital (Sigma|w| = gross_leverage). This is exactly
//      Alpha101's `scale` operator. It makes the position sizing independent of
//      the signal's raw magnitude and of the universe size.
//
//  NaN / out-of-universe handling: a NaN score ("no opinion") is EXCLUDED from
//  the transform AND from the mean/gross reductions, and its weight is forced to
//  exactly 0. We therefore compact the live (non-NaN) scores into a dense buffer,
//  run the atx-core cross-section primitive over that buffer, and scatter the
//  results back to universe positions. (atx::core::stats::rank/zscore operate
//  over the WHOLE span with no NaN handling — feeding a NaN through would make
//  rank's argsort comparisons unordered and zscore's mean/var NaN, contaminating
//  every output; compacting first is mandatory, not an optimization.)
//
//  Degenerate cases (documented, never a div-by-zero):
//    * all-equal live scores: rank/zscore -> a constant vector; demean -> all
//      zeros; Sigma|w| == 0 so we leave the weights at zero (no normalization).
//    * single live instrument: rank(n==1)==0 / zscore(n==1)==0; demean keeps 0;
//      result is flat. One name cannot be dollar-neutral with nonzero weight.
//    * empty / all-NaN: all weights zero.
//
//  Tolerance: Sigma w and Sigma|w| hold to floating-point rounding only. Tests
//  assert within 1e-9; do not treat either as bit-exact.
//
//  rank tie-break / convention (inherited from atx::core::stats::rank, the
//  Phase-3 `CsRank` primitive): normalized ordinal rank in [0,1], smallest -> 0,
//  largest -> 1, TIES AVERAGED (a run of equal values shares the mean of their
//  rank positions). Deterministic (stable argsort). zscore uses POPULATION std.
//
//  industry_neutral (P4-8, NOW WIRED): when true, the transformed cross-section
//  is demeaned WITHIN each industry/sector group (the §0-H IndClass group, DSL
//  `CsDemeanG` per-group-demean semantics) BEFORE the global dollar-neutralize,
//  so each group carries no net group exposure. The group identity arrives as the
//  optional `group_map` parameter of to_target_weights (one u32 group id per
//  universe instrument). It is REQUIRED and universe-aligned when industry_neutral
//  is on — to_target_weights ATX_ASSERTs `group_map.size() == n` (fail-closed on
//  misconfig, the same fail-closed intent the Phase-2 assert had). When off (the
//  default) the flag and the group_map are inert. After per-group demeaning each
//  group sums to ~0, so the subsequent global demean is a ~no-op (correct).
//
//  truncation (P4-8): a per-name absolute cap on |w_i| in the FINAL gross-
//  normalized units (0.0 = OFF, the default). When > 0 a FIXED-iteration clip-
//  renorm (no convergence-dependent early exit, §3.2 determinism) repeatedly
//  clips each |w_i| to `truncation` then re-normalizes Σ|w| back to
//  gross_leverage, so the result respects the cap while holding the target gross
//  for a FEASIBLE cap. It is BRAIN's Truncation overfitting guard. Typical
//  0.01–0.10; Phase-5 calibrates. Degenerate: an INFEASIBLY-small cap
//  (truncation·n_active < gross_leverage) pins every name at the cap and leaves
//  Σ|w| < gross_leverage — both constraints cannot hold and the cap wins (the
//  documented degenerate, never a div-by-zero).
//
//  DEFERRED P4-8 stages (documented residuals, NOT implemented here — see the 4b
//  ledger): DECAY (a stateless const WeightPolicy holds no signal history, so a
//  d-window temporal decay needs a signal-history input / a stateful policy — an
//  architectural change) and FACTOR-neutralize (FactorModel::neutralize ships +
//  is tested in P4-7a, but the model carries no self-describing instrument→universe
//  rows and wiring it pulls the whole risk include chain into this Phase-2 header).
//
// ===========================================================================
//  reconcile — order_target_percent (trade = target - current)
// ===========================================================================
//  For each universe index i (iterated in FIXED index order for determinism):
//    price = market.mark(universe[i]); skip if NaN or <= 0 (no priceable target).
//    target_shares = trunc(w[i] * portfolio.equity() / price)   [toward zero]
//    trade = target_shares - portfolio.holding(universe[i]).qty
//    if trade != 0: emit OrderPayload{ id, qty=trade, Market, limit={}, now }.
//  A zero weight -> target 0 -> the trade closes the position. A long->short
//  flip emits the full delta crossing zero in a single order (sign encodes
//  direction per exec/payloads.hpp). Already-on-target -> trade 0 -> no order.
//
//  Share count is f64 math (w * equity / price are all f64 market quantities,
//  not ledger money) TRUNCATED toward zero to an integer i64 — fractional shares
//  are dropped, never rounded up into capital we do not have. `limit` is an empty
//  (zero) Decimal: Market orders ignore it (see OrderPayload).
//
// ===========================================================================
//  Allocation / return types
// ===========================================================================
//  to_target_weights returns std::vector<f64> (length == universe size);
//  reconcile returns std::vector<OrderPayload> (length is RUNTIME, <= universe
//  size — one order per instrument that needs a trade). Both ALLOCATE once per
//  call. That is acceptable because both run at REBALANCE cadence (not per bar):
//  the rebalance is already the heavy decision step. core::container::fixed_vector
//  is deliberately NOT used — its capacity is a compile-time template parameter
//  and the universe size is a runtime value. A caller-provided-scratch overload
//  (to make these allocation-free) is a tracked deferred residual in the ledger.
//
//  Both methods are const: a WeightPolicy is a pure configuration object (the
//  policy knobs) and holds no mutable state.

#include <algorithm> // std::clamp, std::sort, std::unique (truncate + group-demean)
#include <cmath>     // std::isnan
#include <span>      // std::span (weights / order inputs)
#include <vector>    // std::vector (per-rebalance weights + order list)

#include "atx/core/macro.hpp"               // ATX_ASSERT
#include "atx/core/stats/cross_section.hpp" // rank, zscore (the cross-section primitives)
#include "atx/core/types.hpp"               // f64, i64, u8, usize

#include "atx/engine/exec/payloads.hpp"       // OrderPayload, OrderType
#include "atx/engine/loop/market.hpp"         // Market (mark source)
#include "atx/engine/loop/signal_source.hpp"  // SignalView (the input)
#include "atx/engine/loop/types.hpp"          // InstrumentId, Universe
#include "atx/engine/portfolio/portfolio.hpp" // Portfolio (current positions + equity)

namespace atx::engine {

// ===========================================================================
//  Transform — the cross-sectional normalization applied to the raw signal.
//
//  Closed taxonomy (u8 underlying, ring-discipline style): switches over it are
//  exhaustive with NO `default`, so adding an enumerator surfaces as a /W4
//  warning at every switch rather than a silent fall-through.
//
//    Rank   = normalized ordinal rank in [0,1] (WorldQuant default; magnitude-
//             discarding, tie-averaged — see apply_transform).
//    ZScore = population z-score (preserves relative magnitudes; needs the
//             winsorize guard since one outlier inflates the std).
//    Raw    = IDENTITY PASSTHROUGH — no cross-sectional transform at all. The
//             expression's OWN conditioning (zscore/signedpower/decay baked into
//             the DSL) survives untouched to the demean+gross-normalize tail,
//             instead of being overwritten by a Rank/ZScore re-standardization.
//             With winsorize_limit=0 (band == full range == no-op) and the
//             default dollar_neutral=true / gross_leverage=1.0, Raw reduces
//             to EXACTLY demean-then-L1-normalize over the live cells — the
//             manual alpha101 book.
//
//  APPEND-ONLY: the underlying u8 values are part of the serialized/index
//  contract — Rank=0, ZScore=1 are FROZEN. New enumerators MUST be appended last
//  (Raw=2) so existing serialized values keep their meaning.
// ===========================================================================
enum class Transform : atx::u8 { Rank, ZScore, Raw }; // closed; no `default` in switches

// ===========================================================================
//  WeightPolicy — signal -> target weights + trade list. Pure configuration.
// ===========================================================================
struct WeightPolicy {
  Transform transform = Transform::Rank; // cross-sectional normalization
  bool industry_neutral = false;         // P4-8: per-group demean (needs group_map); see header
  bool dollar_neutral = true;            // center so Sigma w = 0
  atx::f64 gross_leverage = 1.0;         // target Sigma|w| (Alpha101 `scale`)

  // P4-8 Truncation (BRAIN overfitting guard): per-name absolute cap on |w_i| in
  // the FINAL gross-normalized units. 0.0 = OFF (the default, like every knob
  // here — Phase-5 calibrates); typical 0.01–0.10. Applied via a fixed-iteration
  // clip-renorm so the final weights respect the cap while Σ|w| holds at
  // gross_leverage for a FEASIBLE cap (see header for the infeasible degenerate).
  atx::f64 truncation = 0.0;

  // Symmetric outlier-clamp fraction applied to the live cross-section BEFORE the
  // transform: scores are winsorized into the nearest-rank quantile band
  // [winsorize_limit, 1 - winsorize_limit]. Default 0.025 = a standard 95%
  // winsorization (clamp the extreme 2.5% on each tail), the conventional robust-
  // stats default and a near-no-op for Rank but the outlier guard ZScore needs.
  // A knob, like every coefficient here — Phase-5 calibrates it. 0.0 disables
  // winsorization (band == full range). PRECONDITION: 0 <= winsorize_limit <= 0.5.
  atx::f64 winsorize_limit = 0.025;

  /// Map a cross-sectional signal to index-aligned target weights over
  /// `universe`. NaN / out-of-cross-section scores get exactly 0 weight and are
  /// excluded from the transform and reductions. See the header for the full
  /// pipeline + degenerate-case contract. Returns a vector of universe size
  /// (allocates once per rebalance).
  ///
  /// `group_map` (P4-8) is the per-universe-instrument industry/sector group id
  /// (universe-aligned, length == universe size). It is REQUIRED when
  /// `industry_neutral` is on (group-neutralize) and IGNORED otherwise; an empty
  /// span (the default) means group-neutralize OFF. With industry_neutral=false
  /// AND truncation=0.0 AND an empty group_map (all defaults) NONE of the P4-8
  /// branches run, so the output is BYTE-IDENTICAL to the Phase-2 pipeline.
  [[nodiscard]] std::vector<atx::f64>
  to_target_weights(SignalView signal, const Universe &universe,
                    std::span<const atx::u32> group_map = {}) const {
    ATX_ASSERT(signal.values.size() == universe.size());

    const atx::usize n = universe.size();
    // Fail-closed (matching the Phase-2 intent): group-neutralize needs a
    // universe-aligned group map, so a misconfigured industry_neutral without one
    // aborts in debug rather than silently no-op'ing (see header).
    ATX_ASSERT(!industry_neutral || group_map.size() == n);
    std::vector<atx::f64> weights(n, 0.0);
    if (n == 0U) {
      return weights;
    }

    // --- compact the live (non-NaN) scores into a dense buffer -------------
    // `live_idx[k]` is the universe index of the k-th live score; the dense
    // buffer feeds the cross-section primitive (which has no NaN handling).
    std::vector<atx::usize> live_idx;
    std::vector<atx::f64> dense;
    live_idx.reserve(n);
    dense.reserve(n);
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 v = signal.values[i];
      if (!std::isnan(v)) {
        live_idx.push_back(i);
        dense.push_back(v);
      }
    }
    if (dense.empty()) {
      return weights; // no opinions anywhere -> all zero
    }

    // Clamp tail outliers BEFORE transforming so a single extreme score cannot
    // dominate the standardization (material for ZScore; near-no-op for Rank).
    ATX_ASSERT(winsorize_limit >= 0.0 && winsorize_limit <= 0.5);
    atx::core::stats::winsorize(std::span<atx::f64>{dense}, winsorize_limit, 1.0 - winsorize_limit);

    apply_transform(dense);
    // P4-8 GROUP-neutralize (CsDemeanG / §0-H semantics): demean WITHIN each group
    // before the global dollar-neutralize so the book carries no per-group net
    // exposure. Inert when industry_neutral is off (the default). After this each
    // group sums to ~0, so the subsequent global demean is a ~no-op.
    if (industry_neutral) {
      group_demean(dense, live_idx, group_map);
    }
    if (dollar_neutral) {
      atx::core::stats::demean(std::span<atx::f64>{dense}); // Sigma dense = 0
    }
    // gross-normalize so Sigma|dense| == gross_leverage (Alpha101 `scale`). This
    // runs UNCONDITIONALLY (preserving the bit-identical-when-off path) AND puts
    // the weights into the FINAL gross-normalized units the truncation cap is
    // specified in — so the per-name cap compares apples-to-apples.
    gross_normalize(dense);
    // P4-8 TRUNCATE: cap |w_i| at `truncation` (now in gross-normalized units) via
    // a fixed-iteration clip-renorm (inert when truncation == 0). It re-normalizes
    // back to gross_leverage each pass and ENDS ON A CLIP so the cap is hard-
    // binding in the returned weights; for an infeasible cap Σ|w| settles BELOW
    // gross_leverage (the cap wins — see header). No gross_normalize follows: that
    // would rescale the pinned weights and breach the cap.
    if (truncation > 0.0) {
      truncate_renorm(dense);
    }

    // --- scatter the dense weights back to universe positions --------------
    for (atx::usize k = 0; k < live_idx.size(); ++k) {
      weights[live_idx[k]] = dense[k];
    }
    return weights;
  }

  /// order_target_percent reconcile: emit the trade list that moves the current
  /// book toward weights `w` (index-aligned to `universe`). One order per
  /// instrument whose target differs from its current position; iterated in
  /// fixed universe-index order (determinism). Skips instruments with a NaN or
  /// non-positive mark. See the header for the share-count + truncation
  /// contract. Allocates the order vector once per rebalance.
  [[nodiscard]] std::vector<exec::OrderPayload>
  reconcile(std::span<const atx::f64> w, const Universe &universe, const Portfolio &portfolio,
            const Market &market, atx::core::time::Timestamp now) const {
    ATX_ASSERT(w.size() == universe.size());

    const atx::f64 equity = portfolio.equity();
    std::vector<exec::OrderPayload> orders;
    orders.reserve(universe.size());

    for (atx::usize i = 0; i < universe.size(); ++i) {
      const InstrumentId id = universe[i];
      const atx::f64 price = market.mark(id);
      if (std::isnan(price) || price <= 0.0) {
        continue; // unpriced / non-positive mark -> no priceable target
      }
      // f64 share math (market quantities, not ledger money) truncated toward
      // zero: never round a fractional share up into capital we lack.
      const atx::i64 target_shares = static_cast<atx::i64>(w[i] * equity / price);
      const atx::i64 current = portfolio.holding(id).qty;
      const atx::i64 trade = target_shares - current;
      if (trade != 0) {
        orders.push_back(
            exec::OrderPayload{id, trade, exec::OrderType::Market, atx::core::Decimal{}, now});
      }
    }
    return orders;
  }

private:
  /// Apply the configured cross-section transform IN PLACE to the dense live
  /// buffer. Exhaustive over Transform (no `default`): a new enumerator is a /W4
  /// warning here. Rank/ZScore are out-of-place primitives in atx-core, so we
  /// transform through a same-size scratch and swap it in. Raw is the IDENTITY
  /// passthrough: it must leave `dense` EXACTLY as winsorize produced it, so it
  /// returns BEFORE the scratch is allocated/swapped — swapping in the empty
  /// `out` would clobber the raw scores with zeros.
  void apply_transform(std::vector<atx::f64> &dense) const {
    if (transform == Transform::Raw) {
      return; // identity: keep the winsorized raw scores untouched
    }
    std::vector<atx::f64> out(dense.size());
    switch (transform) {
    case Transform::Rank:
      atx::core::stats::rank(std::span<const atx::f64>{dense}, std::span<atx::f64>{out});
      break;
    case Transform::ZScore:
      atx::core::stats::zscore(std::span<const atx::f64>{dense}, std::span<atx::f64>{out});
      break;
    case Transform::Raw:
      return; // unreachable (handled above); the case keeps the switch exhaustive
    }
    dense.swap(out);
  }

  /// Scale the dense buffer so Sigma|dense| == gross_leverage (Alpha101 `scale`).
  /// Guards the all-zero case (demeaned constant / single name): with Sigma|w|==0
  /// there is no finite scale, so the buffer is left at zero rather than dividing
  /// by zero.
  void gross_normalize(std::vector<atx::f64> &dense) const noexcept {
    atx::f64 l1 = 0.0;
    for (const atx::f64 x : dense) {
      l1 += (x < 0.0) ? -x : x;
    }
    if (l1 == 0.0) {
      return; // all-zero (degenerate) -> leave flat, no div-by-zero
    }
    const atx::f64 scale = gross_leverage / l1;
    for (atx::f64 &x : dense) {
      x *= scale;
    }
  }

  /// P4-8 GROUP-neutralize: demean the dense live buffer WITHIN each group (DSL
  /// `CsDemeanG` semantics). `dense[k]` is the transformed score of the live
  /// instrument at universe index `live_idx[k]`, whose group is
  /// `group_map[live_idx[k]]`. DETERMINISTIC, order-fixed: distinct groups are
  /// enumerated in ASCENDING id order (sorted/unique over the live group ids, NOT
  /// an unordered_map iteration) and each group's mean is subtracted from its
  /// members in ascending-k order — no RNG, no hash-order reduction. After the
  /// call each group sums to ~0. group_map is universe-aligned and asserted to
  /// cover the universe by the caller when industry_neutral is on.
  void group_demean(std::vector<atx::f64> &dense, const std::vector<atx::usize> &live_idx,
                    std::span<const atx::u32> group_map) const {
    const atx::usize live = dense.size();
    // Distinct live group ids, ascending — the deterministic enumeration order.
    std::vector<atx::u32> groups;
    groups.reserve(live);
    for (atx::usize k = 0; k < live; ++k) {
      groups.push_back(group_map[live_idx[k]]);
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    // For each distinct group (ascending): two passes — accumulate its mean over
    // the dense buffer, then subtract it from every member (ascending k).
    for (const atx::u32 g : groups) {
      atx::f64 sum = 0.0;
      atx::usize count = 0;
      for (atx::usize k = 0; k < live; ++k) {
        if (group_map[live_idx[k]] == g) {
          sum += dense[k];
          ++count;
        }
      }
      const atx::f64 mean = sum / static_cast<atx::f64>(count); // count >= 1 (g is live)
      for (atx::usize k = 0; k < live; ++k) {
        if (group_map[live_idx[k]] == g) {
          dense[k] -= mean;
        }
      }
    }
  }

  /// P4-8 TRUNCATE: cap |dense[k]| at `truncation` via a FIXED-iteration clip-
  /// renorm (determinism §3.2 — NO convergence-dependent early exit). Each pass
  /// clips every element into [-truncation, +truncation] then re-normalizes
  /// Σ|dense| back to gross_leverage. `dense` arrives ALREADY gross-normalized, so
  /// the cap is compared in the FINAL weight units. The loop redistributes the
  /// mass freed by a clipped name onto the unbinding names; after the fixed passes
  /// the set of binding (at-cap) names has stabilized. A FINAL clip then makes the
  /// cap HARD-binding (|dense| <= cap exactly), and a FINAL deficit-renorm scales
  /// ONLY the unbinding names so Σ|dense| == gross_leverage EXACTLY for a feasible
  /// cap — without ever pushing a pinned name back over the cap. For an INFEASIBLE
  /// cap (truncation·n_active < gross_leverage) EVERY name pins at the cap so there
  /// is no unbinding mass to absorb the deficit and Σ|dense| settles BELOW
  /// gross_leverage — the cap wins (the documented degenerate; no div-by-zero). The
  /// all-zero buffer is a no-op.
  void truncate_renorm(std::vector<atx::f64> &dense) const noexcept {
    const atx::f64 cap = truncation;
    // Fixed clip-renorm passes (no convergence early-exit) settle WHICH names bind.
    for (atx::usize iter = 0; iter < kTruncateIters; ++iter) {
      for (atx::f64 &x : dense) {
        x = std::clamp(x, -cap, cap);
      }
      atx::f64 l1 = 0.0;
      for (const atx::f64 x : dense) {
        l1 += (x < 0.0) ? -x : x;
      }
      if (l1 == 0.0) {
        return; // all-zero -> nothing to renorm / no div-by-zero
      }
      const atx::f64 scale = gross_leverage / l1;
      for (atx::f64 &x : dense) {
        x *= scale;
      }
    }
    finalize_truncation(dense, cap);
  }

  /// Final exact pass for truncate_renorm: clip every name HARD to the cap, then
  /// pour the remaining gross budget onto the UNBINDING (sub-cap) names alone so
  /// Σ|dense| == gross_leverage exactly without reopening the cap. Splitting the
  /// budget into the pinned mass + the unbinding mass keeps the binding names at
  /// EXACTLY the cap (the `Σ|w| ≈ L` and `|w| ≤ cap` invariants both hold to
  /// rounding for a feasible cap); an infeasible cap leaves no unbinding mass, so
  /// `target_unbound <= 0` and the budget cannot be met — Σ|dense| < gross_leverage
  /// and the cap wins (documented degenerate).
  void finalize_truncation(std::vector<atx::f64> &dense, atx::f64 cap) const noexcept {
    atx::f64 pinned = 0.0;  // Σ|w| over names clamped at the cap
    atx::f64 unbound = 0.0; // Σ|w| over names strictly below the cap
    for (atx::f64 &x : dense) {
      x = std::clamp(x, -cap, cap);
      ((x < 0.0 ? -x : x) >= cap ? pinned : unbound) += (x < 0.0) ? -x : x;
    }
    const atx::f64 target_unbound = gross_leverage - pinned;
    // No unbinding mass, or the cap already consumes the whole budget -> leave as
    // clipped (infeasible degenerate: Σ|w| < gross_leverage). Guard div-by-zero.
    if (unbound <= 0.0 || target_unbound <= 0.0) {
      return;
    }
    const atx::f64 scale = target_unbound / unbound;
    for (atx::f64 &x : dense) {
      if ((x < 0.0 ? -x : x) < cap) {
        x *= scale; // only the sub-cap names absorb the deficit
      }
    }
  }

  /// Fixed clip-renorm pass count (determinism: a named constant, no early exit).
  /// 8 passes settle the binding set; finalize_truncation then makes |w| <= cap and
  /// Σ|w| == L exact for a feasible cap (no tolerance reliance on convergence).
  static constexpr atx::usize kTruncateIters = 8;
};

} // namespace atx::engine

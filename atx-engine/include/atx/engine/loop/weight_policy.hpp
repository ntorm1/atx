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
//  Default policy is the canonical long/short construction:
//
//    winsorize (clamp tail outliers)  ->  transform (rank | zscore)
//      ->  dollar-neutralize (subtract mean, Sigma w=0)
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
//  industry_neutral: INERT in Phase-2. No industry-group map exists yet (that is
//  Phase-4's Barra/group layer). The field is kept for forward-compat so the
//  data structure does not reshape later, but enabling it would silently do
//  nothing, which is worse than failing — so to_target_weights ATX_ASSERTs it is
//  false rather than pretending to group-demean. Remove the assert and implement
//  group-demeaning when the group map lands.
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

#include <cmath>  // std::isnan
#include <span>   // std::span (weights / order inputs)
#include <vector> // std::vector (per-rebalance weights + order list)

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
// ===========================================================================
enum class Transform : atx::u8 { Rank, ZScore }; // closed; no `default` in switches

// ===========================================================================
//  WeightPolicy — signal -> target weights + trade list. Pure configuration.
// ===========================================================================
struct WeightPolicy {
  Transform transform = Transform::Rank; // cross-sectional normalization
  bool industry_neutral = false;         // INERT in Phase-2 (asserted false; see header)
  bool dollar_neutral = true;            // center so Sigma w = 0
  atx::f64 gross_leverage = 1.0;         // target Sigma|w| (Alpha101 `scale`)

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
  [[nodiscard]] std::vector<atx::f64> to_target_weights(SignalView signal,
                                                        const Universe &universe) const {
    ATX_ASSERT(signal.values.size() == universe.size());
    // industry_neutral has no group map in Phase-2; enabling it would silently
    // no-op, so fail closed rather than mislead (see header). Remove with the
    // Phase-4 group layer.
    ATX_ASSERT(!industry_neutral);

    const atx::usize n = universe.size();
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
    if (dollar_neutral) {
      atx::core::stats::demean(std::span<atx::f64>{dense}); // Sigma dense = 0
    }
    gross_normalize(dense); // Sigma|dense| = gross_leverage (or untouched if 0)

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
  /// warning here. Both primitives are out-of-place in atx-core, so we transform
  /// through a same-size scratch and copy back.
  void apply_transform(std::vector<atx::f64> &dense) const {
    std::vector<atx::f64> out(dense.size());
    switch (transform) {
    case Transform::Rank:
      atx::core::stats::rank(std::span<const atx::f64>{dense}, std::span<atx::f64>{out});
      break;
    case Transform::ZScore:
      atx::core::stats::zscore(std::span<const atx::f64>{dense}, std::span<atx::f64>{out});
      break;
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
};

} // namespace atx::engine

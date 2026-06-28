#pragma once

// atx::engine::alpha — AlphaStreams + extract_streams: per-alpha PnL / position
// stream extraction (P3c-2). The typed Phase-3 -> Phase-4 handoff.
//
// ===========================================================================
//  What this unit does
// ===========================================================================
//  Phase 3 produces a SignalSet: one named alpha per Program root, each a
//  date-major f64 matrix (dates x instruments, NaN where masked). The Phase-4
//  combiner does NOT consume raw signals — it consumes, per alpha, a realized
//  PnL stream and the position (target-weight) stream that produced it.
//
//  extract_streams turns each alpha's signal cross-section into those two
//  streams by REUSING the existing Phase-2 portfolio glue — it adds NO new
//  portfolio / P&L logic (anti-roadmap; plan §10 watch-item):
//
//    * positions  = loop::WeightPolicy::to_target_weights(signal_row, universe)
//                   (winsorize -> rank/zscore -> dollar-neutral -> gross-scale),
//                   the SAME construction the backtest loop applies each
//                   rebalance.
//    * pnl[t]      = Σ_j w_j[t-1] · ret_j[t]  −  turnover[t] · cost_rate
//                   where ret_j[t] = close_j[t]/close_j[t-1] − 1 and
//                   turnover[t] = Σ_j |w_j[t] − w_j[t-1]|. The cost_rate is the
//                   ExecutionSimulator's per-notional commission rate (see the
//                   COST MODEL note); 0 recovers the frictionless return.
//
// ===========================================================================
//  ALIGNMENT — no look-ahead (w[t-1] earns ret[t])
// ===========================================================================
//  Period index t runs 0 .. n_dates-1. positions[t] is the target weight from
//  date t's signal cross-section. The realized return BOOKED at period t is the
//  PRIOR period's weights times THIS period's instrument return:
//    pnl[t] = Σ_j w_j[t-1] · ret_j[t]  for t >= 1,   pnl[0] = 0.
//  This is the no-look-ahead alignment: the weight set on date t-1 is what is
//  held into date t and earns date t's price move. period 0 has no prior weight
//  and no prior price, so its booked return is exactly 0 (positions[0] is still
//  the date-0 target — it is simply not yet earning).
//
// ===========================================================================
//  COST MODEL — reuse, not reimplement
// ===========================================================================
//  The full ExecutionSimulator drives a bar-by-bar order->queue->settle FIFO
//  loop with slippage / impact / volume-cap. That machinery is per-bar
//  execution detail; a research-cadence per-alpha stream does NOT replay it.
//  Instead extract_streams reads ONE coefficient out of the SAME sim — its
//  PerDollar commission rate (CommissionCfg.per_dollar_bps, basis points on
//  notional) — and charges a lightweight turnover cost
//    cost[t] = Σ_j |w_j[t] − w_j[t-1]| · (per_dollar_bps / 1e4).
//  Rationale: weights are notional fractions (Σ|w| = gross_leverage), so
//  |Δw| IS the traded-notional fraction, and a per-dollar (PerDollar) commission
//  is exactly a linear charge on that notional — this reuses the sim's own
//  coefficient with no new cost formula. PerShare commission, slippage and
//  impact are share-/participation-scaled and have no closed per-turnover form
//  at weight granularity, so they are NOT modelled here (documented residual);
//  cost_rate is taken only from the PerDollar bps field. With costs OFF
//  (per_dollar_bps == 0, the frictionless config) cost[t] == 0 and the stream
//  is the pure analytic Σ w·ret — the bit-exact loop-match case (see the test).
//
// ===========================================================================
//  Storage / shape
// ===========================================================================
//  Dense, id-aligned to the SignalSet (stream index i == alpha i). pnl is
//  [n_alphas][n_periods]; positions is [n_alphas][n_periods][n_instruments],
//  flat-packed. n_periods == panel.dates(), n_instruments == panel.instruments().
//  Allocate-once-per-build (cold research path — the WeightPolicy precedent);
//  no hot loop. extract_streams returns a Result and Err's (never throws) on a
//  shape mismatch (SignalSet vs panel dates/instruments disagreement).

#include <cmath>   // std::isnan, std::abs (return / turnover guards)
#include <span>    // std::span (the non-owning stream accessors)
#include <utility> // std::move (Result hand-off)
#include <vector>  // std::vector (owned dense stream storage)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/alpha/panel.hpp"        // alpha::Panel, alpha::SignalSet
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator, CommissionCfg
#include "atx/engine/loop/signal_source.hpp" // SignalView
#include "atx/engine/loop/types.hpp"         // Universe, InstrumentId
#include "atx/engine/loop/weight_policy.hpp" // WeightPolicy

namespace atx::engine::alpha {

// ===========================================================================
//  AlphaStreams — the dense per-alpha PnL + position streams (Phase-4 input).
//
//  OWNS its storage by value (Rule of Zero). The accessors return non-owning
//  spans into that storage — valid for the lifetime of the AlphaStreams. Built
//  only via extract_streams (the members are public for aggregate assembly there
//  but are sized/filled coherently by that one builder).
// ===========================================================================
struct AlphaStreams {
  // Flat [n_alphas * n_periods] realized-return stream, alpha-major.
  std::vector<atx::f64> pnl_flat;
  // Flat [n_alphas * n_periods * n_instruments] target-weight stream,
  // alpha-major then period-major then instrument-minor.
  std::vector<atx::f64> pos_flat;
  atx::usize n_alphas_{};
  atx::usize n_periods_{};
  atx::usize n_instruments_{};

  /// The realized-return stream for `alpha` (length == n_periods()).
  /// PRECONDITION: alpha < n_alphas() (ABORTS in debug).
  [[nodiscard]] std::span<const atx::f64> pnl(atx::usize alpha) const noexcept {
    ATX_ASSERT(alpha < n_alphas_);
    // SAFETY: alpha < n_alphas_ (asserted) and pnl_flat holds exactly
    //         n_alphas_ * n_periods_ cells, so [alpha*n_periods_, +n_periods_)
    //         lies wholly inside the allocation.
    return std::span<const atx::f64>{pnl_flat.data() + alpha * n_periods_, n_periods_};
  }

  /// The target-weight cross-section for `alpha` at `period` (length ==
  /// n_instruments()). PRECONDITION: alpha < n_alphas() and period < n_periods()
  /// (ABORTS in debug).
  [[nodiscard]] std::span<const atx::f64> positions(atx::usize alpha,
                                                    atx::usize period) const noexcept {
    ATX_ASSERT(alpha < n_alphas_);
    ATX_ASSERT(period < n_periods_);
    // SAFETY: the two indices are asserted in range; pos_flat holds exactly
    //         n_alphas_ * n_periods_ * n_instruments_ cells, so the computed
    //         offset + n_instruments_ stays inside the allocation.
    const atx::usize off = (alpha * n_periods_ + period) * n_instruments_;
    return std::span<const atx::f64>{pos_flat.data() + off, n_instruments_};
  }

  [[nodiscard]] atx::usize n_alphas() const noexcept { return n_alphas_; }
  [[nodiscard]] atx::usize n_periods() const noexcept { return n_periods_; }
  [[nodiscard]] atx::usize n_instruments() const noexcept { return n_instruments_; }
};

namespace detail {

// Per-notional cost rate extracted from the SAME ExecutionSimulator the loop
// uses: the PerDollar commission's basis points on notional, as a fraction.
// PerShare / slippage / impact are share-/participation-scaled and have no
// closed per-turnover form at weight granularity (documented residual), so the
// turnover charge is keyed only off per_dollar_bps. A frictionless sim
// (per_dollar_bps == 0) yields 0 -> the pure analytic stream.
[[nodiscard]] inline atx::f64 turnover_cost_rate(const exec::ExecutionSimulator &sim) noexcept {
  const exec::CommissionCfg &c = sim.commission_cfg();
  return (c.mode == exec::CommissionMode::PerDollar) ? (c.per_dollar_bps / 1e4) : 0.0;
}

// Instrument return ret_j[t] = close_j[t]/close_j[t-1] − 1, guarding a NaN or
// non-positive prior/current close as a 0 contribution (out-of-universe / not-
// yet-listed cells read NaN; a 0 or NaN denominator is not a valid return).
[[nodiscard]] inline atx::f64 instrument_return(atx::f64 prev_close, atx::f64 cur_close) noexcept {
  if (std::isnan(prev_close) || std::isnan(cur_close) || prev_close <= 0.0) {
    return 0.0;
  }
  return cur_close / prev_close - 1.0;
}

} // namespace detail

// Forward declaration: the per-alpha inner build (defined after extract_streams).
inline void fill_alpha_stream(AlphaStreams &out, atx::usize i, const SignalSet &signals,
                              const WeightPolicy &policy, const Universe &universe,
                              std::span<const atx::f64> close, atx::f64 cost_rate,
                              std::span<const atx::u32> group_map = {});

// ===========================================================================
//  extract_streams — SignalSet -> per-alpha PnL + position streams.
//
//  REUSES WeightPolicy (positions) + the ExecutionSimulator cost coefficient
//  (turnover charge). Adds no portfolio / P&L logic. `panel` MUST be the same
//  panel the SignalSet was evaluated over: its Close field gives the per-period
//  instrument returns and its per-date universe mask the live set. Returns Err
//  on a shape mismatch (SignalSet dates/instruments != panel dates/instruments,
//  or the panel has no Close field) — never throws.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<AlphaStreams>
extract_streams(const SignalSet &signals, const WeightPolicy &policy, const Panel &panel,
                const exec::ExecutionSimulator &sim,
                std::span<const atx::u32> group_map = {}) {
  const atx::usize dates = panel.dates();
  const atx::usize insts = panel.instruments();
  if (signals.dates != dates || signals.instruments != insts) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "extract_streams: SignalSet shape disagrees with panel shape");
  }
  ATX_TRY(const FieldId close_id, panel.field_id("close"));

  const atx::usize n_alphas = signals.alphas.size();
  const atx::f64 cost_rate = detail::turnover_cost_rate(sim);

  AlphaStreams out;
  out.n_alphas_ = n_alphas;
  out.n_periods_ = dates;
  out.n_instruments_ = insts;
  out.pnl_flat.assign(n_alphas * dates, 0.0);
  out.pos_flat.assign(n_alphas * dates * insts, 0.0);

  // The universe id span is required by to_target_weights only for its size
  // (cross-section index alignment) and by reconcile (unused here). A synthetic
  // contiguous id span of the right length satisfies the index-alignment
  // contract: weights are positional (weight[i] <-> cross-section index i), and
  // to_target_weights never dereferences an id. Built once, reused per row.
  std::vector<InstrumentId> universe_ids(insts);
  for (atx::usize j = 0; j < insts; ++j) {
    universe_ids[j] = InstrumentId{static_cast<atx::u32>(j)};
  }
  const Universe universe{universe_ids};

  const std::span<const atx::f64> close = panel.field_all(close_id);

  for (atx::usize i = 0; i < n_alphas; ++i) {
    fill_alpha_stream(out, i, signals, policy, universe, close, cost_rate, group_map);
  }
  return atx::core::Ok(std::move(out));
}

// ===========================================================================
//  fill_alpha_stream — one alpha's positions + pnl (the per-alpha inner build).
//
//  Split out of extract_streams so each function stays single-purpose and
//  short (agent §3). Writes alpha `i`'s slice of out.pos_flat / out.pnl_flat.
// ===========================================================================
inline void fill_alpha_stream(AlphaStreams &out, atx::usize i, const SignalSet &signals,
                              const WeightPolicy &policy, const Universe &universe,
                              std::span<const atx::f64> close, atx::f64 cost_rate,
                              std::span<const atx::u32> group_map) {
  const atx::usize dates = out.n_periods_;
  const atx::usize insts = out.n_instruments_;

  // 1. positions[t] = to_target_weights(signal_row(t)) for every date.
  //
  // WHY hoisted buffers: the allocating overload of to_target_weights allocated
  // weights, live_idx, dense, AND a transform temp on EVERY call (~4 heap allocs
  // × dates per alpha). By hoisting all four above the loop and passing them to
  // the scratch overload, the allocator is hit only during warmup (until the live
  // count reaches its run high-water mark); subsequent calls reuse the existing
  // capacity — amortized O(1) allocs per alpha instead of O(dates). The transform
  // temp is hoisted too because apply_transform swaps it into `dense`; without a
  // reused temp the swap would shrink `dense`'s capacity to the live count and
  // re-allocate on most dates (variable NaN patterns). See the to_target_weights
  // scratch-overload contract for the ping-pong detail.
  // Output is byte-identical: the scratch overload zero-fills weights each call
  // and clears live_idx/dense before compacting, exactly matching the original
  // fresh-allocation semantics.
  std::vector<atx::f64> w;          // scratch: target weights (resized/zeroed each date)
  std::vector<atx::usize> live_idx; // scratch: compact live-instrument index (cleared each date)
  std::vector<atx::f64> dense;      // scratch: compact live-score buffer (cleared each date)
  std::vector<atx::f64> tform_tmp;  // scratch: rank/zscore out-of-place temp (ping-pongs w/ dense)

  for (atx::usize t = 0; t < dates; ++t) {
    const SignalView row{signals.alpha_cross_section(i, t)};
    policy.to_target_weights(row, universe, w, live_idx, dense, tform_tmp, group_map);
    const atx::usize off = (i * dates + t) * insts;
    // SAFETY: off + insts <= pos_flat.size() — off = (i*dates+t)*insts with
    //         i<n_alphas, t<dates, and pos_flat sized n_alphas*dates*insts.
    for (atx::usize j = 0; j < insts; ++j) {
      out.pos_flat[off + j] = w[j];
    }
  }

  // 2. pnl[t] = Σ_j w_j[t-1]·ret_j[t] − turnover[t]·cost_rate, for t >= 1.
  //    pnl[0] == 0 (no prior weight / price). w[t-1] earns ret[t] (no look-ahead).
  for (atx::usize t = 1; t < dates; ++t) {
    const std::span<const atx::f64> prev = out.positions(i, t - 1);
    const std::span<const atx::f64> cur = out.positions(i, t);
    atx::f64 gross = 0.0;
    atx::f64 turnover = 0.0;
    for (atx::usize j = 0; j < insts; ++j) {
      const atx::f64 ret =
          detail::instrument_return(close[(t - 1) * insts + j], close[t * insts + j]);
      gross += prev[j] * ret;
      const atx::f64 dw = cur[j] - prev[j];
      turnover += (dw < 0.0) ? -dw : dw;
    }
    out.pnl_flat[i * dates + t] = gross - turnover * cost_rate;
  }
}

} // namespace atx::engine::alpha

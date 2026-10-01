#include "atx/engine/alpha/streams.hpp"

namespace atx::engine::alpha {

namespace detail {

// Per-notional cost rate extracted from the SAME ExecutionSimulator the loop
// uses: the PerDollar commission's basis points on notional, as a fraction.
// PerShare / slippage / impact are share-/participation-scaled and have no
// closed per-turnover form at weight granularity (documented residual), so the
// turnover charge is keyed only off per_dollar_bps. A frictionless sim
// (per_dollar_bps == 0) yields 0 -> the pure analytic stream.
[[nodiscard]] atx::f64 turnover_cost_rate(const exec::ExecutionSimulator &sim) noexcept {
  const exec::CommissionCfg &c = sim.commission_cfg();
  return (c.mode == exec::CommissionMode::PerDollar) ? (c.per_dollar_bps / 1e4) : 0.0;
}

// Instrument return ret_j[t] = close_j[t]/close_j[t-1] − 1, guarding a NaN or
// non-positive prior/current close as a 0 contribution (out-of-universe / not-
// yet-listed cells read NaN; a 0 or NaN denominator is not a valid return).
[[nodiscard]] atx::f64 instrument_return(atx::f64 prev_close, atx::f64 cur_close) noexcept {
  if (std::isnan(prev_close) || std::isnan(cur_close) || prev_close <= 0.0) {
    return 0.0;
  }
  return cur_close / prev_close - 1.0;
}

} // namespace detail

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
[[nodiscard]] atx::core::Result<AlphaStreams>
extract_streams(const SignalSet &signals, const WeightPolicy &policy, const Panel &panel,
                const exec::ExecutionSimulator &sim,
                std::span<const atx::u32> group_map) {
  if (!sim.configuration_valid()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "extract_streams: invalid execution configuration");
  }
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
void fill_alpha_stream(AlphaStreams &out, atx::usize i, const SignalSet &signals,
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

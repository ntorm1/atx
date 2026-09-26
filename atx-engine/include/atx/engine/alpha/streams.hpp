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
#include <string>
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

  // DelayedSurfaceV2 only: full calendar, alpha-major diagnostic arrays. Empty
  // on the preserved legacy path. All return/cost fractions use interval-entry
  // pretrade NAV; turnover is actual one-way filled dollars / that same NAV.
  std::vector<atx::f64> gross_flat{}, execution_cost_flat{}, borrow_cost_flat{};
  std::vector<atx::f64> turnover_flat{}, pretrade_nav_flat{}, end_nav_flat{};
  std::vector<atx::u8> valid_flat{};
  std::vector<atx::usize> names_flat{}, capped_names_flat{};
  std::string execution_context_sha256{};
  atx::usize first_realization_{}, realization_end_{};


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
[[nodiscard]] atx::f64 turnover_cost_rate(const exec::ExecutionSimulator& sim) noexcept;
[[nodiscard]] atx::f64 instrument_return(atx::f64 previous,atx::f64 current) noexcept;
} // namespace detail

[[nodiscard]] atx::core::Result<AlphaStreams> extract_streams(
    const SignalSet& signals,const WeightPolicy& policy,const Panel& panel,
    const exec::ExecutionSimulator& sim,std::span<const atx::u32> group_map={});
void fill_alpha_stream(AlphaStreams& out,atx::usize i,const SignalSet& signals,
    const WeightPolicy& policy,const Universe& universe,std::span<const atx::f64> close,
    atx::f64 cost_rate,std::span<const atx::u32> group_map={});
} // namespace atx::engine::alpha

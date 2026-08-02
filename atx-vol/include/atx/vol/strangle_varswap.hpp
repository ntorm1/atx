#pragma once

// atx-vol XOM strangle-vs-varswap comparison backtest — the strategy that runs
// both sides of the comparison on one clock.
//
// TASK 1 SCOPE: the OPTIONS leg only. A FIXED-EXPIRY, DAILY-RESTRIKE strangle:
//
//   * a CYCLE fixes ONE expiry, taken off the run's session grid, and holds it;
//   * every step inside the cycle CLOSES both wings and REOPENS them at freshly
//     resolved +/-`target_abs_delta` strikes on that step's surface, at the SAME
//     expiry and the same quantity (the engine books the before/after book diff
//     as a roll-close plus an entry, both at today's marks);
//   * at the cycle's expiry session the engine settles the pair at intrinsic and
//     the next cycle is fixed on that same step, while sessions remain.
//
// This isolates the strangle's SPOT/vol-path exposure from calendar drift: an
// ordinary rolling-tenor strangle changes BOTH its strikes and its maturity every
// day, which is precisely the confound a variance-swap comparison must not carry.
//
// The variance-swap leg and the comparison signals are NOT here yet: this class
// grows them in place (Tasks 2-3) rather than being wrapped, so `StrangleVarswap-
// Config` and the class name are the shape the later tasks extend.
//
// ## Thread-safety
//
// Exactly `IStrategy`'s (strategy.hpp): an instance carries mutable per-cycle
// state that `on_step` writes, so ONE instance is driven by ONE engine loop on
// ONE thread. `entry_risk_seeds()` borrows a buffer the next `on_step` rewrites.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/backtest.hpp"         // MarketSnapshot, Lot, PortfolioState, SwapLot
#include "atx/vol/derivatives.hpp"      // DerivConfig, DerivContract, RealizedVarianceSpec
#include "atx/vol/portfolio_pricer.hpp" // PriceOptions
#include "atx/vol/priced_surface.hpp"   // FullGreekSeed
#include "atx/vol/strategy.hpp"         // IStrategy, HedgeSpec
#include "atx/vol/types.hpp"            // Result, Status, Side

namespace atx::vol {

// The comparison's configuration. `session_ts` is the RUN's snapshot timestamp
// grid, SORTED ASCENDING — the same thing `StrategySpec::session_ts` is, and for
// the same reason: an expiry that is not a session the run observes can never be
// settled (`UnpricedLotPolicy` documents that as a calendar bug, not missing
// data). It is corpus-specific, so the DRIVER fills it from its `Clock` refs.
struct StrangleVarswapConfig {
  std::string symbol = "XOM";
  double target_abs_delta = 0.40;       // strangle wing delta
  double tenor_years = 0.25;            // ~3M; snapped to the session grid
  double contracts = 100.0;             // strangle qty per wing (fixed)
  std::vector<std::int64_t> session_ts; // snap grid, driver-supplied

  // ── The variance-swap leg ─────────────────────────────────────────────────
  //
  // One uncapped variance swap per CYCLE, opened on the step that fixes the
  // cycle, struck at that cycle's own fair strike and sized so its vega equals
  // the strangle's ENTRY vega. Equal vega at inception is what makes the two
  // legs comparable: both start the cycle with the same first-order exposure to
  // a parallel vol move, so everything the comparison reports afterwards is
  // second-order — the strangle's local, strike-pinned gamma/vanna against the
  // swap's uniform variance exposure — rather than a size mismatch.
  bool enable_swap_leg = true;

  // Pricing config for the ENTRY SOLVE ONLY (fair strike + vega). The engine
  // marks and settles a live swap under its own hard-coded default DerivConfig
  // (`swap_price_cfg`, backtest.cpp), which no strategy can reach — so a
  // non-default config here changes what the swap is STRUCK at, never how it is
  // subsequently valued. Left at the default the two agree exactly, which is
  // the only setting that opens the swap at a genuine zero PV.
  DerivConfig deriv_cfg{};
};

class StrangleVsVarswapStrategy final : public IStrategy {
public:
  // The config is validated on the FIRST `on_step`, not here: a constructor has
  // no error channel, and the engine's `Status` return is the one place a bad
  // configuration can fail closed instead of being asserted away.
  explicit StrangleVsVarswapStrategy(StrangleVarswapConfig cfg);

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override;

  // Same borrow contract as `DeclarativeStrategy::entry_risk_seeds` — the span
  // names this strategy's own buffer and the next `on_step` rewrites it.
  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return last_entry_seeds_;
  }

  // Delta-to-zero, every session, no band — the shape the sp100 driver uses. The
  // comparison is about VOL P&L, so the strangle's spot exposure is hedged out.
  [[nodiscard]] HedgeSpec hedge_spec() const override;

  // The live cycle's fixed expiry, or 0 before the first cycle and after the
  // session grid is exhausted.
  [[nodiscard]] std::int64_t cycle_expiry_ts_ns() const noexcept { return cycle_expiry_ts_ns_; }

  // Steps on which the surface could not serve the target delta, so the live
  // strikes were kept rather than a strike being fabricated. Never silent: a
  // non-zero count means the restrike schedule has holes in it.
  [[nodiscard]] std::uint64_t unresolved_strike_steps() const noexcept {
    return unresolved_strike_steps_;
  }

  // Cycles that opened WITHOUT a variance-swap leg while the leg was enabled:
  // the entry solve could not produce a fair strike and a usable vega, or the
  // cycle is too short to observe a single return. Those cycles run
  // options-only rather than booking a fabricated quantity, and this is how the
  // comparison (Task 3) knows a cycle is one-legged instead of silently
  // reporting a spread against a swap that was never there. Always 0 when
  // `enable_swap_leg` is false — a leg nobody asked for is not a skip.
  [[nodiscard]] std::uint64_t skipped_swap_cycles() const noexcept { return skipped_swap_cycles_; }

private:
  // Wing order, shared by the resolved strikes, the emitted lots and the seeds.
  static constexpr std::array<Side, 2> kWings{Side::Call, Side::Put};

  // Both wings resolved on one step's surface. Never partially populated: a wing
  // that cannot be resolved makes the whole step a keep-strikes step.
  struct ResolvedStrangle {
    std::uint32_t uid{0};
    double T{0.0};                // residual year-fraction to the cycle expiry
    std::array<double, 2> K{};    // parallel to kWings
    std::array<double, 2> mark{}; // per-share model mark, the entry fill
    std::vector<FullGreekSeed> seeds;
  };

  [[nodiscard]] Status validate_config();

  // The cycle expiry for a step at `base_ts`: the FIRST session at or after
  // `base_ts + tenor`, or the LAST session when the grid ends before the anchor
  // (a final, short cycle that still settles inside the run). 0 when no session
  // strictly after `base_ts` remains.
  [[nodiscard]] std::int64_t select_cycle_expiry(std::int64_t base_ts) const noexcept;

  // `nullopt` when this step's surface cannot serve the target delta or cannot
  // mark the resulting contract — by contract a keep-strikes step, never a
  // fabricated strike and never a 0.0 mark.
  [[nodiscard]] std::optional<ResolvedStrangle>
  resolve_wings(const MarketSnapshot &base, const PriceOptions &price_options) const;

  // The listed-equity contract size — `Lot::multiplier`'s default, spelled out
  // here because the swap leg is sized against the strangle's DOLLAR vega, and
  // that is per-share vega x contracts x THIS. Dropping it would open a swap
  // 100x too small and quietly turn the whole comparison into an options study.
  static constexpr double kMultiplier = 100.0;
  // Unit variance notional: the leg is sized entirely through `SwapLot::qty`,
  // so one dimension carries the sizing and the other stays a constant the
  // reader (and the equal-vega assertion) can check by eye.
  static constexpr double kSwapNotional = 1.0;
  static constexpr double kSwapAnnualization = 252.0; // trading-day variance convention

  // The engine's own `SwapLot` -> `DerivContract` construction (`step_swap_lots`,
  // backtest.cpp), transcribed so the entry solve prices the IDENTICAL contract
  // the mark lane will price one step later — a residual tenor that divided by a
  // different year, or an rv_spec staged differently, would strike the swap
  // against a contract the engine never values. `rv` is the accrual state as of
  // `base_ts` (all zero at entry: the engine seeds on first sight, one step on).
  //
  // Reused by the comparison signals (Task 3), which need the same contract at
  // an arbitrary snapshot to value a live cycle.
  [[nodiscard]] static DerivContract swap_contract(const SwapLot &lot, std::int64_t base_ts,
                                                   const RealizedVarianceSpec &rv) noexcept;

  // Solve and append this cycle's equal-vega variance swap to `book.swap_lots`,
  // consuming one `next_lot_id` on success. FAIL-SOFT: any step of the solve
  // that cannot produce a finite fair strike and a usable vega leaves the book
  // and the id watermark untouched and counts the cycle in
  // `skipped_swap_cycles_`. Called ONLY on a cycle-open step.
  void open_cycle_swap(const MarketSnapshot &base, const ResolvedStrangle &wings,
                       PortfolioState &book, std::uint64_t &next_lot_id);

  StrangleVarswapConfig cfg_;
  std::int64_t cycle_expiry_ts_ns_ = 0; // 0 = no live cycle
  std::int64_t tenor_ns_ = 0;           // cfg_.tenor_years in ns, set by validate_config
  std::uint32_t cycle_index_ = 0;       // Lot::cohort — identifies the CYCLE, not the clip
  std::uint64_t unresolved_strike_steps_ = 0;
  std::uint64_t skipped_swap_cycles_ = 0;
  bool validated_ = false;
  std::vector<FullGreekSeed> last_entry_seeds_;
};

} // namespace atx::vol

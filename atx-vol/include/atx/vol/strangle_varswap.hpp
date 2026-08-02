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

#include "atx/vol/backtest.hpp"         // MarketSnapshot, Lot, PortfolioState
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

  StrangleVarswapConfig cfg_;
  std::int64_t cycle_expiry_ts_ns_ = 0; // 0 = no live cycle
  std::int64_t tenor_ns_ = 0;           // cfg_.tenor_years in ns, set by validate_config
  std::uint32_t cycle_index_ = 0;       // Lot::cohort — identifies the CYCLE, not the clip
  std::uint64_t unresolved_strike_steps_ = 0;
  bool validated_ = false;
  std::vector<FullGreekSeed> last_entry_seeds_;
};

} // namespace atx::vol

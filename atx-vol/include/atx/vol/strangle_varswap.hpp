#pragma once

// atx-vol XOM strangle-vs-varswap comparison backtest — the strategy that runs
// both sides of the comparison on one clock.
//
// THE OPTIONS LEG is a FIXED-EXPIRY, DAILY-RESTRIKE strangle:
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
// THE VARIANCE-SWAP LEG is one uncapped var swap per cycle, struck fair and sized
// to the strangle's entry vega (`enable_swap_leg` below).
//
// THE COMPARISON SIGNALS are per-row swap greeks and leg attribution (`signals`
// below), so a run's own output says which leg carried which exposure on which
// session — and says nothing at all, rather than 0.0, where a leg was not live.
//
// ## Thread-safety
//
// Exactly `IStrategy`'s (strategy.hpp): an instance carries mutable per-cycle
// state that `on_step` writes, so ONE instance is driven by ONE engine loop on
// ONE thread. `entry_risk_seeds()` borrows a buffer the next `on_step` rewrites.
// `signals()` is const but reads state `on_step` writes, so it is safe only on
// that same thread, between steps — which is where the engine calls it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
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

  // The per-step comparison signals, one value per RECORDED row
  // (`BacktestResult::signals`). Eight columns, always all eight — a name the
  // engine does not see on the first recorded row never gets a series at all, so
  // an absent measurement is reported as NaN rather than as a missing column:
  //
  //   swap_delta, swap_gamma, swap_vega, swap_theta, swap_rho
  //       The LIVE swap lots' greeks, qty-scaled and summed, recomputed on THIS
  //       row's snapshot against THIS row's accrual — the `DerivContract` the
  //       engine's mark lane priced this row (residual tenor + the fixings
  //       observed so far), differentiated through the same bridge. On a
  //       cycle-open row the engine has not marked the new lot yet, and the
  //       contract is correspondingly its entry one: nothing realized, PV 0.
  //       NaN, never 0.0, when no swap is live: a one-legged cycle is normal
  //       data (the tail cycle of any corpus whose calendar runs out mid-tenor
  //       is one), and a 0.0 there would read as a measured flat position.
  //       `swap_theta` can be NaN on its own with a swap live — `deriv_greeks`
  //       declines the roll stencil inside one bump width of expiry.
  //   strangle_vega
  //       The option book's DOLLAR vega on this row's snapshot: the very
  //       quantity the cycle's swap was sized against, so on a cycle-open row
  //       that DID get a swap it equals `swap_vega` by construction. NaN on a
  //       step that could not resolve (and therefore could not price) its wings,
  //       and on a step with no live cycle.
  //   skipped_restrikes, skipped_swaps
  //       `skipped_restrikes()` and `skipped_swap_cycles()` as of this row —
  //       CUMULATIVE, so a hole in the schedule stays on the record and a
  //       renderer differences consecutive rows to find the session it opened
  //       on. Genuinely 0.0 before the first skip; these two are never NaN.
  //
  // PRECONDITION — `base` must be the snapshot the most recent `on_step` was
  // given, which is exactly how the engine calls it (`record_signals(*base)`
  // fires immediately after the step, on the same snapshot). Called with any
  // other snapshot the swap greeks report NaN rather than valuing this row's
  // accrual against someone else's market.
  [[nodiscard]] std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot &base) const override;

  // The live cycle's fixed expiry, or 0 before the first cycle and after the
  // session grid is exhausted.
  [[nodiscard]] std::int64_t cycle_expiry_ts_ns() const noexcept { return cycle_expiry_ts_ns_; }

  // Steps on which the surface could not serve the target delta while a pair was
  // LIVE, so the live strikes were KEPT rather than a strike being fabricated.
  // Never silent: a non-zero count means the restrike schedule has holes in it,
  // and each hole is a session the strangle spent at yesterday's strikes.
  [[nodiscard]] std::uint64_t skipped_restrikes() const noexcept { return skipped_restrikes_; }

  // Steps on which the surface could not serve the target delta while the book
  // was EMPTY — inception, or the step a settled cycle rolls on. Nothing was
  // kept because there was nothing to keep: the strangle simply does not exist
  // that session. Counted APART from `skipped_restrikes` deliberately. The two
  // are the same failure of the surface but opposite facts about the position,
  // and one column reporting both would say "the strikes were held" on a session
  // that held nothing.
  [[nodiscard]] std::uint64_t unopened_strangle_steps() const noexcept {
    return unopened_strangle_steps_;
  }

  // Every step the target delta could not be resolved on, whichever of the two
  // dispositions above it took.
  [[nodiscard]] std::uint64_t unresolved_strike_steps() const noexcept {
    return skipped_restrikes_ + unopened_strangle_steps_;
  }

  // Cycles that opened WITHOUT a variance-swap leg while the leg was enabled.
  // Those cycles run options-only rather than booking a fabricated quantity, and
  // this is how the comparison knows a cycle is one-legged instead of silently
  // reporting a spread against a swap that was never there. Always 0 when
  // `enable_swap_leg` is false — a leg nobody asked for is not a skip.
  //
  // ONE COUNTER, SEVEN CAUSES, all of them "no swap on this cycle" and none of
  // them distinguished here:
  //   1. the cycle-open step could not resolve its wings at all (a dark board),
  //      so there is no entry vega to size against;
  //   2. that entry vega came out non-finite or exactly zero;
  //   3. the cycle is too short to observe one return (its fixing window holds a
  //      single session, which the engine spends seeding), or — unreachable on
  //      any real calendar — too long to fit `SwapLot::n_obs_total`;
  //   4. the underlier had no surface on the open step (defence in depth: 1
  //      already covers it);
  //   5. the fair-strike solve failed or returned a non-positive strike;
  //   6. the entry greeks failed, or the swap's vega was non-finite or zero;
  //   7. the resulting quantity was non-finite.
  // Every one of them leaves the book and the lot-id watermark untouched.
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

  // The engine's per-lot `SwapAccrual` (backtest.hpp), MIRRORED. The running
  // fixing state lives in the ENGINE and is exposed to nobody — that is what
  // keeps a `SwapLot` bit-comparable across a step — so a strategy that wants to
  // value its own live swap mid-cycle has no choice but to reproduce it. This is
  // `observe_swap_fixing` (backtest.cpp) transcribed, and it stays in step with
  // the original for two structural reasons:
  //
  //   * SEEDING. A mirror is created on the first `on_step` that SEES the lot in
  //     the book and takes no fixing on that step, because the engine's swap
  //     pass first sees it one step later. Seeding at the open date instead
  //     would shift the whole series by one session and silently mis-age every
  //     mark after it.
  //   * PHASE. The swap pass runs on the same snapshot `on_step` is then called
  //     with, and it runs FIRST — so mirroring one fixing per `on_step` leaves
  //     the mirror exactly as of the row the engine is about to record.
  //
  // `prev_ts_ns` is deliberately absent: the engine's own accrual takes this
  // snapshot's fixing before `on_step` is reached and fails the run closed on a
  // duplicate or backdated timestamp, so this can only ever be advanced by a
  // strictly increasing clock.
  struct SwapMirror {
    std::uint64_t lot_id{0};
    RealizedVarianceSpec rv{};
    double prev_spot{0.0};
    bool have_prev{false};
    // Set when a fixing could not be taken (no surface, or a non-positive spot).
    // The accrual is then permanently behind the engine's, so this lot's greeks
    // report NaN forever rather than valuing a frozen series. Unreachable while
    // the engine fails the whole run on the same condition; it exists so that
    // softening that policy cannot turn into a silently wrong number here.
    bool desynced{false};
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

  // The option book's DOLLAR vega per 1.00 of parallel vol for a freshly
  // resolved pair: per-share American vega (positive on BOTH wings) x contracts
  // x contract size, i.e. the very scaling the portfolio pricer applies to a
  // position's greeks (`out.vega[i] = w * g.vega`). ONE definition, because the
  // number that SIZES the swap and the number the comparison REPORTS have to be
  // the same number or the equal-vega claim is unfalsifiable.
  [[nodiscard]] double strangle_dollar_vega(const ResolvedStrangle &wings) const noexcept;

  // Solve and append this cycle's variance swap to `book.swap_lots`, sized so
  // its vega equals `strangle_vega`, consuming one `next_lot_id` on success.
  // FAIL-SOFT: any step of the solve that cannot produce a finite fair strike
  // and a usable vega leaves the book and the id watermark untouched and counts
  // the cycle in `skipped_swap_cycles_`. Called ONLY on a cycle-open step.
  void open_cycle_swap(const MarketSnapshot &base, const ResolvedStrangle &wings,
                       double strangle_vega, PortfolioState &book, std::uint64_t &next_lot_id);

  // The strategy's own `on_step` body. Wrapped so the signal state below is
  // refreshed on EVERY path out of it — a step that opened nothing must report
  // that, not the previous step's numbers.
  [[nodiscard]] Status step(const MarketSnapshot &base, PortfolioState &book,
                            std::uint64_t &next_lot_id, const PriceOptions &price_options);

  // Take this snapshot's fixing into the mirrors, adopt lots the engine settled
  // out of / this step opened into the book, and stamp the snapshot the cached
  // signal state is as-of.
  void refresh_signal_state(const MarketSnapshot &base, const PortfolioState &book);

  [[nodiscard]] const SwapMirror *find_mirror(std::uint64_t lot_id) const noexcept;

  StrangleVarswapConfig cfg_;
  std::int64_t cycle_expiry_ts_ns_ = 0; // 0 = no live cycle
  std::int64_t tenor_ns_ = 0;           // cfg_.tenor_years in ns, set by validate_config
  std::uint32_t cycle_index_ = 0;       // Lot::cohort — identifies the CYCLE, not the clip
  std::uint64_t skipped_restrikes_ = 0;
  std::uint64_t unopened_strangle_steps_ = 0;
  std::uint64_t skipped_swap_cycles_ = 0;
  bool validated_ = false;
  std::vector<FullGreekSeed> last_entry_seeds_;

  // ── Signal state, as of the last completed `on_step` ───────────────────────
  //
  // `signals()` is const and is handed nothing but a snapshot, so everything it
  // cannot re-derive from one is captured here. The swap GREEKS are deliberately
  // NOT: they are recomputed inside `signals()`, which the engine calls only on
  // RECORDED rows, so a run at `record_every_n > 1` pays for the finite
  // differences on the rows that keep them and not on the rest. The accrual
  // below still advances every step, because it is path-dependent.
  std::vector<SwapLot> live_swaps_;      // `book.swap_lots` after the last step
  std::vector<SwapMirror> swap_mirrors_; // their accruals, keyed by lot id
  std::int64_t signal_ts_ns_ = 0;        // the snapshot the two above are as-of
  bool stepped_ = false;                 // ... and whether there has been one
  double last_strangle_vega_ = std::numeric_limits<double>::quiet_NaN();
};

} // namespace atx::vol

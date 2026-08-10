#pragma once

// atx-vol swap-leg toolkit — the reusable pieces every swap-carrying strategy
// shares, extracted from the strangle-vs-varswap comparison strategy:
//
//   * `swap_contract_for_lot` — the engine's `SwapLot` -> `DerivContract`
//     construction (`step_swap_lots`, backtest.cpp), transcribed so an entry
//     solve and a mid-cycle signal price the IDENTICAL contract the engine's
//     mark lane values: a residual tenor divided by a different year, or an
//     rv_spec staged differently, would strike or measure a contract the
//     engine never prices.
//   * `SwapSignalProbe` — a strategy-side mirror of the engine's per-lot
//     fixing/accrual state (`SwapAccrual`, backtest.hpp). That state lives in
//     the ENGINE and is exposed to nobody — which is what keeps a `SwapLot`
//     bit-comparable across a step — so a strategy that wants to value its own
//     live swap mid-cycle has no choice but to reproduce it. The probe is
//     `observe_swap_fixing` (backtest.cpp) transcribed, and it stays in step
//     with the original for two structural reasons:
//
//       - SEEDING. A mirror is created on the first step that SEES the lot in
//         the book and takes no fixing on that step, because the engine's swap
//         pass first sees it one step later. Seeding at the open date instead
//         would shift the whole series by one session and silently mis-age
//         every mark after it.
//       - PHASE. The engine's swap pass runs on the same snapshot `on_step` is
//         then called with, and it runs FIRST — so mirroring one fixing per
//         step leaves the mirror exactly as of the row the engine is about to
//         record.
//
// ## Thread-safety
//
// `swap_contract_for_lot` is a pure function, safe from any thread. A
// `SwapSignalProbe` carries per-step mutable state its owner's `on_step`
// writes, so ONE probe belongs to ONE strategy instance driven by ONE engine
// loop on ONE thread — exactly `IStrategy`'s own rule (strategy.hpp).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"    // SwapLot, PortfolioState, MarketSnapshot
#include "atx/vol/derivatives.hpp" // DerivContract, DerivConfig, RealizedVarianceSpec

namespace atx::vol {

// The `DerivContract` a swap lot IS on the snapshot at `base_ts`: the engine's
// `residual_T` verbatim (unsigned-differenced so the subtraction is defined for
// any pair of timestamps; an expired lot reports a NEGATIVE maturity rather
// than clamping), the lot's own terms, and the caller's accrual staged into
// `rv_spec`. At entry the accrual is all zero — the engine seeds a new lot's
// series one step after it opens — so the solve prices a purely
// forward-looking contract.
[[nodiscard]] DerivContract swap_contract_for_lot(const SwapLot &lot, std::int64_t base_ts,
                                                  const RealizedVarianceSpec &rv) noexcept;

// One cycle's swap-leg entry solve, as a request. The lot's terms except the
// two the solve OWNS: `strike_dec` (struck fair on the open snapshot) and
// `qty` (sized to the caller's target vega).
struct CycleSwapRequest {
  std::uint32_t uid{0};
  // Task F-2: `DerivKind::GammaSwap` passes through this field, `swap_
  // contract_for_lot`, and `solve_cycle_swap` identically to every other
  // kind -- none of this file's own logic branches on `kind` (it reads
  // `DerivGreeks::vega` and `DerivQuote::fair_strike_dec` generically,
  // whichever pricer produced them), so a gamma-swap entry solve works today
  // with no code change here. The one gap this does NOT close: `backtest.cpp`
  // 's `valid_deriv_kind` (out of this task's file list) does not yet admit
  // `GammaSwap` into a live `SwapLot`, so a strategy that hands a solved
  // gamma-swap lot to the ENGINE's book still fails loud at that boundary
  // (`validate_swap_lot_economics`) -- wiring the live backtest engine for
  // gamma swaps is a separate, future task. `solve_cycle_swap` itself (a
  // standalone fair-strike/vega solve against a `SurfaceRef`, no engine
  // involved) has no such gap; see `SwapLeg.GammaSwapKindPassesThrough`
  // (swap_leg_test.cpp).
  DerivKind kind{DerivKind::VarSwap};
  double cap_dec{0.0}; // > 0 required on a capped kind; must be 0 otherwise
  // Sizing rides entirely on `qty`, so notional stays a constant the reader
  // (and any equal-vega assertion) can check by eye.
  double notional{1.0};
  double annualization{252.0}; // trading-day variance convention
  std::int64_t open_ts_ns{0};
  std::int64_t expiry_ts_ns{0};
  // The run's session grid, SORTED ASCENDING — the fixing schedule the ENGINE
  // will actually observe, from which the solve counts `n_obs_total`.
  std::span<const std::int64_t> session_ts;
  // ENTRY SOLVE ONLY (fair strike + vega). The engine marks and settles a live
  // swap under its own hard-coded default DerivConfig (`swap_price_cfg`,
  // backtest.cpp), which no strategy can reach — so a non-default config here
  // changes what the swap is STRUCK at, never how it is subsequently valued.
  // Left at the default the two agree exactly, which is the only setting that
  // opens the swap at a genuine zero PV.
  DerivConfig deriv_cfg{};
};

// Solve one cycle's swap leg on the open snapshot's surface: count the fixing
// schedule off `session_ts`, strike FAIR through the same `deriv_price_on_ref`
// bridge the engine's mark lane prices through (the only construction that
// opens at genuine zero PV — the PricedSurface-native fair-strike entry
// derives its carry differently, and striking against one while marking
// against the other would land the whole artifact in the first step's
// `swap_pnl`), then size `qty = target_vega / (swap entry vega)` so the leg's
// dollar vega equals `target_vega` (sign carries: a long-vega target sizes a
// long, variance-receiving swap).
//
// FAIL-SOFT BY CONTRACT: every cause that cannot produce a usable lot returns
// Err(Unavailable, <cause>) and mutates nothing — a non-finite/zero target, a
// cycle too short to observe one return (its single in-window session is spent
// seeding), a schedule that cannot fit `n_obs_total`, a failed or non-positive
// fair strike, failed or non-finite/zero entry vega, a non-finite qty. The
// caller counts refusals and reports them; it never guesses. `lot.id` is left
// 0 for the caller's monotonic watermark.
//
// `surface_certified_wing_band` (FIT-C7 / Task C-6): `surface`'s own
// certified band, resolved by the caller from the SAME snapshot's provenance
// (`certified_wing_band_for`, backtest.hpp) at the uid `surface` was resolved
// from. Threaded into BOTH the fair-strike solve and the entry vega, so the
// strike a swap opens at and the vega it is sized against trust the SAME
// band. `std::nullopt` (the default) resolves the mode-blind band —
// unchanged prior behaviour for a caller that does not (yet) supply one.
[[nodiscard]] Result<SwapLot>
solve_cycle_swap(const SurfaceRef &surface, const CycleSwapRequest &req, double target_vega,
                 std::optional<double> surface_certified_wing_band = std::nullopt);

// Strategy-side mirror of the engine's swap accruals, driving the five
// `swap_*` greek signal columns. Usage, per `on_step`:
//
//   probe_.capture_pre_step(book);        // BEFORE the strategy touches it
//   ATX_TRY_VOID(step(...));              // the strategy's own body
//   probe_.refresh(base, book);           // only on SUCCESS — an errored step
//                                         // aborts the run, and refreshing off
//                                         // a half-built book would be state
//                                         // nobody can use
//
// `capture_pre_step` decides how a lot with no mirror is treated when
// `refresh` adopts it: one the step OPENED (absent from the capture) starts a
// clean accrual; one that was ALREADY in the book is a checkpoint restore
// whose realized variance is unreachable — `run_backtest_incremental` reloads
// `BacktestCheckpoint::portfolio` AND `::swap_accruals`, but a strategy has no
// checkpoint of its own, so a fresh mirror would describe a swap that had
// realized NOTHING: finite, plausible, and wrong for the whole resumed
// segment. Such a lot is marked desynced and reports NaN forever — unreachable
// is reported as unknown, never as a confident wrong number.
class SwapSignalProbe {
public:
  // Record the swap-lot ids present in `book` before the owning strategy
  // mutates it this step. A lot in the book at `refresh` but not captured here
  // is one this step opened; a lot in both was carried in.
  void capture_pre_step(const PortfolioState &book);

  // Advance the mirrors to this snapshot: drop mirrors whose lots the engine
  // settled out of the book, adopt lots this step opened (no fixing on first
  // sight — see SEEDING above), take one fixing per already-mirrored live lot,
  // and stamp the snapshot the cached state is as-of. A fixing that cannot be
  // taken (no surface, or a non-positive spot) marks that lot desynced; the
  // engine fails the whole run on the same condition, so the mark exists so
  // that softening that policy can never turn into a silently wrong number.
  void refresh(const MarketSnapshot &base, const PortfolioState &book);

  // Append the five signal columns — swap_delta, swap_gamma, swap_vega,
  // swap_theta, swap_rho — to `out`, ALWAYS all five: an absent measurement is
  // NaN, never a missing column and never 0.0. They are the live lots' greeks,
  // qty-scaled and summed (exactly as the engine scales this lane's marks:
  // position dollars), recomputed on `base` against the mirrored accrual under
  // the ENGINE's mark config (`DerivConfig{}` — the config the run is actually
  // paid on, see backtest.cpp `swap_price_cfg`). All five are NaN when: the
  // probe has never refreshed; `base` is not the as-of snapshot (the cached
  // accrual would value this row's fixings against someone else's market); no
  // swap is live (a one-legged cycle is normal data, and 0.0 would read as a
  // measured flat position); any mirror is desynced; or any lot fails to
  // price (never a PARTIAL total). `swap_theta` can be NaN on its own with a
  // swap live — `deriv_greeks` declines the roll stencil inside one bump width
  // of expiry.
  void append_swap_greek_signals(const MarketSnapshot &base,
                                 std::vector<std::pair<std::string, double>> &out) const;

  // Whether `refresh` has ever run — i.e. whether the as-of stamp is real.
  [[nodiscard]] bool stepped() const noexcept { return stepped_; }

private:
  // One live lot's mirrored accrual. See the class comment for the seeding and
  // desync rules; the arithmetic is `SwapAccrual`'s (backtest.hpp), transcribed.
  struct Mirror {
    std::uint64_t lot_id{0};
    RealizedVarianceSpec rv{};
    double prev_spot{0.0};
    bool have_prev{false};
    bool desynced{false};
  };

  [[nodiscard]] const Mirror *find_mirror(std::uint64_t lot_id) const noexcept;

  std::vector<SwapLot> live_swaps_;             // `book.swap_lots` after the last refresh
  std::vector<Mirror> mirrors_;                 // their accruals, keyed by lot id
  std::vector<std::uint64_t> ids_before_step_;  // the capture_pre_step scratch
  std::int64_t signal_ts_ns_{0};                // the snapshot the state is as-of
  bool stepped_{false};
};

} // namespace atx::vol

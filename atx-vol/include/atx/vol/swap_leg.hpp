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
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"    // SwapLot, PortfolioState, MarketSnapshot
#include "atx/vol/derivatives.hpp" // DerivContract, RealizedVarianceSpec

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

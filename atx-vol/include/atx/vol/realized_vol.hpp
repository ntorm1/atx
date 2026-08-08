#pragma once

// OHLC realized-vol estimator suite (THEO-1).
//
// Five estimators of historical annualized volatility from daily OHLC bars:
// close-to-close (the naive log-return sample stdev), Parkinson (high-low
// range), Garman-Klass and Rogers-Satchell (open/high/low/close range
// combinations), and Yang-Zhang (overnight + open-to-close + Rogers-Satchell
// drift-independent blend). Every entry is a pure function of its `bars`
// argument — no globals, no scratch, no shared state — so concurrent calls
// from any threads are safe. No exceptions cross this API: expected failures
// (too few bars, non-finite/non-positive OHLC) return `Err` via `Result<T>`.
//
// Greenfield: this is the first RV estimator surface in atx-vol (the only
// prior art is an unrelated close-to-close accumulator elsewhere in the
// codebase). Feeds later theo-module tasks (RvPanel as a feature input); this
// header carries no dependency on them. Tier-B: reachable by explicit
// include, deliberately outside the `atx/vol/vol.hpp` umbrella.

#include <array>
#include <cstdint>
#include <span>

#include "atx/vol/types.hpp"

namespace atx::vol {

// One OHLC bar. `ts_ns` is the bar's timestamp in epoch nanoseconds
// (bar-close convention is the caller's; this header only orders bars by
// array index, never by `ts_ns`).
struct OhlcBar {
  std::int64_t ts_ns{0};
  double open{0};
  double high{0};
  double low{0};
  double close{0};
};

// Which realized-vol estimator to compute. Explicit values: this is a wire-
// stable selector, not just a local tag.
enum class RvEstimator : std::uint8_t {
  CloseToClose = 0,
  Parkinson = 1,
  GarmanKlass = 2,
  RogersSatchell = 3,
  YangZhang = 4,
};

// Annualized realized volatility (not variance) of `bars` under `est`.
//
// @param bars           OHLC bars in chronological order, oldest first.
// @param est            estimator selector.
// @param annualization  bars-per-year scale factor (default 252 trading
//                        days); the per-bar variance estimate is multiplied
//                        by this before the sqrt.
// @return  annualized vol, or `Err(InvalidArgument, ...)` when:
//            - `bars.size() < 2`, or `annualization <= 0`;
//            - `est == YangZhang` and fewer than 3 return terms are
//              available (i.e. `bars.size() < 4`);
//            - any bar is non-finite, or fails
//              `0 < low <= min(open,close) <= max(open,close) <= high`.
[[nodiscard]] Result<double> realized_vol(std::span<const OhlcBar> bars, RvEstimator est,
                                          double annualization = 252.0);

// Trailing-window realized-vol panel: one vol per window in `window`, each a
// call to `realized_vol` over the trailing slice of `bars` ending at the last
// bar. A window longer than `bars.size()` falls back to the whole span; a
// resulting slice shorter than 2 bars yields `vol = NaN` for that slot rather
// than an error (this is a per-slot flag, not a failure of the whole panel).
struct RvPanel {
  std::array<double, 4> vol{};
  std::array<std::uint16_t, 4> window{5, 21, 63, 252};
};

// @param bars           OHLC bars in chronological order, oldest first.
// @param est            estimator applied to every window (default YangZhang).
// @param annualization  forwarded to `realized_vol` for each window.
// @return  the panel, or `Err(InvalidArgument, ...)` when `bars` is empty,
//          `annualization <= 0`, or a window's >=2-bar trailing slice is
//          itself rejected by `realized_vol` (failed OHLC validation, or —
//          for `est == YangZhang` — a slice with fewer than 3 return terms).
//          Only the "< 2 bars available" case is a per-slot NaN flag rather
//          than a panel-level error; the YangZhang minimum-terms floor is not
//          special-cased here.
[[nodiscard]] Result<RvPanel> realized_vol_panel(std::span<const OhlcBar> bars,
                                                 RvEstimator est = RvEstimator::YangZhang,
                                                 double annualization = 252.0);

} // namespace atx::vol

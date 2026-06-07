#pragma once

// atx::engine::alpha — time-series VM kernels (P3-8).
//
// The per-instrument-column, causal trailing-window opcodes for the fast
// vectorized VM (vm.hpp): delay / delta / sum / mean / std / var / min / max /
// argmin / argmax / rank / corr / cov / product / decay_linear / ema / wma /
// skew / kurt / med / mad / slope / rsquare / resid plus the P3b-2 superset
// zscore / backfill / av_diff / quantile / scale / count_nans (the 30 `Ts*`
// ops). These free functions are the PRODUCTION-path counterparts of oracle.hpp's
// `detail::ts_unary_at` / `ts_binary_at`; vm.hpp's `Engine::eval_time_series`
// resolves the window from the op's last operand and calls into them per cell.
// They MUST reproduce the oracle BIT-FOR-BIT — the P3-8 differential test runs
// both paths and asserts every cell agrees (NaN==NaN).
//
// ===========================================================================
//  PINNED SEMANTIC CONTRACT — re-implemented here, self-contained
// ===========================================================================
//  vm.hpp's time-series path does NOT borrow oracle.hpp; this header re-states
//  the SAME numeric policy independently (the differential TEST proves they
//  agree). All kernels operate on instrument column `j` of a date-major
//  (`date*instruments + inst`) buffer, over the trailing window `[t-d+1, t]`:
//    * Window `d` is the op's LAST operand's scalar value, truncated toward
//      zero; d<=0 / NaN -> all NaN (resolved by the caller via tsv_window_of).
//    * min_periods = FULL window: if the window is incomplete (t+1 < d) OR ANY
//      cell in it is NaN -> the output is NaN. delay/delta are the exception:
//      they need only the single shifted observation x[t-d] (NaN if t < d);
//      delta = x[t] - x[t-d].
//    * Per-op formulas EXACTLY as the oracle: sum; mean = sum/n; var/std SAMPLE
//      (ddof=1), var NaN if n<2; min/max; argmin/argmax = 1-based position of
//      the FIRST extreme (chronological); rank = average-rank of the LAST
//      element normalized to [0,1] over n-1 (singleton 0.5); product; med
//      (sorted median, even n = mean of the middle two); mad (mean abs dev
//      about the mean); decay_linear / wma (linear weights 1..n oldest->newest,
//      normalized to sum 1 — ALIASES); ema (alpha=2/(n+1), seeded with the
//      OLDEST window element, run forward); skew (n<3->NaN; mean((v-m)^3)/sd^3,
//      sd=sample); kurt (n<4->NaN; mean((v-m)^4)/var^2 - 3 excess);
//      slope/rsquare/resid (OLS on the time axis 0..n-1; resid = last -
//      fitted_last); correlation (Pearson, population cross-moments so in
//      [-1,1], zero variance -> NaN); covariance (sample ddof=1).
//
//  BIT-EXACTNESS — IDENTICAL SUMMATION ORDER. We deliberately do NOT use an
//  incremental / online O(1) rolling accumulator (Welford, running sums):
//  re-summing incrementally would change the floating-point operation order and
//  diverge from the oracle's per-window recompute. Instead each output cell
//  recomputes over its trailing window in the SAME chronological order the
//  oracle uses (oldest->newest): mean = (Σ chronologically)/n, then a second
//  chronological pass for variance / covariance / regression. This is O(d)/cell
//  like the oracle, but the VM is still faster because it allocates NOTHING per
//  cell — the strided-walk reductions need no buffer, and the two sort/pair ops
//  reuse caller-owned scratch (resized only when `d` grows). RESIDUAL: a true
//  incremental O(1) rolling kernel is DEFERRED; it would require a documented
//  ULP tolerance in the differential and is out of scope here.
//
//  Distinct names from oracle.hpp's `detail::ts_*` / `gather_window` / `sum_of`
//  / `sample_var` / `pearson` / `sample_cov` / `lin_fit` / `arg_extreme`: the
//  differential test TU includes BOTH headers in this shared `detail`
//  namespace, so a name clash would be an ODR violation (the trap P3-6/P3-7
//  sidestepped with `vm_*` / `cs_*_row` names). Here the prefix is `tsv_*`
//  (time-series VM) with `ts_value_at` / `ts_pair_at` as the per-cell entries.
//
// Header-only; every free function is `inline`. The strided-window walks read a
// column with stride `instruments`; reductions accumulate in chronological
// (oldest->newest) order to mirror the oracle's vector loops exactly.

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/registry.hpp" // OpCode

namespace atx::engine::alpha::detail {

inline constexpr atx::f64 kTsNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] inline bool ts_is_nan(atx::f64 x) noexcept { return std::isnan(x); }

// Window size from a Ts op's last operand, truncated toward zero. <=0 or NaN
// yields 0 (the kernels then emit NaN, the documented degenerate case). Mirrors
// oracle.hpp's `window_of` exactly.
[[nodiscard]] inline atx::usize tsv_window_of(std::span<const atx::f64> col) noexcept {
  const atx::f64 v = col.empty() ? kTsNaN : col.front();
  if (ts_is_nan(v) || v < 1.0) {
    return 0;
  }
  return static_cast<atx::usize>(v);
}

// ===========================================================================
//  Strided window gather + reductions. The trailing window of instrument
//  column `j` is x[(t-d+1)*I + j], …, x[t*I + j] — strided by `instruments`.
//  `tsv_window_valid` reports whether a full, NaN-free window exists (the
//  uniform "short window / any-NaN -> NaN" policy); the reductions then walk
//  the same stride in chronological (oldest->newest) order.
// ===========================================================================

// SAFETY (strided-window walk): indices stay in [0, dates*instruments) because
// the caller guarantees t < dates and j < instruments, and the window is only
// walked when t+1 >= d (so t-d+1 >= 0). Each step advances by `instruments`,
// landing on the same instrument column on the previous/next date.
[[nodiscard]] inline bool tsv_window_valid(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                           atx::usize d, atx::usize instruments) noexcept {
  if (d == 0 || t + 1 < d) {
    return false;
  }
  for (atx::usize s = t + 1 - d; s <= t; ++s) {
    if (ts_is_nan(x[s * instruments + j])) {
      return false;
    }
  }
  return true;
}

// Σ of the window in chronological order (matches oracle `sum_of`).
[[nodiscard]] inline atx::f64 tsv_sum(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                      atx::usize d, atx::usize instruments) noexcept {
  atx::f64 s = 0.0;
  for (atx::usize k = t + 1 - d; k <= t; ++k) {
    s += x[k * instruments + j];
  }
  return s;
}

// Sample (ddof=1) variance: m = Σ/n, then Σ(v-m)² chronologically; NaN if n<2.
// Mirrors oracle `sample_var` operation order exactly.
[[nodiscard]] inline atx::f64 tsv_var(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                      atx::usize d, atx::usize instruments) noexcept {
  if (d < 2) {
    return kTsNaN;
  }
  const atx::f64 m = tsv_sum(x, t, j, d, instruments) / static_cast<atx::f64>(d);
  atx::f64 ss = 0.0;
  for (atx::usize k = t + 1 - d; k <= t; ++k) {
    const atx::f64 v = x[k * instruments + j];
    ss += (v - m) * (v - m);
  }
  return ss / static_cast<atx::f64>(d - 1);
}

// 1-based position of the FIRST extreme in the window (chronological). Mirrors
// oracle `arg_extreme`: scan oldest->newest, strictly-better replaces.
[[nodiscard]] inline atx::f64 tsv_arg_extreme(std::span<const atx::f64> x, atx::usize t,
                                              atx::usize j, atx::usize d, atx::usize instruments,
                                              bool want_max) noexcept {
  const atx::usize base = (t + 1 - d) * instruments + j;
  atx::f64 best = x[base];
  atx::usize best_off = 0;
  for (atx::usize i = 1; i < d; ++i) {
    const atx::f64 v = x[base + i * instruments];
    if ((want_max && v > best) || (!want_max && v < best)) {
      best = v;
      best_off = i;
    }
  }
  return static_cast<atx::f64>(best_off + 1);
}

// Gather the window into `buf` (chronological order) for the sort/pair ops that
// genuinely need a materialized buffer. `buf` is caller-owned scratch.
inline void tsv_gather(std::span<const atx::f64> x, atx::usize t, atx::usize j, atx::usize d,
                       atx::usize instruments, std::vector<atx::f64> &buf) {
  for (atx::usize i = 0; i < d; ++i) {
    buf[i] = x[(t + 1 - d + i) * instruments + j];
  }
}

// OLS of the window on the time axis 0..d-1 (matches oracle `lin_fit` summation
// order). Returns {slope, intercept, r2, fitted_last}; NaN fields if d<2 or the
// time/value variance is zero.
struct TsvFit {
  atx::f64 slope{kTsNaN};
  atx::f64 intercept{kTsNaN};
  atx::f64 r2{kTsNaN};
  atx::f64 fitted_last{kTsNaN};
};

[[nodiscard]] inline TsvFit tsv_lin_fit(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                        atx::usize d, atx::usize instruments) noexcept {
  TsvFit f;
  if (d < 2) {
    return f;
  }
  const atx::f64 nf = static_cast<atx::f64>(d);
  const atx::usize base = (t + 1 - d) * instruments + j;
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  for (atx::usize i = 0; i < d; ++i) {
    const atx::f64 xi = static_cast<atx::f64>(i);
    const atx::f64 yi = x[base + i * instruments];
    sx += xi;
    sy += yi;
    sxx += xi * xi;
    sxy += xi * yi;
  }
  const atx::f64 denom = nf * sxx - sx * sx;
  if (denom == 0.0) {
    return f;
  }
  f.slope = (nf * sxy - sx * sy) / denom;
  f.intercept = (sy - f.slope * sx) / nf;
  const atx::f64 my = sy / nf;
  atx::f64 ss_tot = 0.0;
  atx::f64 ss_res = 0.0;
  for (atx::usize i = 0; i < d; ++i) {
    const atx::f64 yi = x[base + i * instruments];
    const atx::f64 fit = f.intercept + f.slope * static_cast<atx::f64>(i);
    ss_tot += (yi - my) * (yi - my);
    ss_res += (yi - fit) * (yi - fit);
  }
  f.r2 = (ss_tot == 0.0) ? kTsNaN : 1.0 - ss_res / ss_tot;
  f.fitted_last = f.intercept + f.slope * static_cast<atx::f64>(d - 1);
  return f;
}

// Average-rank percentile of the LAST window element in [0,1] (matches oracle's
// TsRank branch). Singleton -> 0.5.
[[nodiscard]] inline atx::f64 tsv_rank(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                       atx::usize d, atx::usize instruments) noexcept {
  const atx::f64 last = x[t * instruments + j];
  atx::usize less = 0;
  atx::usize equal = 0;
  for (atx::usize k = t + 1 - d; k <= t; ++k) {
    const atx::f64 v = x[k * instruments + j];
    if (v < last) {
      ++less;
    } else if (v == last) {
      ++equal;
    }
  }
  const atx::f64 avg = static_cast<atx::f64>(less) + (static_cast<atx::f64>(equal) - 1.0) / 2.0;
  return d == 1 ? 0.5 : avg / static_cast<atx::f64>(d - 1);
}

// ts_backfill (P3b-2): the most recent valid (non-NaN) value within the
// trailing window [t-d+1, t]. Scans newest -> oldest and returns the first
// finite cell (so it looks PAST NaNs — it deliberately does NOT use the
// any-NaN -> NaN policy). All-NaN window (or d==0) -> NaN. The scan is purely
// causal (only indices <= t) and order-independent (no float reduction), so the
// VM and oracle agree bit-for-bit. The window underflow at date 0 is guarded by
// `t >= i` (we never index a negative date).
[[nodiscard]] inline atx::f64 tsv_backfill(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                                           atx::usize d, atx::usize instruments) noexcept {
  for (atx::usize i = 0; i < d && t >= i; ++i) {
    const atx::f64 v = x[(t - i) * instruments + j];
    if (!ts_is_nan(v)) {
      return v;
    }
  }
  return kTsNaN;
}

// ts_count_nans (P3b-2): count of NaNs in the trailing window [t-d+1, t]. This
// is a data-quality signal that returns a FINITE count even when the window has
// NaNs (it does NOT propagate NaN). CONVENTION: full-window-only — like the
// simplest existing partial op (delay/delta, which return NaN until t >= d), an
// incomplete window (t+1 < d) yields NaN; once the window is full we count its
// NaN cells. The count walk is chronological for index hygiene (order is
// irrelevant for an integer count) and bit-trivially matches the oracle.
[[nodiscard]] inline atx::f64 tsv_count_nans(std::span<const atx::f64> x, atx::usize t,
                                             atx::usize j, atx::usize d,
                                             atx::usize instruments) noexcept {
  if (d == 0 || t + 1 < d) {
    return kTsNaN; // incomplete window -> NaN (full-window-only convention)
  }
  atx::usize cnt = 0;
  for (atx::usize k = t + 1 - d; k <= t; ++k) {
    if (ts_is_nan(x[k * instruments + j])) {
      ++cnt;
    }
  }
  return static_cast<atx::f64>(cnt);
}

// ===========================================================================
//  ts_value_at — single-cell value of a UNARY-series Ts op at (t, j) over the
//  trailing window `d`. delay/delta short-circuit (they need only the shifted
//  observation, not a full window); ts_backfill/ts_count_nans short-circuit too
//  (they have their OWN NaN policy — backfill looks past NaNs, count_nans counts
//  them); every other op requires a full, NaN-free window (tsv_window_valid).
//  `sort_buf` is caller-owned scratch (>= d) reused by the median branch only. A
//  partial `switch` over the unary Ts opcodes with `default: ATX_UNREACHABLE()`
//  (mirrors vm.hpp's eval_binary style).
// ===========================================================================
[[nodiscard]] inline atx::f64 ts_value_at(OpCode op, std::span<const atx::f64> x, atx::usize t,
                                          atx::usize j, atx::usize d, atx::usize instruments,
                                          std::vector<atx::f64> &sort_buf) {
  // delay/delta: x[t-d] with min_periods==1; NaN if the shift falls off the top.
  if (op == OpCode::TsDelay || op == OpCode::TsDelta) {
    if (d == 0 || t < d) {
      return kTsNaN;
    }
    const atx::f64 shifted = x[(t - d) * instruments + j];
    return op == OpCode::TsDelay ? shifted : x[t * instruments + j] - shifted;
  }
  // ts_backfill / ts_count_nans: their own NaN policy (NOT the any-NaN -> NaN
  // gate). They short-circuit before tsv_window_valid for the same reason.
  if (op == OpCode::TsBackfill) {
    return tsv_backfill(x, t, j, d, instruments);
  }
  if (op == OpCode::TsCountNans) {
    return tsv_count_nans(x, t, j, d, instruments);
  }

  if (!tsv_window_valid(x, t, j, d, instruments)) {
    return kTsNaN; // short window or any-NaN -> NaN (pinned policy)
  }
  const atx::f64 nf = static_cast<atx::f64>(d);
  switch (op) {
  case OpCode::TsSum:
    return tsv_sum(x, t, j, d, instruments);
  case OpCode::TsMean:
    return tsv_sum(x, t, j, d, instruments) / nf;
  case OpCode::TsVar:
    return tsv_var(x, t, j, d, instruments);
  case OpCode::TsStd:
    return std::sqrt(tsv_var(x, t, j, d, instruments));
  case OpCode::TsMin:
  case OpCode::TsMax: {
    const atx::usize base = (t + 1 - d) * instruments + j;
    atx::f64 best = x[base];
    const bool want_max = (op == OpCode::TsMax);
    for (atx::usize i = 1; i < d; ++i) {
      const atx::f64 v = x[base + i * instruments];
      best = want_max ? (v > best ? v : best) : (v < best ? v : best);
    }
    return best;
  }
  case OpCode::TsArgMin:
    return tsv_arg_extreme(x, t, j, d, instruments, /*want_max=*/false);
  case OpCode::TsArgMax:
    return tsv_arg_extreme(x, t, j, d, instruments, /*want_max=*/true);
  case OpCode::TsProduct: {
    atx::f64 p = 1.0;
    for (atx::usize k = t + 1 - d; k <= t; ++k) {
      p *= x[k * instruments + j];
    }
    return p;
  }
  case OpCode::TsRank:
    return tsv_rank(x, t, j, d, instruments);
  case OpCode::TsMed: {
    tsv_gather(x, t, j, d, instruments, sort_buf);
    std::sort(sort_buf.begin(), sort_buf.begin() + static_cast<std::ptrdiff_t>(d));
    return (d % 2 == 1) ? sort_buf[d / 2] : (sort_buf[d / 2 - 1] + sort_buf[d / 2]) / 2.0;
  }
  case OpCode::TsMad: {
    const atx::f64 m = tsv_sum(x, t, j, d, instruments) / nf;
    atx::f64 s = 0.0;
    for (atx::usize k = t + 1 - d; k <= t; ++k) {
      s += std::fabs(x[k * instruments + j] - m);
    }
    return s / nf;
  }
  case OpCode::TsDecayLinear:
  case OpCode::TsWma: {
    // Linear weights 1..d (oldest=1 .. newest=d), normalized to sum 1. The two
    // ops are aliases (same loop, same order).
    const atx::usize base = (t + 1 - d) * instruments + j;
    atx::f64 acc = 0.0;
    atx::f64 wsum = 0.0;
    for (atx::usize i = 0; i < d; ++i) {
      const atx::f64 wt = static_cast<atx::f64>(i + 1);
      acc += wt * x[base + i * instruments];
      wsum += wt;
    }
    return acc / wsum;
  }
  case OpCode::TsEma: {
    // alpha = 2/(d+1), seeded with the OLDEST window element, run forward.
    const atx::usize base = (t + 1 - d) * instruments + j;
    const atx::f64 alpha = 2.0 / (nf + 1.0);
    atx::f64 ema = x[base];
    for (atx::usize i = 1; i < d; ++i) {
      ema = alpha * x[base + i * instruments] + (1.0 - alpha) * ema;
    }
    return ema;
  }
  case OpCode::TsSkew: {
    if (d < 3) {
      return kTsNaN;
    }
    const atx::f64 m = tsv_sum(x, t, j, d, instruments) / nf;
    const atx::f64 sd = std::sqrt(tsv_var(x, t, j, d, instruments));
    if (sd == 0.0) {
      return kTsNaN;
    }
    atx::f64 s = 0.0;
    for (atx::usize k = t + 1 - d; k <= t; ++k) {
      const atx::f64 v = x[k * instruments + j];
      s += (v - m) * (v - m) * (v - m);
    }
    return s / nf / (sd * sd * sd);
  }
  case OpCode::TsKurt: {
    if (d < 4) {
      return kTsNaN;
    }
    const atx::f64 m = tsv_sum(x, t, j, d, instruments) / nf;
    const atx::f64 var = tsv_var(x, t, j, d, instruments);
    if (var == 0.0) {
      return kTsNaN;
    }
    atx::f64 s = 0.0;
    for (atx::usize k = t + 1 - d; k <= t; ++k) {
      const atx::f64 v = x[k * instruments + j];
      s += (v - m) * (v - m) * (v - m) * (v - m);
    }
    return s / nf / (var * var) - 3.0; // excess kurtosis
  }
  case OpCode::TsSlope:
    return tsv_lin_fit(x, t, j, d, instruments).slope;
  case OpCode::TsRsquare:
    return tsv_lin_fit(x, t, j, d, instruments).r2;
  case OpCode::TsResid: {
    const TsvFit f = tsv_lin_fit(x, t, j, d, instruments);
    return x[t * instruments + j] - f.fitted_last;
  }
  case OpCode::TsZscore: {
    // (x[t] - rolling mean) / rolling SAMPLE std; reuse tsv_sum / tsv_var so the
    // reduction order matches ts_mean / ts_std (hence the oracle) bit-for-bit.
    const atx::f64 mean = tsv_sum(x, t, j, d, instruments) / nf;
    const atx::f64 sd = std::sqrt(tsv_var(x, t, j, d, instruments));
    return (x[t * instruments + j] - mean) / sd; // sd NaN (d<2) -> NaN
  }
  case OpCode::TsAvDiff: {
    // x[t] - rolling mean (deviation from the trailing mean).
    const atx::f64 mean = tsv_sum(x, t, j, d, instruments) / nf;
    return x[t * instruments + j] - mean;
  }
  case OpCode::TsQuantile: {
    // Rolling median (quantile 0.5) — identical kernel to TsMed (same gather,
    // same sort, same even-n midpoint average), so the two agree bit-for-bit.
    tsv_gather(x, t, j, d, instruments, sort_buf);
    std::sort(sort_buf.begin(), sort_buf.begin() + static_cast<std::ptrdiff_t>(d));
    return (d % 2 == 1) ? sort_buf[d / 2] : (sort_buf[d / 2 - 1] + sort_buf[d / 2]) / 2.0;
  }
  case OpCode::TsScale: {
    // Rolling min-max: (x[t] - min) / (max - min); a flat window (max == min)
    // -> 0 (avoid /0). Same full-window min_periods as TsMin/TsMax.
    const atx::usize base = (t + 1 - d) * instruments + j;
    atx::f64 lo = x[base];
    atx::f64 hi = x[base];
    for (atx::usize i = 1; i < d; ++i) {
      const atx::f64 v = x[base + i * instruments];
      lo = v < lo ? v : lo;
      hi = v > hi ? v : hi;
    }
    const atx::f64 range = hi - lo;
    return range == 0.0 ? 0.0 : (x[t * instruments + j] - lo) / range;
  }
  default:
    ATX_UNREACHABLE();
  }
}

// ===========================================================================
//  ts_pair_at — corr/cov of the trailing windows of x and y at (t, j). Both
//  windows must be full and NaN-free. Population cross-moments for corr (so the
//  result is in [-1,1], zero variance -> NaN); sample (ddof=1) for cov. Matches
//  oracle `pearson` / `sample_cov` summation order. `bx`/`by` are caller-owned
//  scratch (>= d) holding the two gathered windows.
// ===========================================================================
[[nodiscard]] inline atx::f64 ts_pair_at(OpCode op, std::span<const atx::f64> x,
                                         std::span<const atx::f64> y, atx::usize t, atx::usize j,
                                         atx::usize d, atx::usize instruments,
                                         std::vector<atx::f64> &bx, std::vector<atx::f64> &by) {
  if (!tsv_window_valid(x, t, j, d, instruments) || !tsv_window_valid(y, t, j, d, instruments)) {
    return kTsNaN;
  }
  if (d < 2) {
    return kTsNaN;
  }
  tsv_gather(x, t, j, d, instruments, bx);
  tsv_gather(y, t, j, d, instruments, by);
  const atx::f64 nf = static_cast<atx::f64>(d);
  atx::f64 sxa = 0.0;
  atx::f64 sya = 0.0;
  for (atx::usize i = 0; i < d; ++i) {
    sxa += bx[i];
    sya += by[i];
  }
  const atx::f64 ma = sxa / nf;
  const atx::f64 mb = sya / nf;
  if (op == OpCode::TsCov) {
    atx::f64 s = 0.0;
    for (atx::usize i = 0; i < d; ++i) {
      s += (bx[i] - ma) * (by[i] - mb);
    }
    return s / static_cast<atx::f64>(d - 1);
  }
  // TsCorr: Pearson with population cross-moments (n cancels).
  atx::f64 sab = 0.0;
  atx::f64 saa = 0.0;
  atx::f64 sbb = 0.0;
  for (atx::usize i = 0; i < d; ++i) {
    sab += (bx[i] - ma) * (by[i] - mb);
    saa += (bx[i] - ma) * (bx[i] - ma);
    sbb += (by[i] - mb) * (by[i] - mb);
  }
  const atx::f64 denom = std::sqrt(saa * sbb);
  return denom == 0.0 ? kTsNaN : sab / denom;
}

// ===========================================================================
//  P3d E1 — Rolling AR(1) OLS fit helper (ou_ar1_fit)
//
//  Fits x[s] = a + b*x[s-1] over the lagged pairs in the trailing window
//  (oldest..newest). Pairs with a NaN endpoint are skipped. Returns {a, b,
//  resid_std, n}. NaN fields when <2 valid pairs or zero predictor variance.
//  resid_std is the POPULATION std of residuals over the n valid pairs (mirrors
//  the population cross-moment convention used by tsv_lin_fit's r2 path).
//  noexcept: all operations are floating-point arithmetic with no allocation.
// ===========================================================================

struct OuAr1Fit {
  atx::f64 a{kTsNaN};
  atx::f64 b{kTsNaN};
  atx::f64 resid_std{kTsNaN};
  atx::usize n{0};
};

// OLS of x[s] on x[s-1] over a trailing window (oldest..newest). Pairs with a
// NaN endpoint are skipped. NaN fields when <2 valid pairs or zero predictor
// variance. resid_std is the POPULATION std of residuals over the n pairs.
[[nodiscard]] inline OuAr1Fit ou_ar1_fit(std::span<const atx::f64> w) noexcept {
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  atx::usize n = 0;
  for (atx::usize s = 1; s < w.size(); ++s) {
    const atx::f64 xp = w[s - 1];
    const atx::f64 yc = w[s];
    if (ts_is_nan(xp) || ts_is_nan(yc)) {
      continue;
    }
    sx += xp;
    sy += yc;
    sxx += xp * xp;
    sxy += xp * yc;
    ++n;
  }
  if (n < 2) {
    return {};
  }
  const atx::f64 dn = static_cast<atx::f64>(n);
  const atx::f64 denom = sxx - sx * sx / dn;
  if (denom == 0.0) {
    return {};
  }
  const atx::f64 b = (sxy - sx * sy / dn) / denom;
  const atx::f64 a = (sy - b * sx) / dn;
  atx::f64 ss = 0.0;
  for (atx::usize s = 1; s < w.size(); ++s) {
    const atx::f64 xp = w[s - 1];
    const atx::f64 yc = w[s];
    if (ts_is_nan(xp) || ts_is_nan(yc)) {
      continue;
    }
    const atx::f64 r = yc - (a + b * xp);
    ss += r * r;
  }
  return {a, b, std::sqrt(ss / dn), n};
}

} // namespace atx::engine::alpha::detail

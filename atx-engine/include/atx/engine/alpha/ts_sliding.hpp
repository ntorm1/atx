#pragma once

// atx::engine::alpha — O(1)-per-cell SLIDING window kernels (Lane 1).
//
// Push/pop accumulators for the windowed ops whose batch kernels recompute
// O(d) per cell:
//
//   * CoMoment  — ts_corr / ts_cov / ts_regression (x on y) over (x, y) pairs.
//   * LinDecay  — ts_decay_linear / ts_wma (linear weights 1..d, oldest..newest)
//                 via  W' = W - S + d*x_new,  S' = S - x_old + x_new.
//   * TimeReg   — ts_slope / ts_rsquare / ts_resid (OLS on the time axis 0..d-1),
//                 the same (S, W) pair plus Q = sum of squares.
//
// The SAME per-instrument "lane" structs drive the batch column/panel sweeps
// here and the streaming engine (streaming_engine.hpp): a lane is stepped with
// the entering value, the leaving value (if any) and a view of the current
// window, and returns that date's output.
//
// ===========================================================================
//  NUMERICS — shifted sums + periodic re-centering (ResearchFast tier)
// ===========================================================================
//  Raw running sums (Sx, Sxx, ...) lose digits to cancellation when a column's
//  mean is large relative to its spread (prices ~1e4, volume ~1e8). Every lane
//  therefore accumulates SHIFTED values v - c, where the shift c is a recent
//  window value; each lane re-centres (c := newest value, sums rebuilt from the
//  window in O(d)) once every kReseedMul*d clean steps. That bounds both the
//  cancellation (the window mean stays within a few window-spreads of c) and the
//  add/subtract roundoff drift (it never accumulates for more than one reseed
//  period). The rebuild costs O(d) per O(d) steps -> O(1) amortized.
//
//  A centred second moment that is below the rounding noise of the raw shifted
//  sum (cxx <= kCancelGuard * sxx) is treated as exactly zero -> the zero-
//  variance NaN of the batch kernels. On exactly-representable constant windows
//  both paths produce NaN; on a constant window of a non-representable decimal
//  the batch two-pass returns a finite value computed from pure roundoff, and
//  this path returns NaN — the documented, more-correct divergence.
//
//  These kernels are NOT bit-identical to the batch two-pass recompute, so they
//  ship only under EvalMode::ResearchFast (ts_ops.hpp routes the unary ones;
//  the pair ops are exported for the VM owner to wire — see sweep_comoment).
//
//  NaN / inf POLICY — identical outputs to the batch kernels:
//    * warm-up (t+1 < d) or any NaN in the window -> NaN.
//    * corr/cov/regression/slope/rsquare/resid: a +/-inf in the window makes the
//      batch two-pass produce NaN (inf - inf); the lanes count non-finite cells
//      as missing -> NaN.
//    * decay_linear/wma: the batch weighted sum of a window holding +inf (only)
//      is +inf, -inf (only) is -inf, both is NaN — the lane counts each sign and
//      reproduces exactly that.
//
// Header-only; every function is `inline`.

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/registry.hpp" // OpCode

namespace atx::engine::alpha::sliding {

inline constexpr atx::f64 kSlNaN = std::numeric_limits<atx::f64>::quiet_NaN();
inline constexpr atx::f64 kCancelGuard = 64.0 * std::numeric_limits<atx::f64>::epsilon();
inline constexpr atx::usize kReseedMul = 2;
inline constexpr atx::usize kReseedMin = 4;

[[nodiscard]] inline atx::usize reseed_period(atx::usize d) noexcept {
  return std::max(kReseedMin, kReseedMul * d);
}

// ===========================================================================
//  CoMoment — shifted co-moment sums of (x, y) pairs.
// ===========================================================================
struct CoMoment {
  atx::f64 cx{0.0};
  atx::f64 cy{0.0};
  atx::f64 sx{0.0};
  atx::f64 sy{0.0};
  atx::f64 sxx{0.0};
  atx::f64 syy{0.0};
  atx::f64 sxy{0.0};
  atx::usize n{0};

  void reset(atx::f64 shift_x, atx::f64 shift_y) noexcept {
    *this = CoMoment{};
    cx = shift_x;
    cy = shift_y;
  }
  void push(atx::f64 x, atx::f64 y) noexcept {
    if (n == 0) {
      reset(x, y); // an empty window re-centres for free on its first pair
    }
    const atx::f64 a = x - cx;
    const atx::f64 b = y - cy;
    sx += a;
    sy += b;
    sxx += a * a;
    syy += b * b;
    sxy += a * b;
    ++n;
  }
  void pop(atx::f64 x, atx::f64 y) noexcept {
    if (n <= 1) {
      reset(cx, cy); // last pair leaves: drop any residual roundoff
      return;
    }
    const atx::f64 a = x - cx;
    const atx::f64 b = y - cy;
    sx -= a;
    sy -= b;
    sxx -= a * a;
    syy -= b * b;
    sxy -= a * b;
    --n;
  }
  // Centred sums given inv = 1/n (one division per output, not three); a value
  // inside the roundoff noise of its raw sum is zero.
  [[nodiscard]] atx::f64 cxx(atx::f64 inv) const noexcept {
    const atx::f64 c = sxx - sx * sx * inv;
    return c <= kCancelGuard * sxx ? 0.0 : c;
  }
  [[nodiscard]] atx::f64 cyy(atx::f64 inv) const noexcept {
    const atx::f64 c = syy - sy * sy * inv;
    return c <= kCancelGuard * syy ? 0.0 : c;
  }
  [[nodiscard]] atx::f64 cxy(atx::f64 inv) const noexcept { return sxy - sx * sy * inv; }
  [[nodiscard]] atx::f64 inv_n() const noexcept { return 1.0 / static_cast<atx::f64>(n); }

  // Sample (ddof=1) covariance; NaN for n < 2.
  [[nodiscard]] atx::f64 cov() const noexcept {
    return n < 2 ? kSlNaN : cxy(inv_n()) / static_cast<atx::f64>(n - 1);
  }
  // Pearson correlation clamped to [-1, 1]; NaN for n < 2 or a zero variance.
  [[nodiscard]] atx::f64 corr() const noexcept {
    if (n < 2) {
      return kSlNaN;
    }
    const atx::f64 inv = inv_n();
    const atx::f64 vx = cxx(inv);
    const atx::f64 vy = cyy(inv);
    if (vx == 0.0 || vy == 0.0) {
      return kSlNaN;
    }
    const atx::f64 r = cxy(inv) / std::sqrt(vx * vy);
    return std::clamp(r, -1.0, 1.0);
  }
  // OLS slope of x (dependent) on y (predictor) — ts_regression; NaN if the
  // predictor variance is zero or n < 2.
  [[nodiscard]] atx::f64 slope() const noexcept {
    if (n < 2) {
      return kSlNaN;
    }
    const atx::f64 inv = inv_n();
    const atx::f64 vy = cyy(inv);
    return vy == 0.0 ? kSlNaN : cxy(inv) / vy;
  }
  // Residual of the pair (x, y) against the x-on-y fit.
  [[nodiscard]] atx::f64 resid(atx::f64 x, atx::f64 y) const noexcept {
    const atx::f64 b = slope();
    const atx::f64 nf = static_cast<atx::f64>(n);
    return (x - cx) - sx / nf - b * ((y - cy) - sy / nf);
  }
};

[[nodiscard]] inline bool is_comoment_op(OpCode op) noexcept {
  return op == OpCode::TsCorr || op == OpCode::TsCov || op == OpCode::TsRegression;
}

// Tiny windows (d <= kDirectMaxWindow) are computed by the batch two-pass over
// the window instead of from the running sums: with d = 2 or 3 a window's own
// spread can be orders of magnitude below its distance from the shift (a random
// walk that barely moved between two dates), and the raw-moment difference
// sxx - sx^2/n would lose those digits. O(d) with d <= 4 costs no more than the
// slide, and it reproduces the batch summation order (ts_pair_at) exactly.
inline constexpr atx::usize kDirectMaxWindow = 4;

template <class Win>
[[nodiscard]] inline atx::f64 direct_pair(OpCode op, atx::usize d, const Win &win) noexcept {
  if (d < 2) {
    return kSlNaN;
  }
  const atx::f64 nf = static_cast<atx::f64>(d);
  atx::f64 sxa = 0.0;
  atx::f64 sya = 0.0;
  for (atx::usize i = 0; i < d; ++i) {
    const auto [a, b] = win(i);
    sxa += a;
    sya += b;
  }
  const atx::f64 ma = sxa / nf;
  const atx::f64 mb = sya / nf;
  atx::f64 sab = 0.0;
  atx::f64 saa = 0.0;
  atx::f64 sbb = 0.0;
  for (atx::usize i = 0; i < d; ++i) {
    const auto [a, b] = win(i);
    sab += (a - ma) * (b - mb);
    saa += (a - ma) * (a - ma);
    sbb += (b - mb) * (b - mb);
  }
  switch (op) {
  case OpCode::TsCorr: {
    const atx::f64 denom = std::sqrt(saa * sbb);
    return denom == 0.0 ? kSlNaN : sab / denom;
  }
  case OpCode::TsCov:
    return sab / (nf - 1.0);
  case OpCode::TsRegression:
    return sbb == 0.0 ? kSlNaN : sab / sbb;
  default:
    ATX_UNREACHABLE();
  }
}

// One instrument's sliding pair state + the batch NaN gate. `Win` is a callable
// `(atx::usize i) -> std::pair<f64,f64>` giving window pair i (0 = oldest) of the
// CURRENT d-cell window; it is consulted only on a re-centre.
struct CoMomentLane {
  CoMoment m;
  atx::usize miss{0}; // non-finite pairs currently in the window
  atx::usize age{0};  // clean steps since the last re-centre

  template <class Win>
  [[nodiscard]] atx::f64 step(OpCode op, atx::f64 xe, atx::f64 ye, bool has_leave, atx::f64 xl,
                              atx::f64 yl, bool full, atx::usize d, const Win &win) noexcept {
    if (std::isfinite(xe) && std::isfinite(ye)) {
      m.push(xe, ye);
    } else {
      ++miss;
    }
    if (has_leave) {
      if (std::isfinite(xl) && std::isfinite(yl)) {
        m.pop(xl, yl);
      } else {
        --miss;
      }
    }
    if (!full || miss != 0) {
      return kSlNaN;
    }
    if (d <= kDirectMaxWindow) {
      return direct_pair(op, d, win);
    }
    if (++age >= reseed_period(d)) {
      age = 0;
      m.reset(xe, ye);
      for (atx::usize i = 0; i < d; ++i) {
        const auto [wx, wy] = win(i);
        m.push(wx, wy);
      }
    }
    switch (op) {
    case OpCode::TsCorr:
      return m.corr();
    case OpCode::TsCov:
      return m.cov();
    case OpCode::TsRegression:
      return m.slope();
    default:
      ATX_UNREACHABLE();
    }
  }
};

// ===========================================================================
//  LinDecay — linear-decay weighted mean (weights 1..d oldest..newest, /Σw).
// ===========================================================================
struct LinDecay {
  atx::f64 c{0.0}; // shift
  atx::f64 s{0.0}; // Σ (v - c) over the d positions (missing = 0)
  atx::f64 w{0.0}; // Σ k (v - c), k = 1 (oldest) .. d (newest)

  // Slide one position: `e` enters at weight d, `l` leaves from weight 1 (both
  // already shifted; 0 for a missing / pre-start cell).
  void slide(atx::f64 e, atx::f64 l, atx::usize d) noexcept {
    w = w - s + static_cast<atx::f64>(d) * e;
    s = s - l + e;
  }
  [[nodiscard]] atx::f64 value(atx::usize d) const noexcept {
    const atx::f64 df = static_cast<atx::f64>(d);
    return c + w / (df * (df + 1.0) / 2.0);
  }
};

[[nodiscard]] inline bool is_decay_op(OpCode op) noexcept {
  return op == OpCode::TsDecayLinear || op == OpCode::TsWma;
}

// Per-instrument lane for LinDecay (also serves TimeReg's (S, W) pair). `Win` is
// `(atx::usize i) -> f64` over the current window (0 = oldest).
struct LinDecayLane {
  LinDecay k;
  atx::usize fin{0}; // finite cells currently in the window
  atx::usize nan{0};
  atx::usize pinf{0};
  atx::usize ninf{0};
  atx::usize age{0};

  [[nodiscard]] atx::f64 shifted(atx::f64 v) const noexcept {
    return std::isfinite(v) ? v - k.c : 0.0;
  }
  void count(atx::f64 v, atx::isize sign) noexcept {
    if (std::isnan(v)) {
      nan = static_cast<atx::usize>(static_cast<atx::isize>(nan) + sign);
    } else if (v == std::numeric_limits<atx::f64>::infinity()) {
      pinf = static_cast<atx::usize>(static_cast<atx::isize>(pinf) + sign);
    } else if (v == -std::numeric_limits<atx::f64>::infinity()) {
      ninf = static_cast<atx::usize>(static_cast<atx::isize>(ninf) + sign);
    }
  }

  template <class Win>
  [[nodiscard]] atx::f64 step(atx::f64 xe, bool has_leave, atx::f64 xl, bool full, atx::usize d,
                              const Win &win) noexcept {
    // Steady state: a full, all-finite window with finite enter/leave cells.
    // Exactly the operations the general path below performs for that case
    // (count() and the fin bookkeeping are no-ops / cancel), minus its branches.
    if (full && has_leave && fin == d && nan == 0 && pinf == 0 && ninf == 0 &&
        std::isfinite(xe) && std::isfinite(xl)) {
      k.slide(xe - k.c, xl - k.c, d);
      if (++age >= reseed_period(d)) {
        recentre(xe, d, win);
      }
      return k.value(d);
    }
    if (std::isfinite(xe)) {
      if (fin == 0) {
        k = LinDecay{}; // no finite cell in the window: every sum is exactly 0
        k.c = xe;
      }
      ++fin;
    }
    count(xe, +1);
    const atx::f64 l = has_leave ? shifted(xl) : 0.0;
    if (has_leave) {
      count(xl, -1);
      fin -= static_cast<atx::usize>(std::isfinite(xl));
    }
    k.slide(shifted(xe), l, d);
    if (!full || nan != 0) {
      return kSlNaN;
    }
    if (pinf != 0 || ninf != 0) {
      if (pinf != 0 && ninf != 0) {
        return kSlNaN;
      }
      return pinf != 0 ? std::numeric_limits<atx::f64>::infinity()
                       : -std::numeric_limits<atx::f64>::infinity();
    }
    if (++age >= reseed_period(d)) {
      recentre(xe, d, win);
    }
    return k.value(d);
  }

private:
  // Rebuild (S, W) from the window around the newest value. Precondition: the
  // window is full and every cell finite.
  template <class Win> void recentre(atx::f64 xe, atx::usize d, const Win &win) noexcept {
    age = 0;
    k = LinDecay{};
    k.c = xe;
    for (atx::usize i = 0; i < d; ++i) {
      const atx::f64 v = win(i) - k.c;
      k.s += v;
      k.w += static_cast<atx::f64>(i + 1) * v;
    }
  }
};

// ===========================================================================
//  TimeReg — OLS of the window on the time axis 0..d-1 (slope / r² / resid).
// ===========================================================================
[[nodiscard]] inline bool is_timereg_op(OpCode op) noexcept {
  return op == OpCode::TsSlope || op == OpCode::TsRsquare || op == OpCode::TsResid;
}

// Tiny-window direct fit — the batch tsv_lin_fit summation order (see
// kDirectMaxWindow for why tiny windows skip the running sums). Precondition:
// d >= 2 and every window cell finite.
template <class Win>
[[nodiscard]] inline atx::f64 direct_timereg(OpCode op, atx::usize d, atx::f64 last,
                                             const Win &win) noexcept {
  const atx::f64 nf = static_cast<atx::f64>(d);
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  for (atx::usize i = 0; i < d; ++i) {
    const atx::f64 xi = static_cast<atx::f64>(i);
    const atx::f64 yi = win(i);
    sx += xi;
    sy += yi;
    sxx += xi * xi;
    sxy += xi * yi;
  }
  const atx::f64 slope = (nf * sxy - sx * sy) / (nf * sxx - sx * sx);
  const atx::f64 intercept = (sy - slope * sx) / nf;
  switch (op) {
  case OpCode::TsSlope:
    return slope;
  case OpCode::TsResid:
    return last - (intercept + slope * (nf - 1.0));
  case OpCode::TsRsquare: {
    const atx::f64 my = sy / nf;
    atx::f64 ss_tot = 0.0;
    atx::f64 ss_res = 0.0;
    for (atx::usize i = 0; i < d; ++i) {
      const atx::f64 yi = win(i);
      const atx::f64 fit = intercept + slope * static_cast<atx::f64>(i);
      ss_tot += (yi - my) * (yi - my);
      ss_res += (yi - fit) * (yi - fit);
    }
    return ss_tot == 0.0 ? kSlNaN : 1.0 - ss_res / ss_tot;
  }
  default:
    ATX_UNREACHABLE();
  }
}

struct TimeRegLane {
  LinDecay k;      // shift c, S = Σ(v-c), W = Σ k (v-c)
  atx::f64 q{0.0};  // Σ (v - c)^2
  atx::usize miss{0}; // non-finite cells in the window
  atx::usize fin{0};  // finite cells in the window
  atx::usize age{0};

  template <class Win>
  [[nodiscard]] atx::f64 step(OpCode op, atx::f64 xe, bool has_leave, atx::f64 xl, bool full,
                              atx::usize d, const Win &win) noexcept {
    atx::f64 e = 0.0;
    if (std::isfinite(xe)) {
      if (fin == 0) {
        k = LinDecay{}; // empty of finite cells: re-centre for free
        k.c = xe;
        q = 0.0;
      }
      ++fin;
      e = xe - k.c;
    } else {
      ++miss;
    }
    atx::f64 l = 0.0;
    if (has_leave) {
      if (std::isfinite(xl)) {
        l = xl - k.c;
        --fin;
      } else {
        --miss;
      }
    }
    k.slide(e, l, d);
    q = q - l * l + e * e;
    if (!full || miss != 0 || d < 2) {
      return kSlNaN;
    }
    if (d <= kDirectMaxWindow) {
      return direct_timereg(op, d, xe, win);
    }
    if (++age >= reseed_period(d)) {
      age = 0;
      k = LinDecay{};
      k.c = xe;
      q = 0.0;
      for (atx::usize i = 0; i < d; ++i) {
        const atx::f64 v = win(i) - k.c;
        k.s += v;
        k.w += static_cast<atx::f64>(i + 1) * v;
        q += v * v;
      }
      e = xe - k.c;
    }
    const atx::f64 df = static_cast<atx::f64>(d);
    const atx::f64 ibar = (df - 1.0) / 2.0;
    const atx::f64 sii = df * (df * df - 1.0) / 12.0;
    const atx::f64 num = k.w - (ibar + 1.0) * k.s; // Σ (i - ibar)(v - c)
    const atx::f64 slope = num / sii;
    switch (op) {
    case OpCode::TsSlope:
      return slope;
    case OpCode::TsResid:
      return e - (k.s / df + slope * ibar);
    case OpCode::TsRsquare: {
      const atx::f64 ss_tot = q - k.s * k.s / df;
      if (ss_tot <= kCancelGuard * q) {
        return kSlNaN;
      }
      return std::clamp(num * num / (sii * ss_tot), 0.0, 1.0);
    }
    default:
      ATX_UNREACHABLE();
    }
  }
};

// ===========================================================================
//  Sweeps over a date-major (date*instruments + inst) panel. Date-outer,
//  instrument-inner: every row read is contiguous, so a whole-panel sweep runs
//  at streaming bandwidth. [j0, j1) selects the instrument columns (a single
//  column is the per-column VM entry).
// ===========================================================================

// Unary sliding ops (decay_linear / wma / slope / rsquare / resid).
[[nodiscard]] inline bool is_unary_sliding_op(OpCode op) noexcept {
  return is_decay_op(op) || is_timereg_op(op);
}

inline void sweep_unary(OpCode op, std::span<const atx::f64> x, std::span<atx::f64> out,
                        atx::usize dates, atx::usize instruments, atx::usize d, atx::usize j0,
                        atx::usize j1) {
  ATX_ASSERT(is_unary_sliding_op(op) && j0 <= j1 && j1 <= instruments);
  const atx::usize lanes = j1 - j0;
  if (d == 0) {
    for (atx::usize t = 0; t < dates; ++t) {
      for (atx::usize j = j0; j < j1; ++j) {
        out[t * instruments + j] = kSlNaN;
      }
    }
    return;
  }
  const bool decay = is_decay_op(op);
  // A single column (the per-column VM entry) keeps its lane on the stack, so the
  // column sweep allocates nothing; a multi-column sweep allocates once per call.
  LinDecayLane one_dl;
  TimeRegLane one_tl;
  std::vector<LinDecayLane> many_dl(decay && lanes > 1 ? lanes : 0);
  std::vector<TimeRegLane> many_tl(!decay && lanes > 1 ? lanes : 0);
  LinDecayLane *dl = lanes > 1 ? many_dl.data() : &one_dl;
  TimeRegLane *tl = lanes > 1 ? many_tl.data() : &one_tl;
  for (atx::usize t = 0; t < dates; ++t) {
    const bool has_leave = t >= d;
    const bool full = t + 1 >= d;
    const atx::f64 *row = x.data() + t * instruments;
    const atx::f64 *lrow = has_leave ? x.data() + (t - d) * instruments : row;
    atx::f64 *orow = out.data() + t * instruments;
    for (atx::usize j = j0; j < j1; ++j) {
      // SAFETY: the window view is only read on a re-centre, i.e. when full
      // (t+1 >= d), so (t+1-d+i) is a valid date for i < d.
      const auto win = [&x, t, d, instruments, j](atx::usize i) noexcept {
        return x[(t + 1 - d + i) * instruments + j];
      };
      orow[j] = decay ? dl[j - j0].step(row[j], has_leave, lrow[j], full, d, win)
                      : tl[j - j0].step(op, row[j], has_leave, lrow[j], full, d, win);
    }
  }
}

// Pair ops (corr / cov / regression). Same layout contract as sweep_unary.
inline void sweep_comoment(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> y,
                           std::span<atx::f64> out, atx::usize dates, atx::usize instruments,
                           atx::usize d, atx::usize j0, atx::usize j1) {
  ATX_ASSERT(is_comoment_op(op) && j0 <= j1 && j1 <= instruments);
  if (d == 0) {
    for (atx::usize t = 0; t < dates; ++t) {
      for (atx::usize j = j0; j < j1; ++j) {
        out[t * instruments + j] = kSlNaN;
      }
    }
    return;
  }
  CoMomentLane one;
  std::vector<CoMomentLane> many(j1 - j0 > 1 ? j1 - j0 : 0);
  CoMomentLane *lanes = j1 - j0 > 1 ? many.data() : &one;
  for (atx::usize t = 0; t < dates; ++t) {
    const bool has_leave = t >= d;
    const bool full = t + 1 >= d;
    const atx::usize r = t * instruments;
    const atx::usize lr = has_leave ? (t - d) * instruments : r;
    for (atx::usize j = j0; j < j1; ++j) {
      const auto win = [&x, &y, t, d, instruments, j](atx::usize i) noexcept {
        const atx::usize k = (t + 1 - d + i) * instruments + j;
        return std::pair<atx::f64, atx::f64>{x[k], y[k]};
      };
      out[r + j] = lanes[j - j0].step(op, x[r + j], y[r + j], has_leave, x[lr + j], y[lr + j],
                                      full, d, win);
    }
  }
}

} // namespace atx::engine::alpha::sliding

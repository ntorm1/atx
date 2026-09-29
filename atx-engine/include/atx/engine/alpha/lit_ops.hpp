#pragma once

// atx::engine::alpha — platform-v7 W2 literature-family kernels (A7).
//
// The production kernels of the W2 opcodes (registry.hpp, appended after Free).
// vm.hpp and streaming_engine.hpp BOTH call the per-cell / per-row functions
// below, so streaming == batch holds by construction; the reference oracle
// restates every rule independently (src/alpha/oracle_lit.cpp) and the
// differential suite (tests/alpha/alpha_lit_ops_test.cpp) proves VM == oracle
// bit-for-bit. Nothing here touches a pre-W2 opcode.
//
// ===========================================================================
//  PINNED SEMANTICS (window = the op's LAST operand, trailing [t-w+1, t])
// ===========================================================================
//  Full-window ops follow the house rule (ts_ops.hpp): NaN while t+1 < w.
//  * ts_topk_mean(x, w, k): NaN if the window is short or holds any NaN; else
//    the mean of the k largest values (ascending sort, the top k summed in
//    ascending order, / k). k in [1, w].
//  * ts_count_increases(x, w): x is read as a signed CHANGE series (an increase
//    is x > 0, a decrease x < 0, no event x == 0). NaN if the window is short or
//    x[t] is NaN; else walking back from t: x > 0 counts, x == 0 is skipped,
//    x < 0 or NaN ends the run. Result in [0, w]. For a level series L use
//    ts_count_increases(delta(L, 1), w); for nincr feed the report-gated YoY
//    sign (see the task report).
//  * ts_resid_on / ts_beta_on(y, x1[, x2[, x3]], w): OLS of y on [1, x1..xk]
//    over the window. NaN if the window is short or ANY y / x cell in it is
//    non-finite, or the design is degenerate (a flat regressor: population std
//    <= 1e-10 |mean| as A-09; or a Cholesky pivot <= 1e-12 of its diagonal:
//    collinear). A flat y yields exactly 0 (residual and slope). resid_on is
//    the residual at t; beta_on the slope on x1 (partial, given x2..xk).
//  Min-periods family (x, w, m) — pandas rolling(w, min_periods=m) semantics:
//  the window is the last min(t+1, w) dates (short at the panel start); a cell
//  counts iff FINITE (NaN / +-inf are missing); n = count.
//  * ts_sum_mp / ts_mean_mp: NaN if n < m, else the chronological sum / sum/n.
//  * ts_std_mp: NaN if n < max(m, 2); sample std (ddof 1); a flat window -> 0.
//  * ts_zscore_mp: NaN if x[t] is not finite or n < max(m, 2) or the window is
//    flat; else (x[t] - mean) / sample std.
//  * ts_min_mp / ts_max_mp: NaN if n < m, else the extreme of the finite cells.
//  * decay_linear_mp: NaN if n < m; a cell of age a (0 = t) weighs w - a; the
//    weighted mean over finite cells (Σ w·x / Σ w, chronological).
//  * ts_corr_mp(x, y, w, m): pairs with both finite; NaN if n < max(m, 2), a
//    flat side (A-09) or a zero denominator; else Pearson.
//  Cross-sectional (per date; the VM's Cs eligibility mask applies):
//  * bucket(x, n) -> Group: the bucket index quantile(x, n) assigns, as a label
//    in [0, n-1] (average rank, so ties share a bucket; quantile(x, n) ==
//    bucket(x, n) / (n-1) bit-for-bit). NaN x -> NaN (no group). n in [2, 65535].
//  * cs_resid_on(x, c1..ck), k <= 4: OLS of x on [1, c1..ck] over the rows where
//    x and every covariate are finite; the residual there, NaN elsewhere; the
//    whole date NaN when degenerate (as above; n < k + 2 included); flat x -> 0.
//  Element-wise:
//  * group_cross(g1, g2) -> Group: g1 * 2^26 + g2 when both are integers in
//    [0, 2^26) (exact in f64), else NaN (no group). Stable across dates.
//  * pack2 / pack3: copy the operand columns into the record block.
//
//  Summation order is chronological (Ts) / ascending instrument (Cs); the OLS
//  accumulates every centred cross-product in row order, then solves by
//  Cholesky in index order — the oracle restates the identical sequence.
//
// Header-only; every free function is `inline`.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/cs_ops.hpp"   // CsScratch, cs_stable_argsort, cs_for_each_rank
#include "atx/engine/alpha/registry.hpp" // OpCode, lit_reg_*

namespace atx::engine::alpha::detail {

inline constexpr atx::f64 kLitNaN = std::numeric_limits<atx::f64>::quiet_NaN();
inline constexpr atx::f64 kLitFlatRelTol = 1e-10;     // A-09's relative flat test
inline constexpr atx::f64 kLitCollinearTol = 1e-12;   // Cholesky pivot / its diagonal
inline constexpr atx::f64 kLitGroupRadix = 67108864.0; // 2^26: group_cross label radix
inline constexpr atx::usize kLitMaxReg = 4;            // max regressors of any W2 OLS

// A-09 flat test: population std at or below kLitFlatRelTol * |mean|.
[[nodiscard]] inline bool lit_flat(atx::f64 ss, atx::f64 mean, atx::usize n) noexcept {
  return std::sqrt(ss / static_cast<atx::f64>(n)) <= kLitFlatRelTol * std::fabs(mean);
}

// Caller-owned scratch, grown on demand (never inside a steady-state loop once
// the largest window has been seen).
struct LitScratch {
  std::vector<atx::f64> win;    // n_in gathered windows, input-major
  std::vector<atx::f64> sort;   // ts_topk_mean sort buffer
  std::vector<atx::f64> rows;   // OLS design rows, row-major (1 + k columns)
  std::vector<atx::usize> rset; // cs_resid_on regression rows
};

// ---------------------------------------------------------------------------
//  OLS core (shared by ts_resid_on / ts_beta_on / cs_resid_on)
// ---------------------------------------------------------------------------

struct LitOls {
  std::array<atx::f64, kLitMaxReg + 1> mean{}; // [0] y, [1 + j] x_j
  std::array<atx::f64, kLitMaxReg> beta{};
  bool ok{false};    // design non-degenerate, beta valid
  bool yflat{false}; // y flat (A-09): residual and slope are exactly 0
};

// OLS of y on [1, x_1..x_k] over `n` rows of `rows` (row i: y at [i*(k+1)], x_j
// at [i*(k+1) + 1 + j]). Degenerate (ok == false): k outside [1, 4], n < k + 2,
// a flat regressor, or a collinear / non-finite Cholesky pivot.
[[nodiscard]] inline LitOls lit_ols(std::span<const atx::f64> rows, atx::usize n,
                                    atx::usize k) noexcept {
  LitOls f;
  const atx::usize c = k + 1;
  if (k == 0 || k > kLitMaxReg || n < k + 2) {
    return f;
  }
  for (atx::usize col = 0; col < c; ++col) {
    atx::f64 s = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      s += rows[i * c + col];
    }
    f.mean[col] = s / static_cast<atx::f64>(n);
  }
  std::array<std::array<atx::f64, kLitMaxReg>, kLitMaxReg> sxx{};
  std::array<atx::f64, kLitMaxReg> sxy{};
  atx::f64 syy = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 yc = rows[i * c] - f.mean[0];
    syy += yc * yc;
    for (atx::usize j = 0; j < k; ++j) {
      const atx::f64 xj = rows[i * c + 1 + j] - f.mean[1 + j];
      sxy[j] += xj * yc;
      for (atx::usize l = 0; l <= j; ++l) {
        sxx[j][l] += xj * (rows[i * c + 1 + l] - f.mean[1 + l]);
      }
    }
  }
  for (atx::usize j = 0; j < k; ++j) {
    if (lit_flat(sxx[j][j], f.mean[1 + j], n)) {
      return f; // flat regressor: undefined slope (A-09)
    }
  }
  std::array<std::array<atx::f64, kLitMaxReg>, kLitMaxReg> ch{};
  for (atx::usize j = 0; j < k; ++j) {
    atx::f64 dj = sxx[j][j];
    for (atx::usize l = 0; l < j; ++l) {
      dj -= ch[j][l] * ch[j][l];
    }
    if (!(dj > kLitCollinearTol * sxx[j][j])) {
      return f; // collinear (or non-finite) design
    }
    ch[j][j] = std::sqrt(dj);
    for (atx::usize q = j + 1; q < k; ++q) {
      atx::f64 a = sxx[q][j];
      for (atx::usize l = 0; l < j; ++l) {
        a -= ch[q][l] * ch[j][l];
      }
      ch[q][j] = a / ch[j][j];
    }
  }
  std::array<atx::f64, kLitMaxReg> z{};
  for (atx::usize j = 0; j < k; ++j) {
    atx::f64 a = sxy[j];
    for (atx::usize l = 0; l < j; ++l) {
      a -= ch[j][l] * z[l];
    }
    z[j] = a / ch[j][j];
  }
  for (atx::usize j = k; j-- > 0;) {
    atx::f64 a = z[j];
    for (atx::usize l = j + 1; l < k; ++l) {
      a -= ch[l][j] * f.beta[l];
    }
    f.beta[j] = a / ch[j][j];
  }
  f.ok = true;
  f.yflat = lit_flat(syy, f.mean[0], n);
  return f;
}

// Residual of row i under a successful fit (0 when y is flat).
[[nodiscard]] inline atx::f64 lit_ols_resid(const LitOls &f, std::span<const atx::f64> rows,
                                            atx::usize i, atx::usize k) noexcept {
  if (f.yflat) {
    return 0.0;
  }
  const atx::usize c = k + 1;
  atx::f64 r = rows[i * c] - f.mean[0];
  for (atx::usize j = 0; j < k; ++j) {
    r -= f.beta[j] * (rows[i * c + 1 + j] - f.mean[1 + j]);
  }
  return r;
}

// ---------------------------------------------------------------------------
//  Trailing-window cell kernels (window `w` chronological, newest last)
// ---------------------------------------------------------------------------

[[nodiscard]] inline atx::f64 lit_topk_mean(std::span<const atx::f64> w, atx::usize d,
                                            atx::usize k, std::vector<atx::f64> &buf) {
  if (w.size() < d || k == 0 || k > w.size()) {
    return kLitNaN;
  }
  for (const atx::f64 v : w) {
    if (std::isnan(v)) {
      return kLitNaN;
    }
  }
  buf.assign(w.begin(), w.end());
  std::sort(buf.begin(), buf.end());
  atx::f64 sum = 0.0;
  for (atx::usize i = buf.size() - k; i < buf.size(); ++i) {
    sum += buf[i];
  }
  return sum / static_cast<atx::f64>(k);
}

[[nodiscard]] inline atx::f64 lit_count_increases(std::span<const atx::f64> w,
                                                  atx::usize d) noexcept {
  if (w.empty() || w.size() < d || std::isnan(w.back())) {
    return kLitNaN;
  }
  atx::usize count = 0;
  for (atx::usize i = w.size(); i-- > 0;) {
    const atx::f64 v = w[i];
    if (std::isnan(v) || v < 0.0) {
      break;
    }
    if (v > 0.0) {
      ++count;
    }
  }
  return static_cast<atx::f64>(count);
}

// Finite-cell count and chronological sum of a min-periods window.
struct LitMpSum {
  atx::usize n{0};
  atx::f64 sum{0.0};
};

[[nodiscard]] inline LitMpSum lit_mp_sum(std::span<const atx::f64> w) noexcept {
  LitMpSum s;
  for (const atx::f64 v : w) {
    if (std::isfinite(v)) {
      ++s.n;
      s.sum += v;
    }
  }
  return s;
}

// Σ(v - mean)² over the finite cells, chronological.
[[nodiscard]] inline atx::f64 lit_mp_ss(std::span<const atx::f64> w, atx::f64 mean) noexcept {
  atx::f64 ss = 0.0;
  for (const atx::f64 v : w) {
    if (std::isfinite(v)) {
      ss += (v - mean) * (v - mean);
    }
  }
  return ss;
}

[[nodiscard]] inline atx::f64 lit_mp_extreme(std::span<const atx::f64> w, bool want_max) noexcept {
  atx::f64 best = kLitNaN;
  for (const atx::f64 v : w) {
    if (!std::isfinite(v)) {
      continue;
    }
    if (std::isnan(best) || (want_max ? v > best : v < best)) {
      best = v;
    }
  }
  return best;
}

// Age-weighted mean over the finite cells: the cell of age a (0 = newest) of a
// declared window d weighs d - a (so a full valid window weighs 1..d, oldest
// first, as decay_linear).
[[nodiscard]] inline atx::f64 lit_mp_decay(std::span<const atx::f64> w, atx::usize d) noexcept {
  const atx::usize len = w.size();
  atx::f64 num = 0.0;
  atx::f64 den = 0.0;
  for (atx::usize i = 0; i < len; ++i) {
    const atx::f64 v = w[i];
    if (std::isfinite(v)) {
      const atx::f64 wt = static_cast<atx::f64>(d - (len - 1 - i));
      num += wt * v;
      den += wt;
    }
  }
  return num / den;
}

// The unary min-periods family (TsSumMp .. TsDecayLinearMp); `m` >= 1.
[[nodiscard]] inline atx::f64 lit_mp_unary(OpCode op, std::span<const atx::f64> w, atx::usize d,
                                           atx::usize m) noexcept {
  const LitMpSum s = lit_mp_sum(w);
  const bool needs_two = op == OpCode::TsStdMp || op == OpCode::TsZscoreMp;
  const atx::usize need = needs_two ? std::max<atx::usize>(m, 2) : m;
  if (w.empty() || s.n < need) {
    return kLitNaN;
  }
  const atx::f64 nf = static_cast<atx::f64>(s.n);
  switch (op) {
  case OpCode::TsSumMp:
    return s.sum;
  case OpCode::TsMeanMp:
    return s.sum / nf;
  case OpCode::TsStdMp: {
    const atx::f64 mean = s.sum / nf;
    const atx::f64 ss = lit_mp_ss(w, mean);
    return lit_flat(ss, mean, s.n) ? 0.0 : std::sqrt(ss / static_cast<atx::f64>(s.n - 1));
  }
  case OpCode::TsZscoreMp: {
    const atx::f64 last = w.back();
    if (!std::isfinite(last)) {
      return kLitNaN;
    }
    const atx::f64 mean = s.sum / nf;
    const atx::f64 ss = lit_mp_ss(w, mean);
    if (lit_flat(ss, mean, s.n)) {
      return kLitNaN;
    }
    return (last - mean) / std::sqrt(ss / static_cast<atx::f64>(s.n - 1));
  }
  case OpCode::TsMinMp:
    return lit_mp_extreme(w, /*want_max=*/false);
  case OpCode::TsMaxMp:
    return lit_mp_extreme(w, /*want_max=*/true);
  case OpCode::TsDecayLinearMp:
    return lit_mp_decay(w, d);
  default:
    ATX_UNREACHABLE();
  }
}

[[nodiscard]] inline atx::f64 lit_corr_mp(std::span<const atx::f64> a, std::span<const atx::f64> b,
                                          atx::usize m) noexcept {
  atx::usize n = 0;
  atx::f64 sa = 0.0;
  atx::f64 sb = 0.0;
  for (atx::usize i = 0; i < a.size(); ++i) {
    if (std::isfinite(a[i]) && std::isfinite(b[i])) {
      ++n;
      sa += a[i];
      sb += b[i];
    }
  }
  if (n < std::max<atx::usize>(m, 2)) {
    return kLitNaN;
  }
  const atx::f64 ma = sa / static_cast<atx::f64>(n);
  const atx::f64 mb = sb / static_cast<atx::f64>(n);
  atx::f64 sab = 0.0;
  atx::f64 saa = 0.0;
  atx::f64 sbb = 0.0;
  for (atx::usize i = 0; i < a.size(); ++i) {
    if (std::isfinite(a[i]) && std::isfinite(b[i])) {
      sab += (a[i] - ma) * (b[i] - mb);
      saa += (a[i] - ma) * (a[i] - ma);
      sbb += (b[i] - mb) * (b[i] - mb);
    }
  }
  if (lit_flat(saa, ma, n) || lit_flat(sbb, mb, n)) {
    return kLitNaN;
  }
  const atx::f64 den = std::sqrt(saa * sbb);
  return den == 0.0 ? kLitNaN : sab / den;
}

// ts_resid_on / ts_beta_on over `win` = [y window | x_1 window | .. | x_k window],
// each of length `len` (full-window rule: len == d and every cell finite).
[[nodiscard]] inline atx::f64 lit_ts_regress(OpCode op, std::span<const atx::f64> win,
                                             atx::usize k, atx::usize len, atx::usize d,
                                             LitScratch &s) {
  if (len < d || len == 0 || k == 0 || k > kLitMaxReg) {
    return kLitNaN;
  }
  const atx::usize c = k + 1;
  if (s.rows.size() < len * c) {
    s.rows.resize(len * c);
  }
  for (atx::usize i = 0; i < len; ++i) {
    for (atx::usize col = 0; col < c; ++col) {
      const atx::f64 v = win[col * len + i];
      if (!std::isfinite(v)) {
        return kLitNaN;
      }
      s.rows[i * c + col] = v;
    }
  }
  const std::span<const atx::f64> rows{s.rows.data(), len * c};
  const LitOls f = lit_ols(rows, len, k);
  if (!f.ok) {
    return kLitNaN;
  }
  if (op == OpCode::TsBetaOn) {
    return f.yflat ? 0.0 : f.beta[0];
  }
  return lit_ols_resid(f, rows, len - 1, k);
}

// One output cell of a W2 trailing-window op. `win` holds `n_in` chronological
// windows of equal length `len` = min(t+1, d) (input c at [c*len, (c+1)*len)):
// n_in = 1 for the unary ops, 2 for ts_corr_mp, 1 + k for the regressions (y
// first). `p0` is the peeled count (k of topk, m of the mp family).
[[nodiscard]] inline atx::f64 lit_ts_cell(OpCode op, std::span<const atx::f64> win,
                                          atx::usize n_in, atx::usize len, atx::usize d,
                                          atx::f64 p0, LitScratch &s) {
  const std::span<const atx::f64> x = win.first(len);
  switch (op) {
  case OpCode::TsTopkMean:
    return lit_topk_mean(x, d, static_cast<atx::usize>(p0), s.sort);
  case OpCode::TsCountIncreases:
    return lit_count_increases(x, d);
  case OpCode::TsResidOn:
  case OpCode::TsBetaOn:
    return lit_ts_regress(op, win, n_in - 1, len, d, s);
  case OpCode::TsCorrMp:
    return lit_corr_mp(x, win.subspan(len, len), static_cast<atx::usize>(p0));
  default:
    return lit_mp_unary(op, x, d, static_cast<atx::usize>(p0));
  }
}

// Number of input columns a W2 Ts instruction reads (see lit_ts_cell).
[[nodiscard]] inline atx::usize lit_ts_inputs(OpCode op, atx::u32 param) noexcept {
  if (op == OpCode::TsCorrMp) {
    return 2;
  }
  if (op == OpCode::TsResidOn || op == OpCode::TsBetaOn) {
    return 1 + lit_reg_wb(param);
  }
  return 1;
}

// ---------------------------------------------------------------------------
//  Cross-sectional row kernels (valid = non-NaN x in ascending instrument order,
//  already masked by the caller; `out` pre-filled with NaN)
// ---------------------------------------------------------------------------

inline void lit_bucket_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                           int nb, std::span<atx::f64> out, CsScratch &scratch) {
  const atx::usize m = valid.size();
  if (m == 0 || nb < 2) {
    return;
  }
  std::vector<atx::usize> &order = scratch.order;
  order.assign(valid.begin(), valid.end());
  cs_stable_argsort(x, std::span<atx::usize>{order}, scratch.radix);
  const atx::f64 rdenom = static_cast<atx::f64>(m - 1);
  cs_for_each_rank(x, std::span<const atx::usize>{order}, RankTies::Average,
                   [&out, m, nb, rdenom](atx::usize i, atx::f64 rank) noexcept {
                     const atx::f64 p = (m == 1) ? 0.5 : rank / rdenom;
                     int b = static_cast<int>(p * static_cast<atx::f64>(nb)); // p >= 0
                     if (b >= nb) {
                       b = nb - 1;
                     }
                     out[i] = static_cast<atx::f64>(b);
                   });
}

// cs_resid_on for one date: `cov[0..k)` are the covariate rows.
inline void lit_cs_resid_row(std::span<const atx::f64> x,
                             std::span<const std::span<const atx::f64>> cov,
                             const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                             LitScratch &s) {
  const atx::usize k = cov.size();
  if (k == 0 || k > kLitMaxReg) {
    return;
  }
  s.rset.clear();
  for (const atx::usize i : valid) {
    bool ok = std::isfinite(x[i]);
    for (atx::usize j = 0; j < k && ok; ++j) {
      ok = std::isfinite(cov[j][i]);
    }
    if (ok) {
      s.rset.push_back(i);
    }
  }
  const atx::usize n = s.rset.size();
  const atx::usize c = k + 1;
  if (s.rows.size() < n * c) {
    s.rows.resize(n * c);
  }
  for (atx::usize p = 0; p < n; ++p) {
    const atx::usize i = s.rset[p];
    s.rows[p * c] = x[i];
    for (atx::usize j = 0; j < k; ++j) {
      s.rows[p * c + 1 + j] = cov[j][i];
    }
  }
  const std::span<const atx::f64> rows{s.rows.data(), n * c};
  const LitOls f = lit_ols(rows, n, k);
  if (!f.ok) {
    return; // degenerate date: every cell stays NaN
  }
  for (atx::usize p = 0; p < n; ++p) {
    out[s.rset[p]] = lit_ols_resid(f, rows, p, k);
  }
}

// ---------------------------------------------------------------------------
//  Element-wise
// ---------------------------------------------------------------------------

[[nodiscard]] inline atx::f64 lit_group_cross(atx::f64 g1, atx::f64 g2) noexcept {
  const auto label = [](atx::f64 g) noexcept {
    return g >= 0.0 && g < kLitGroupRadix && g == std::floor(g); // false for NaN
  };
  if (!label(g1) || !label(g2)) {
    return kLitNaN;
  }
  return g1 * kLitGroupRadix + g2;
}

} // namespace atx::engine::alpha::detail

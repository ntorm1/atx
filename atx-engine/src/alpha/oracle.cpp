#include "atx/engine/alpha/oracle.hpp"

#include <algorithm> // std::stable_sort, std::sort, std::min_element, std::max_element
#include <array>     // std::array (entropy bucket counts)
#include <cmath>     // std::sqrt, std::fabs, std::log, std::exp, std::pow
#include <span>      // std::span
#include <vector>    // std::vector

namespace atx::engine::alpha {

namespace detail {

// =========================================================================
//  Cross-sectional kernels — per date-row over the valid set.
// =========================================================================

// Ordinal percentile rank in [0,1] over `valid` indices, tie-broken by ascending
// instrument index (NaNs already excluded). Rank r (0-based) of n maps to
// r/(n-1); a singleton set maps to 0.5 (centred — avoids a degenerate 0/0).
void cs_rank(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
             std::span<atx::f64> out) {
  const atx::usize n = valid.size();
  if (n == 0) {
    return;
  }
  std::vector<atx::usize> order = valid;
  // Stable sort by value, ties by instrument index (order already ascending in
  // index, std::stable_sort preserves it) -> deterministic ordinal tie-break.
  std::stable_sort(order.begin(), order.end(),
                   [&](atx::usize i, atx::usize j) { return x[i] < x[j]; });
  for (atx::usize r = 0; r < n; ++r) {
    const atx::f64 pct = (n == 1) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(n - 1);
    out[order[r]] = pct;
  }
}

// CsZscore: (x - mean) / sample-std over the valid set; out-of-set -> NaN. With
// fewer than 2 valid observations the std is undefined -> all NaN.
void cs_zscore(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
               std::span<atx::f64> out) {
  const std::vector<atx::f64> v = gather(x, valid);
  const atx::f64 mean = mean_of(v);
  const atx::f64 sd = sample_std(v, mean);
  for (const atx::usize i : valid) {
    out[i] = (x[i] - mean) / sd; // sd NaN -> NaN; propagates correctly
  }
}

// CsScale: rescale so the sum of absolute values over the valid set equals `a`.
// A zero L1 norm (all-zero valid set) leaves the row at 0 (a/0 would be inf).
void cs_scale(std::span<const atx::f64> x, const std::vector<atx::usize> &valid, atx::f64 a,
              std::span<atx::f64> out) {
  atx::f64 l1 = 0.0;
  for (const atx::usize i : valid) {
    l1 += std::fabs(x[i]);
  }
  const atx::f64 k = (l1 == 0.0) ? 0.0 : a / l1;
  for (const atx::usize i : valid) {
    out[i] = x[i] * k;
  }
}

// CsDemeanG / CsNeutG: subtract the per-group mean within the valid set.
void cs_group_demean(std::span<const atx::f64> x, std::span<const atx::f64> g,
                     const std::vector<atx::usize> &valid, std::span<atx::f64> out) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue; // no group label -> stays NaN (out-of-set)
    }
    atx::f64 sum = 0.0;
    atx::usize cnt = 0;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        sum += x[j];
        ++cnt;
      }
    }
    out[i] = x[i] - sum / static_cast<atx::f64>(cnt);
  }
}

// CsResidualize (S3.1): per-date regression-residual neutralization. With `z`
// empty this IS the per-group demean (cs_group_demean) bit-for-bit (the boundary
// pin). With `z` present it is the Frisch-Waugh-Lovell partial-out: within-group
// demean x and z, then remove the OLS slope beta = Σ x~·z~ / Σ z~² of demeaned-x
// on demeaned-z; residual r = x~ - beta·z~. The regression set is the valid
// cells with a non-NaN group label AND a non-NaN covariate; others stay NaN. A
// zero covariate variance yields beta = 0 (collapses to the demean). Summation
// order ascending — bit-identical to vm.hpp's cs_residualize_row.
void cs_residualize(std::span<const atx::f64> x, std::span<const atx::f64> g,
                    std::span<const atx::f64> z, const std::vector<atx::usize> &valid,
                    std::span<atx::f64> out) {
  if (z.empty()) {
    cs_group_demean(x, g, valid, out);
    return;
  }
  std::vector<atx::usize> rset;
  rset.reserve(valid.size());
  for (const atx::usize i : valid) {
    if (!is_nan(g[i]) && !is_nan(z[i])) {
      rset.push_back(i);
    }
  }
  std::vector<atx::f64> xtil(rset.size());
  std::vector<atx::f64> ztil(rset.size());
  for (atx::usize p = 0; p < rset.size(); ++p) {
    const atx::usize i = rset[p];
    atx::f64 sx = 0.0;
    atx::f64 sz = 0.0;
    atx::usize cnt = 0;
    for (const atx::usize j : rset) {
      if (g[j] == g[i]) {
        sx += x[j];
        sz += z[j];
        ++cnt;
      }
    }
    const atx::f64 cntf = static_cast<atx::f64>(cnt);
    xtil[p] = x[i] - sx / cntf;
    ztil[p] = z[i] - sz / cntf;
  }
  atx::f64 sxz = 0.0;
  atx::f64 szz = 0.0;
  for (atx::usize p = 0; p < rset.size(); ++p) {
    sxz += xtil[p] * ztil[p];
    szz += ztil[p] * ztil[p];
  }
  const atx::f64 beta = (szz == 0.0) ? 0.0 : sxz / szz;
  for (atx::usize p = 0; p < rset.size(); ++p) {
    out[rset[p]] = xtil[p] - beta * ztil[p];
  }
}

// CsRankG / CsZscoreG: rank (ordinal percentile) or sample-zscore WITHIN each
// group of the valid set. `zscore` selects the variant.
void cs_group(std::span<const atx::f64> x, std::span<const atx::f64> g,
              const std::vector<atx::usize> &valid, std::span<atx::f64> out, bool zscore) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue;
    }
    std::vector<atx::usize> members;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        members.push_back(j);
      }
    }
    if (zscore) {
      const std::vector<atx::f64> v = gather(x, members);
      const atx::f64 mean = mean_of(v);
      const atx::f64 sd = sample_std(v, mean);
      out[i] = (x[i] - mean) / sd;
    } else {
      cs_rank(x, members, out); // writes ranks for the whole group (idempotent)
    }
  }
}

// CsNormalize (P3b-2): cross-sectional demean — x - mean over the valid set.
void cs_normalize(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                  std::span<atx::f64> out) {
  const std::vector<atx::f64> v = gather(x, valid);
  const atx::f64 mean = mean_of(v);
  for (const atx::usize i : valid) {
    out[i] = x[i] - mean;
  }
}

// CsWinsorize (P3b-2): clamp each valid cell to [mean - k·σ, mean + k·σ] over
// the valid set; σ = SAMPLE std (ddof=1). Fewer than 2 valid -> σ NaN -> the
// comparisons are false -> the value passes through unclamped.
void cs_winsorize(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                  atx::f64 k, std::span<atx::f64> out) {
  const std::vector<atx::f64> v = gather(x, valid);
  const atx::f64 mean = mean_of(v);
  const atx::f64 sd = sample_std(v, mean);
  const atx::f64 lo = mean - k * sd;
  const atx::f64 hi = mean + k * sd;
  for (const atx::usize i : valid) {
    const atx::f64 xv = x[i];
    out[i] = (xv < lo) ? lo : (xv > hi ? hi : xv);
  }
}

// CsCountG / CsMeanG (P3b-2): broadcast the within-group member count or mean to
// each valid member. A NaN group label -> stays NaN (out-of-set).
void cs_group_count_mean(std::span<const atx::f64> x, std::span<const atx::f64> g,
                         const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                         bool want_mean) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue;
    }
    atx::f64 sum = 0.0;
    atx::usize cnt = 0;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        sum += x[j];
        ++cnt;
      }
    }
    out[i] = want_mean ? sum / static_cast<atx::f64>(cnt) : static_cast<atx::f64>(cnt);
  }
}

// CsScaleG (P3b-2): scale within each group so Σ|x| over the group's valid
// members == 1 (zero-L1 group -> 0). A NaN group label -> stays NaN.
void cs_group_scale(std::span<const atx::f64> x, std::span<const atx::f64> g,
                    const std::vector<atx::usize> &valid, std::span<atx::f64> out) {
  for (const atx::usize i : valid) {
    if (is_nan(g[i])) {
      continue;
    }
    atx::f64 l1 = 0.0;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        l1 += std::fabs(x[j]);
      }
    }
    const atx::f64 kfac = (l1 == 0.0) ? 0.0 : 1.0 / l1;
    out[i] = x[i] * kfac;
  }
}

// CsQuantile (S3.3): discretize the valid set into `n` buckets. Ordinal-rank
// each valid cell (ascending value, tie-broken by ascending instrument index,
// as cs_rank), map percentile p to bucket b = floor(p·n) clamped to [0, n-1],
// emit b/(n-1). Singleton -> p == 0.5. n < 2 -> NaN. Summation/sort order is
// bit-identical to cs_ops.hpp's cs_quantile_row.
void cs_quantile(std::span<const atx::f64> x, const std::vector<atx::usize> &valid, atx::f64 n_real,
                 std::span<atx::f64> out) {
  const atx::usize m = valid.size();
  if (m == 0) {
    return;
  }
  const int nb = static_cast<int>(n_real);
  if (nb < 2) {
    for (const atx::usize i : valid) {
      out[i] = kNaN;
    }
    return;
  }
  std::vector<atx::usize> order = valid;
  std::stable_sort(order.begin(), order.end(),
                   [&](atx::usize i, atx::usize j) { return x[i] < x[j]; });
  const atx::f64 denom = static_cast<atx::f64>(nb - 1);
  for (atx::usize r = 0; r < m; ++r) {
    const atx::f64 p = (m == 1) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(m - 1);
    int b = static_cast<int>(p * static_cast<atx::f64>(nb));
    if (b >= nb) {
      b = nb - 1;
    }
    out[order[r]] = static_cast<atx::f64>(b) / denom;
  }
}

// CsVecSum / CsVecAvg (S3.3): reduce over the valid set (sum / mean) and
// broadcast the scalar back to every valid cell. Ascending-index summation —
// bit-identical to cs_ops.hpp's cs_vec_reduce_row.
void cs_vec_reduce(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                   std::span<atx::f64> out, bool want_avg) {
  if (valid.empty()) {
    return;
  }
  atx::f64 sum = 0.0;
  for (const atx::usize i : valid) {
    sum += x[i];
  }
  const atx::f64 v = want_avg ? sum / static_cast<atx::f64>(valid.size()) : sum;
  for (const atx::usize i : valid) {
    out[i] = v;
  }
}

atx::core::Status Oracle::eval_cross_section(const Instr &in) {
  const std::span<const atx::f64> x = src_col(in, 0);
  std::span<atx::f64> out = dst_col(in);
  // Group ops take the classifier in src[1]; CsScale/CsWinsorize read a scalar
  // (target L1 norm / std multiplier) from src[1]'s [0] cell.
  const bool grouped =
      (in.op == OpCode::CsDemeanG || in.op == OpCode::CsNeutG || in.op == OpCode::CsRankG ||
       in.op == OpCode::CsZscoreG || in.op == OpCode::CsCountG || in.op == OpCode::CsMeanG ||
       in.op == OpCode::CsScaleG || in.op == OpCode::CsResidualize);
  std::span<const atx::f64> g{};
  std::span<const atx::f64> z{}; // cs_residualize optional style covariate (src[2])
  atx::f64 scale_a = 1.0;
  if (grouped) {
    g = src_col(in, 1);
    if (in.op == OpCode::CsResidualize && in.src[2] != kNoSlot) {
      z = src_col(in, 2); // present only for arity-3 cs_residualize(x, g, z)
    }
  } else if (in.op == OpCode::CsScale || in.op == OpCode::CsWinsorize ||
             in.op == OpCode::CsQuantile) {
    scale_a = detail::scalar_of(src_col(in, 1));
  }
  for (atx::usize d = 0; d < dates_; ++d) {
    const std::span<const atx::f64> xr = x.subspan(d * instruments_, instruments_);
    const std::span<atx::f64> orow = out.subspan(d * instruments_, instruments_);
    const std::span<const atx::f64> grow =
        grouped ? g.subspan(d * instruments_, instruments_) : std::span<const atx::f64>{};
    const std::span<const atx::f64> zrow =
        z.empty() ? std::span<const atx::f64>{} : z.subspan(d * instruments_, instruments_);
    cs_one_date(in.op, xr, grow, zrow, scale_a, orow);
  }
  return atx::core::Ok();
}

// cs_one_date — apply one cross-sectional op to a single date's row. `x` is the
// row's values, `g` the group labels (empty for ungrouped ops), `scale_a` the
// CsScale factor. `out` starts as all-NaN (run() pre-fills the SignalSet, but
// scratch slots are reused, so we MUST write every cell — invalid cells -> NaN).
void Oracle::cs_one_date(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> g,
                         std::span<const atx::f64> z, atx::f64 scale_a,
                         std::span<atx::f64> out) const {
  // The valid set: in-universe at this date AND non-NaN. The Panel universe was
  // already folded into NaN by LoadField, so "non-NaN" captures both here.
  std::vector<atx::usize> valid;
  valid.reserve(x.size());
  for (atx::usize i = 0; i < x.size(); ++i) {
    out[i] = detail::kNaN; // default every cell (out-of-set stays NaN)
    if (!detail::is_nan(x[i])) {
      valid.push_back(i);
    }
  }
  switch (op) {
  case OpCode::CsRank:
    cs_rank(x, valid, out);
    return;
  case OpCode::CsZscore:
    cs_zscore(x, valid, out);
    return;
  case OpCode::CsScale:
    cs_scale(x, valid, scale_a, out);
    return;
  case OpCode::CsNormalize:
    cs_normalize(x, valid, out);
    return;
  case OpCode::CsWinsorize:
    cs_winsorize(x, valid, scale_a, out);
    return;
  case OpCode::CsDemeanG:
  case OpCode::CsNeutG: // SAFETY: residualize-on-group-dummies == per-group demean
    cs_group_demean(x, g, valid, out);
    return;
  case OpCode::CsResidualize: // demean (z empty) or FWL partial-out (z present)
    cs_residualize(x, g, z, valid, out);
    return;
  case OpCode::CsQuantile: // discretize the valid set into `scale_a` buckets
    cs_quantile(x, valid, scale_a, out);
    return;
  case OpCode::CsVecSum:
    cs_vec_reduce(x, valid, out, /*want_avg=*/false);
    return;
  case OpCode::CsVecAvg:
    cs_vec_reduce(x, valid, out, /*want_avg=*/true);
    return;
  case OpCode::CsRankG:
    cs_group(x, g, valid, out, /*zscore=*/false);
    return;
  case OpCode::CsZscoreG:
    cs_group(x, g, valid, out, /*zscore=*/true);
    return;
  case OpCode::CsCountG:
    cs_group_count_mean(x, g, valid, out, /*want_mean=*/false);
    return;
  case OpCode::CsMeanG:
    cs_group_count_mean(x, g, valid, out, /*want_mean=*/true);
    return;
  case OpCode::CsScaleG:
    cs_group_scale(x, g, valid, out);
    return;
  default:
    ATX_UNREACHABLE();
  }
}

// =========================================================================
//  Time-series kernels — per instrument column, causal trailing window.
// =========================================================================

atx::core::Status Oracle::eval_time_series(const Instr &in) {
  const std::span<const atx::f64> x = src_col(in, 0);
  std::span<atx::f64> out = dst_col(in);
  // Window from the LAST operand (delay/delta/unary-window: src[1]; corr/cov:
  // src[2]). Find the highest populated operand slot.
  atx::usize last = 0;
  for (atx::usize k = 0; k < in.src.size(); ++k) {
    if (in.src.at(k) != kNoSlot) {
      last = k;
    }
  }
  const atx::usize d = detail::window_of(src_col(in, last));
  // Binary-series ops (corr/cov + S3.2 ts_regression) read a second series.
  const bool binary_series =
      (in.op == OpCode::TsCorr || in.op == OpCode::TsCov || in.op == OpCode::TsRegression);
  const std::span<const atx::f64> y = binary_series ? src_col(in, 1) : std::span<const atx::f64>{};
  const atx::f64 p0 = in.imm[0]; // peeled hparam (decay f / moment k / entropy buckets)

  for (atx::usize j = 0; j < instruments_; ++j) {
    for (atx::usize t = 0; t < dates_; ++t) {
      out[t * instruments_ + j] =
          binary_series ? ts_binary_at(in.op, x, y, t, j, d) : ts_unary_at(in.op, x, t, j, d, p0);
    }
  }
  return atx::core::Ok();
}

// ---------------------------------------------------------------------------
//  Window gather + scalar statistics (shared by the Ts unary kernels).
// ---------------------------------------------------------------------------

// Collect the trailing window x[t-d+1 .. t] for instrument column `j` into
// `win` (chronological order). Returns false if the window is incomplete
// (t+1 < d) OR any cell is NaN -> the uniform "any-NaN/short window -> NaN"
// policy. Empty `win` on false.
bool gather_window(std::span<const atx::f64> x, atx::usize t, atx::usize j,
                   atx::usize d, atx::usize instruments, std::vector<atx::f64> &win) {
  win.clear();
  if (d == 0 || t + 1 < d) {
    return false;
  }
  for (atx::usize s = t + 1 - d; s <= t; ++s) {
    const atx::f64 v = x[s * instruments + j];
    if (is_nan(v)) {
      return false;
    }
    win.push_back(v);
  }
  return true;
}

// Pearson product-moment correlation of two equal-length windows (population
// cross-moments cancel n, so corr in [-1,1]); NaN if either has zero variance.
atx::f64 pearson(const std::vector<atx::f64> &a,
                 const std::vector<atx::f64> &b) noexcept {
  const atx::usize n = a.size();
  if (n < 2) {
    return kNaN;
  }
  const atx::f64 ma = sum_of(a) / static_cast<atx::f64>(n);
  const atx::f64 mb = sum_of(b) / static_cast<atx::f64>(n);
  atx::f64 sab = 0.0;
  atx::f64 saa = 0.0;
  atx::f64 sbb = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    sab += (a[i] - ma) * (b[i] - mb);
    saa += (a[i] - ma) * (a[i] - ma);
    sbb += (b[i] - mb) * (b[i] - mb);
  }
  const atx::f64 denom = std::sqrt(saa * sbb);
  return denom == 0.0 ? kNaN : sab / denom;
}

// Sample covariance (ddof=1) of two equal-length windows.
atx::f64 sample_cov(const std::vector<atx::f64> &a,
                    const std::vector<atx::f64> &b) noexcept {
  const atx::usize n = a.size();
  if (n < 2) {
    return kNaN;
  }
  const atx::f64 ma = sum_of(a) / static_cast<atx::f64>(n);
  const atx::f64 mb = sum_of(b) / static_cast<atx::f64>(n);
  atx::f64 s = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    s += (a[i] - ma) * (b[i] - mb);
  }
  return s / static_cast<atx::f64>(n - 1);
}

LinFit lin_fit(const std::vector<atx::f64> &y) noexcept {
  const atx::usize n = y.size();
  LinFit f;
  if (n < 2) {
    return f;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 xi = static_cast<atx::f64>(i);
    sx += xi;
    sy += y[i];
    sxx += xi * xi;
    sxy += xi * y[i];
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
  for (atx::usize i = 0; i < n; ++i) {
    const atx::f64 fit = f.intercept + f.slope * static_cast<atx::f64>(i);
    ss_tot += (y[i] - my) * (y[i] - my);
    ss_res += (y[i] - fit) * (y[i] - fit);
  }
  f.r2 = (ss_tot == 0.0) ? kNaN : 1.0 - ss_res / ss_tot;
  f.fitted_last = f.intercept + f.slope * static_cast<atx::f64>(n - 1);
  return f;
}

OuFit ou_fit(const std::vector<atx::f64> &w) noexcept {
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  atx::usize n = 0;
  for (atx::usize s = 1; s < w.size(); ++s) {
    sx += w[s - 1];
    sy += w[s];
    sxx += w[s - 1] * w[s - 1];
    sxy += w[s - 1] * w[s];
    ++n;
  }
  OuFit f;
  if (n < 2) {
    return f;
  }
  const atx::f64 dn = static_cast<atx::f64>(n);
  const atx::f64 denom = sxx - sx * sx / dn;
  if (denom == 0.0) {
    return f;
  }
  f.b = (sxy - sx * sy / dn) / denom;
  f.a = (sy - f.b * sx) / dn;
  atx::f64 ss = 0.0;
  for (atx::usize s = 1; s < w.size(); ++s) {
    const atx::f64 r = w[s] - (f.a + f.b * w[s - 1]);
    ss += r * r;
  }
  f.resid_std = std::sqrt(ss / dn);
  return f;
}

// OU derived-quantity per-cell value over a gathered window `w` (newest last).
// INDEPENDENT restatement of the ts_ops ou_*_of mappers:
//   theta    = -ln(b)                       valid when b in (0,1)
//   halflife = ln2 / theta                  valid when b in (0,1)
//   mean     = a / (1-b)                     valid when b < 1 (b not NaN)
//   zscore   = (w.back()-mean) / sigma_eq    sigma_eq = resid_std/sqrt(1-b^2)
atx::f64 ou_unary_at(OpCode op, const std::vector<atx::f64> &w) noexcept {
  const OuFit f = ou_fit(w);
  switch (op) {
  case OpCode::OuTheta:
    return (f.b > 0.0 && f.b < 1.0) ? -std::log(f.b) : kNaN;
  case OpCode::OuHalflife: {
    if (!(f.b > 0.0 && f.b < 1.0)) {
      return kNaN;
    }
    const atx::f64 th = -std::log(f.b);
    return std::log(2.0) / th;
  }
  case OpCode::OuMean:
    return (!is_nan(f.b) && f.b < 1.0) ? f.a / (1.0 - f.b) : kNaN;
  case OpCode::OuZscore: {
    if (!(f.b > 0.0 && f.b < 1.0)) {
      return kNaN;
    }
    const atx::f64 sig = f.resid_std / std::sqrt(1.0 - f.b * f.b);
    if (sig == 0.0 || is_nan(sig)) {
      return kNaN;
    }
    return (w.back() - f.a / (1.0 - f.b)) / sig;
  }
  default:
    ATX_UNREACHABLE();
  }
}

// ts_unary_at — single-cell value of a unary-series Ts op at (t, j). delay/delta
// short-circuit (they need only the shifted observation, not a full window).
atx::f64 Oracle::ts_unary_at(OpCode op, std::span<const atx::f64> x, atx::usize t, atx::usize j,
                             atx::usize d, atx::f64 p0) const {
  // delay/delta: x[t-d] with min_periods==1; NaN if the shift falls off the top.
  if (op == OpCode::TsDelay || op == OpCode::TsDelta) {
    if (d == 0 || t < d) {
      return detail::kNaN;
    }
    const atx::f64 shifted = x[(t - d) * instruments_ + j];
    return op == OpCode::TsDelay ? shifted : x[t * instruments_ + j] - shifted;
  }
  // ts_backfill (P3b-2): most recent valid value in [t-d+1, t], looking PAST
  // NaNs (its own policy, NOT the any-NaN -> NaN gate). Scan newest -> oldest;
  // the `t >= i` guard keeps the walk causal and underflow-safe at date 0.
  if (op == OpCode::TsBackfill) {
    for (atx::usize i = 0; i < d && t >= i; ++i) {
      const atx::f64 v = x[(t - i) * instruments_ + j];
      if (!detail::is_nan(v)) {
        return v;
      }
    }
    return detail::kNaN;
  }
  // ts_count_nans (P3b-2): full-window-only NaN count (returns a finite count,
  // never propagates NaN). An incomplete window (t+1 < d) -> NaN, like delay.
  if (op == OpCode::TsCountNans) {
    if (d == 0 || t + 1 < d) {
      return detail::kNaN;
    }
    atx::usize cnt = 0;
    for (atx::usize s = t + 1 - d; s <= t; ++s) {
      if (detail::is_nan(x[s * instruments_ + j])) {
        ++cnt;
      }
    }
    return static_cast<atx::f64>(cnt);
  }

  std::vector<atx::f64> w;
  if (!detail::gather_window(x, t, j, d, instruments_, w)) {
    return detail::kNaN; // short window or any-NaN -> NaN (pinned policy)
  }
  const atx::usize n = w.size();
  // The min_periods policy is the single authority for "enough observations".
  // gather_window already enforces a full, NaN-free window, so for the current
  // full-window policy this is a defensive no-op; it keeps the policy live so a
  // future partial-window op (or an OpSig min_periods field) routes through here.
  if (n < detail::min_periods(op, d)) {
    return detail::kNaN;
  }
  switch (op) {
  case OpCode::TsSum:
    return detail::sum_of(w);
  case OpCode::TsMean:
    return detail::sum_of(w) / static_cast<atx::f64>(n);
  case OpCode::TsVar:
    return detail::sample_var(w);
  case OpCode::TsStd:
    return std::sqrt(detail::sample_var(w));
  case OpCode::TsMin:
    return *std::min_element(w.begin(), w.end());
  case OpCode::TsMax:
    return *std::max_element(w.begin(), w.end());
  case OpCode::TsArgMin:
    return detail::arg_extreme(w, /*want_max=*/false);
  case OpCode::TsArgMax:
    return detail::arg_extreme(w, /*want_max=*/true);
  case OpCode::TsProduct: {
    atx::f64 p = 1.0;
    for (const atx::f64 v : w) {
      p *= v;
    }
    return p;
  }
  case OpCode::TsRank: {
    // Ordinal percentile rank of the LAST element within its window, [0,1].
    atx::usize less = 0;
    atx::usize equal = 0;
    for (const atx::f64 v : w) {
      if (v < w.back()) {
        ++less;
      } else if (v == w.back()) {
        ++equal;
      }
    }
    // Average-rank for ties, then normalize to [0,1] over n elements.
    const atx::f64 avg = static_cast<atx::f64>(less) + (static_cast<atx::f64>(equal) - 1.0) / 2.0;
    return n == 1 ? 0.5 : avg / static_cast<atx::f64>(n - 1);
  }
  case OpCode::TsMed: {
    std::vector<atx::f64> s = w;
    std::sort(s.begin(), s.end());
    return (n % 2 == 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0;
  }
  case OpCode::TsMad: {
    const atx::f64 m = detail::sum_of(w) / static_cast<atx::f64>(n);
    atx::f64 s = 0.0;
    for (const atx::f64 v : w) {
      s += std::fabs(v - m);
    }
    return s / static_cast<atx::f64>(n);
  }
  case OpCode::TsDecayLinear: {
    // Linear weights d, d-1, .. 1 (newest heaviest), normalized to sum 1.
    atx::f64 acc = 0.0;
    atx::f64 wsum = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 wt = static_cast<atx::f64>(i + 1); // oldest=1 .. newest=n
      acc += wt * w[i];
      wsum += wt;
    }
    return acc / wsum;
  }
  case OpCode::TsWma: {
    // Weighted MA, same linear weights as decay_linear (alias here).
    atx::f64 acc = 0.0;
    atx::f64 wsum = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 wt = static_cast<atx::f64>(i + 1);
      acc += wt * w[i];
      wsum += wt;
    }
    return acc / wsum;
  }
  case OpCode::TsEma: {
    // Exponential MA over the window, alpha = 2/(d+1), seeded with the oldest.
    const atx::f64 alpha = 2.0 / (static_cast<atx::f64>(n) + 1.0);
    atx::f64 ema = w.front();
    for (atx::usize i = 1; i < n; ++i) {
      ema = alpha * w[i] + (1.0 - alpha) * ema;
    }
    return ema;
  }
  case OpCode::TsSkew: {
    if (n < 3) {
      return detail::kNaN;
    }
    const atx::f64 m = detail::sum_of(w) / static_cast<atx::f64>(n);
    const atx::f64 sd = std::sqrt(detail::sample_var(w));
    if (sd == 0.0) {
      return detail::kNaN;
    }
    atx::f64 s = 0.0;
    for (const atx::f64 v : w) {
      s += (v - m) * (v - m) * (v - m);
    }
    return s / static_cast<atx::f64>(n) / (sd * sd * sd);
  }
  case OpCode::TsKurt: {
    if (n < 4) {
      return detail::kNaN;
    }
    const atx::f64 m = detail::sum_of(w) / static_cast<atx::f64>(n);
    const atx::f64 var = detail::sample_var(w);
    if (var == 0.0) {
      return detail::kNaN;
    }
    atx::f64 s = 0.0;
    for (const atx::f64 v : w) {
      s += (v - m) * (v - m) * (v - m) * (v - m);
    }
    return s / static_cast<atx::f64>(n) / (var * var) - 3.0; // excess kurtosis
  }
  case OpCode::TsSlope:
    return detail::lin_fit(w).slope;
  case OpCode::TsRsquare:
    return detail::lin_fit(w).r2;
  case OpCode::TsResid:
    return w.back() - detail::lin_fit(w).fitted_last;
  case OpCode::TsZscore: {
    // (x[t] - mean) / sample-std over the window; same reductions as mean/std.
    const atx::f64 mean = detail::sum_of(w) / static_cast<atx::f64>(n);
    const atx::f64 sd = std::sqrt(detail::sample_var(w));
    return (w.back() - mean) / sd; // sd NaN (n<2) -> NaN
  }
  case OpCode::TsAvDiff:
    return w.back() - detail::sum_of(w) / static_cast<atx::f64>(n);
  case OpCode::TsQuantile: {
    // Rolling median (quantile 0.5) — identical to TsMed (sort + even-n midpoint).
    std::vector<atx::f64> s = w;
    std::sort(s.begin(), s.end());
    return (n % 2 == 1) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2.0;
  }
  case OpCode::TsScale: {
    // Rolling min-max: (x[t] - min) / (max - min); flat window -> 0.
    const atx::f64 lo = *std::min_element(w.begin(), w.end());
    const atx::f64 hi = *std::max_element(w.begin(), w.end());
    const atx::f64 range = hi - lo;
    return range == 0.0 ? 0.0 : (w.back() - lo) / range;
  }
  case OpCode::TsDecayExp: {
    // Exponential decay: weight p0^(n-1-i), newest (i=n-1) heaviest (p0^0==1).
    atx::f64 acc = 0.0;
    atx::f64 wsum = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      const atx::f64 wt = std::pow(p0, static_cast<atx::f64>(n - 1 - i));
      acc += wt * w[i];
      wsum += wt;
    }
    return wsum == 0.0 ? detail::kNaN : acc / wsum;
  }
  case OpCode::TsMoment: {
    // k-th central moment (1/n) Σ (v - mean)^k; p0 is the integer order k.
    const int kord = static_cast<int>(p0);
    const atx::f64 m = detail::sum_of(w) / static_cast<atx::f64>(n);
    atx::f64 s = 0.0;
    for (const atx::f64 v : w) {
      const atx::f64 dv = v - m;
      atx::f64 p = 1.0;
      for (int e = 0; e < kord; ++e) {
        p *= dv;
      }
      s += p;
    }
    return s / static_cast<atx::f64>(n);
  }
  case OpCode::TsEntropy: {
    // Shannon entropy over `nb` equal-width buckets across [min, max]; flat -> 0.
    constexpr atx::usize kMaxBuckets = 256;
    atx::usize nb = static_cast<atx::usize>(p0);
    if (nb < 1) {
      nb = 1;
    }
    if (nb > kMaxBuckets) {
      nb = kMaxBuckets;
    }
    const atx::f64 lo = *std::min_element(w.begin(), w.end());
    const atx::f64 hi = *std::max_element(w.begin(), w.end());
    const atx::f64 range = hi - lo;
    if (range == 0.0) {
      return 0.0;
    }
    std::array<atx::usize, kMaxBuckets> cnt{};
    for (const atx::f64 v : w) {
      atx::usize b = static_cast<atx::usize>((v - lo) / range * static_cast<atx::f64>(nb));
      if (b >= nb) {
        b = nb - 1;
      }
      ++cnt[b];
    }
    atx::f64 entropy = 0.0;
    for (atx::usize b = 0; b < nb; ++b) {
      if (cnt[b] > 0) {
        const atx::f64 pb = static_cast<atx::f64>(cnt[b]) / static_cast<atx::f64>(n);
        entropy -= pb * std::log(pb);
      }
    }
    return entropy;
  }
  case OpCode::OuTheta:
  case OpCode::OuHalflife:
  case OpCode::OuMean:
  case OpCode::OuZscore:
    return ou_unary_at(op, w);
  default:
    ATX_UNREACHABLE();
  }
}

// ts_binary_at — corr/cov of the trailing windows of x and y at (t, j).
atx::f64 Oracle::ts_binary_at(OpCode op, std::span<const atx::f64> x,
                              std::span<const atx::f64> y, atx::usize t, atx::usize j,
                              atx::usize d) const {
  std::vector<atx::f64> wx;
  std::vector<atx::f64> wy;
  if (!detail::gather_window(x, t, j, d, instruments_, wx) ||
      !detail::gather_window(y, t, j, d, instruments_, wy)) {
    return detail::kNaN;
  }
  if (op == OpCode::TsRegression) {
    // Rolling OLS slope of the dependent (wx) on the predictor (wy):
    // beta = Σ(wy-my)(wx-mx) / Σ(wy-my)². Zero predictor variance -> NaN.
    const atx::usize n = wx.size();
    if (n < 2) {
      return detail::kNaN;
    }
    const atx::f64 mx = detail::sum_of(wx) / static_cast<atx::f64>(n);
    const atx::f64 my = detail::sum_of(wy) / static_cast<atx::f64>(n);
    atx::f64 sab = 0.0;
    atx::f64 sbb = 0.0;
    for (atx::usize i = 0; i < n; ++i) {
      sab += (wy[i] - my) * (wx[i] - mx);
      sbb += (wy[i] - my) * (wy[i] - my);
    }
    return sbb == 0.0 ? detail::kNaN : sab / sbb;
  }
  return op == OpCode::TsCorr ? detail::pearson(wx, wy) : detail::sample_cov(wx, wy);
}

// =========================================================================
//  Stateful causal recurrences — INDEPENDENT reference (P3b-3).
//
//  trade_when / hump carry TRUE cross-date state from the panel's first date
//  forward (no trailing window), so they do NOT route through eval_time_series.
//  This is the obviously-correct REFERENCE the fast VM (vm.hpp::eval_recurrence)
//  must reproduce BIT-FOR-BIT; the branch order and NaN policy below are PINNED
//  and re-stated independently of state_ops.hpp (the differential test proves
//  the two agree). Per instrument column j we walk dates t=0…D-1 forward,
//  carrying the prior output in a scalar `prior`.
//
//  SAFETY: causal by construction. The inner step reads only `prior`
//  (== out[t-1,j]) and the date-t operand cells at index t*I+j; there is no
//  index into a future date or future state. The first date (t==0) is special-
//  cased, seeding `prior` without any look-back. No std container grows in the
//  scan (the SignalSet/slot buffers are pre-sized), so this allocates nothing.
// =========================================================================
// KalmanLevel oracle reference — scalar local-level Kalman filter, per-instrument
// forward scan. Hyperparams Q (process noise) and R (observation noise) from
// in.imm[0/1]. The recurrence math is restated INLINE here (NOT via the shared
// state_ops kernels the VM uses), so the differential proves the two paths agree
// independently. Stack-local {x, P, seeded} per instrument — no shared buffer.
// SAFETY: causal by construction (reads only prior state + date-t input).
atx::core::Status Oracle::eval_kalman_level(const Instr &in, std::span<atx::f64> out) const {
  const std::span<const atx::f64> z = src_col(in, 0);
  const atx::f64 Q = in.imm[0];
  const atx::f64 R = in.imm[1];
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 x = 0.0;
    atx::f64 P = 0.0;
    bool seeded = false;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize idx = t * instruments_ + j;
      const atx::f64 zv = z[idx];
      if (!seeded) {
        if (detail::is_nan(zv)) {
          out[idx] = detail::kNaN; // unseeded NaN -> NaN, stay unseeded
          continue;
        }
        x = zv; // seed: x=z, P=R
        P = R;
        seeded = true;
        out[idx] = x;
        continue;
      }
      P += Q; // predict
      if (!detail::is_nan(zv)) {
        const atx::f64 K = P / (P + R);
        x += K * (zv - x);
        P = (1.0 - K) * P;
      }
      out[idx] = x;
    }
  }
  return atx::core::Ok();
}

// OuFilter oracle reference — OU AR(1) pull-to-mean smoother, per-instrument
// forward scan. Hyperparams theta (mean-reversion) and mu (long-run mean) from
// in.imm[0/1]. Restated INLINE (NOT the shared state_ops kernel). The pull is
// OBSERVATION-FREE after seeding: xhat = mu + phi*(xhat - mu), phi = exp(-theta);
// the new x[t] is intentionally ignored once seeded (spec §4.3). Stack-local
// {xhat, seeded} per instrument. SAFETY: causal (reads only prior xhat + date-t x).
atx::core::Status Oracle::eval_ou_filter(const Instr &in, std::span<atx::f64> out) const {
  const std::span<const atx::f64> x = src_col(in, 0);
  const atx::f64 theta = in.imm[0];
  const atx::f64 mu = in.imm[1];
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 xhat = 0.0;
    bool seeded = false;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize idx = t * instruments_ + j;
      const atx::f64 xv = x[idx];
      if (!seeded) {
        if (detail::is_nan(xv)) {
          out[idx] = detail::kNaN; // unseeded NaN -> NaN, stay unseeded
          continue;
        }
        xhat = xv; // seed
        seeded = true;
        out[idx] = xhat;
        continue;
      }
      const atx::f64 phi = std::exp(-theta);
      xhat = mu + phi * (xhat - mu); // observation-free pull toward mu
      out[idx] = xhat;
    }
  }
  return atx::core::Ok();
}

// KalmanReg oracle reference — Chan 2-state time-varying regression of y on x,
// per-instrument forward scan. Hyperparams delta and R from in.imm[0/1].
// Writes three contiguous output columns: alpha=pool_.column(dst+0),
// beta=pool_.column(dst+1), resid=pool_.column(dst+2). The 2x2 Chan recursion
// is restated INLINE here — does NOT call state_ops::kalman_reg_step — so the
// VM-vs-oracle differential is an independent cross-check.
// Diffuse prior: a=b=0, P00=P11=1, P01=0 (matches KalmanRegState defaults).
// Incomplete obs (y or x NaN): predict-only (P00+=w, P11+=w), output NaN.
// SAFETY: causal by construction (reads only prior state + date-t inputs).
atx::core::Status Oracle::eval_kalman_reg(const Instr &in) {
  const std::span<const atx::f64> yv = src_col(in, 0);
  const std::span<const atx::f64> xv = src_col(in, 1);
  const atx::f64 delta = in.imm[0];
  const atx::f64 R = in.imm[1];
  const std::span<atx::f64> oa = pool_.column(in.dst + 0);
  const std::span<atx::f64> ob = pool_.column(in.dst + 1);
  const std::span<atx::f64> orr = pool_.column(in.dst + 2);
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 a = 0.0;
    atx::f64 b = 0.0;
    atx::f64 P00 = 1.0;
    atx::f64 P01 = 0.0;
    atx::f64 P11 = 1.0;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize i = t * instruments_ + j;
      const atx::f64 y = yv[i];
      const atx::f64 x = xv[i];
      const atx::f64 w = delta / (1.0 - delta);
      if (detail::is_nan(y) || detail::is_nan(x)) {
        P00 += w;
        P11 += w;
        oa[i] = detail::kNaN;
        ob[i] = detail::kNaN;
        orr[i] = detail::kNaN;
        continue;
      }
      const atx::f64 P00p = P00 + w;
      const atx::f64 P01p = P01;
      const atx::f64 P11p = P11 + w;
      const atx::f64 e = y - (a + b * x);
      const atx::f64 pf0 = P00p + P01p * x;
      const atx::f64 pf1 = P01p + P11p * x;
      const atx::f64 Qv = pf0 + x * pf1 + R;
      const atx::f64 k0 = pf0 / Qv;
      const atx::f64 k1 = pf1 / Qv;
      a += k0 * e;
      b += k1 * e;
      P00 = P00p - k0 * pf0;
      P01 = P01p - k0 * pf1;
      P11 = P11p - k1 * pf1;
      oa[i] = a;
      ob[i] = b;
      orr[i] = e / std::sqrt(Qv);
    }
  }
  return atx::core::Ok();
}

atx::core::Status Oracle::eval_recurrence(const Instr &in) {
  std::span<atx::f64> out = dst_col(in);
  if (in.op == OpCode::KalmanLevel) {
    return eval_kalman_level(in, out);
  }
  if (in.op == OpCode::OuFilter) {
    return eval_ou_filter(in, out);
  }
  if (in.op == OpCode::Hump) {
    const std::span<const atx::f64> x = src_col(in, 0);
    // Scalar threshold from the 2nd operand's [0] cell (read like CsScale's `a`);
    // absent optional -> the 0.01 default (P3b-1 default-fill usually supplies it).
    const atx::f64 thr =
        in.src.at(1) == kNoSlot ? atx::f64{0.01} : detail::scalar_of(src_col(in, 1));
    for (atx::usize j = 0; j < instruments_; ++j) {
      atx::f64 prior = detail::kNaN;
      for (atx::usize t = 0; t < dates_; ++t) {
        const atx::usize i = t * instruments_ + j;
        // first date -> x[0]; else pass x[t] iff |x[t]-prior| STRICTLY > thr,
        // holding the prior otherwise (a NaN diff is never > thr -> holds).
        const atx::f64 v = (t == 0) ? x[i] : (std::fabs(x[i] - prior) > thr ? x[i] : prior);
        out[i] = v;
        prior = v;
      }
    }
    return atx::core::Ok();
  }
  // TradeWhen: trigger=src[0], alpha=src[1], exit=src[2]. Exit checked FIRST.
  const std::span<const atx::f64> trig = src_col(in, 0);
  const std::span<const atx::f64> alpha = src_col(in, 1);
  const std::span<const atx::f64> exit_v = src_col(in, 2);
  for (atx::usize j = 0; j < instruments_; ++j) {
    atx::f64 prior = detail::kNaN;
    for (atx::usize t = 0; t < dates_; ++t) {
      const atx::usize i = t * instruments_ + j;
      atx::f64 v;
      if (detail::mask_true(exit_v[i])) {
        v = detail::kNaN; // close / no position
      } else if (detail::mask_true(trig[i])) {
        v = alpha[i]; // (re)enter with the new signal
      } else {
        v = (t == 0) ? detail::kNaN : prior; // hold (flat on the first date)
      }
      out[i] = v;
      prior = v;
    }
  }
  return atx::core::Ok();
}

} // namespace detail

// =========================================================================
//  evaluate_reference — the public entry point.
// =========================================================================

// Execute `prog` over `panel`, returning one alpha per Program root. The Program
// MUST have been compiled (its field ids index `panel`'s field dictionary by
// the SAME names via `prog.fields`). Err(InvalidArgument) if a referenced field
// is absent from the Panel; Err(Internal) never (the linearizer's invariants
// hold). Borrows both inputs by const ref; allocates a fresh SlotPool per call.
atx::core::Result<SignalSet> evaluate_reference(const Program &prog, const Panel &panel) {
  // Validate every LoadField references a field present in the Panel, mapping
  // the Program's field-dictionary id to the Panel's field id IN PLACE is not
  // possible (Program is const); instead we pre-resolve and rewrite a local copy
  // of LoadField params. Simpler: verify names match and ids align.
  // The linearizer stores field NAMES in prog.fields, indexed by the param id.
  // We require the Panel to expose those same fields; resolve each used field.
  for (const Instr &in : prog.code) {
    if (in.op == OpCode::LoadField) {
      if (in.param >= prog.fields.size()) {
        return atx::core::Err(atx::core::ErrorCode::Internal,
                              "evaluate_reference: LoadField param out of field-dictionary range");
      }
      ATX_TRY(const FieldId pid, panel.field_id(prog.fields[in.param]));
      ATX_UNUSED(pid);
    }
  }

  // Build a Program copy whose LoadField params index the PANEL's fields (the
  // dictionaries may differ in order). This keeps eval_load_field a direct
  // field_all() lookup with no per-cell name resolution.
  Program local = prog;
  for (Instr &in : local.code) {
    if (in.op == OpCode::LoadField) {
      ATX_TRY(const FieldId pid, panel.field_id(prog.fields[in.param]));
      in.param = pid;
    }
  }

  detail::Oracle oracle{local, panel};
  return oracle.run();
}

} // namespace atx::engine::alpha

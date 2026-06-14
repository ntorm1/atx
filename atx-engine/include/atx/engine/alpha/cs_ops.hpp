#pragma once

// atx::engine::alpha — cross-sectional VM kernels (P3-7).
//
// The per-date-row cross-sectional opcodes for the fast vectorized VM
// (vm.hpp): rank / zscore / scale / normalize / winsorize / indneutralize
// (== group demean) / group_neutralize (== demean) / group_rank / group_zscore
// / group_count / group_mean / group_scale (the last six P3b-2). These free
// functions are the PRODUCTION-path counterparts of oracle.hpp's
// `detail::cs_*`; vm.hpp's `Engine::eval_cross_section` slices each slot
// buffer to a single date row and calls into them. They MUST reproduce the
// oracle BIT-FOR-BIT — the P3-7 differential test runs both paths and asserts
// every cell agrees (NaN==NaN).
//
// ===========================================================================
//  PINNED SEMANTIC CONTRACT — re-implemented here, self-contained
// ===========================================================================
//  vm.hpp's cross-sectional path does NOT borrow oracle.hpp; this header
//  re-states the SAME numeric policy independently (the differential TEST
//  proves they agree). All kernels operate on ONE date-row's VALID SET:
//    * VALID SET = the cells of the row that are non-NaN (LoadField already
//      folded the point-in-time universe into NaN upstream, so "non-NaN"
//      captures universe + missing). Out-of-set cells -> NaN in `out`; EVERY
//      output cell is written (scratch slots are recycled).
//    * cs_rank_row: ORDINAL percentile in [0,1], sorted ascending by value
//      with ties broken by ascending instrument index (a stable sort by value
//      preserves the index order). Rank r (0-based) of n maps to r/(n-1); a
//      singleton valid set -> 0.5. NOT average-rank (that is L6's policy).
//    * cs_zscore_row: (x - mean) / SAMPLE std (ddof=1) over the valid set;
//      fewer than 2 valid -> NaN (the NaN sd propagates).
//    * cs_scale_row(x, a): rescale so the L1 norm Σ|x| over the valid set
//      equals `a`; a zero L1 norm leaves the row at 0 (avoids a/0 == inf).
//    * cs_group_demean_row(x, g): subtract the per-group mean within the valid
//      set (the WLS residual on a pure group-dummy design IS the demean), so
//      both indneutralize (CsDemeanG) and group_neutralize (CsNeutG) route
//      here — the full residualizer is deferred, matching the oracle.
//    * cs_group_row(x, g, zscore): rank (ordinal, as cs_rank_row) or
//      sample-zscore WITHIN each group of the valid set; a cell with a NaN
//      group label stays NaN (out-of-set).
//
//  Distinct names from oracle.hpp's `detail::cs_*` / `gather` / `mean_of` /
//  `sample_std`: the differential test TU includes BOTH headers in this shared
//  `detail` namespace, so a name clash would be an ODR violation (exactly the
//  trap P3-6 sidestepped with its `vm_*` map kernels).
//
// Header-only; every free function is `inline`. Operates on caller-sliced
// per-date spans (length == instruments). Cross-sectional ops carry reductions
// over the valid set, so these are scalar loops (not SIMD lanes) — but the
// per-date scratch (the valid index list, the gathered value vector) is built
// once per row, and the kernels themselves are short and branch-light.

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::alpha::detail {

inline constexpr atx::f64 kCsNaN = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] inline bool cs_is_nan(atx::f64 x) noexcept { return std::isnan(x); }

// Mean of a dense value vector; NaN on empty.
[[nodiscard]] inline atx::f64 cs_mean(std::span<const atx::f64> xs) noexcept {
  if (xs.empty()) {
    return kCsNaN;
  }
  atx::f64 sum = 0.0;
  for (const atx::f64 v : xs) {
    sum += v;
  }
  return sum / static_cast<atx::f64>(xs.size());
}

// Sample (ddof=1) standard deviation; NaN if fewer than 2 observations. Matches
// oracle.hpp's `sample_std` summation order exactly (Σ(v-mean)² then /(n-1)).
[[nodiscard]] inline atx::f64 cs_sample_std(std::span<const atx::f64> xs, atx::f64 mean) noexcept {
  if (xs.size() < 2) {
    return kCsNaN;
  }
  atx::f64 ss = 0.0;
  for (const atx::f64 v : xs) {
    const atx::f64 d = v - mean;
    ss += d * d;
  }
  return std::sqrt(ss / static_cast<atx::f64>(xs.size() - 1));
}

// Gather the valid-set values of `x` into a dense vector (drops invalid cells),
// in ascending instrument-index order (the `valid` list is index-ascending).
[[nodiscard]] inline std::vector<atx::f64> cs_gather(std::span<const atx::f64> x,
                                                     const std::vector<atx::usize> &valid) {
  std::vector<atx::f64> v;
  v.reserve(valid.size());
  for (const atx::usize i : valid) {
    v.push_back(x[i]);
  }
  return v;
}

// ===========================================================================
//  CsRank — ordinal percentile in [0,1] over `valid`, tie-broken by ascending
//  instrument index. Rank r (0-based) of n maps to r/(n-1); a singleton set
//  maps to 0.5 (centred — avoids a degenerate 0/0). NaNs already excluded.
// ===========================================================================
inline void cs_rank_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                        std::span<atx::f64> out) {
  const atx::usize n = valid.size();
  if (n == 0) {
    return;
  }
  std::vector<atx::usize> order = valid; // already ascending in instrument index
  // Stable sort by value; ties keep ascending-index order -> deterministic
  // ordinal tie-break (identical to oracle.hpp's cs_rank).
  std::stable_sort(order.begin(), order.end(),
                   [&](atx::usize i, atx::usize j) { return x[i] < x[j]; });
  for (atx::usize r = 0; r < n; ++r) {
    const atx::f64 pct = (n == 1) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(n - 1);
    out[order[r]] = pct;
  }
}

// ===========================================================================
//  CsZscore — (x - mean) / sample-std over the valid set; out-of-set -> NaN.
//  Fewer than 2 valid observations -> NaN sd -> all NaN.
// ===========================================================================
inline void cs_zscore_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                          std::span<atx::f64> out) {
  const std::vector<atx::f64> v = cs_gather(x, valid);
  const atx::f64 mean = cs_mean(v);
  const atx::f64 sd = cs_sample_std(v, mean);
  for (const atx::usize i : valid) {
    out[i] = (x[i] - mean) / sd; // sd NaN -> NaN; propagates correctly
  }
}

// ===========================================================================
//  CsScale — rescale so Σ|x| over the valid set equals `a`. A zero L1 norm
//  (all-zero valid set) leaves the row at 0 (a/0 would be inf).
// ===========================================================================
inline void cs_scale_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                         atx::f64 a, std::span<atx::f64> out) {
  atx::f64 l1 = 0.0;
  for (const atx::usize i : valid) {
    l1 += std::fabs(x[i]);
  }
  const atx::f64 k = (l1 == 0.0) ? 0.0 : a / l1;
  for (const atx::usize i : valid) {
    out[i] = x[i] * k;
  }
}

// ===========================================================================
//  CsDemeanG / CsNeutG — subtract the per-group mean within the valid set. A
//  cell with a NaN group label has no group -> stays NaN (out-of-set).
// ===========================================================================
inline void cs_group_demean_row(std::span<const atx::f64> x, std::span<const atx::f64> g,
                                const std::vector<atx::usize> &valid, std::span<atx::f64> out) {
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
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

// ===========================================================================
//  CsResidualize (S3.1) — per-date regression-residual neutralization. Regress
//  the cross-section on a group-dummy design plus an optional continuous style
//  covariate `z`; emit the residual over the valid set.
//
//  With `z` EMPTY this dispatches to cs_group_demean_row, so it IS the per-group
//  demean BIT-FOR-BIT (the boundary pin == indneutralize). With `z` present it
//  is the Frisch–Waugh–Lovell partial-out: within-group-demean x and z, then
//  remove the global OLS slope beta = Σ x~·z~ / Σ z~² of demeaned-x on
//  demeaned-z; residual r = x~ - beta·z~. The regression valid set is the valid
//  cells with a non-NaN group label AND a non-NaN covariate; all others stay NaN
//  (out-of-set). A zero within-group covariate variance (Σ z~² == 0) yields
//  beta = 0, collapsing the residual back to the demean. Summation order is
//  ascending instrument index (matches the oracle twin bit-for-bit).
// ===========================================================================
inline void cs_residualize_row(std::span<const atx::f64> x, std::span<const atx::f64> g,
                               std::span<const atx::f64> z, const std::vector<atx::usize> &valid,
                               std::span<atx::f64> out) {
  if (z.empty()) {
    cs_group_demean_row(x, g, valid, out); // boundary pin: bit-for-bit demean
    return;
  }
  std::vector<atx::usize> rset; // valid cells with a group label AND a covariate
  rset.reserve(valid.size());
  for (const atx::usize i : valid) {
    if (!cs_is_nan(g[i]) && !cs_is_nan(z[i])) {
      rset.push_back(i);
    }
  }
  std::vector<atx::f64> xtil(rset.size()); // within-group demeaned x, parallel to rset
  std::vector<atx::f64> ztil(rset.size()); // within-group demeaned z, parallel to rset
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

// ===========================================================================
//  CsQuantile (S3.3) — bucket the valid set into `n` quantiles (like CsRank but
//  discretized). Ordinal-rank each valid cell (ascending value, tie-broken by
//  ascending instrument index, exactly as cs_rank_row), map its percentile
//  p in [0,1] to a bucket b = floor(p·n) clamped to [0, n-1], and emit
//  b/(n-1) so the output spans [0,1] in n discrete levels. A singleton valid set
//  ranks to p == 0.5 (centred), matching cs_rank_row. `n_real` is the scalar 2nd
//  operand truncated toward zero; a degenerate n < 2 has no defined spacing
//  (b/(n-1) would divide by zero) and yields NaN for every valid cell.
// ===========================================================================
inline void cs_quantile_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                            atx::f64 n_real, std::span<atx::f64> out) {
  const atx::usize m = valid.size();
  if (m == 0) {
    return;
  }
  const int nb = static_cast<int>(n_real); // bucket count (truncate toward zero)
  if (nb < 2) {
    for (const atx::usize i : valid) {
      out[i] = kCsNaN; // no defined quantile spacing
    }
    return;
  }
  std::vector<atx::usize> order = valid; // already ascending in instrument index
  std::stable_sort(order.begin(), order.end(),
                   [&](atx::usize i, atx::usize j) { return x[i] < x[j]; });
  const atx::f64 denom = static_cast<atx::f64>(nb - 1);
  for (atx::usize r = 0; r < m; ++r) {
    const atx::f64 p = (m == 1) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(m - 1);
    int b = static_cast<int>(p * static_cast<atx::f64>(nb)); // p>=0 -> trunc == floor
    if (b >= nb) {
      b = nb - 1; // p == 1.0 -> floor lands on nb; clamp into [0, nb-1]
    }
    out[order[r]] = static_cast<atx::f64>(b) / denom;
  }
}

// ===========================================================================
//  CsVecSum / CsVecAvg (S3.3) — reduce over the valid set (sum or mean) and
//  broadcast the single scalar back to EVERY valid cell. `want_avg` selects the
//  variant. Out-of-set cells stay NaN. Summation is ascending instrument index
//  (matches the oracle twin bit-for-bit). An empty valid set writes nothing
//  (every cell stays NaN).
// ===========================================================================
inline void cs_vec_reduce_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
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

// ===========================================================================
//  CsRankG / CsZscoreG — rank (ordinal percentile) or sample-zscore WITHIN each
//  group of the valid set. `zscore` selects the variant. A cell with a NaN
//  group label stays NaN. Mirrors oracle.hpp's `cs_group` exactly: per valid
//  cell `i`, collect its group's members and apply the within-group op.
// ===========================================================================
inline void cs_group_row(std::span<const atx::f64> x, std::span<const atx::f64> g,
                         const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                         bool zscore) {
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    std::vector<atx::usize> members;
    for (const atx::usize j : valid) {
      if (g[j] == g[i]) {
        members.push_back(j);
      }
    }
    if (zscore) {
      const std::vector<atx::f64> v = cs_gather(x, members);
      const atx::f64 mean = cs_mean(v);
      const atx::f64 sd = cs_sample_std(v, mean);
      out[i] = (x[i] - mean) / sd;
    } else {
      cs_rank_row(x, members, out); // writes ranks for the whole group (idempotent)
    }
  }
}

// ===========================================================================
//  CsNormalize (P3b-2) — cross-sectional demean: x - mean over the valid set.
//  Out-of-set cells stay NaN. With an empty valid set the mean is NaN, but the
//  loop body never executes, so every cell stays NaN (matches the oracle).
// ===========================================================================
inline void cs_normalize_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                             std::span<atx::f64> out) {
  const std::vector<atx::f64> v = cs_gather(x, valid);
  const atx::f64 mean = cs_mean(v);
  for (const atx::usize i : valid) {
    out[i] = x[i] - mean;
  }
}

// ===========================================================================
//  CsWinsorize (P3b-2) — clamp each valid cell to [mean - k·σ, mean + k·σ]
//  where mean/σ are over the valid set, σ = SAMPLE std (ddof=1, the pinned
//  zscore policy) and `k` is the scalar 2nd operand. With fewer than 2 valid
//  observations σ is NaN, so every comparison is false and the value passes
//  through unclamped (a sub-2 valid set has no dispersion to clip against).
// ===========================================================================
inline void cs_winsorize_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                             atx::f64 k, std::span<atx::f64> out) {
  const std::vector<atx::f64> v = cs_gather(x, valid);
  const atx::f64 mean = cs_mean(v);
  const atx::f64 sd = cs_sample_std(v, mean);
  const atx::f64 lo = mean - k * sd;
  const atx::f64 hi = mean + k * sd;
  for (const atx::usize i : valid) {
    const atx::f64 xv = x[i];
    out[i] = (xv < lo) ? lo : (xv > hi ? hi : xv); // NaN bounds -> pass-through
  }
}

// ===========================================================================
//  CsCountG / CsMeanG (P3b-2) — broadcast a within-group aggregate (member
//  count or mean) to every valid member of the group. `want_mean` selects the
//  variant. A cell with a NaN group label has no group -> stays NaN.
// ===========================================================================
inline void cs_group_count_mean_row(std::span<const atx::f64> x, std::span<const atx::f64> g,
                                    const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                                    bool want_mean) {
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
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
    out[i] = want_mean ? sum / static_cast<atx::f64>(cnt) : static_cast<atx::f64>(cnt);
  }
}

// ===========================================================================
//  CsScaleG (P3b-2) — scale WITHIN each group so Σ|x| over the group's valid
//  members equals 1 (a zero-L1 group leaves its members at 0, like
//  cs_scale_row). A cell with a NaN group label stays NaN.
// ===========================================================================
inline void cs_group_scale_row(std::span<const atx::f64> x, std::span<const atx::f64> g,
                               const std::vector<atx::usize> &valid, std::span<atx::f64> out) {
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
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

} // namespace atx::engine::alpha::detail

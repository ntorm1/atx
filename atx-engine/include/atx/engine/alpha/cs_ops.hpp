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
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::alpha::detail {

// ===========================================================================
//  PERFORMANCE — fast O(n) grouped cross-section (default) vs. O(n²) reference
// ===========================================================================
//  The grouped Cs* kernels (demean / count / mean / scale / group-rank /
//  group-zscore / residualize) were originally O(n²) per date: for every valid
//  cell they rescanned the whole valid set to find its group's members (rank
//  even re-sorted each group once PER member -> O(n²·log n)). At ~20k names ×
//  ~1600 dates that quadratic term dominates alpha evaluation.
//
//  The DEFAULT path here replaces those rescans with a single ascending pass
//  that bins each cell into its group via a reusable flat hash (`CsScratch`),
//  accumulating per-group reductions in lanes — O(n) (O(n·log n) for the
//  rank/group-rank sorts), with ZERO per-date heap allocation (the scratch is
//  Engine-owned and grows monotonically). Because every per-group reduction is
//  accumulated by scanning the valid set in the SAME ascending instrument-index
//  order as the reference loops, the fast path is BIT-FOR-BIT identical to the
//  reference — the oracle differential (alpha_cs_test) proves it on every Cs*
//  opcode.
//
//  Defining ATX_ALPHA_CS_REFERENCE (a whole-build switch; see the top-level
//  CMake option) compiles the literal O(n²) reference loops instead. It is an
//  audit / bisection hatch — the default fast path is already byte-identical —
//  and MUST be set build-wide (these are inline functions: a per-TU definition
//  would be an ODR violation).

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
//  CsScratch — reusable per-date working memory for the cross-sectional
//  kernels, owned by the Engine and threaded in by eval_cross_section. It makes
//  the grouped kernels O(n) (vs. O(n²)) and removes the per-date heap traffic
//  the old gather / order / member vectors caused.
//
//  Group identity: a flat open-addressing table maps each group LABEL to a
//  dense group id. The key is the label's IEEE bit pattern AFTER `+ 0.0`, which
//  folds -0.0 into +0.0 so the hash partition matches the kernels' pinned
//  `g[j] == g[i]` equality EXACTLY (±0.0 are the only distinct-bit doubles that
//  compare equal; NaN labels are excluded upstream). Group ids are assigned in
//  first-encounter (ascending instrument-index) order, and every reduction lane
//  is accumulated by scanning the valid set in that same ascending order — so
//  each kernel reproduces the reference's summation order bit-for-bit.
//
//  Capacity grows monotonically across dates AND evaluate() calls; a date reset
//  clears only the O(#groups) occupied table slots (via `used`), never the whole
//  table. The Engine is single-owner per worker, so the scratch is touched by
//  exactly one thread at a time.
// ===========================================================================
struct CsScratch {
  static constexpr atx::i64 kEmpty = -1;

  std::vector<atx::usize> order; // CsRank / CsQuantile sort permutation (or rset)
  std::vector<atx::i64> table;   // group hash: slot -> group id (kEmpty if free)
  std::vector<atx::usize> used;  // occupied slot indices (O(#groups) date reset)
  std::vector<atx::u64> gkey;    // group id -> normalized label bits
  std::vector<atx::f64> gsum;    // group id -> Σ x (or Σ x within rset)
  std::vector<atx::f64> gaux;    // group id -> Σ|x| | Σ(x-mean)² | Σ z
  std::vector<atx::usize> gcnt;  // group id -> member count
  std::vector<atx::usize> goff;  // CsRankG CSR offsets (size ngroups+1)
  std::vector<atx::usize> gcur;  // CsRankG scatter cursors (size ngroups)
  std::vector<atx::usize> gmem;  // CsRankG flat members, ascending within group
  std::vector<atx::f64> fa;      // residualize within-group-demeaned x (∥ rset)
  std::vector<atx::f64> fb;      // residualize within-group-demeaned z (∥ rset)
  atx::usize ngroups = 0;
  atx::usize tmask = 0; // table.size()-1 (power of two); 0 when unsized

  // splitmix64 finalizer — a cheap, well-mixed integer hash.
  [[nodiscard]] static atx::u64 mix(atx::u64 z) noexcept {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  // Normalize a group label to its hash key: `+ 0.0` maps -0.0 -> +0.0 so the
  // bit key partitions identically to `==` (NaN labels excluded upstream).
  [[nodiscard]] static atx::u64 label_key(atx::f64 g) noexcept {
    const atx::f64 v = g + 0.0;
    atx::u64 bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
  }

  // Reset for a new date holding up to `cap` distinct groups. On growth the
  // table is sized to the next power of two >= 2*cap+1 (load factor < 0.5);
  // otherwise only the previously-occupied slots are cleared.
  void begin_date(atx::usize cap) {
    atx::usize want = 8;
    while (want < cap * 2 + 1) {
      want <<= 1;
    }
    if (table.size() < want) {
      table.assign(want, kEmpty);
      tmask = want - 1;
      used.clear();
    } else {
      for (const atx::usize s : used) {
        table[s] = kEmpty;
      }
      used.clear();
    }
    ngroups = 0;
  }

  // Look up (or insert) the dense group id for a normalized label key. New ids
  // are assigned 0,1,2,… in first-encounter order with their lanes zeroed.
  [[nodiscard]] atx::usize group_id(atx::u64 key) {
    atx::usize idx = static_cast<atx::usize>(mix(key)) & tmask;
    while (table[idx] != kEmpty) {
      const atx::usize gid = static_cast<atx::usize>(table[idx]);
      if (gkey[gid] == key) {
        return gid;
      }
      idx = (idx + 1) & tmask;
    }
    const atx::usize gid = ngroups++;
    if (gid >= gkey.size()) {
      gkey.push_back(key);
      gsum.push_back(0.0);
      gaux.push_back(0.0);
      gcnt.push_back(0);
    } else {
      gkey[gid] = key;
      gsum[gid] = 0.0;
      gaux[gid] = 0.0;
      gcnt[gid] = 0;
    }
    table[idx] = static_cast<atx::i64>(gid);
    used.push_back(idx);
    return gid;
  }
};

// ===========================================================================
//  CsRank — ordinal percentile in [0,1] over `valid`, tie-broken by ascending
//  instrument index. Rank r (0-based) of n maps to r/(n-1); a singleton set
//  maps to 0.5 (centred — avoids a degenerate 0/0). NaNs already excluded.
//
//  INVARIANT (REQUIRED for AuditExact): `valid` MUST be in ascending instrument-
//  index order. The stable sort below preserves the pre-sort order of equal
//  values, so `valid`'s order IS the tie-break criterion — a non-ascending
//  `valid` would silently reorder tied ranks. Callers (cs_one_date) build it via
//  a forward scan; see the invariant block there.
// ===========================================================================
inline void cs_rank_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                        std::span<atx::f64> out, CsScratch &scratch) {
  const atx::usize n = valid.size();
  if (n == 0) {
    return;
  }
  std::vector<atx::usize> &order = scratch.order; // reused buffer (no per-date alloc)
  order.assign(valid.begin(), valid.end());       // already ascending in instrument index
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
//
//  INVARIANT (REQUIRED for AuditExact): `valid` MUST be in ascending instrument-
//  index order. The per-cell zscore is order-agnostic, BUT the Σx and Σ(x-mean)²
//  reductions accumulate in `valid` order and f64 addition is not associative —
//  a permuted `valid` changes the summed bits (hence mean/sd, hence every cell)
//  for ill-conditioned rows (large + small magnitudes). Ascending instrument
//  index is the canonical reduction order, matching the oracle's gather+sum.
// ===========================================================================
inline void cs_zscore_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                          std::span<atx::f64> out) {
  const atx::usize n = valid.size();
  if (n == 0) {
    return; // empty valid set -> every cell stays NaN (matches the reference)
  }
  // Streaming mean + sample (ddof=1) std over the valid set — same ascending
  // summation order as cs_gather + cs_mean + cs_sample_std, so bit-identical,
  // but with no per-date scratch vector.
  atx::f64 sum = 0.0;
  for (const atx::usize i : valid) {
    sum += x[i];
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 sd = kCsNaN; // fewer than 2 obs -> NaN (matches cs_sample_std)
  if (n >= 2) {
    atx::f64 ss = 0.0;
    for (const atx::usize i : valid) {
      const atx::f64 d = x[i] - mean;
      ss += d * d;
    }
    sd = std::sqrt(ss / static_cast<atx::f64>(n - 1));
  }
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
                                const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                                CsScratch &scratch) {
#ifdef ATX_ALPHA_CS_REFERENCE
  (void)scratch;
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
#else
  // Pass 1: bin each valid cell into its group, accumulating Σx / count in
  // ascending index order (== the reference's inner-loop order -> identical
  // sums). Pass 2: emit x - group-mean. NaN-group cells are skipped -> NaN.
  scratch.begin_date(valid.size());
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    scratch.gsum[gid] += x[i];
    ++scratch.gcnt[gid];
  }
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    out[i] = x[i] - scratch.gsum[gid] / static_cast<atx::f64>(scratch.gcnt[gid]);
  }
#endif
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
                               std::span<atx::f64> out, CsScratch &scratch) {
  if (z.empty()) {
    cs_group_demean_row(x, g, valid, out, scratch); // boundary pin: bit-for-bit demean
    return;
  }
#ifdef ATX_ALPHA_CS_REFERENCE
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
#else
  // rset (valid cells with a group label AND a covariate), reusing scratch.order.
  std::vector<atx::usize> &rset = scratch.order;
  rset.clear();
  for (const atx::usize i : valid) {
    if (!cs_is_nan(g[i]) && !cs_is_nan(z[i])) {
      rset.push_back(i);
    }
  }
  const atx::usize m = rset.size();
  // Pass 1: per-group Σx / Σz / count over rset in ascending order (== the
  // reference inner-loop order -> identical group sums).
  scratch.begin_date(m);
  for (atx::usize p = 0; p < m; ++p) {
    const atx::usize i = rset[p];
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    scratch.gsum[gid] += x[i];
    scratch.gaux[gid] += z[i];
    ++scratch.gcnt[gid];
  }
  // Pass 2: within-group demean x and z (parallel to rset).
  scratch.fa.resize(m);
  scratch.fb.resize(m);
  for (atx::usize p = 0; p < m; ++p) {
    const atx::usize i = rset[p];
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    const atx::f64 cntf = static_cast<atx::f64>(scratch.gcnt[gid]);
    scratch.fa[p] = x[i] - scratch.gsum[gid] / cntf;
    scratch.fb[p] = z[i] - scratch.gaux[gid] / cntf;
  }
  // OLS slope of demeaned-x on demeaned-z, then the residual (ascending order).
  atx::f64 sxz = 0.0;
  atx::f64 szz = 0.0;
  for (atx::usize p = 0; p < m; ++p) {
    sxz += scratch.fa[p] * scratch.fb[p];
    szz += scratch.fb[p] * scratch.fb[p];
  }
  const atx::f64 beta = (szz == 0.0) ? 0.0 : sxz / szz;
  for (atx::usize p = 0; p < m; ++p) {
    out[rset[p]] = scratch.fa[p] - beta * scratch.fb[p];
  }
#endif
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
                            atx::f64 n_real, std::span<atx::f64> out, CsScratch &scratch) {
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
  std::vector<atx::usize> &order = scratch.order; // reused buffer (no per-date alloc)
  order.assign(valid.begin(), valid.end());       // already ascending in instrument index
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
                         const std::vector<atx::usize> &valid, std::span<atx::f64> out, bool zscore,
                         CsScratch &scratch) {
#ifdef ATX_ALPHA_CS_REFERENCE
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
      cs_rank_row(x, members, out, scratch); // writes ranks for the whole group (idempotent)
    }
  }
#else
  scratch.begin_date(valid.size());
  if (zscore) {
    // Pass 1: per-group Σx / count. Pass 2: per-group Σ(x-mean)². Pass 3: emit
    // (x-mean)/sample-std. Each Σ accumulates in ascending index order within
    // the group -> identical mean and std to the reference's per-cell gather.
    for (const atx::usize i : valid) {
      if (cs_is_nan(g[i])) {
        continue;
      }
      const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
      scratch.gsum[gid] += x[i];
      ++scratch.gcnt[gid];
    }
    for (const atx::usize i : valid) {
      if (cs_is_nan(g[i])) {
        continue;
      }
      const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
      const atx::f64 d = x[i] - scratch.gsum[gid] / static_cast<atx::f64>(scratch.gcnt[gid]);
      scratch.gaux[gid] += d * d;
    }
    for (const atx::usize i : valid) {
      if (cs_is_nan(g[i])) {
        continue;
      }
      const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
      const atx::usize cnt = scratch.gcnt[gid];
      const atx::f64 mean = scratch.gsum[gid] / static_cast<atx::f64>(cnt);
      const atx::f64 sd =
          (cnt < 2) ? kCsNaN : std::sqrt(scratch.gaux[gid] / static_cast<atx::f64>(cnt - 1));
      out[i] = (x[i] - mean) / sd; // sd NaN (singleton) -> NaN
    }
    return;
  }
  // Group rank: count members, lay out a CSR member array (ascending within
  // each group), then ordinal-rank each group's slice ONCE. The per-group
  // stable_sort over the same ascending members is bit-identical to the
  // reference's per-member cs_rank_row (which is idempotent over the group).
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    ++scratch.gcnt[gid];
  }
  const atx::usize ng = scratch.ngroups;
  if (ng == 0) {
    return;
  }
  scratch.goff.assign(ng + 1, 0);
  for (atx::usize k = 0; k < ng; ++k) {
    scratch.goff[k + 1] = scratch.goff[k] + scratch.gcnt[k];
  }
  scratch.gcur.assign(scratch.goff.begin(), scratch.goff.begin() + static_cast<std::ptrdiff_t>(ng));
  scratch.gmem.resize(scratch.goff[ng]);
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    scratch.gmem[scratch.gcur[gid]++] = i; // ascending scan -> ascending within group
  }
  for (atx::usize k = 0; k < ng; ++k) {
    const atx::usize b = scratch.goff[k];
    const atx::usize e = scratch.goff[k + 1];
    const atx::usize cnt = e - b;
    std::stable_sort(scratch.gmem.begin() + static_cast<std::ptrdiff_t>(b),
                     scratch.gmem.begin() + static_cast<std::ptrdiff_t>(e),
                     [&](atx::usize i, atx::usize j) { return x[i] < x[j]; });
    for (atx::usize r = 0; r < cnt; ++r) {
      const atx::f64 pct =
          (cnt == 1) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(cnt - 1);
      out[scratch.gmem[b + r]] = pct;
    }
  }
#endif
}

// ===========================================================================
//  CsNormalize (P3b-2) — cross-sectional demean: x - mean over the valid set.
//  Out-of-set cells stay NaN. With an empty valid set the mean is NaN, but the
//  loop body never executes, so every cell stays NaN (matches the oracle).
// ===========================================================================
inline void cs_normalize_row(std::span<const atx::f64> x, const std::vector<atx::usize> &valid,
                             std::span<atx::f64> out) {
  const atx::usize n = valid.size();
  if (n == 0) {
    return; // empty valid set -> every cell stays NaN (matches the reference)
  }
  atx::f64 sum = 0.0; // ascending-order sum == cs_gather + cs_mean, no scratch
  for (const atx::usize i : valid) {
    sum += x[i];
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
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
  const atx::usize n = valid.size();
  if (n == 0) {
    return; // empty valid set -> every cell stays NaN (matches the reference)
  }
  atx::f64 sum = 0.0; // streaming mean + sample std (same order as the gather path)
  for (const atx::usize i : valid) {
    sum += x[i];
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 sd = kCsNaN; // fewer than 2 obs -> NaN sd -> comparisons false -> pass-through
  if (n >= 2) {
    atx::f64 ss = 0.0;
    for (const atx::usize i : valid) {
      const atx::f64 d = x[i] - mean;
      ss += d * d;
    }
    sd = std::sqrt(ss / static_cast<atx::f64>(n - 1));
  }
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
                                    bool want_mean, CsScratch &scratch) {
#ifdef ATX_ALPHA_CS_REFERENCE
  (void)scratch;
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
#else
  scratch.begin_date(valid.size());
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    scratch.gsum[gid] += x[i];
    ++scratch.gcnt[gid];
  }
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    out[i] = want_mean ? scratch.gsum[gid] / static_cast<atx::f64>(scratch.gcnt[gid])
                       : static_cast<atx::f64>(scratch.gcnt[gid]);
  }
#endif
}

// ===========================================================================
//  CsScaleG (P3b-2) — scale WITHIN each group so Σ|x| over the group's valid
//  members equals 1 (a zero-L1 group leaves its members at 0, like
//  cs_scale_row). A cell with a NaN group label stays NaN.
// ===========================================================================
inline void cs_group_scale_row(std::span<const atx::f64> x, std::span<const atx::f64> g,
                               const std::vector<atx::usize> &valid, std::span<atx::f64> out,
                               CsScratch &scratch) {
#ifdef ATX_ALPHA_CS_REFERENCE
  (void)scratch;
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
#else
  // Pass 1: per-group Σ|x| in ascending order. Pass 2: emit x / Σ|x|.
  scratch.begin_date(valid.size());
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    scratch.gaux[gid] += std::fabs(x[i]);
  }
  for (const atx::usize i : valid) {
    if (cs_is_nan(g[i])) {
      continue;
    }
    const atx::usize gid = scratch.group_id(CsScratch::label_key(g[i]));
    const atx::f64 l1 = scratch.gaux[gid];
    out[i] = x[i] * ((l1 == 0.0) ? 0.0 : 1.0 / l1);
  }
#endif
}

} // namespace atx::engine::alpha::detail

#pragma once

// atx::engine::alpha — sliding-window ORDER STATISTICS (Lane 1).
//
// ts_rank / ts_median / ts_quantile(0.5) over a trailing window of d cells, in
// O(log d) (+ a small memmove) per slide instead of the batch path's O(d) count
// (rank) or O(d log d) gather-and-sort (median). Two window structures:
//
//   * SortedWindow — the window's values kept in a sorted array; insert/erase
//     are a binary search plus a memmove. For d up to a few hundred the memmove
//     is a handful of cache lines and beats any tree.
//   * FenwickWindow — a Binary Indexed Tree over the RANK-COMPRESSED values of
//     the whole column (ranks assigned once per column by a radix argsort):
//     insert/erase/rank/k-th are O(log U), independent of d.
//
// BIT-EXACTNESS (these ops are routed on the DEFAULT AuditExact path):
//   * rank: the batch kernel counts less = #{v < last} and equal = #{v == last}
//     over the window and emits (less + (equal-1)/2) / (d-1). Both structures
//     yield the SAME integer counts (IEEE `<` / `==`, so -0.0 == +0.0) and the
//     emit formula is restated verbatim -> identical bits.
//   * median: the batch kernel sorts the window and emits the middle element
//     (odd d) or the mean of the middle two (even d). The sorted multiset is the
//     same, so the selected VALUES agree — except for the SIGN OF A ZERO: -0.0
//     and +0.0 compare equal, so which one lands in a middle slot depends on the
//     sort algorithm's handling of ties. The sweep therefore tracks how many
//     -0.0 cells are in the window: when a selected middle element is a zero and
//     the window holds a -0.0, that ONE cell falls back to the batch gather +
//     std::sort (the literal batch code, hence its exact bits); when the window
//     holds no -0.0, every zero is +0.0 and the selected zero is emitted as +0.0.
//   * NaN / warm-up: identical gate to tsv_window_valid — output only when the
//     window is full (t+1 >= d) and NaN-free; +/-inf are ordinary values here.
//
// Header-only. The column sweep keeps its structures in thread_local scratch that
// grows monotonically (no per-cell allocation; a warm sweep allocates nothing).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/cs_radix.hpp"
#include "atx/engine/alpha/registry.hpp" // OpCode

namespace atx::engine::alpha::ordstat {

inline constexpr atx::f64 kOsNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Batch-identical average-rank percentile of the last element from its counts.
[[nodiscard]] inline atx::f64 rank_from_counts(atx::usize less, atx::usize equal,
                                               atx::usize d) noexcept {
  const atx::f64 avg = static_cast<atx::f64>(less) + (static_cast<atx::f64>(equal) - 1.0) / 2.0;
  return d == 1 ? 0.5 : avg / static_cast<atx::f64>(d - 1);
}

// Branchless counts of w[0..n) strictly below / equal to v. A flat compare-and-
// add loop the compiler vectorizes: for the window sizes alpha formulas use
// (d <= ~128) it beats a binary search, whose data-dependent branches mispredict
// on every level.
inline void count_less_equal(const atx::f64 *w, atx::usize n, atx::f64 v, atx::usize &less,
                             atx::usize &equal) noexcept {
  atx::usize l = 0;
  atx::usize e = 0;
  for (atx::usize i = 0; i < n; ++i) {
    l += static_cast<atx::usize>(w[i] < v);
    e += static_cast<atx::usize>(w[i] == v);
  }
  less = l;
  equal = e;
}

// ===========================================================================
//  SortedWindow — sorted array of the window's values. Capacity fixed at
//  construction (grow-only via reset). push/pop are O(log d + d/4 moves).
// ===========================================================================
class SortedWindow {
public:
  SortedWindow() = default;
  explicit SortedWindow(atx::usize capacity) : vals_(capacity) {}

  // Empty the window and make room for `capacity` values (grow-only).
  void reset(atx::usize capacity) {
    if (vals_.size() < capacity) {
      vals_.resize(capacity);
    }
    n_ = 0;
  }

  [[nodiscard]] atx::usize size() const noexcept { return n_; }

  // Insert v (not NaN). Precondition: size() < capacity.
  void push(atx::f64 v) noexcept {
    ATX_ASSERT(n_ < vals_.size());
    atx::f64 *b = vals_.data();
    atx::usize less = 0;
    atx::usize equal = 0;
    count_less_equal(b, n_, v, less, equal);
    const atx::usize pos = less + equal; // == upper_bound on the sorted array
    std::memmove(b + pos + 1, b + pos, (n_ - pos) * sizeof(atx::f64));
    b[pos] = v;
    ++n_;
  }

  // Erase one occurrence of v (not NaN). Precondition: v is in the window. Any
  // element comparing equal is removed; a signed-zero swap is harmless because
  // the sweep never trusts the sign of a stored zero (see the header comment).
  void pop(atx::f64 v) noexcept {
    atx::f64 *b = vals_.data();
    atx::usize pos = 0; // == lower_bound on the sorted array
    atx::usize equal = 0;
    count_less_equal(b, n_, v, pos, equal);
    ATX_ASSERT(pos < n_ && b[pos] == v);
    std::memmove(b + pos, b + pos + 1, (n_ - pos - 1) * sizeof(atx::f64));
    --n_;
  }

  // Counts of window values strictly below / equal to v.
  void counts(atx::f64 v, atx::usize &less, atx::usize &equal) const noexcept {
    count_less_equal(vals_.data(), n_, v, less, equal);
  }

  // k-th smallest (0-based). Precondition: k < size().
  [[nodiscard]] atx::f64 kth(atx::usize k) const noexcept {
    ATX_ASSERT(k < n_);
    return vals_[k];
  }

  // Average-rank percentile of `last` (which must be in the window).
  [[nodiscard]] atx::f64 rank_of(atx::f64 last) const noexcept {
    atx::usize less = 0;
    atx::usize equal = 0;
    counts(last, less, equal);
    return rank_from_counts(less, equal, n_);
  }

private:
  std::vector<atx::f64> vals_;
  atx::usize n_{0};
};

// ===========================================================================
//  FenwickWindow — BIT of counts over U rank-compressed values. The caller
//  supplies each cell's compressed rank (0..U-1; equal values share a rank) and
//  the rank -> representative value table. O(log U) per operation.
// ===========================================================================
class FenwickWindow {
public:
  // Reset to an empty window over `universe` distinct ranks (grow-only).
  void reset(atx::usize universe) {
    if (tree_.size() < universe + 1) {
      tree_.resize(universe + 1);
    }
    std::fill(tree_.begin(), tree_.begin() + static_cast<std::ptrdiff_t>(universe + 1), 0U);
    u_ = universe;
    n_ = 0;
    top_ = 1;
    while ((top_ << 1U) <= u_) {
      top_ <<= 1U;
    }
  }

  [[nodiscard]] atx::usize size() const noexcept { return n_; }

  void push(atx::u32 r) noexcept {
    add(r, 1);
    ++n_;
  }
  void pop(atx::u32 r) noexcept {
    add(r, -1);
    --n_;
  }

  // Number of window cells with rank < r.
  [[nodiscard]] atx::usize count_below(atx::u32 r) const noexcept {
    atx::i64 s = 0;
    for (atx::usize i = r; i > 0; i &= i - 1) {
      s += tree_[i];
    }
    return static_cast<atx::usize>(s);
  }

  // Rank of the k-th smallest window cell (0-based). Precondition: k < size().
  [[nodiscard]] atx::u32 kth_rank(atx::usize k) const noexcept {
    ATX_ASSERT(k < n_);
    atx::usize pos = 0;
    atx::i64 rem = static_cast<atx::i64>(k);
    for (atx::usize step = top_; step > 0; step >>= 1U) {
      const atx::usize nxt = pos + step;
      if (nxt <= u_ && tree_[nxt] <= rem) {
        pos = nxt;
        rem -= tree_[nxt];
      }
    }
    return static_cast<atx::u32>(pos); // 1-based pos+1 -> 0-based rank pos
  }

private:
  void add(atx::u32 r, atx::i32 delta) noexcept {
    for (atx::usize i = static_cast<atx::usize>(r) + 1; i <= u_; i += i & (~i + 1)) {
      tree_[i] += delta;
    }
  }

  std::vector<atx::i32> tree_; // 1-based
  atx::usize u_{0};
  atx::usize n_{0};
  atx::usize top_{1};
};

// Rank-compress a column: rank[t] in 0..U-1 for every non-NaN x[t] (equal values
// share a rank; -0.0 and +0.0 share one), uniq[r] = a representative value.
// NaN cells get rank U32_MAX (never inserted). Returns U. Scratch grows only.
struct Compressed {
  std::vector<atx::u32> rank;
  std::vector<atx::f64> uniq;
  std::vector<atx::usize> ids;
  atx::engine::alpha::detail::CsRadixScratch radix;
};

inline atx::usize compress_column(std::span<const atx::f64> col, Compressed &c) {
  const atx::usize n = col.size();
  if (c.rank.size() < n) {
    c.rank.resize(n);
    c.uniq.resize(n);
  }
  c.ids.clear();
  for (atx::usize t = 0; t < n; ++t) {
    c.rank[t] = std::numeric_limits<atx::u32>::max();
    if (!std::isnan(col[t])) {
      c.ids.push_back(t);
    }
  }
  atx::engine::alpha::detail::cs_stable_argsort(col, std::span<atx::usize>{c.ids}, c.radix);
  atx::usize u = 0;
  for (atx::usize i = 0; i < c.ids.size(); ++i) {
    const atx::f64 v = col[c.ids[i]];
    if (u == 0 || !(c.uniq[u - 1] == v)) {
      c.uniq[u++] = v;
    }
    c.rank[c.ids[i]] = static_cast<atx::u32>(u - 1);
  }
  return u;
}

// ===========================================================================
//  Column sweep (the ts_ops.hpp routing target for TsRank / TsMed / TsQuantile)
// ===========================================================================

[[nodiscard]] inline bool is_order_stat_op(OpCode op) noexcept {
  return op == OpCode::TsRank || op == OpCode::TsMed || op == OpCode::TsQuantile;
}

// Windows up to this length use SortedWindow; longer ones use FenwickWindow.
inline constexpr atx::usize kSortedMaxWindow = 160;

// Per-thread scratch for the column sweep. Grow-only; never shrinks.
struct SweepScratch {
  SortedWindow sorted;
  FenwickWindow fenwick;
  Compressed comp;
  std::vector<atx::f64> col;    // contiguous column extract
  std::vector<atx::f64> gather; // batch-fallback window buffer
};

[[nodiscard]] inline SweepScratch &sweep_scratch() {
  thread_local SweepScratch s;
  return s;
}

// The batch median of col[t+1-d .. t] — the literal ts_value_at median code
// (gather chronologically, std::sort, pick middle) so the fallback's bits are the
// batch bits.
[[nodiscard]] inline atx::f64 batch_median(std::span<const atx::f64> col, atx::usize t,
                                           atx::usize d, std::vector<atx::f64> &buf) {
  if (buf.size() < d) {
    buf.resize(d);
  }
  for (atx::usize i = 0; i < d; ++i) {
    buf[i] = col[t + 1 - d + i];
  }
  std::sort(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(d));
  return (d % 2 == 1) ? buf[d / 2] : (buf[d / 2 - 1] + buf[d / 2]) / 2.0;
}

// Median from the two middle order statistics with the signed-zero rule. `lo_v`
// and `hi_v` are the (d/2-1)-th and (d/2)-th smallest (hi only for odd d).
// Returns false when the cell must fall back to the batch median.
[[nodiscard]] inline bool median_from(atx::f64 lo_v, atx::f64 hi_v, atx::usize d,
                                      atx::usize neg_zero, atx::f64 &out) noexcept {
  if (hi_v == 0.0 || (d % 2 == 0 && lo_v == 0.0)) {
    if (neg_zero != 0) {
      return false;
    }
    hi_v = hi_v == 0.0 ? 0.0 : hi_v; // canonical +0.0: the window holds no -0.0
    lo_v = lo_v == 0.0 ? 0.0 : lo_v;
  }
  out = (d % 2 == 1) ? hi_v : (lo_v + hi_v) / 2.0;
  return true;
}

// ts_rank for small windows: the batch count (less / equal over the window) as a
// branchless vectorized loop on the contiguous column, with the NaN gate kept
// incrementally. Same integer counts -> same bits as tsv_rank.
inline void sweep_rank_direct(std::span<const atx::f64> col, atx::usize d, std::span<atx::f64> out,
                              atx::usize ostride, atx::usize ooff) noexcept {
  const atx::usize dates = col.size();
  atx::usize nan_cnt = 0;
  for (atx::usize t = 0; t < dates; ++t) {
    nan_cnt += static_cast<atx::usize>(std::isnan(col[t]));
    if (t >= d) {
      nan_cnt -= static_cast<atx::usize>(std::isnan(col[t - d]));
    }
    atx::f64 &o = out[t * ostride + ooff];
    if (t + 1 < d || nan_cnt != 0) {
      o = kOsNaN;
      continue;
    }
    atx::usize less = 0;
    atx::usize equal = 0;
    count_less_equal(col.data() + (t + 1 - d), d, col[t], less, equal);
    o = rank_from_counts(less, equal, d);
  }
}

// Sweep one contiguous column `col` (length dates) for op in {TsRank, TsMed,
// TsQuantile} with window d, writing out[t * ostride + ooff] for every t.
inline void sweep_column(OpCode op, std::span<const atx::f64> col, atx::usize d,
                         std::span<atx::f64> out, atx::usize ostride, atx::usize ooff,
                         SweepScratch &s) {
  ATX_ASSERT(is_order_stat_op(op));
  const atx::usize dates = col.size();
  if (d == 0) {
    for (atx::usize t = 0; t < dates; ++t) {
      out[t * ostride + ooff] = kOsNaN;
    }
    return;
  }
  const bool rank = (op == OpCode::TsRank);
  const bool fenwick = d > kSortedMaxWindow;
  if (rank && !fenwick) {
    sweep_rank_direct(col, d, out, ostride, ooff);
    return;
  }
  if (fenwick) {
    s.fenwick.reset(compress_column(col, s.comp));
  } else {
    s.sorted.reset(d + 1); // push-before-pop holds d+1 values transiently
  }
  atx::usize nan_cnt = 0;
  atx::usize neg_zero = 0;
  for (atx::usize t = 0; t < dates; ++t) {
    const atx::f64 enter = col[t];
    if (std::isnan(enter)) {
      ++nan_cnt;
    } else {
      neg_zero += static_cast<atx::usize>(enter == 0.0 && std::signbit(enter));
      if (fenwick) {
        s.fenwick.push(s.comp.rank[t]);
      } else {
        s.sorted.push(enter);
      }
    }
    if (t >= d) {
      const atx::f64 leave = col[t - d];
      if (std::isnan(leave)) {
        --nan_cnt;
      } else {
        neg_zero -= static_cast<atx::usize>(leave == 0.0 && std::signbit(leave));
        if (fenwick) {
          s.fenwick.pop(s.comp.rank[t - d]);
        } else {
          s.sorted.pop(leave);
        }
      }
    }
    atx::f64 &o = out[t * ostride + ooff];
    if (t + 1 < d || nan_cnt != 0) {
      o = kOsNaN;
      continue;
    }
    if (rank) {
      if (fenwick) {
        const atx::u32 r = s.comp.rank[t];
        const atx::usize less = s.fenwick.count_below(r);
        const atx::usize equal = s.fenwick.count_below(r + 1) - less;
        o = rank_from_counts(less, equal, d);
      } else {
        o = s.sorted.rank_of(enter);
      }
      continue;
    }
    const atx::usize hi_k = d / 2;
    const atx::usize lo_k = d % 2 == 0 ? hi_k - 1 : hi_k;
    const atx::f64 hi_v =
        fenwick ? s.comp.uniq[s.fenwick.kth_rank(hi_k)] : s.sorted.kth(hi_k);
    const atx::f64 lo_v =
        fenwick ? s.comp.uniq[s.fenwick.kth_rank(lo_k)] : s.sorted.kth(lo_k);
    if (!median_from(lo_v, hi_v, d, neg_zero, o)) {
      o = batch_median(col, t, d, s.gather);
    }
  }
}

// Strided entry matching the ts_ops.hpp online-sweep signature: column j of a
// date-major buffer. Extracts the column once (contiguous) then sweeps it.
inline void sweep_strided(OpCode op, std::span<const atx::f64> x, std::span<atx::f64> out,
                          atx::usize dates, atx::usize j, atx::usize d, atx::usize instruments) {
  SweepScratch &s = sweep_scratch();
  if (s.col.size() < dates) {
    s.col.resize(dates);
  }
  for (atx::usize t = 0; t < dates; ++t) {
    s.col[t] = x[t * instruments + j];
  }
  sweep_column(op, std::span<const atx::f64>{s.col.data(), dates}, d, out, instruments, j, s);
}

} // namespace atx::engine::alpha::ordstat

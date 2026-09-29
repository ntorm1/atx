// atx::engine::alpha — reference-oracle twins of the platform-v7 W2 literature
// ops (A7). The semantics are pinned in lit_ops.hpp; this file restates each
// rule INDEPENDENTLY (it does not include lit_ops.hpp) in the oracle's
// gather-then-compute style: windows and valid sets are materialized as dense
// vectors, every sum runs in chronological (Ts) / ascending-instrument (Cs)
// order, and the OLS solve follows the same Cholesky sequence — so the VM
// differential (alpha_lit_ops_test.cpp) is bit-exact and a real cross-check of
// the VM's strided gathers, block reads, masks and short-window handling.
#include "atx/engine/alpha/oracle.hpp"

#include <algorithm> // std::sort, std::stable_sort, std::min_element, std::max_element
#include <cmath>     // std::sqrt, std::isfinite, std::isnan, std::floor
#include <span>      // std::span
#include <vector>    // std::vector

namespace atx::engine::alpha {

namespace detail {

namespace {

constexpr atx::f64 kRefCollinearTol = 1e-12;    // Cholesky pivot / its diagonal
constexpr atx::f64 kRefGroupRadix = 67108864.0; // 2^26

// ---- OLS reference ----------------------------------------------------------

struct RefFit {
  atx::f64 mean_y{};
  std::vector<atx::f64> mean_x;
  std::vector<atx::f64> beta;
  bool ok{false};
  bool yflat{false};
};

// Σ over rows of (a - ma)(b - mb), rows in order.
[[nodiscard]] atx::f64 ref_cross(const std::vector<atx::f64> &a, atx::f64 ma,
                                 const std::vector<atx::f64> &b, atx::f64 mb) {
  atx::f64 s = 0.0;
  for (atx::usize i = 0; i < a.size(); ++i) {
    s += (a[i] - ma) * (b[i] - mb);
  }
  return s;
}

// OLS of y on [1, xs...] (one dense vector per regressor, row i = index i).
[[nodiscard]] RefFit ref_ols(const std::vector<atx::f64> &y,
                             const std::vector<std::vector<atx::f64>> &xs) {
  RefFit f;
  const atx::usize n = y.size();
  const atx::usize k = xs.size();
  if (k == 0 || k > 4 || n < k + 2) {
    return f;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  f.mean_y = sum_of(y) / nf;
  f.mean_x.assign(k, 0.0);
  for (atx::usize j = 0; j < k; ++j) {
    f.mean_x[j] = sum_of(xs[j]) / nf;
  }
  std::vector<std::vector<atx::f64>> s(k, std::vector<atx::f64>(k, 0.0));
  std::vector<atx::f64> sy(k, 0.0);
  for (atx::usize j = 0; j < k; ++j) {
    sy[j] = ref_cross(xs[j], f.mean_x[j], y, f.mean_y);
    for (atx::usize l = 0; l <= j; ++l) {
      s[j][l] = ref_cross(xs[j], f.mean_x[j], xs[l], f.mean_x[l]);
    }
  }
  const atx::f64 syy = ref_cross(y, f.mean_y, y, f.mean_y);
  for (atx::usize j = 0; j < k; ++j) {
    if (window_is_flat(s[j][j], f.mean_x[j], n)) {
      return f;
    }
  }
  std::vector<std::vector<atx::f64>> ch(k, std::vector<atx::f64>(k, 0.0));
  for (atx::usize j = 0; j < k; ++j) {
    atx::f64 dj = s[j][j];
    for (atx::usize l = 0; l < j; ++l) {
      dj -= ch[j][l] * ch[j][l];
    }
    if (!(dj > kRefCollinearTol * s[j][j])) {
      return f;
    }
    ch[j][j] = std::sqrt(dj);
    for (atx::usize q = j + 1; q < k; ++q) {
      atx::f64 a = s[q][j];
      for (atx::usize l = 0; l < j; ++l) {
        a -= ch[q][l] * ch[j][l];
      }
      ch[q][j] = a / ch[j][j];
    }
  }
  std::vector<atx::f64> z(k, 0.0);
  for (atx::usize j = 0; j < k; ++j) {
    atx::f64 a = sy[j];
    for (atx::usize l = 0; l < j; ++l) {
      a -= ch[j][l] * z[l];
    }
    z[j] = a / ch[j][j];
  }
  f.beta.assign(k, 0.0);
  for (atx::usize j = k; j-- > 0;) {
    atx::f64 a = z[j];
    for (atx::usize l = j + 1; l < k; ++l) {
      a -= ch[l][j] * f.beta[l];
    }
    f.beta[j] = a / ch[j][j];
  }
  f.ok = true;
  f.yflat = window_is_flat(syy, f.mean_y, n);
  return f;
}

[[nodiscard]] atx::f64 ref_resid(const RefFit &f, const std::vector<atx::f64> &y,
                                 const std::vector<std::vector<atx::f64>> &xs, atx::usize i) {
  if (f.yflat) {
    return 0.0;
  }
  atx::f64 r = y[i] - f.mean_y;
  for (atx::usize j = 0; j < xs.size(); ++j) {
    r -= f.beta[j] * (xs[j][i] - f.mean_x[j]);
  }
  return r;
}

// ---- trailing-window twins ---------------------------------------------------

// The window [max(0, t-d+1), t] of column j, chronological, unfiltered.
[[nodiscard]] std::vector<atx::f64> ref_window(std::span<const atx::f64> x, atx::usize t,
                                               atx::usize j, atx::usize d, atx::usize inst) {
  const atx::usize len = std::min(t + 1, d);
  std::vector<atx::f64> w;
  w.reserve(len);
  for (atx::usize s = t + 1 - len; s <= t; ++s) {
    w.push_back(x[s * inst + j]);
  }
  return w;
}

[[nodiscard]] atx::f64 ref_topk(const std::vector<atx::f64> &w, atx::usize d, atx::usize k) {
  if (w.size() < d || k == 0 || k > w.size()) {
    return kNaN;
  }
  for (const atx::f64 v : w) {
    if (is_nan(v)) {
      return kNaN;
    }
  }
  std::vector<atx::f64> sorted = w;
  std::sort(sorted.begin(), sorted.end());
  atx::f64 sum = 0.0;
  for (atx::usize i = sorted.size() - k; i < sorted.size(); ++i) {
    sum += sorted[i];
  }
  return sum / static_cast<atx::f64>(k);
}

[[nodiscard]] atx::f64 ref_count_increases(const std::vector<atx::f64> &w, atx::usize d) {
  if (w.empty() || w.size() < d || is_nan(w.back())) {
    return kNaN;
  }
  atx::f64 count = 0.0;
  for (atx::usize back = 0; back < w.size(); ++back) {
    const atx::f64 v = w[w.size() - 1 - back];
    if (is_nan(v) || v < 0.0) {
      break;
    }
    if (v > 0.0) {
      count += 1.0;
    }
  }
  return count;
}

[[nodiscard]] atx::f64 ref_mp(OpCode op, const std::vector<atx::f64> &w, atx::usize d,
                              atx::usize m) {
  std::vector<atx::f64> v; // the finite cells, chronological
  for (const atx::f64 x : w) {
    if (std::isfinite(x)) {
      v.push_back(x);
    }
  }
  const atx::usize n = v.size();
  const bool needs_two = op == OpCode::TsStdMp || op == OpCode::TsZscoreMp;
  if (w.empty() || n < (needs_two ? std::max<atx::usize>(m, 2) : m)) {
    return kNaN;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  if (op == OpCode::TsSumMp) {
    return sum_of(v);
  }
  if (op == OpCode::TsMeanMp) {
    return sum_of(v) / nf;
  }
  if (op == OpCode::TsMinMp) {
    return *std::min_element(v.begin(), v.end());
  }
  if (op == OpCode::TsMaxMp) {
    return *std::max_element(v.begin(), v.end());
  }
  if (op == OpCode::TsDecayLinearMp) {
    atx::f64 num = 0.0;
    atx::f64 den = 0.0;
    for (atx::usize i = 0; i < w.size(); ++i) {
      if (std::isfinite(w[i])) {
        const atx::usize age = w.size() - 1 - i;
        num += static_cast<atx::f64>(d - age) * w[i];
        den += static_cast<atx::f64>(d - age);
      }
    }
    return num / den;
  }
  const atx::f64 mean = sum_of(v) / nf;
  atx::f64 ss = 0.0;
  for (const atx::f64 x : v) {
    ss += (x - mean) * (x - mean);
  }
  const bool flat = window_is_flat(ss, mean, n);
  if (op == OpCode::TsStdMp) {
    return flat ? 0.0 : std::sqrt(ss / static_cast<atx::f64>(n - 1));
  }
  // TsZscoreMp: the current cell must itself be finite.
  if (!std::isfinite(w.back()) || flat) {
    return kNaN;
  }
  return (w.back() - mean) / std::sqrt(ss / static_cast<atx::f64>(n - 1));
}

[[nodiscard]] atx::f64 ref_corr_mp(const std::vector<atx::f64> &x, const std::vector<atx::f64> &y,
                                   atx::usize m) {
  std::vector<atx::f64> a;
  std::vector<atx::f64> b;
  for (atx::usize i = 0; i < x.size(); ++i) {
    if (std::isfinite(x[i]) && std::isfinite(y[i])) {
      a.push_back(x[i]);
      b.push_back(y[i]);
    }
  }
  const atx::usize n = a.size();
  if (n < std::max<atx::usize>(m, 2)) {
    return kNaN;
  }
  const atx::f64 ma = sum_of(a) / static_cast<atx::f64>(n);
  const atx::f64 mb = sum_of(b) / static_cast<atx::f64>(n);
  const atx::f64 sab = ref_cross(a, ma, b, mb);
  const atx::f64 saa = ref_cross(a, ma, a, ma);
  const atx::f64 sbb = ref_cross(b, mb, b, mb);
  if (window_is_flat(saa, ma, n) || window_is_flat(sbb, mb, n)) {
    return kNaN;
  }
  const atx::f64 den = std::sqrt(saa * sbb);
  return den == 0.0 ? kNaN : sab / den;
}

// ts_resid_on / ts_beta_on: `ins[0]` = y, `ins[1..]` = the regressors.
[[nodiscard]] atx::f64 ref_ts_regress(OpCode op, const std::vector<std::span<const atx::f64>> &ins,
                                      atx::usize t, atx::usize j, atx::usize d, atx::usize inst) {
  if (t + 1 < d || d == 0) {
    return kNaN;
  }
  const std::vector<atx::f64> y = ref_window(ins[0], t, j, d, inst);
  std::vector<std::vector<atx::f64>> xs;
  for (atx::usize c = 1; c < ins.size(); ++c) {
    xs.push_back(ref_window(ins[c], t, j, d, inst));
  }
  for (atx::usize i = 0; i < d; ++i) {
    if (!std::isfinite(y[i])) {
      return kNaN;
    }
    for (const std::vector<atx::f64> &col : xs) {
      if (!std::isfinite(col[i])) {
        return kNaN;
      }
    }
  }
  const RefFit f = ref_ols(y, xs);
  if (!f.ok) {
    return kNaN;
  }
  if (op == OpCode::TsBetaOn) {
    return f.yflat ? 0.0 : f.beta[0];
  }
  return ref_resid(f, y, xs, d - 1);
}

[[nodiscard]] atx::f64 ref_ts_cell(OpCode op, const std::vector<std::span<const atx::f64>> &ins,
                                   atx::usize t, atx::usize j, atx::usize d, atx::f64 p0,
                                   atx::usize inst) {
  if (d == 0) {
    return kNaN;
  }
  if (op == OpCode::TsResidOn || op == OpCode::TsBetaOn) {
    return ref_ts_regress(op, ins, t, j, d, inst);
  }
  const std::vector<atx::f64> w = ref_window(ins[0], t, j, d, inst);
  const auto count = static_cast<atx::usize>(p0);
  switch (op) {
  case OpCode::TsTopkMean:
    return ref_topk(w, d, count);
  case OpCode::TsCountIncreases:
    return ref_count_increases(w, d);
  case OpCode::TsCorrMp:
    return ref_corr_mp(w, ref_window(ins[1], t, j, d, inst), count);
  default:
    return ref_mp(op, w, d, count);
  }
}

// ---- cross-sectional twins ---------------------------------------------------

// Average-rank bucket (restated; ties share the mean ordinal position).
void ref_bucket(std::span<const atx::f64> x, const std::vector<atx::usize> &valid, int nb,
                std::span<atx::f64> out) {
  const atx::usize m = valid.size();
  if (m == 0 || nb < 2) {
    return;
  }
  std::vector<atx::usize> order = valid;
  std::stable_sort(order.begin(), order.end(),
                   [&x](atx::usize a, atx::usize b) { return x[a] < x[b]; });
  for (atx::usize lo = 0; lo < m;) {
    atx::usize hi = lo;
    while (hi + 1 < m && x[order[hi + 1]] == x[order[lo]]) {
      ++hi;
    }
    const atx::f64 pos = static_cast<atx::f64>(lo) + static_cast<atx::f64>(hi - lo) / 2.0;
    const atx::f64 p = (m == 1) ? 0.5 : pos / static_cast<atx::f64>(m - 1);
    int b = static_cast<int>(p * static_cast<atx::f64>(nb));
    if (b >= nb) {
      b = nb - 1;
    }
    for (atx::usize r = lo; r <= hi; ++r) {
      out[order[r]] = static_cast<atx::f64>(b);
    }
    lo = hi + 1;
  }
}

void ref_cs_resid(std::span<const atx::f64> x, const std::vector<std::span<const atx::f64>> &cov,
                  const std::vector<atx::usize> &valid, std::span<atx::f64> out) {
  std::vector<atx::usize> rset;
  for (const atx::usize i : valid) {
    bool ok = std::isfinite(x[i]);
    for (const std::span<const atx::f64> c : cov) {
      ok = ok && std::isfinite(c[i]);
    }
    if (ok) {
      rset.push_back(i);
    }
  }
  std::vector<atx::f64> y;
  std::vector<std::vector<atx::f64>> xs(cov.size());
  for (const atx::usize i : rset) {
    y.push_back(x[i]);
    for (atx::usize j = 0; j < cov.size(); ++j) {
      xs[j].push_back(cov[j][i]);
    }
  }
  const RefFit f = ref_ols(y, xs);
  if (!f.ok) {
    return;
  }
  for (atx::usize p = 0; p < rset.size(); ++p) {
    out[rset[p]] = ref_resid(f, y, xs, p);
  }
}

[[nodiscard]] atx::f64 ref_group_cross(atx::f64 g1, atx::f64 g2) {
  const bool l1 = !is_nan(g1) && g1 >= 0.0 && g1 < kRefGroupRadix && g1 == std::floor(g1);
  const bool l2 = !is_nan(g2) && g2 >= 0.0 && g2 < kRefGroupRadix && g2 == std::floor(g2);
  return (l1 && l2) ? g1 * kRefGroupRadix + g2 : kNaN;
}

} // namespace

// Dispatch of every W2 opcode (registry.hpp is_lit_op). Regressor blocks are
// read as consecutive pool columns starting at the operand's slot, widths from
// Instr::param (registry.hpp lit_reg_*).
atx::core::Status Oracle::eval_lit(const Instr &in) {
  std::span<atx::f64> out = dst_col(in);
  if (in.op == OpCode::ArgPack) {
    for (atx::u32 c = 0; c < in.n_out; ++c) {
      const std::span<const atx::f64> s = src_col(in, c);
      const std::span<atx::f64> o = pool_.column(in.dst + c);
      for (atx::usize i = 0; i < cells_; ++i) {
        o[i] = s[i];
      }
    }
    return atx::core::Ok();
  }
  if (in.op == OpCode::GroupCross) {
    const std::span<const atx::f64> g1 = src_col(in, 0);
    const std::span<const atx::f64> g2 = src_col(in, 1);
    for (atx::usize i = 0; i < cells_; ++i) {
      out[i] = ref_group_cross(g1[i], g2[i]);
    }
    return atx::core::Ok();
  }
  std::vector<std::span<const atx::f64>> ins{src_col(in, 0)};
  if (is_pack_consumer(in.op)) {
    for (atx::u32 c = 0; c < lit_reg_wb(in.param); ++c) {
      ins.push_back(pool_.column(in.src[1] + c));
    }
    for (atx::u32 c = 0; c < lit_reg_wc(in.param); ++c) {
      ins.push_back(pool_.column(in.src[2] + c));
    }
  } else if (in.op == OpCode::TsCorrMp) {
    ins.push_back(src_col(in, 1));
  }
  if (in.op == OpCode::CsBucket || in.op == OpCode::CsResidOn) {
    const std::vector<std::span<const atx::f64>> cov(ins.begin() + 1, ins.end());
    for (atx::usize d = 0; d < dates_; ++d) {
      const std::span<const atx::f64> xr = ins[0].subspan(d * instruments_, instruments_);
      const std::span<atx::f64> orow = out.subspan(d * instruments_, instruments_);
      std::vector<atx::usize> valid;
      for (atx::usize i = 0; i < instruments_; ++i) {
        orow[i] = kNaN;
        if (!is_nan(xr[i])) {
          valid.push_back(i);
        }
      }
      if (in.op == OpCode::CsBucket) {
        ref_bucket(xr, valid, static_cast<int>(in.imm[0]), orow);
        continue;
      }
      std::vector<std::span<const atx::f64>> crow;
      for (const std::span<const atx::f64> c : cov) {
        crow.push_back(c.subspan(d * instruments_, instruments_));
      }
      ref_cs_resid(xr, crow, valid, orow);
    }
    return atx::core::Ok();
  }
  // Trailing-window ops: the window is the LAST populated operand.
  atx::usize last = 0;
  for (atx::usize k = 0; k < in.src.size(); ++k) {
    if (in.src.at(k) != kNoSlot) {
      last = k;
    }
  }
  const atx::usize d = window_of(src_col(in, last));
  for (atx::usize j = 0; j < instruments_; ++j) {
    for (atx::usize t = 0; t < dates_; ++t) {
      out[t * instruments_ + j] = ref_ts_cell(in.op, ins, t, j, d, in.imm[0], instruments_);
    }
  }
  return atx::core::Ok();
}

} // namespace detail

} // namespace atx::engine::alpha

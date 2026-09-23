// atx::engine::combine — signal-space combiner bodies (Lane 5). Contracts live in
// combine/signal_combiner.hpp; this file only realizes them.

#include "atx/engine/combine/signal_combiner.hpp"

#include <algorithm> // std::max, std::min
#include <cmath>     // std::abs, std::isfinite, std::pow, std::sqrt
#include <utility>   // std::move
#include <vector>    // std::vector

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::combine {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

namespace {

// Complete-case rows of a row-major (rows × k) matrix → Eigen MatX.
[[nodiscard]] MatX complete_rows(const std::vector<f64> &flat, usize rows, usize k) {
  std::vector<usize> keep;
  keep.reserve(rows);
  for (usize r = 0U; r < rows; ++r) {
    bool ok = true;
    for (usize a = 0U; a < k && ok; ++a) {
      ok = std::isfinite(flat[r * k + a]);
    }
    if (ok) {
      keep.push_back(r);
    }
  }
  MatX out(static_cast<Eigen::Index>(keep.size()), static_cast<Eigen::Index>(k));
  for (usize i = 0U; i < keep.size(); ++i) {
    for (usize a = 0U; a < k; ++a) {
      out(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(a)) = flat[keep[i] * k + a];
    }
  }
  return out;
}

// Per-column t-stat mean/(sd/√T) (sample sd); NaN when T < 2 or sd == 0.
[[nodiscard]] std::vector<f64> column_tstats(const MatX &x) {
  const Eigen::Index t = x.rows();
  std::vector<f64> out(static_cast<usize>(x.cols()), kSignalNaN);
  if (t < 2) {
    return out;
  }
  for (Eigen::Index c = 0; c < x.cols(); ++c) {
    const f64 mean = x.col(c).mean();
    const f64 ss = (x.col(c).array() - mean).square().sum();
    const f64 sd = std::sqrt(ss / static_cast<f64>(t - 1));
    if (sd > 0.0) {
      out[static_cast<usize>(c)] = mean / (sd / std::sqrt(static_cast<f64>(t)));
    }
  }
  return out;
}

// Demean each row of `m` (rows × n_cols) within groups given by compact group ids
// (size n_cols, values in [0, n_groups)). One group == plain row demeaning.
void demean_rows_within(MatX &m, const std::vector<usize> &group, usize n_groups) {
  std::vector<f64> sum(n_groups);
  std::vector<f64> cnt(n_groups);
  for (Eigen::Index r = 0; r < m.rows(); ++r) {
    std::fill(sum.begin(), sum.end(), 0.0);
    std::fill(cnt.begin(), cnt.end(), 0.0);
    for (Eigen::Index c = 0; c < m.cols(); ++c) {
      sum[group[static_cast<usize>(c)]] += m(r, c);
      cnt[group[static_cast<usize>(c)]] += 1.0;
    }
    for (Eigen::Index c = 0; c < m.cols(); ++c) {
      const usize g = group[static_cast<usize>(c)];
      m(r, c) -= sum[g] / cnt[g];
    }
  }
}

// Compact arbitrary u32 cluster ids (in first-seen order of ascending sorted id) to
// [0, n_groups). Empty input → every column in group 0.
[[nodiscard]] std::vector<usize> compact_groups(std::span<const atx::u32> ids, usize n,
                                                usize &n_groups) {
  std::vector<usize> out(n, 0U);
  if (ids.empty()) {
    n_groups = 1U;
    return out;
  }
  std::vector<atx::u32> uniq(ids.begin(), ids.end());
  std::sort(uniq.begin(), uniq.end());
  uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
  for (usize i = 0U; i < n; ++i) {
    out[i] = static_cast<usize>(std::lower_bound(uniq.begin(), uniq.end(), ids[i]) - uniq.begin());
  }
  n_groups = uniq.size();
  return out;
}

} // namespace

void normalize_gross(std::vector<f64> &w) noexcept {
  f64 gross = 0.0;
  for (const f64 x : w) {
    gross += std::abs(x);
  }
  if (!(gross > 0.0) || !std::isfinite(gross)) {
    return;
  }
  for (f64 &x : w) {
    x /= gross;
  }
}

atx::core::Status validate_window(const SignalStore &s, FitWindow w, usize min_rows) {
  if (s.n_alphas() == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "signal combiner: empty store");
  }
  if (w.end > s.n_dates() || w.end <= w.begin) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "signal combiner: window outside the store's dates");
  }
  if (w.size() < min_rows) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "signal combiner: window has too few dates");
  }
  return atx::core::Ok();
}

// ---------------------------------------------------------------------------
//  Grinold-Kahn
// ---------------------------------------------------------------------------
atx::core::Result<CombineWeights> grinold_kahn_weights(const MatX &ic, CovTarget target) {
  const usize k = static_cast<usize>(ic.cols());
  std::vector<f64> flat(static_cast<usize>(ic.rows()) * k);
  for (Eigen::Index r = 0; r < ic.rows(); ++r) {
    for (Eigen::Index a = 0; a < ic.cols(); ++a) {
      flat[static_cast<usize>(r) * k + static_cast<usize>(a)] = ic(r, a);
    }
  }
  const MatX x = complete_rows(flat, static_cast<usize>(ic.rows()), k);
  if (x.rows() < 2 || k == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "grinold_kahn_weights: need >= 2 complete IC rows");
  }
  const VecX mean = x.colwise().mean().transpose();
  ATX_TRY(MatX omega, estimate_covariance(x, target));
  Eigen::LLT<MatX> llt(omega);
  if (llt.info() != Eigen::Success) {
    const f64 tr = omega.trace();
    const f64 jitter = 1e-10 * ((tr > 0.0) ? tr / static_cast<f64>(k) : 1.0);
    omega.diagonal().array() += jitter;
    llt.compute(omega);
    if (llt.info() != Eigen::Success) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "grinold_kahn_weights: IC covariance not SPD");
    }
  }
  const VecX sol = llt.solve(mean);
  CombineWeights out;
  out.w.assign(sol.data(), sol.data() + sol.size());
  normalize_gross(out.w);
  out.tstat = column_tstats(x);
  return atx::core::Ok(std::move(out));
}

atx::core::Result<CombineWeights> GrinoldKahnCombiner::fit(const SignalStore &s, FitWindow w) const {
  ATX_TRY_VOID(validate_window(s, w, 2U));
  const usize k = s.n_alphas();
  const std::vector<f64> flat = ic_matrix(s, w);
  const MatX ic = complete_rows(flat, w.size(), k);
  ATX_TRY(CombineWeights out, grinold_kahn_weights(ic, target));
  out.fit_begin = w.begin;
  out.fit_end = w.end;
  return atx::core::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
//  ICIR-EWMA
// ---------------------------------------------------------------------------
atx::core::Result<CombineWeights> IcirEwmaCombiner::fit(const SignalStore &s, FitWindow w) const {
  ATX_TRY_VOID(validate_window(s, w, 2U));
  const usize k = s.n_alphas();
  const usize rows = w.size();
  const std::vector<f64> ic = ic_matrix(s, w);
  const f64 decay = (half_life > 0.0) ? std::pow(0.5, 1.0 / half_life) : 1.0;
  // Row weights: newest row (rows-1) has weight 1; row r has decay^(rows-1-r).
  std::vector<f64> rw(rows);
  f64 cur = 1.0;
  for (usize i = rows; i-- > 0U;) {
    rw[i] = cur;
    cur *= decay;
  }
  CombineWeights out;
  out.w.assign(k, 0.0);
  out.tstat.assign(k, kSignalNaN);
  for (usize a = 0U; a < k; ++a) {
    f64 sw = 0.0;
    f64 sw2 = 0.0;
    f64 swx = 0.0;
    for (usize r = 0U; r < rows; ++r) {
      const f64 x = ic[r * k + a];
      if (std::isfinite(x)) {
        sw += rw[r];
        sw2 += rw[r] * rw[r];
        swx += rw[r] * x;
      }
    }
    if (!(sw > 0.0)) {
      continue;
    }
    const f64 mean = swx / sw;
    f64 swv = 0.0;
    for (usize r = 0U; r < rows; ++r) {
      const f64 x = ic[r * k + a];
      if (std::isfinite(x)) {
        swv += rw[r] * (x - mean) * (x - mean);
      }
    }
    const f64 n_eff = (sw * sw) / sw2;
    if (n_eff < 2.0 - 1e-12 || mean == 0.0) {
      continue;
    }
    // A zero-dispersion IC series (a perfectly stable edge) is floored rather than
    // dropped, so it dominates the normalized blend instead of vanishing.
    const f64 sd = std::sqrt(std::max(swv / sw, 1e-24));
    const f64 icir = mean / sd;
    const f64 t = icir * std::sqrt(n_eff);
    out.tstat[a] = t;
    const f64 shrink = (tstat_haircut > 0.0) ? std::max(0.0, 1.0 - tstat_haircut / std::abs(t)) : 1.0;
    out.w[a] = icir * shrink;
  }
  normalize_gross(out.w);
  out.fit_begin = w.begin;
  out.fit_end = w.end;
  return atx::core::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
//  Fama-MacBeth ridge
// ---------------------------------------------------------------------------
atx::core::Result<CombineWeights> FamaMacBethRidge::fit(const SignalStore &s, FitWindow w) const {
  ATX_TRY_VOID(validate_window(s, w, 1U));
  if (lambda < 0.0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "FamaMacBethRidge: lambda < 0");
  }
  const usize k = s.n_alphas();
  const usize ni = s.n_instruments();
  const auto ke = static_cast<Eigen::Index>(k);
  std::vector<VecX> betas;
  betas.reserve(w.size());
  MatX z(static_cast<Eigen::Index>(ni), ke);
  VecX y(static_cast<Eigen::Index>(ni));
  for (usize t = w.begin; t < w.end; ++t) {
    const auto fwd = s.fwd_row(t);
    Eigen::Index n = 0;
    for (usize i = 0U; i < ni; ++i) {
      bool ok = std::isfinite(fwd[i]);
      for (usize a = 0U; a < k && ok; ++a) {
        ok = std::isfinite(s.signal_row(a, t)[i]);
      }
      if (!ok) {
        continue;
      }
      for (usize a = 0U; a < k; ++a) {
        z(n, static_cast<Eigen::Index>(a)) = s.signal_row(a, t)[i];
      }
      y[n] = fwd[i];
      ++n;
    }
    if (n < ke + ((lambda > 0.0) ? 0 : 1) || n < 2) {
      continue; // under-identified date (OLS needs n > K)
    }
    const auto zt = z.topRows(n);
    MatX g = zt.transpose() * zt;
    g.diagonal().array() += lambda * static_cast<f64>(n);
    const Eigen::LDLT<MatX> ldlt(g);
    if (ldlt.info() != Eigen::Success || !ldlt.isPositive()) {
      continue;
    }
    VecX b = ldlt.solve(zt.transpose() * y.head(n));
    if (b.allFinite()) {
      betas.push_back(std::move(b));
    }
  }
  if (betas.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "FamaMacBethRidge: no identified cross-section in the window");
  }
  MatX bm(static_cast<Eigen::Index>(betas.size()), ke);
  for (usize r = 0U; r < betas.size(); ++r) {
    bm.row(static_cast<Eigen::Index>(r)) = betas[r].transpose();
  }
  CombineWeights out;
  const VecX mean = bm.colwise().mean().transpose();
  out.w.assign(mean.data(), mean.data() + mean.size());
  out.tstat = column_tstats(bm);
  out.fit_begin = w.begin;
  out.fit_end = w.end;
  return atx::core::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
//  Kakushadze-Yu "Billion Alphas"
// ---------------------------------------------------------------------------
atx::core::Result<std::vector<f64>> kakushadze_weights(const MatX &r, std::span<const f64> expected,
                                                       std::span<const atx::u32> clusters,
                                                       f64 ridge_rel) {
  const Eigen::Index m = r.rows();
  const Eigen::Index n = r.cols();
  if (m < 3 || n < 1 || static_cast<usize>(n) != expected.size() ||
      (!clusters.empty() && clusters.size() != static_cast<usize>(n)) || !(ridge_rel >= 0.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "kakushadze_weights: need M>=3, |E|==N, |clusters| in {0,N}, ridge>=0");
  }
  if (!r.allFinite()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "kakushadze_weights: non-finite alpha returns");
  }
  // (ii)-(iii) serial demean + σ (sample, M−1).
  const Eigen::RowVectorXd mu = r.colwise().mean();
  const VecX sigma = ((r.rowwise() - mu).array().square().colwise().sum() / static_cast<f64>(m - 1))
                         .sqrt()
                         .transpose();
  std::vector<Eigen::Index> active;
  active.reserve(static_cast<usize>(n));
  for (Eigen::Index a = 0; a < n; ++a) {
    if (sigma[a] > 0.0 && std::isfinite(expected[static_cast<usize>(a)])) {
      active.push_back(a);
    }
  }
  std::vector<f64> w(static_cast<usize>(n), 0.0);
  const auto na = static_cast<Eigen::Index>(active.size());
  if (na == 0) {
    return atx::core::Ok(std::move(w));
  }
  // (iv)-(vi) Y = X/σ, keep M−1 rows, cross-sectionally (or within-cluster) demean.
  // L is laid out alphas × (M−1) (the regression design matrix).
  const Eigen::Index cols = m - 1;
  MatX l(na, cols);
  VecX et(na);
  std::vector<atx::u32> act_clusters;
  if (!clusters.empty()) {
    act_clusters.reserve(active.size());
  }
  for (Eigen::Index j = 0; j < na; ++j) {
    const Eigen::Index a = active[static_cast<usize>(j)];
    const f64 inv = 1.0 / sigma[a];
    for (Eigen::Index sidx = 0; sidx < cols; ++sidx) {
      l(j, sidx) = (r(sidx, a) - mu[a]) * inv;
    }
    et[j] = expected[static_cast<usize>(a)] * inv; // (viii) Ẽ = E/σ
    if (!clusters.empty()) {
      act_clusters.push_back(clusters[static_cast<usize>(a)]);
    }
  }
  usize n_groups = 1U;
  const std::vector<usize> group = compact_groups(act_clusters, static_cast<usize>(na), n_groups);
  {
    MatX lt = l.transpose(); // (M−1) × Na: demean each date row across alphas
    demean_rows_within(lt, group, n_groups);
    l = lt.transpose();
  }
  if (!clusters.empty()) {
    MatX e_row = et.transpose();
    demean_rows_within(e_row, group, n_groups); // cluster dummies (Frisch-Waugh)
    et = e_row.transpose();
  }
  // (ix) residual of Ẽ on L (no intercept): ε = ρ (L Lᵀ + ρ I)⁻¹ Ẽ, evaluated in the
  // smaller dimension (push-through identity when Na > M−1).
  VecX eps;
  if (na > cols) {
    MatX g = MatX::Zero(cols, cols);
    g.selfadjointView<Eigen::Lower>().rankUpdate(l.transpose());
    g.triangularView<Eigen::StrictlyUpper>() = g.transpose();
    const f64 rho = ridge_rel * g.trace() / static_cast<f64>(cols);
    if (rho > 0.0) {
      g.diagonal().array() += rho;
    }
    const Eigen::LDLT<MatX> ldlt(g);
    const VecX beta = ldlt.solve(l.transpose() * et);
    eps = et - l * beta;
  } else {
    MatX g = l * l.transpose();
    const f64 rho = ridge_rel * g.trace() / static_cast<f64>(na);
    if (!(rho > 0.0)) {
      eps = et; // degenerate (no dispersion or ridge 0 with N <= M−1): nothing to project
    } else {
      g.diagonal().array() += rho;
      const Eigen::LDLT<MatX> ldlt(g);
      eps = rho * ldlt.solve(et);
    }
  }
  // (x) w = ε/σ, Σ|w| = 1.
  for (Eigen::Index j = 0; j < na; ++j) {
    const Eigen::Index a = active[static_cast<usize>(j)];
    w[static_cast<usize>(a)] = eps[j] / sigma[a];
  }
  for (const f64 x : w) {
    if (!std::isfinite(x)) {
      return atx::core::Err(atx::core::ErrorCode::Internal, "kakushadze_weights: non-finite weight");
    }
  }
  normalize_gross(w);
  return atx::core::Ok(std::move(w));
}

atx::core::Result<CombineWeights> KakushadzeRegression::fit(const SignalStore &s, FitWindow w) const {
  ATX_TRY_VOID(validate_window(s, w, 3U));
  const usize k = s.n_alphas();
  if (!clusters.empty() && clusters.size() != k) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "KakushadzeRegression: clusters.size() != n_alphas");
  }
  const std::vector<f64> flat = alpha_return_matrix(s, w);
  const MatX r = complete_rows(flat, w.size(), k);
  if (r.rows() < 3) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "KakushadzeRegression: < 3 complete alpha-return rows");
  }
  const Eigen::Index tail =
      (expected_window == 0U) ? r.rows()
                              : std::min<Eigen::Index>(r.rows(), static_cast<Eigen::Index>(expected_window));
  const VecX e = r.bottomRows(tail).colwise().mean().transpose();
  ATX_TRY(std::vector<f64> wv, kakushadze_weights(r, std::span<const f64>(e.data(), k), clusters,
                                                   ridge_rel));
  CombineWeights out;
  out.w = std::move(wv);
  out.tstat = column_tstats(r);
  out.fit_begin = w.begin;
  out.fit_end = w.end;
  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::combine

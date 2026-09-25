// atx::engine::combine — orthogonalization bodies (Lane 5). Contracts live in
// combine/orthogonalize.hpp.

#include "atx/engine/combine/orthogonalize.hpp"

#include <algorithm> // std::fill
#include <cmath>   // std::isfinite, std::sqrt
#include <utility> // std::move
#include <vector>  // std::vector

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/signal_store.hpp" // cross_section_corr, kSignalNaN
#include "atx/engine/eval/hac.hpp"             // W0-E0a: HAC t-statistic (E-03)

namespace atx::engine::combine {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

namespace {

[[nodiscard]] bool panel_ok(const PanelView &p, usize t, usize n) noexcept {
  return p.n_dates == t && p.n_instruments == n && p.data.size() == t * n;
}

// Weighted LS residual y − Xβ̂ with β̂ the min-norm solution of ‖√w (y − Xβ)‖. Returns
// false when the regression is not identified (n <= rank X).
[[nodiscard]] bool wls_residual(const MatX &x, const VecX &y, const VecX &sw, VecX &res) {
  if (x.cols() == 0) {
    res = y;
    return y.size() > 0;
  }
  const MatX xw = sw.asDiagonal() * x;
  const VecX yw = sw.asDiagonal() * y;
  const Eigen::CompleteOrthogonalDecomposition<MatX> cod(xw);
  if (y.size() <= cod.rank()) {
    return false;
  }
  const VecX beta = cod.solve(yw);
  res = y - x * beta;
  return res.allFinite();
}

} // namespace

atx::core::Result<std::vector<f64>> residualize_signal(PanelView s, ExposureView b,
                                                       std::span<const f64> spec_var,
                                                       std::span<const PanelView> pool) {
  const usize t_n = s.n_dates;
  const usize n = s.n_instruments;
  if (!panel_ok(s, t_n, n) || t_n == 0U || n == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "residualize_signal: bad signal");
  }
  const usize k = b.n_factors;
  if (k > 0U && ((b.n_dates != 1U && b.n_dates != t_n) || b.n_instruments != n ||
                 b.data.size() != b.n_dates * n * k)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "residualize_signal: exposure shape mismatch");
  }
  if (!spec_var.empty() && spec_var.size() != n && spec_var.size() != t_n * n) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "residualize_signal: spec_var must be empty, N or T*N");
  }
  for (const PanelView &p : pool) {
    if (!panel_ok(p, t_n, n)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "residualize_signal: pool panel shape mismatch");
    }
  }
  const usize p_cols = k + pool.size();
  std::vector<f64> out(t_n * n, kSignalNaN);
  std::vector<usize> rows;
  rows.reserve(n);
  for (usize t = 0U; t < t_n; ++t) {
    const usize bt = (b.n_dates == 1U) ? 0U : t;
    rows.clear();
    for (usize i = 0U; i < n; ++i) {
      bool ok = std::isfinite(s.data[t * n + i]);
      if (ok && !spec_var.empty()) {
        const f64 v = spec_var[(spec_var.size() == n) ? i : t * n + i];
        ok = std::isfinite(v) && v > 0.0;
      }
      for (usize f = 0U; f < k && ok; ++f) {
        ok = std::isfinite(b.data[(bt * n + i) * k + f]);
      }
      for (usize j = 0U; j < pool.size() && ok; ++j) {
        ok = std::isfinite(pool[j].data[t * n + i]);
      }
      if (ok) {
        rows.push_back(i);
      }
    }
    const auto m = static_cast<Eigen::Index>(rows.size());
    MatX x(m, static_cast<Eigen::Index>(p_cols));
    VecX y(m);
    VecX sw(m);
    for (Eigen::Index r = 0; r < m; ++r) {
      const usize i = rows[static_cast<usize>(r)];
      y[r] = s.data[t * n + i];
      sw[r] = spec_var.empty() ? 1.0
                               : 1.0 / std::sqrt(spec_var[(spec_var.size() == n) ? i : t * n + i]);
      for (usize f = 0U; f < k; ++f) {
        x(r, static_cast<Eigen::Index>(f)) = b.data[(bt * n + i) * k + f];
      }
      for (usize j = 0U; j < pool.size(); ++j) {
        x(r, static_cast<Eigen::Index>(k + j)) = pool[j].data[t * n + i];
      }
    }
    VecX res;
    if (m == 0 || !wls_residual(x, y, sw, res)) {
      continue; // unidentified date → row stays NaN
    }
    for (Eigen::Index r = 0; r < m; ++r) {
      out[t * n + rows[static_cast<usize>(r)]] = res[r];
    }
  }
  return atx::core::Ok(std::move(out));
}

atx::core::Result<std::vector<std::vector<f64>>>
lowdin_orthogonalize(std::span<const PanelView> panels) {
  if (panels.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "lowdin_orthogonalize: no panels");
  }
  const usize t_n = panels[0].n_dates;
  const usize n = panels[0].n_instruments;
  for (const PanelView &p : panels) {
    if (!panel_ok(p, t_n, n)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "lowdin_orthogonalize: panel shape mismatch");
    }
  }
  const usize k = panels.size();
  const usize cells = t_n * n;
  std::vector<unsigned char> valid(cells, 1U);
  usize n_valid = 0U;
  for (usize c = 0U; c < cells; ++c) {
    for (usize j = 0U; j < k && valid[c] != 0U; ++j) {
      valid[c] = std::isfinite(panels[j].data[c]) ? 1U : 0U;
    }
    n_valid += valid[c];
  }
  if (n_valid < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "lowdin_orthogonalize: < 2 complete cells");
  }
  const auto ke = static_cast<Eigen::Index>(k);
  MatX g = MatX::Zero(ke, ke);
  VecX v(ke);
  for (usize c = 0U; c < cells; ++c) {
    if (valid[c] == 0U) {
      continue;
    }
    for (usize j = 0U; j < k; ++j) {
      v[static_cast<Eigen::Index>(j)] = panels[j].data[c];
    }
    g.selfadjointView<Eigen::Lower>().rankUpdate(v);
  }
  g.triangularView<Eigen::StrictlyUpper>() = g.transpose();
  g /= static_cast<f64>(n_valid);
  const Eigen::SelfAdjointEigenSolver<MatX> es(g);
  if (es.info() != Eigen::Success) {
    return atx::core::Err(atx::core::ErrorCode::Internal, "lowdin_orthogonalize: eig failed");
  }
  const VecX &lam = es.eigenvalues();
  if (!(lam[0] > 1e-12 * lam[ke - 1])) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "lowdin_orthogonalize: signals are (numerically) collinear");
  }
  const MatX &u = es.eigenvectors();
  const MatX inv_sqrt = u * lam.array().rsqrt().matrix().asDiagonal() * u.transpose();
  std::vector<std::vector<f64>> out(k, std::vector<f64>(cells, kSignalNaN));
  for (usize c = 0U; c < cells; ++c) {
    if (valid[c] == 0U) {
      continue;
    }
    for (usize col = 0U; col < k; ++col) {
      f64 acc = 0.0;
      for (usize j = 0U; j < k; ++j) {
        acc += panels[j].data[c] *
               inv_sqrt(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(col));
      }
      out[col][c] = acc;
    }
  }
  return atx::core::Ok(std::move(out));
}

atx::core::Result<MarginalIc> marginal_ic(PanelView cand, std::span<const PanelView> pool,
                                          PanelView fwd, usize begin, usize end,
                                          eval::hac::TStatRule tstat_rule, usize label_horizon) {
  using eval::hac::TStatRule;
  if (label_horizon == 0U ||
      (tstat_rule != TStatRule::IidV1 && tstat_rule != TStatRule::NeweyWestAutoV2 &&
       tstat_rule != TStatRule::HorizonAwareV3)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "marginal_ic: invalid inference rule or label horizon");
  }
  const usize t_n = cand.n_dates;
  const usize n = cand.n_instruments;
  if (end == 0U) {
    end = t_n;
  }
  if (!panel_ok(cand, t_n, n) || !panel_ok(fwd, t_n, n) || begin >= end || end > t_n) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "marginal_ic: bad shape/range");
  }
  for (const PanelView &p : pool) {
    if (!panel_ok(p, t_n, n)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "marginal_ic: pool panel shape mismatch");
    }
  }
  const usize p_cols = pool.size() + 1U; // intercept + pool
  std::vector<f64> ics;
  ics.reserve(end - begin);
  std::vector<usize> rows;
  std::vector<f64> resid(n);
  for (usize t = begin; t < end; ++t) {
    rows.clear();
    for (usize i = 0U; i < n; ++i) {
      bool ok = std::isfinite(cand.data[t * n + i]) && std::isfinite(fwd.data[t * n + i]);
      for (usize j = 0U; j < pool.size() && ok; ++j) {
        ok = std::isfinite(pool[j].data[t * n + i]);
      }
      if (ok) {
        rows.push_back(i);
      }
    }
    const auto m = static_cast<Eigen::Index>(rows.size());
    MatX x(m, static_cast<Eigen::Index>(p_cols));
    VecX y(m);
    for (Eigen::Index r = 0; r < m; ++r) {
      const usize i = rows[static_cast<usize>(r)];
      y[r] = cand.data[t * n + i];
      x(r, 0) = 1.0;
      for (usize j = 0U; j < pool.size(); ++j) {
        x(r, static_cast<Eigen::Index>(j + 1U)) = pool[j].data[t * n + i];
      }
    }
    VecX res;
    if (m < 3 || !wls_residual(x, y, VecX::Ones(m), res)) {
      continue;
    }
    // Fully spanned by the pool: the residual is round-off, its IC would be noise.
    const f64 y_dev = (y.array() - y.mean()).matrix().norm();
    if (!(res.norm() > 1e-10 * y_dev)) {
      continue;
    }
    std::fill(resid.begin(), resid.end(), kSignalNaN);
    for (Eigen::Index r = 0; r < m; ++r) {
      resid[rows[static_cast<usize>(r)]] = res[r];
    }
    const f64 ic = cross_section_corr(resid, fwd.data.subspan(t * n, n));
    if (std::isfinite(ic)) {
      ics.push_back(ic);
    }
  }
  MarginalIc out;
  out.n_dates = ics.size();
  if (ics.empty()) {
    return atx::core::Ok(out);
  }
  f64 sum = 0.0;
  for (const f64 x : ics) {
    sum += x;
  }
  out.mean_ic = sum / static_cast<f64>(ics.size());
  // W0-E0a / E-03: the marginal-IC series inherits the MA(h-1) overlap of the forward
  // return, so its t-stat is HAC (Newey-West at the NW-1994 automatic lag) by default.
  // TStatRule::IidV1 reproduces the pre-W0 mean / (sd / sqrt(n)) term for term. Lags run
  // over consecutive USABLE dates (skipped dates are dropped, not NaN-filled). The
  // contract is unchanged: 0 when n < 2 or the (long-run) variance is zero.
  const eval::hac::MeanInference mi = eval::hac::mean_tstat(ics, tstat_rule, label_horizon);
  out.tstat = (mi.defined != 0U) ? mi.t : 0.0;
  return atx::core::Ok(out);
}

} // namespace atx::engine::combine

// atx::engine::risk — L7 hybrid fundamental + statistical factor model (bodies).
//
// See hybrid_factor_model.hpp for the model, the PIT convention and determinism.
#include "atx/engine/risk/hybrid_factor_model.hpp"

#include <algorithm> // std::sort, std::unique, std::lower_bound, std::min
#include <cmath>     // std::isfinite, std::isnan, std::sqrt, std::pow
#include <limits>    // std::numeric_limits
#include <utility>   // std::move

#include <Eigen/Dense> // Eigen::LLT

#include "atx/core/macro.hpp" // ATX_TRY

#include "atx/core/linalg/decompose.hpp" // symmetric_eig

#include "atx/engine/risk/cov_ewma.hpp"          // ewma_factor_covariance
#include "atx/engine/risk/shrinkage.hpp"         // shrunk_factor_covariance
#include "atx/engine/risk/stat_factor_model.hpp" // APCA kernels, bai_ng_ic2, mp_edge_count

namespace atx::engine::risk {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
// A date's reduced normal matrix is treated as singular below this reciprocal
// condition estimate (a collinear style block, or an industry with one name that is
// also the only name of another column's support).
constexpr atx::f64 kRcondFloor = 1e-12;
constexpr atx::f64 kSpecFloor = 1e-12; // specific-variance floor (matches FactorModel)

// Column offsets of the full design [market][industries][styles].
struct Layout {
  atx::usize oi; // first industry column
  atx::usize os; // first style column
  atx::usize g;  // industries
  atx::usize ks; // styles
  atx::usize k;  // total
  bool market;
};

Layout layout_of(const ExposureSeries &e) noexcept {
  const atx::usize m = e.n_market();
  return Layout{m, m + e.n_ind(), e.n_ind(), e.n_style(), e.n_factors(), e.market};
}

bool has_caps(const ExposureSeries &e) noexcept {
  return !e.cap_series.empty() || !e.cap.empty();
}

// Cap of asset i at exposure row `row` (cap_series wins; the static `cap` otherwise).
// PRECONDITION: has_caps(e).
atx::f64 cap_at(const ExposureSeries &e, atx::usize i, atx::usize row) noexcept {
  if (!e.cap_series.empty()) {
    return (e.cap_series.size() == 1U ? e.cap_series[0] : e.cap_series[row])[i];
  }
  return e.cap[i];
}

// √cap regression weight (1 without caps); 0 ⇒ the asset is excluded. `row` is the
// EXPOSURE row the weight is dated at (t+1 for return row t; as_of for the model).
atx::f64 reg_weight(const ExposureSeries &e, atx::usize i, atx::usize row) noexcept {
  if (!has_caps(e)) {
    return 1.0;
  }
  const atx::f64 c = cap_at(e, i, row);
  return (std::isfinite(c) && c > 0.0) ? std::sqrt(c) : 0.0;
}

// Cap weight of the industry sum-to-zero constraint (1 ⇒ name count without caps).
atx::f64 cons_weight(const ExposureSeries &e, atx::usize i, atx::usize row) noexcept {
  return has_caps(e) ? cap_at(e, i, row) : 1.0;
}

// An asset enters a date's design iff its style row is finite, it has an industry
// (when industries are modelled) and a positive weight.
bool asset_valid(const ExposureSeries &e, const Layout &lay, const MatX &xs, atx::usize i,
                 atx::usize xrow) noexcept {
  if (lay.g > 0U && e.industry[i] >= lay.g) {
    return false;
  }
  if (!(reg_weight(e, i, xrow) > 0.0)) {
    return false;
  }
  const Eigen::Index row = static_cast<Eigen::Index>(i);
  for (Eigen::Index c = 0; c < xs.cols(); ++c) {
    if (!std::isfinite(xs(row, c))) {
      return false;
    }
  }
  return true;
}

// Scratch reused across dates (the per-date solve allocates nothing after warm-up).
struct Workspace {
  MatX a;                         // K×K upper-triangular accumulation
  VecX b;                         // K
  std::vector<atx::f64> cw;       // per-industry constraint weight
  std::vector<atx::usize> cnt;    // per-industry valid-name count
  std::vector<atx::usize> valid;  // valid asset list
  std::vector<atx::usize> free_;  // free full-coordinate index list
  std::vector<atx::f64> alpha;    // per free coord: coefficient in f_ref (0 for non-industry)
  std::vector<atx::u8> ok;        // per-asset validity flag
  std::vector<atx::usize> gv;     // industry of each valid asset
  VecX wv;                        // regression weight of each valid asset
  VecX rv;                        // return of each valid asset
  MatX xv;                        // valid style rows (nv×Ks)
  MatX xw;                        // W·xv
  VecX fit;                       // style fit of each valid asset
  MatX af;                        // reduced normal matrix
  VecX bf;
};

// The free coordinates of one date (or of the model at as_of): every non-industry
// column, every industry with a positive count, minus the reference industry when the
// market + industry constraint is active. Returns the reference industry (full index)
// or K when unconstrained. `alpha[p]` = −c_P/c_ref for a free industry coordinate.
atx::usize free_coords(const Layout &lay, const std::vector<atx::f64> &cw,
                       const std::vector<atx::usize> &cnt, std::vector<atx::usize> &free_,
                       std::vector<atx::f64> &alpha) {
  free_.clear();
  alpha.clear();
  atx::usize ref = lay.k;
  if (lay.market && lay.g > 0U) {
    atx::f64 best = 0.0;
    for (atx::usize g = 0U; g < lay.g; ++g) {
      if (cnt[g] > 0U && cw[g] > best) {
        best = cw[g];
        ref = lay.oi + g;
      }
    }
  }
  const atx::f64 c_ref = (ref < lay.k) ? cw[ref - lay.oi] : 1.0;
  for (atx::usize p = 0U; p < lay.k; ++p) {
    const bool is_ind = (p >= lay.oi && p < lay.os);
    if (p == ref || (is_ind && cnt[p - lay.oi] == 0U)) {
      continue;
    }
    free_.push_back(p);
    alpha.push_back((is_ind && ref < lay.k) ? -cw[p - lay.oi] / c_ref : 0.0);
  }
  return ref;
}

// One date's structured WLS: return row t on exposure row t+1 (styles `xs` and the
// caps of that row). Writes f (full coordinates, length K) and the residual row;
// returns false when the date is skipped.
bool solve_date(const ExposureSeries &e, const Layout &lay, const MatX &xs, const MatX &rr,
                Eigen::Index t, Workspace &ws, Eigen::Ref<VecX> f, Eigen::Ref<VecX> resid,
                atx::f64 &r2) {
  const atx::usize xrow = static_cast<atx::usize>(t) + 1U; // exposure / cap row
  const atx::usize n = static_cast<atx::usize>(rr.cols());
  const Eigen::Index ks = static_cast<Eigen::Index>(lay.ks);
  // Validity by contiguous column sweeps (the style block is column-major N×Ks).
  ws.ok.assign(n, 0U);
  for (atx::usize i = 0U; i < n; ++i) {
    const bool ind_ok = lay.g == 0U || e.industry[i] < lay.g;
    ws.ok[i] = (std::isfinite(rr(t, static_cast<Eigen::Index>(i))) && ind_ok &&
                reg_weight(e, i, xrow) > 0.0)
                   ? 1U
                   : 0U;
  }
  for (Eigen::Index c = 0; c < ks; ++c) {
    for (atx::usize i = 0U; i < n; ++i) {
      if (ws.ok[i] != 0U && !std::isfinite(xs(static_cast<Eigen::Index>(i), c))) {
        ws.ok[i] = 0U;
      }
    }
  }
  ws.valid.clear();
  for (atx::usize i = 0U; i < n; ++i) {
    if (ws.ok[i] != 0U) {
      ws.valid.push_back(i);
    }
  }
  const Eigen::Index nv = static_cast<Eigen::Index>(ws.valid.size());
  // Gather the valid rows: weights, returns, industries, styles (column by column).
  ws.wv.resize(nv);
  ws.rv.resize(nv);
  ws.gv.resize(ws.valid.size());
  for (Eigen::Index j = 0; j < nv; ++j) {
    const atx::usize i = ws.valid[static_cast<atx::usize>(j)];
    ws.wv[j] = reg_weight(e, i, xrow);
    ws.rv[j] = rr(t, static_cast<Eigen::Index>(i));
    ws.gv[static_cast<atx::usize>(j)] = (lay.g > 0U) ? static_cast<atx::usize>(e.industry[i]) : 0U;
  }
  ws.xv.resize(nv, ks);
  for (Eigen::Index c = 0; c < ks; ++c) {
    for (Eigen::Index j = 0; j < nv; ++j) {
      ws.xv(j, c) = xs(static_cast<Eigen::Index>(ws.valid[static_cast<atx::usize>(j)]), c);
    }
  }
  ws.xw = ws.xv.array().colwise() * ws.wv.array(); // W·X  (nv×Ks)

  const Eigen::Index k = static_cast<Eigen::Index>(lay.k);
  const Eigen::Index os = static_cast<Eigen::Index>(lay.os);
  const Eigen::Index oi = static_cast<Eigen::Index>(lay.oi);
  ws.a.setZero(k, k);
  ws.b.setZero(k);
  std::fill(ws.cw.begin(), ws.cw.end(), 0.0);
  std::fill(ws.cnt.begin(), ws.cnt.end(), 0U);
  // Dense style block XᵀWX / XᵀWr (GEMM / GEMV).
  if (ks > 0) {
    ws.a.block(os, os, ks, ks).noalias() = ws.xw.transpose() * ws.xv;
    ws.b.segment(os, ks).noalias() = ws.xw.transpose() * ws.rv;
  }
  if (lay.market) {
    ws.a(0, 0) = ws.wv.sum();
    ws.b[0] = ws.wv.dot(ws.rv);
    if (ks > 0) {
      ws.a.block(0, os, 1, ks) = ws.xw.colwise().sum();
    }
  }
  // One-hot industry blocks: scattered sums into the small G-row band.
  if (lay.g > 0U) {
    for (Eigen::Index j = 0; j < nv; ++j) {
      const atx::usize g = ws.gv[static_cast<atx::usize>(j)];
      const Eigen::Index gc = oi + static_cast<Eigen::Index>(g);
      const atx::f64 w = ws.wv[j];
      ws.cw[g] += cons_weight(e, ws.valid[static_cast<atx::usize>(j)], xrow);
      ++ws.cnt[g];
      ws.a(gc, gc) += w;
      ws.b[gc] += w * ws.rv[j];
      if (lay.market) {
        ws.a(0, gc) += w;
      }
    }
    for (Eigen::Index c = 0; c < ks; ++c) {
      for (Eigen::Index j = 0; j < nv; ++j) {
        ws.a(oi + static_cast<Eigen::Index>(ws.gv[static_cast<atx::usize>(j)]), os + c) +=
            ws.xw(j, c);
      }
    }
  }
  const atx::usize ref = free_coords(lay, ws.cw, ws.cnt, ws.free_, ws.alpha);
  const atx::usize kf = ws.free_.size();
  if (ws.valid.size() <= kf) {
    return false; // under-determined cross-section
  }
  // Symmetric view of the upper-triangular accumulation.
  auto a_at = [&](atx::usize p, atx::usize q) {
    return (p <= q) ? ws.a(static_cast<Eigen::Index>(p), static_cast<Eigen::Index>(q))
                    : ws.a(static_cast<Eigen::Index>(q), static_cast<Eigen::Index>(p));
  };
  // Reduced system Af = Rᵀ A R, bf = Rᵀ b (R = identity on free coords, the ref row
  // carries alpha).
  const Eigen::Index kfi = static_cast<Eigen::Index>(kf);
  ws.af.resize(kfi, kfi);
  ws.bf.resize(kfi);
  const bool con = ref < lay.k;
  for (atx::usize p = 0U; p < kf; ++p) {
    const atx::usize pp = ws.free_[p];
    const atx::f64 ap = ws.alpha[p];
    ws.bf[static_cast<Eigen::Index>(p)] =
        ws.b[static_cast<Eigen::Index>(pp)] + (con ? ap * ws.b[static_cast<Eigen::Index>(ref)] : 0.0);
    for (atx::usize q = 0U; q < kf; ++q) {
      const atx::usize qq = ws.free_[q];
      const atx::f64 aq = ws.alpha[q];
      atx::f64 v = a_at(pp, qq);
      if (con) {
        v += ap * a_at(ref, qq) + aq * a_at(pp, ref) + ap * aq * a_at(ref, ref);
      }
      ws.af(static_cast<Eigen::Index>(p), static_cast<Eigen::Index>(q)) = v;
    }
  }
  VecX g(kfi);
  if (kf > 0U) {
    const Eigen::LLT<MatX> llt(ws.af);
    if (llt.info() != Eigen::Success || !(llt.rcond() > kRcondFloor)) {
      return false; // singular design on this date
    }
    g = llt.solve(ws.bf);
  }
  f.setZero();
  atx::f64 f_ref = 0.0;
  for (atx::usize p = 0U; p < kf; ++p) {
    f[static_cast<Eigen::Index>(ws.free_[p])] = g[static_cast<Eigen::Index>(p)];
    f_ref += ws.alpha[p] * g[static_cast<Eigen::Index>(p)];
  }
  if (con) {
    f[static_cast<Eigen::Index>(ref)] = f_ref;
  }
  // Residuals + weighted R² (style fit as one GEMV).
  resid.setConstant(kNaN);
  ws.fit.resize(nv);
  if (ks > 0) {
    ws.fit.noalias() = ws.xv * f.segment(os, ks);
  } else {
    ws.fit.setZero();
  }
  const atx::f64 sw = ws.wv.sum();
  const atx::f64 rbar = ws.wv.dot(ws.rv) / sw;
  atx::f64 ssr = 0.0;
  atx::f64 sst = 0.0;
  for (Eigen::Index j = 0; j < nv; ++j) {
    atx::f64 fit = ws.fit[j] + (lay.market ? f[0] : 0.0);
    if (lay.g > 0U) {
      fit += f[oi + static_cast<Eigen::Index>(ws.gv[static_cast<atx::usize>(j)])];
    }
    const atx::f64 r = ws.rv[j];
    const atx::f64 u = r - fit;
    resid[static_cast<Eigen::Index>(ws.valid[static_cast<atx::usize>(j)])] = u;
    const atx::f64 w = ws.wv[j];
    ssr += w * u * u;
    sst += w * (r - rbar) * (r - rbar);
  }
  r2 = (sst > 0.0) ? 1.0 - ssr / sst : kNaN;
  return true;
}

// Factor covariance of a (dates × factors, newest-first) series per the config.
atx::core::Result<MatX> factor_cov(const MatX &series, const HybridCfg &cfg) {
  if (cfg.vol_halflife > 0U || cfg.corr_halflife > 0U || cfg.nw_lags > 0U) {
    return atx::core::Ok(
        ewma_factor_covariance(series, cfg.vol_halflife, cfg.corr_halflife, cfg.nw_lags));
  }
  if (cfg.factor_cov_target.has_value()) {
    return shrunk_factor_covariance(series, *cfg.factor_cov_target);
  }
  return atx::core::Ok(detail::factor_covariance(series, cfg.factor_cov_shrink));
}

// (Weighted) variance over the observed entries of one residual row; `hl` > 0 ⇒
// EWMA weights 0.5^(j/hl) by local date index j (0 = newest). Floored.
atx::f64 spec_var(const MatX &e, Eigen::Index row, atx::usize hl) {
  const atx::f64 decay = (hl > 0U) ? std::pow(0.5, 1.0 / static_cast<atx::f64>(hl)) : 1.0;
  atx::f64 sw = 0.0;
  atx::f64 swx = 0.0;
  atx::f64 wj = 1.0;
  for (Eigen::Index j = 0; j < e.cols(); ++j, wj *= decay) {
    const atx::f64 v = e(row, j);
    if (!std::isnan(v)) {
      sw += wj;
      swx += wj * v;
    }
  }
  if (!(sw > 0.0)) {
    return kSpecFloor;
  }
  const atx::f64 mean = swx / sw;
  atx::f64 ss = 0.0;
  wj = 1.0;
  for (Eigen::Index j = 0; j < e.cols(); ++j, wj *= decay) {
    const atx::f64 v = e(row, j);
    if (!std::isnan(v)) {
      ss += wj * (v - mean) * (v - mean);
    }
  }
  const atx::f64 var = ss / sw;
  return var < kSpecFloor ? kSpecFloor : var;
}

// Eigendecomposition of an (exactly symmetrized) Gram.
atx::core::Result<atx::core::linalg::EigResult> sym_eig(const MatX &gram) {
  const MatX sym = 0.5 * (gram + gram.transpose()); // exact symmetry for the eigensolver
  return atx::core::linalg::symmetric_eig(sym);
}

// Chosen K_s for the APCA panel `ue` (Ne × T, NaN-filled with 0, row-demeaned).
// `gram_eig` receives the eigensystem of UᵀU when the rule computed it (Bai-Ng): it is
// the APCA pass-1 Gram up to the 1/N scale, so the stat block reuses its eigenvectors.
atx::core::Result<StatSelection> select_k(const MatX &ue, const HybridCfg &cfg,
                                          atx::core::linalg::EigResult &gram_eig) {
  StatSelection sel;
  const atx::usize ne = static_cast<atx::usize>(ue.rows());
  const atx::usize t = static_cast<atx::usize>(ue.cols());
  const atx::usize k_cap = std::min(ne, t) > 0U ? std::min(ne, t) - 1U : 0U;
  switch (cfg.select) {
  case StatFactorSelect::Fixed:
    sel.k = std::min(cfg.n_stat_fixed, k_cap);
    return atx::core::Ok(std::move(sel));
  case StatFactorSelect::BaiNgIc2: {
    ATX_TRY(atx::core::linalg::EigResult eig, sym_eig(MatX(ue.transpose() * ue))); // UᵀU
    VecX ev = eig.values.reverse();
    sel.k = std::min(detail::bai_ng_ic2(ev, ne, t, cfg.k_max), k_cap);
    sel.eigenvalues = std::move(ev);
    gram_eig = std::move(eig);
    return atx::core::Ok(std::move(sel));
  }
  case StatFactorSelect::MarchenkoPastur: {
    MatX z = ue;
    for (Eigen::Index r = 0; r < z.rows(); ++r) {
      const atx::f64 sd = std::sqrt(z.row(r).squaredNorm() / static_cast<atx::f64>(t));
      if (sd > 0.0) {
        z.row(r) /= sd;
      } else {
        z.row(r).setZero();
      }
    }
    ATX_TRY(atx::core::linalg::EigResult eig,
            sym_eig(MatX((z.transpose() * z) / static_cast<atx::f64>(t))));
    VecX ev = eig.values.reverse();
    const detail::MpEdgeResult mp = detail::mp_edge_count(ev, ev.sum(), ne, t, cfg.k_max);
    sel.k = std::min(mp.k, k_cap);
    sel.mp_edge = mp.edge;
    sel.eigenvalues = std::move(ev);
    return atx::core::Ok(std::move(sel));
  }
  }
  return atx::core::Ok(std::move(sel)); // unreachable (switch exhaustive)
}

// Statistical block output.
struct StatBlock {
  MatX b;     // M×k loadings
  MatX fhat;  // T×k factor returns (newest first)
  MatX resid; // M×T residual after the block (NaN where unobserved)
};

// APCA on the model's residual panel `u` (M × T, NaN = unobserved).
atx::core::Result<StatBlock> stat_block(const MatX &u, atx::usize k, const HybridCfg &cfg,
                                        const std::vector<atx::usize> &panel_rows,
                                        const atx::core::linalg::EigResult &gram_eig) {
  const Eigen::Index t = u.cols();
  MatX filled = u.unaryExpr([](atx::f64 v) { return std::isnan(v) ? 0.0 : v; });
  MatX ue(static_cast<Eigen::Index>(panel_rows.size()), t);
  for (atx::usize j = 0U; j < panel_rows.size(); ++j) {
    ue.row(static_cast<Eigen::Index>(j)) = filled.row(static_cast<Eigen::Index>(panel_rows[j]));
  }
  detail::demean_rows(ue);
  MatX fhat;
  if (gram_eig.values.size() == t) {
    fhat = detail::top_k_factors(gram_eig, k); // pass 1 from the selection's eigensystem
  } else {
    ATX_TRY(MatX f1, detail::apca_factor_returns(ue, k));
    fhat = std::move(f1);
  }
  if (cfg.gls_reweight) {
    ATX_TRY(MatX be, detail::exposures(ue, fhat));
    const VecX s = detail::specific_variances(ue, be, fhat);
    ATX_TRY(MatX fg, detail::apca_factor_returns(detail::gls_reweight(ue, s), k));
    fhat = std::move(fg);
  }
  ATX_TRY(MatX b, detail::exposures(filled, fhat)); // loadings for EVERY model asset
  MatX resid = u - b * fhat.transpose();            // NaN entries stay NaN
  return atx::core::Ok(StatBlock{std::move(b), std::move(fhat), std::move(resid)});
}

} // namespace

atx::core::Result<FactorReturnSeries> estimate_factor_returns(const ReturnPanel &ret,
                                                              const ExposureSeries &exp,
                                                              atx::usize as_of,
                                                              atx::usize window) {
  const atx::usize n = ret.n_assets();
  if (window == 0U || as_of + window > ret.n_dates()) {
    return Err(ErrorCode::InvalidArgument,
               "estimate_factor_returns: window must be > 0 and within the return panel");
  }
  if (exp.style.size() > 1U && exp.style.size() < as_of + window + 1U) {
    return Err(ErrorCode::InvalidArgument,
               "estimate_factor_returns: exposure series must cover row as_of + window");
  }
  for (const MatX &s : exp.style) {
    if (static_cast<atx::usize>(s.rows()) != n ||
        static_cast<atx::usize>(s.cols()) != exp.n_style()) {
      return Err(ErrorCode::InvalidArgument,
                 "estimate_factor_returns: every style matrix must be N×Ks");
    }
  }
  if ((!exp.industry.empty() && exp.industry.size() != n) ||
      (!exp.cap.empty() && exp.cap.size() != n)) {
    return Err(ErrorCode::InvalidArgument,
               "estimate_factor_returns: industry / cap length must equal the asset count");
  }
  if (exp.cap_series.size() > 1U && exp.cap_series.size() < as_of + window + 1U) {
    return Err(ErrorCode::InvalidArgument,
               "estimate_factor_returns: cap series must cover row as_of + window");
  }
  for (const std::vector<atx::f64> &c : exp.cap_series) {
    if (c.size() != n) {
      return Err(ErrorCode::InvalidArgument,
                 "estimate_factor_returns: every cap_series row must have N entries");
    }
  }
  const Layout lay = layout_of(exp);
  if (lay.k == 0U) {
    return Err(ErrorCode::InvalidArgument, "estimate_factor_returns: no factor columns");
  }
  const MatX empty_style(static_cast<Eigen::Index>(n), 0);
  FactorReturnSeries out;
  out.as_of = as_of;
  out.f.setConstant(static_cast<Eigen::Index>(window), static_cast<Eigen::Index>(lay.k), kNaN);
  out.resid.setConstant(static_cast<Eigen::Index>(window), static_cast<Eigen::Index>(n), kNaN);
  out.used.assign(window, 0U);
  out.r2.assign(window, kNaN);
  Workspace ws;
  ws.cw.assign(lay.g, 0.0);
  ws.cnt.assign(lay.g, 0U);
  ws.valid.reserve(n);
  VecX f(static_cast<Eigen::Index>(lay.k));
  VecX resid(static_cast<Eigen::Index>(n));
  for (atx::usize j = 0U; j < window; ++j) {
    const atx::usize t = as_of + j;
    const MatX &xs = exp.style.empty() ? empty_style : exp.at(t + 1U);
    atx::f64 r2 = kNaN;
    if (!solve_date(exp, lay, xs, ret.r, static_cast<Eigen::Index>(t), ws, f, resid, r2)) {
      continue;
    }
    out.f.row(static_cast<Eigen::Index>(j)) = f.transpose();
    out.resid.row(static_cast<Eigen::Index>(j)) = resid.transpose();
    out.used[j] = 1U;
    out.r2[j] = r2;
    ++out.n_used;
  }
  return atx::core::Ok(std::move(out));
}

atx::core::Result<HybridModel> HybridFactorModelBuilder::build(const ReturnPanel &ret,
                                                               const ExposureSeries &exp,
                                                               const HybridCfg &cfg,
                                                               atx::usize as_of) {
  if (cfg.window < 2U) {
    return Err(ErrorCode::InvalidArgument, "HybridFactorModelBuilder: window must be >= 2");
  }
  ATX_TRY(FactorReturnSeries fr, estimate_factor_returns(ret, exp, as_of, cfg.window));
  const Layout lay = layout_of(exp);
  if (fr.n_used < 2U) {
    return Err(ErrorCode::InvalidArgument, "HybridFactorModelBuilder: fewer than 2 usable dates");
  }
  const atx::usize n = ret.n_assets();
  const Eigen::Index tu = static_cast<Eigen::Index>(fr.n_used);

  // Model assets: valid at as_of with enough residual history.
  const MatX empty_style(static_cast<Eigen::Index>(n), 0);
  const MatX &x0 = exp.style.empty() ? empty_style : exp.at(as_of);
  std::vector<atx::usize> assets;
  for (atx::usize i = 0U; i < n; ++i) {
    atx::usize obs = 0U;
    for (atx::usize j = 0U; j < cfg.window; ++j) {
      obs += std::isnan(fr.resid(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(i))) ? 0U
                                                                                             : 1U;
    }
    if (obs >= cfg.min_spec_obs && obs >= 2U && asset_valid(exp, lay, x0, i, as_of)) {
      assets.push_back(i);
    }
  }
  if (assets.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "HybridFactorModelBuilder: no asset has enough residual observations");
  }
  const Eigen::Index m = static_cast<Eigen::Index>(assets.size());

  // Free coordinates at as_of (the reference industry is eliminated from F).
  std::vector<atx::f64> cw(lay.g, 0.0);
  std::vector<atx::usize> cnt(lay.g, 1U); // keep every industry coordinate in the model
  for (const atx::usize i : assets) {
    if (lay.g > 0U) {
      cw[exp.industry[i]] += cons_weight(exp, i, as_of);
    }
  }
  std::vector<atx::usize> free_;
  std::vector<atx::f64> alpha;
  const atx::usize ref = free_coords(lay, cw, cnt, free_, alpha);
  const bool con = ref < lay.k;
  const atx::usize kf = free_.size();
  if (fr.n_used < kf) {
    return Err(ErrorCode::InvalidArgument,
               "HybridFactorModelBuilder: fewer usable dates than free fundamental factors");
  }

  // X_fund·R (M×Kf): column p = X(:,P) + alpha_p·X(:,ref).
  MatX xf(m, static_cast<Eigen::Index>(kf));
  for (Eigen::Index r = 0; r < m; ++r) {
    const atx::usize i = assets[static_cast<atx::usize>(r)];
    const atx::u32 gi = (lay.g > 0U) ? exp.industry[i] : kNoIndustry;
    const bool in_ref = con && (lay.oi + gi == ref);
    for (atx::usize p = 0U; p < kf; ++p) {
      const atx::usize pp = free_[p];
      atx::f64 v = 0.0;
      if (lay.market && pp == 0U) {
        v = 1.0;
      } else if (pp >= lay.oi && pp < lay.os) {
        v = (lay.oi + gi == pp) ? 1.0 : 0.0;
      } else if (pp >= lay.os) {
        v = x0(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(pp - lay.os));
      }
      if (in_ref) {
        v += alpha[p];
      }
      xf(r, static_cast<Eigen::Index>(p)) = v;
    }
  }
  // Free-coordinate factor-return series over the used dates (newest first).
  MatX gser(tu, static_cast<Eigen::Index>(kf));
  MatX u(m, tu); // model residual panel (asset rows, used-date columns)
  {
    Eigen::Index col = 0;
    for (atx::usize j = 0U; j < cfg.window; ++j) {
      if (fr.used[j] == 0U) {
        continue;
      }
      const Eigen::Index jj = static_cast<Eigen::Index>(j);
      for (atx::usize p = 0U; p < kf; ++p) {
        gser(col, static_cast<Eigen::Index>(p)) = fr.f(jj, static_cast<Eigen::Index>(free_[p]));
      }
      for (Eigen::Index r = 0; r < m; ++r) {
        u(r, col) = fr.resid(jj, static_cast<Eigen::Index>(assets[static_cast<atx::usize>(r)]));
      }
      ++col;
    }
  }

  // Statistical block.
  std::vector<atx::usize> panel_rows;
  for (Eigen::Index r = 0; r < m; ++r) {
    atx::usize obs = 0U;
    for (Eigen::Index c = 0; c < tu; ++c) {
      obs += std::isnan(u(r, c)) ? 0U : 1U;
    }
    if (static_cast<atx::f64>(obs) >= cfg.min_coverage * static_cast<atx::f64>(tu)) {
      panel_rows.push_back(static_cast<atx::usize>(r));
    }
  }
  StatSelection sel;
  atx::core::linalg::EigResult gram_eig;
  if (panel_rows.size() >= 2U) {
    MatX ue(static_cast<Eigen::Index>(panel_rows.size()), tu);
    for (atx::usize j = 0U; j < panel_rows.size(); ++j) {
      ue.row(static_cast<Eigen::Index>(j)) =
          u.row(static_cast<Eigen::Index>(panel_rows[j]))
              .unaryExpr([](atx::f64 v) { return std::isnan(v) ? 0.0 : v; });
    }
    detail::demean_rows(ue);
    ATX_TRY(StatSelection s, select_k(ue, cfg, gram_eig));
    sel = std::move(s);
  }
  const atx::usize ks = sel.k;
  MatX b;
  MatX fhat;
  MatX e = u;
  if (ks > 0U) {
    ATX_TRY(StatBlock sb, stat_block(u, ks, cfg, panel_rows, gram_eig));
    b = std::move(sb.b);
    fhat = std::move(sb.fhat);
    e = std::move(sb.resid);
  }
  if (kf + ks == 0U) {
    return Err(ErrorCode::InvalidArgument, "HybridFactorModelBuilder: model has no factors");
  }

  // Assemble X = [X_fund·R | B], F = blockdiag(F_fund, F_stat), D.
  const Eigen::Index kt = static_cast<Eigen::Index>(kf + ks);
  MatX x(m, kt);
  MatX f = MatX::Zero(kt, kt);
  if (kf > 0U) {
    x.leftCols(static_cast<Eigen::Index>(kf)) = xf;
    ATX_TRY(MatX ff, factor_cov(gser, cfg));
    f.topLeftCorner(static_cast<Eigen::Index>(kf), static_cast<Eigen::Index>(kf)) = ff;
  }
  if (ks > 0U) {
    x.rightCols(static_cast<Eigen::Index>(ks)) = b;
    ATX_TRY(MatX fs, factor_cov(fhat, cfg));
    f.bottomRightCorner(static_cast<Eigen::Index>(ks), static_cast<Eigen::Index>(ks)) = fs;
  }
  VecX d(m);
  for (Eigen::Index r = 0; r < m; ++r) {
    d[r] = spec_var(e, r, cfg.spec_halflife);
  }
  ATX_TRY(FactorModel model, FactorModel::create(std::move(x), std::move(f), std::move(d), as_of,
                                                 as_of + cfg.window));
  return atx::core::Ok(HybridModel{std::move(model), std::move(assets), kf, ks, std::move(sel),
                                   std::move(fr)});
}

namespace {

// Shared body of the two build_panel_series entry points. `cap_of_row(t)` is the cap
// span used for style row t; `pit_caps` stores those rows as cap_series (else the
// row-0 span becomes the static ExposureSeries::cap).
template <typename CapOfRow>
atx::core::Result<PanelSeries>
panel_series_impl(const PanelView &panel, const FundamentalPanel *fund, const FundamentalCfg &cfg,
                  std::span<const atx::i64> row_dates, const CapOfRow &cap_of_row,
                  std::span<const atx::u32> group_id, atx::usize window, bool pit_caps) {
  const atx::usize n = panel.instruments();
  if (window == 0U || panel.rows() < window + 2U || row_dates.size() < window + 1U) {
    return Err(ErrorCode::InvalidArgument,
               "build_panel_series: need window > 0, rows >= window+2, dates >= window+1");
  }
  if (!group_id.empty() && group_id.size() != n) {
    return Err(ErrorCode::InvalidArgument, "build_panel_series: group_id length mismatch");
  }
  FundamentalCfg style_cfg = cfg;
  style_cfg.mask.reset(static_cast<atx::usize>(StyleFactor::Market));
  style_cfg.sector_factors = false;
  PanelSeries out;
  out.exposures.market = cfg.mask.test(static_cast<atx::usize>(StyleFactor::Market));
  for (atx::usize b = 0U; b < kAllStyleFactorCount; ++b) {
    if (style_cfg.mask.test(b)) {
      out.exposures.style_tags.push_back(static_cast<StyleFactor>(b));
    }
  }
  const Eigen::Index ks = static_cast<Eigen::Index>(out.exposures.style_tags.size());
  out.exposures.style.reserve(window + 1U);
  for (atx::usize t = 0U; t <= window; ++t) {
    const std::span<const atx::f64> caps_t = cap_of_row(t);
    ATX_TRY(ExposureMatrix xm, build_fundamental_exposures(panel, fund, style_cfg, t, row_dates[t],
                                                           caps_t, {}));
    MatX s = MatX::Constant(static_cast<Eigen::Index>(n), ks, kNaN);
    for (atx::usize r = 0U; r < xm.instrument_rows.size(); ++r) {
      s.row(static_cast<Eigen::Index>(xm.instrument_rows[r])) =
          xm.x.row(static_cast<Eigen::Index>(r));
    }
    out.exposures.style.push_back(std::move(s));
    if (pit_caps) {
      out.exposures.cap_series.emplace_back(caps_t.begin(), caps_t.end());
    }
  }
  if (cfg.sector_factors && !group_id.empty()) {
    std::vector<atx::u32> ids(group_id.begin(), group_id.end());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    out.exposures.n_industries = static_cast<atx::u32>(ids.size());
    out.exposures.industry.reserve(n);
    for (const atx::u32 g : group_id) {
      out.exposures.industry.push_back(static_cast<atx::u32>(
          std::lower_bound(ids.begin(), ids.end(), g) - ids.begin()));
    }
  }
  if (!pit_caps) {
    const std::span<const atx::f64> c0 = cap_of_row(0U);
    out.exposures.cap.assign(c0.begin(), c0.end());
  }
  out.returns.r.resize(static_cast<Eigen::Index>(window), static_cast<Eigen::Index>(n));
  for (atx::usize t = 0U; t < window; ++t) {
    for (atx::usize i = 0U; i < n; ++i) {
      out.returns.r(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(i)) =
          detail::step_return(panel, t, i);
    }
  }
  return atx::core::Ok(std::move(out));
}

} // namespace

atx::core::Result<PanelSeries>
build_panel_series(const PanelView &panel, const FundamentalPanel *fund, const FundamentalCfg &cfg,
                   std::span<const atx::i64> row_dates, std::span<const atx::f64> market_cap,
                   std::span<const atx::u32> group_id, atx::usize window) {
  return panel_series_impl(
      panel, fund, cfg, row_dates, [market_cap](atx::usize) { return market_cap; }, group_id,
      window, false);
}

atx::core::Result<PanelSeries>
build_panel_series_pit_caps(const PanelView &panel, const FundamentalPanel *fund,
                            const FundamentalCfg &cfg, std::span<const atx::i64> row_dates,
                            std::span<const atx::f64> cap_rows,
                            std::span<const atx::u32> group_id, atx::usize window) {
  const atx::usize n = panel.instruments();
  if (cap_rows.size() < (window + 1U) * n) {
    return Err(ErrorCode::InvalidArgument,
               "build_panel_series_pit_caps: cap_rows must hold (window+1)×N entries");
  }
  return panel_series_impl(
      panel, fund, cfg, row_dates,
      [cap_rows, n](atx::usize t) { return cap_rows.subspan(t * n, n); }, group_id, window, true);
}

} // namespace atx::engine::risk

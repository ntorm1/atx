#pragma once

// atx::engine::combine — Hierarchical Risk Parity and Nested Clustered Optimization
// (Lane 5). Header-only.
//
//   hrp_weights   López de Prado (2016), "Building Diversified Portfolios that
//                 Outperform Out-of-Sample" (SSRN 2708678):
//                   1. d_ij = √(½(1 − ρ_ij)); d̃_ij = ‖d_·i − d_·j‖₂ (the paper's
//                      distance-of-distances, what `linkage(dist, 'single')` computes)
//                   2. single-linkage agglomeration (MST/Kruskal, O(N²) + GEMM for d̃)
//                   3. quasi-diagonalization = dendrogram leaf order
//                   4. recursive bisection, split α = 1 − V_L/(V_L + V_R) with V the
//                      inverse-variance-portfolio variance of each half
//                 Long-only, Σw = 1, no Σ⁻¹ anywhere.
//   nco_weights   López de Prado (2019) Nested Clustered Optimization: cut the same
//                 single-linkage tree into k clusters, solve min-variance (or
//                 max-Sharpe when μ is given) inside each cluster, then on the k×k
//                 reduced covariance; w = W_intra · w_inter.
//   HrpCombiner   HRP over the window's alpha-return covariance (cov_targets.hpp),
//                 with each alpha oriented by the sign of its window mean return.
//
//  Determinism: ties in the linkage are broken by (distance, lower index, higher
//  index); cluster ids follow scipy's convention (leaves 0..N−1, merge k → N+k, the
//  smaller id listed first), so leaf order matches the reference implementation.

#include <algorithm> // std::sort, std::min, std::max
#include <cmath>     // std::sqrt, std::abs
#include <limits>    // std::numeric_limits
#include <numeric>   // std::iota
#include <span>      // std::span
#include <utility>   // std::move, std::pair
#include <vector>    // std::vector

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp"           // MatX, VecX
#include "atx/engine/combine/cov_targets.hpp"   // CovTarget, estimate_covariance
#include "atx/engine/combine/signal_combiner.hpp" // CombineWeights, Combiner, normalize_gross
#include "atx/engine/combine/signal_store.hpp"  // SignalStore, FitWindow, alpha_return_matrix

namespace atx::engine::combine {

// One agglomeration step: clusters `a` < `b` merged at `dist` into id N + step.
struct LinkageStep {
  atx::usize a = 0U;
  atx::usize b = 0U;
  atx::f64 dist = 0.0;
};

namespace hrp_detail {

[[nodiscard]] inline atx::core::Status validate_cov(const atx::core::linalg::MatX &cov) {
  if (cov.rows() < 1 || cov.rows() != cov.cols() || !cov.allFinite()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "hrp: cov must be square, finite");
  }
  if ((cov.diagonal().array() <= 0.0).any()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "hrp: non-positive variance");
  }
  return atx::core::Ok();
}

// d̃ (N×N) from a covariance matrix.
[[nodiscard]] inline atx::core::linalg::MatX distance_of_distances(const atx::core::linalg::MatX &cov) {
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;
  const VecX inv_sd = cov.diagonal().array().rsqrt().matrix();
  const MatX corr = inv_sd.asDiagonal() * cov * inv_sd.asDiagonal();
  const MatX d = (0.5 * (1.0 - corr.array())).max(0.0).sqrt().matrix();
  const VecX sq = d.colwise().squaredNorm().transpose();
  MatX dd = -2.0 * (d.transpose() * d);
  dd.colwise() += sq;
  dd.rowwise() += sq.transpose();
  return dd.array().max(0.0).sqrt().matrix();
}

} // namespace hrp_detail

// Single-linkage agglomeration of an N×N symmetric distance matrix (N−1 steps).
// Prim's MST in O(N²), then Kruskal-order union-find to emit scipy-style merges.
[[nodiscard]] inline std::vector<LinkageStep> single_linkage(const atx::core::linalg::MatX &dist) {
  const auto n = static_cast<atx::usize>(dist.rows());
  std::vector<LinkageStep> out;
  if (n < 2U) {
    return out;
  }
  // Prim: parent[i] / key[i] = cheapest edge from the tree to i.
  constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();
  std::vector<atx::f64> key(n, kInf);
  std::vector<atx::usize> parent(n, 0U);
  std::vector<unsigned char> in_tree(n, 0U);
  key[0] = 0.0;
  struct Edge {
    atx::f64 w;
    atx::usize u;
    atx::usize v;
  };
  std::vector<Edge> edges;
  edges.reserve(n - 1U);
  for (atx::usize it = 0U; it < n; ++it) {
    atx::usize best = n;
    for (atx::usize i = 0U; i < n; ++i) {
      if (in_tree[i] == 0U && (best == n || key[i] < key[best])) {
        best = i;
      }
    }
    in_tree[best] = 1U;
    if (it > 0U) {
      edges.push_back({key[best], std::min(best, parent[best]), std::max(best, parent[best])});
    }
    for (atx::usize i = 0U; i < n; ++i) {
      const atx::f64 w = dist(static_cast<Eigen::Index>(best), static_cast<Eigen::Index>(i));
      if (in_tree[i] == 0U && w < key[i]) {
        key[i] = w;
        parent[i] = best;
      }
    }
  }
  std::sort(edges.begin(), edges.end(), [](const Edge &x, const Edge &y) {
    if (x.w != y.w) {
      return x.w < y.w;
    }
    if (x.u != y.u) {
      return x.u < y.u;
    }
    return x.v < y.v;
  });
  // Union-find over leaves; cluster_id[root] = current scipy-style cluster id.
  std::vector<atx::usize> uf(n);
  std::iota(uf.begin(), uf.end(), 0U);
  std::vector<atx::usize> cluster_id(n);
  std::iota(cluster_id.begin(), cluster_id.end(), 0U);
  const auto find = [&uf](atx::usize x) {
    while (uf[x] != x) {
      uf[x] = uf[uf[x]];
      x = uf[x];
    }
    return x;
  };
  out.reserve(n - 1U);
  for (const Edge &e : edges) {
    const atx::usize ru = find(e.u);
    const atx::usize rv = find(e.v);
    const atx::usize ca = cluster_id[ru];
    const atx::usize cb = cluster_id[rv];
    out.push_back({std::min(ca, cb), std::max(ca, cb), e.w});
    uf[rv] = ru;
    cluster_id[ru] = n + out.size() - 1U;
  }
  return out;
}

// Dendrogram leaf order (quasi-diagonalization): expand the root, left child first.
[[nodiscard]] inline std::vector<atx::usize> quasi_diag(const std::vector<LinkageStep> &link,
                                                        atx::usize n) {
  if (n == 0U) {
    return {};
  }
  if (link.empty()) {
    return {0U};
  }
  std::vector<atx::usize> order;
  order.reserve(n);
  std::vector<atx::usize> stack{n + link.size() - 1U};
  while (!stack.empty()) { // bounded: each of the 2N−1 nodes is pushed once
    const atx::usize c = stack.back();
    stack.pop_back();
    if (c < n) {
      order.push_back(c);
      continue;
    }
    const LinkageStep &s = link[c - n];
    stack.push_back(s.b); // right pushed first so left is expanded first
    stack.push_back(s.a);
  }
  return order;
}

// Inverse-variance-portfolio variance of the sub-covariance on `idx`.
[[nodiscard]] inline atx::f64 ivp_cluster_var(const atx::core::linalg::MatX &cov,
                                              std::span<const atx::usize> idx) {
  const auto m = static_cast<Eigen::Index>(idx.size());
  atx::core::linalg::VecX w(m);
  atx::f64 s = 0.0;
  for (Eigen::Index i = 0; i < m; ++i) {
    const auto ii = static_cast<Eigen::Index>(idx[static_cast<atx::usize>(i)]);
    w[i] = 1.0 / cov(ii, ii);
    s += w[i];
  }
  w /= s;
  atx::f64 v = 0.0;
  for (Eigen::Index i = 0; i < m; ++i) {
    for (Eigen::Index j = 0; j < m; ++j) {
      v += w[i] * w[j] *
           cov(static_cast<Eigen::Index>(idx[static_cast<atx::usize>(i)]),
               static_cast<Eigen::Index>(idx[static_cast<atx::usize>(j)]));
    }
  }
  return v;
}

// HRP weights (long-only, Σw = 1) for an N×N covariance. Err on a non-square,
// non-finite, or non-positive-variance input.
[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
hrp_weights(const atx::core::linalg::MatX &cov) {
  ATX_TRY_VOID(hrp_detail::validate_cov(cov));
  const auto n = static_cast<atx::usize>(cov.rows());
  const std::vector<LinkageStep> link = single_linkage(hrp_detail::distance_of_distances(cov));
  const std::vector<atx::usize> order = quasi_diag(link, n);
  std::vector<atx::f64> w(n, 1.0);
  // Recursive bisection over contiguous ranges of `order` (explicit work list).
  std::vector<std::pair<atx::usize, atx::usize>> work{{0U, n}};
  while (!work.empty()) { // bounded: every split strictly shrinks a range
    const auto [lo, hi] = work.back();
    work.pop_back();
    if (hi - lo < 2U) {
      continue;
    }
    const atx::usize mid = lo + (hi - lo) / 2U;
    const std::span<const atx::usize> left(order.data() + lo, mid - lo);
    const std::span<const atx::usize> right(order.data() + mid, hi - mid);
    const atx::f64 vl = ivp_cluster_var(cov, left);
    const atx::f64 vr = ivp_cluster_var(cov, right);
    const atx::f64 alpha = 1.0 - vl / (vl + vr);
    for (const atx::usize i : left) {
      w[i] *= alpha;
    }
    for (const atx::usize i : right) {
      w[i] *= 1.0 - alpha;
    }
    work.emplace_back(mid, hi);
    work.emplace_back(lo, mid);
  }
  return atx::core::Ok(std::move(w));
}

// Cut a linkage into (at most) k clusters: undo the last k−1 merges. labels[i] in
// [0, k), numbered by first appearance in ascending leaf index.
[[nodiscard]] inline std::vector<atx::usize> cut_tree(const std::vector<LinkageStep> &link,
                                                      atx::usize n, atx::usize k) {
  k = std::clamp<atx::usize>(k, 1U, std::max<atx::usize>(n, 1U));
  const atx::usize keep = n - k; // merges applied
  std::vector<atx::usize> uf(n);
  std::iota(uf.begin(), uf.end(), 0U);
  std::vector<atx::usize> rep(n + link.size()); // cluster id → a representative leaf
  std::iota(rep.begin(), rep.begin() + static_cast<std::ptrdiff_t>(n), 0U);
  const auto find = [&uf](atx::usize x) {
    while (uf[x] != x) {
      uf[x] = uf[uf[x]];
      x = uf[x];
    }
    return x;
  };
  for (atx::usize s = 0U; s < link.size(); ++s) {
    const atx::usize ra = find(rep[link[s].a]);
    const atx::usize rb = find(rep[link[s].b]);
    if (s < keep) {
      uf[rb] = ra;
    }
    rep[n + s] = ra;
  }
  std::vector<atx::usize> labels(n);
  std::vector<atx::usize> root_label(n, n);
  atx::usize next = 0U;
  for (atx::usize i = 0U; i < n; ++i) {
    const atx::usize r = find(i);
    if (root_label[r] == n) {
      root_label[r] = next++;
    }
    labels[i] = root_label[r];
  }
  return labels;
}

namespace hrp_detail {

// LdP optPort: w = Σ⁻¹μ / 1ᵀΣ⁻¹μ (μ = 1 → min-variance). Err if Σ not SPD or the
// normalizer vanishes.
[[nodiscard]] inline atx::core::Result<atx::core::linalg::VecX>
opt_port(const atx::core::linalg::MatX &cov, const atx::core::linalg::VecX &mu) {
  const Eigen::LLT<atx::core::linalg::MatX> llt(cov);
  if (llt.info() != Eigen::Success) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "nco: covariance not SPD");
  }
  atx::core::linalg::VecX w = llt.solve(mu);
  const atx::f64 s = w.sum();
  if (!(std::abs(s) > 1e-300) || !w.allFinite()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "nco: degenerate normalizer");
  }
  return atx::core::Ok(atx::core::linalg::VecX(w / s));
}

} // namespace hrp_detail

// NCO weights with k clusters. `mu` empty → min-variance; else max-Sharpe per LdP.
// Σw = 1. Err on invalid cov, |mu| ∉ {0, N}, or a non-SPD (sub-)covariance.
[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
nco_weights(const atx::core::linalg::MatX &cov, std::span<const atx::f64> mu, atx::usize k) {
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;
  ATX_TRY_VOID(hrp_detail::validate_cov(cov));
  const auto n = static_cast<atx::usize>(cov.rows());
  if (!mu.empty() && mu.size() != n) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "nco: |mu| != N");
  }
  const std::vector<atx::usize> labels =
      cut_tree(single_linkage(hrp_detail::distance_of_distances(cov)), n, k);
  atx::usize nk = 0U;
  for (const atx::usize l : labels) {
    nk = std::max(nk, l + 1U);
  }
  const VecX mu_all = mu.empty() ? VecX::Ones(static_cast<Eigen::Index>(n))
                                 : VecX(Eigen::Map<const VecX>(mu.data(), static_cast<Eigen::Index>(n)));
  MatX w_intra = MatX::Zero(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(nk));
  for (atx::usize c = 0U; c < nk; ++c) {
    std::vector<Eigen::Index> idx;
    for (atx::usize i = 0U; i < n; ++i) {
      if (labels[i] == c) {
        idx.push_back(static_cast<Eigen::Index>(i));
      }
    }
    const auto m = static_cast<Eigen::Index>(idx.size());
    MatX sub(m, m);
    VecX sub_mu(m);
    for (Eigen::Index i = 0; i < m; ++i) {
      sub_mu[i] = mu_all[idx[static_cast<atx::usize>(i)]];
      for (Eigen::Index j = 0; j < m; ++j) {
        sub(i, j) = cov(idx[static_cast<atx::usize>(i)], idx[static_cast<atx::usize>(j)]);
      }
    }
    ATX_TRY(VecX wc, hrp_detail::opt_port(sub, sub_mu));
    for (Eigen::Index i = 0; i < m; ++i) {
      w_intra(idx[static_cast<atx::usize>(i)], static_cast<Eigen::Index>(c)) = wc[i];
    }
  }
  const MatX cov_inter = w_intra.transpose() * cov * w_intra;
  const VecX mu_inter = mu.empty() ? VecX::Ones(static_cast<Eigen::Index>(nk))
                                   : VecX(w_intra.transpose() * mu_all);
  ATX_TRY(VecX w_inter, hrp_detail::opt_port(cov_inter, mu_inter));
  const VecX w = w_intra * w_inter;
  return atx::core::Ok(std::vector<atx::f64>(w.data(), w.data() + w.size()));
}

// HRP over the window's alpha-return covariance. Alphas are oriented by the sign of
// their window mean return (a negative-edge alpha is held short), then HRP sizes the
// risk; Σ|w| = 1. Zero-variance alphas get 0. tstat = mean/(sd/√T).
struct HrpCombiner {
  CovTarget target = CovTarget::LwIdentity;

  [[nodiscard]] atx::core::Result<CombineWeights> fit(const SignalStore &s, FitWindow win) const {
    using atx::core::linalg::MatX;
    ATX_TRY_VOID(validate_window(s, win, 2U));
    const atx::usize k = s.n_alphas();
    const std::vector<atx::f64> flat = alpha_return_matrix(s, win);
    std::vector<atx::usize> rows;
    for (atx::usize r = 0U; r < win.size(); ++r) {
      bool ok = true;
      for (atx::usize a = 0U; a < k && ok; ++a) {
        ok = std::isfinite(flat[r * k + a]);
      }
      if (ok) {
        rows.push_back(r);
      }
    }
    if (rows.size() < 2U) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "HrpCombiner: < 2 complete rows");
    }
    MatX x(static_cast<Eigen::Index>(rows.size()), static_cast<Eigen::Index>(k));
    for (atx::usize r = 0U; r < rows.size(); ++r) {
      for (atx::usize a = 0U; a < k; ++a) {
        x(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(a)) = flat[rows[r] * k + a];
      }
    }
    CombineWeights out;
    out.w.assign(k, 0.0);
    out.tstat.assign(k, kSignalNaN);
    std::vector<Eigen::Index> live;
    const atx::f64 tn = static_cast<atx::f64>(x.rows());
    for (Eigen::Index a = 0; a < x.cols(); ++a) {
      const atx::f64 m = x.col(a).mean();
      const atx::f64 sd = std::sqrt((x.col(a).array() - m).square().sum() / (tn - 1.0));
      if (sd > 0.0) {
        live.push_back(a);
        out.tstat[static_cast<atx::usize>(a)] = m / (sd / std::sqrt(tn));
      }
    }
    if (!live.empty()) {
      MatX xl(x.rows(), static_cast<Eigen::Index>(live.size()));
      std::vector<atx::f64> sign(live.size());
      for (atx::usize j = 0U; j < live.size(); ++j) {
        sign[j] = (x.col(live[j]).mean() < 0.0) ? -1.0 : 1.0;
        xl.col(static_cast<Eigen::Index>(j)) = sign[j] * x.col(live[j]);
      }
      ATX_TRY(MatX cov, estimate_covariance(xl, target));
      ATX_TRY(std::vector<atx::f64> w, hrp_weights(cov));
      for (atx::usize j = 0U; j < live.size(); ++j) {
        out.w[static_cast<atx::usize>(live[j])] = sign[j] * w[j];
      }
    }
    normalize_gross(out.w);
    out.fit_begin = win.begin;
    out.fit_end = win.end;
    return atx::core::Ok(std::move(out));
  }
};

static_assert(Combiner<HrpCombiner>);

} // namespace atx::engine::combine

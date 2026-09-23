#pragma once

// atx::engine::combine — covariance shrinkage targets for the signal-space combiner
// (Lane 5). Header-only; every estimator takes a T×N observation matrix X (rows =
// periods, columns = variables — alphas, IC series, factor returns) and returns an
// N×N covariance estimate. They are applied to K×K alpha/IC matrices, never to the
// dense asset covariance (that is the structural factor model's job).
//
//   CovTarget::Sample       S = XcᵀXc / T                     (MLE, demeaned)
//   CovTarget::LwIdentity   Ledoit-Wolf 2004, target μ·I       (JMVA 88(2))
//   CovTarget::LwConstCorr  Ledoit-Wolf 2003, constant-corr    ("Honey, I shrunk…")
//   CovTarget::LwNonlinear  Ledoit-Wolf 2020 analytical nonlinear shrinkage
//                           (Annals of Statistics 48(5)), incl. the c>1 null-space
//                           variant
//   CovTarget::RmtClip      Marchenko-Pastur eigenvalue clipping of the correlation
//                           matrix (atx-core rmt_clean, Mode::Clip), re-scaled by the
//                           sample standard deviations
//
// Determinism: no RNG; Eigen products / SelfAdjointEigenSolver are deterministic for
// a fixed build; all hand reductions run in ascending index order.
//
// Reference: the LW2020 and LW2003 paths are checked against an independent numpy port
// of the authors' published Matlab code (tests/combine/fixtures/gen_cov_targets_fixture.py)
// to 1e-8.

#include <algorithm> // std::clamp, std::max, std::min
#include <cmath>     // std::sqrt, std::log, std::abs
#include <numbers>   // std::numbers::pi
#include <utility>   // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, u8

#include "atx/core/linalg/linalg.hpp"    // MatX, VecX
#include "atx/core/linalg/rmt_clean.hpp" // rmt_clean (RmtClip target)

namespace atx::engine::combine {

// Covariance estimator selector for GrinoldKahnCombiner / HRP inputs. Appending only.
enum class CovTarget : atx::u8 {
  Sample,
  LwIdentity,
  LwConstCorr,
  LwNonlinear,
  RmtClip,
};

// A shrunk covariance plus the linear intensity used (NaN-free; 0 for non-linear).
struct ShrunkCovariance {
  atx::core::linalg::MatX sigma;
  atx::f64 delta = 0.0;
};

namespace cov_detail {

// Column-demeaned copy of X (T×N).
[[nodiscard]] inline atx::core::linalg::MatX demean_columns(const atx::core::linalg::MatX &x) {
  const Eigen::RowVectorXd mu = x.colwise().mean();
  return x.rowwise() - mu;
}

[[nodiscard]] inline atx::core::Status validate(const atx::core::linalg::MatX &x, Eigen::Index min_t,
                                                const char *who) {
  if (x.cols() < 1 || x.rows() < min_t) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, who);
  }
  if (!x.allFinite()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, who);
  }
  return atx::core::Ok();
}

} // namespace cov_detail

// MLE sample covariance of the demeaned columns (divisor T). Err on T<2 / non-finite.
[[nodiscard]] inline atx::core::Result<atx::core::linalg::MatX>
sample_covariance(const atx::core::linalg::MatX &x) {
  ATX_TRY_VOID(cov_detail::validate(x, 2, "sample_covariance: need T>=2, N>=1, finite X"));
  const atx::core::linalg::MatX xc = cov_detail::demean_columns(x);
  return atx::core::Ok(atx::core::linalg::MatX((xc.transpose() * xc) / static_cast<atx::f64>(x.rows())));
}

// Ledoit-Wolf (2004) linear shrinkage toward μ·I, μ = tr(S)/N. Same closed form as
// combiner.hpp's detail::ledoit_wolf_intensity (1/T² normalization), but O(N²T) via
// the fourth-moment identity Σ_t‖x_t x_tᵀ − S‖² = Σ_ij (x²)ᵀ(x²) − T‖S‖², not T outer
// products.
[[nodiscard]] inline atx::core::Result<ShrunkCovariance>
shrink_lw_identity(const atx::core::linalg::MatX &x) {
  using atx::core::linalg::MatX;
  ATX_TRY_VOID(cov_detail::validate(x, 2, "shrink_lw_identity: need T>=2, N>=1, finite X"));
  const MatX xc = cov_detail::demean_columns(x);
  const atx::f64 t = static_cast<atx::f64>(x.rows());
  const atx::f64 n = static_cast<atx::f64>(x.cols());
  const MatX s = (xc.transpose() * xc) / t;
  const atx::f64 mu = s.trace() / n;
  MatX diff = s;
  diff.diagonal().array() -= mu;
  const atx::f64 d2 = diff.squaredNorm();
  atx::f64 delta = 0.0;
  if (d2 > 0.0) {
    const MatX y = xc.array().square().matrix();
    const atx::f64 pi_sum = (y.transpose() * y).sum() - t * s.squaredNorm();
    const atx::f64 b2 = std::min(pi_sum / (t * t), d2);
    delta = std::clamp(b2 / d2, 0.0, 1.0);
  }
  MatX sigma = (1.0 - delta) * s;
  sigma.diagonal().array() += delta * mu;
  return atx::core::Ok(ShrunkCovariance{std::move(sigma), delta});
}

// Ledoit-Wolf (2003) shrinkage toward the constant-correlation target
// F_ij = r̄·√(s_ii s_jj), F_ii = s_ii (the covCor.m estimator). Requires N>=2 and
// strictly positive sample variances (a flat column → Err).
[[nodiscard]] inline atx::core::Result<ShrunkCovariance>
shrink_lw_const_corr(const atx::core::linalg::MatX &x) {
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;
  ATX_TRY_VOID(cov_detail::validate(x, 2, "shrink_lw_const_corr: need T>=2, N>=1, finite X"));
  const MatX xc = cov_detail::demean_columns(x);
  const atx::f64 t = static_cast<atx::f64>(x.rows());
  const MatX s = (xc.transpose() * xc) / t;
  if (x.cols() < 2) {
    return atx::core::Ok(ShrunkCovariance{s, 0.0}); // one variable: nothing to pool
  }
  const Eigen::Index n = x.cols();
  const atx::f64 nf = static_cast<atx::f64>(n);
  const VecX var = s.diagonal();
  if ((var.array() <= 0.0).any()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "shrink_lw_const_corr: zero-variance column");
  }
  const VecX sd = var.array().sqrt().matrix();
  const MatX sdsd = sd * sd.transpose();
  const atx::f64 rbar = ((s.array() / sdsd.array()).sum() - nf) / (nf * (nf - 1.0));
  MatX f = rbar * sdsd;
  f.diagonal() = var;
  const MatX y = xc.array().square().matrix();
  const MatX phi_mat = (y.transpose() * y) / t - s.array().square().matrix();
  const atx::f64 phi = phi_mat.sum();
  const MatX x3 = xc.array().cube().matrix();
  MatX theta = (x3.transpose() * xc) / t;
  for (Eigen::Index j = 0; j < n; ++j) {
    for (Eigen::Index i = 0; i < n; ++i) {
      theta(i, j) -= var[i] * s(i, j);
    }
  }
  theta.diagonal().setZero();
  atx::f64 rho_off = 0.0;
  for (Eigen::Index j = 0; j < n; ++j) {
    for (Eigen::Index i = 0; i < n; ++i) {
      rho_off += (sd[j] / sd[i]) * theta(i, j);
    }
  }
  const atx::f64 rho = phi_mat.diagonal().sum() + rbar * rho_off;
  const atx::f64 gamma = (s - f).squaredNorm();
  atx::f64 delta = 0.0;
  if (gamma > 0.0) {
    delta = std::clamp(((phi - rho) / gamma) / t, 0.0, 1.0);
  }
  MatX sigma = delta * f + (1.0 - delta) * s;
  return atx::core::Ok(ShrunkCovariance{std::move(sigma), delta});
}

// Ledoit-Wolf (2020) analytical nonlinear shrinkage. X is demeaned internally and the
// effective sample size is n = T−1 (the authors' k=1 convention). Each sample
// eigenvalue λ_i is replaced by
//   c = p/n ≤ 1:  d̃_i = λ_i / [(π c λ_i f̃_i)² + (1 − c − π c λ_i Hf̃_i)²]
//   c > 1      :  the n non-null eigenvalues get λ_i / [π² λ_i² (f̃_i² + Hf̃_i²)] and the
//                 p−n null-space eigenvalues share d̃₀ = 1 / (π (p−n)/n · Hf̃₀)
// where f̃ / Hf̃ are the Epanechnikov-kernel density of the spectrum and its Hilbert
// transform with the locally adaptive bandwidth h·λ_j, h = n^{-1/3}. Cost O(p³) for
// the eigendecomposition plus O(min(p,n)²) for the kernel sums.
// Preconditions: n = T−1 >= 12 (so √5·h < 1 in the null-space term), finite X.
[[nodiscard]] inline atx::core::Result<atx::core::linalg::MatX>
shrink_nonlinear_lw2020(const atx::core::linalg::MatX &x) {
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;
  ATX_TRY_VOID(cov_detail::validate(x, 13, "shrink_nonlinear_lw2020: need T>=13, N>=1, finite X"));
  constexpr atx::f64 kPi = std::numbers::pi;
  const atx::f64 sqrt5 = std::sqrt(5.0);
  const Eigen::Index p = x.cols();
  const Eigen::Index n = x.rows() - 1;
  const atx::f64 pf = static_cast<atx::f64>(p);
  const atx::f64 nf = static_cast<atx::f64>(n);
  const MatX xc = cov_detail::demean_columns(x);
  // Retained spectrum: the m = min(p, n) largest eigenpairs of S = XcᵀXc/n. When p > n
  // they come from the T×T dual XcXcᵀ/n (u_j = Xcᵀv_j/√(nλ_j)), so the p×p
  // eigendecomposition — O(p³), ~20 s at p = 2000 — is never formed.
  const Eigen::Index m = std::min(p, n);
  const bool dual = p > n;
  const MatX gram = dual ? MatX((xc * xc.transpose()) / nf) : MatX((xc.transpose() * xc) / nf);
  const Eigen::SelfAdjointEigenSolver<MatX> es(gram);
  if (es.info() != Eigen::Success) {
    return atx::core::Err(atx::core::ErrorCode::Internal,
                          "shrink_nonlinear_lw2020: eigendecomposition failed");
  }
  const VecX lam = es.eigenvalues().tail(m); // ascending
  if ((lam.array() <= 0.0).any()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "shrink_nonlinear_lw2020: non-positive retained eigenvalue");
  }
  MatX um; // p × m retained eigenvectors (ascending eigenvalue order)
  if (dual) {
    um = xc.transpose() * es.eigenvectors().rightCols(m);
    for (Eigen::Index j = 0; j < m; ++j) {
      um.col(j) /= std::sqrt(nf * lam[j]);
    }
  } else {
    um = es.eigenvectors();
  }
  const atx::f64 h = std::pow(nf, -1.0 / 3.0);
  VecX ft(m);
  VecX hft(m);
  for (Eigen::Index i = 0; i < m; ++i) {
    atx::f64 fsum = 0.0;
    atx::f64 hsum = 0.0;
    for (Eigen::Index j = 0; j < m; ++j) {
      const atx::f64 hj = h * lam[j];
      const atx::f64 u = (lam[i] - lam[j]) / hj;
      const atx::f64 k = 1.0 - u * u / 5.0;
      fsum += std::max(k, 0.0) / hj;
      atx::f64 hf = (-3.0 / 10.0 / kPi) * u;
      if (std::abs(u) != sqrt5) {
        hf += (3.0 / 4.0 / sqrt5 / kPi) * k * std::log(std::abs((sqrt5 - u) / (sqrt5 + u)));
      }
      hsum += hf / hj;
    }
    ft[i] = (3.0 / 4.0 / sqrt5) * fsum / static_cast<atx::f64>(m);
    hft[i] = hsum / static_cast<atx::f64>(m);
  }
  VecX d(m);
  MatX sigma;
  if (!dual) {
    const atx::f64 c = pf / nf;
    for (Eigen::Index i = 0; i < p; ++i) {
      const atx::f64 a = kPi * c * lam[i] * ft[i];
      const atx::f64 b = 1.0 - c - kPi * c * lam[i] * hft[i];
      d[i] = lam[i] / (a * a + b * b);
    }
    sigma = um * d.asDiagonal() * um.transpose();
  } else {
    const atx::f64 inv_mean = (1.0 / lam.array()).mean();
    const atx::f64 hf0 = (1.0 / kPi) *
                         (3.0 / 10.0 / (h * h) + 3.0 / 4.0 / sqrt5 / h * (1.0 - 1.0 / 5.0 / (h * h)) *
                                                     std::log((1.0 + sqrt5 * h) / (1.0 - sqrt5 * h))) *
                         inv_mean;
    const atx::f64 d0 = 1.0 / (kPi * (pf - nf) / nf * hf0);
    for (Eigen::Index i = 0; i < m; ++i) {
      d[i] = lam[i] / (kPi * kPi * lam[i] * lam[i] * (ft[i] * ft[i] + hft[i] * hft[i])) - d0;
    }
    // Null space shares d0: Σ̃ = d0·I + U_m diag(d1 − d0) U_mᵀ.
    sigma = um * d.asDiagonal() * um.transpose();
    sigma.diagonal().array() += d0;
  }
  sigma = 0.5 * (sigma + sigma.transpose()); // exact symmetry for downstream Cholesky
  return atx::core::Ok(std::move(sigma));
}

// Marchenko-Pastur clip: correlation R of X cleaned by atx-core rmt_clean (Mode::Clip,
// q = N/T), then re-scaled Σ = D R_clean D with D = diag(sample sd). Flat columns → Err.
[[nodiscard]] inline atx::core::Result<atx::core::linalg::MatX>
shrink_rmt_clip(const atx::core::linalg::MatX &x) {
  using atx::core::linalg::MatX;
  using atx::core::linalg::VecX;
  ATX_TRY(MatX s, sample_covariance(x));
  const VecX var = s.diagonal();
  if ((var.array() <= 0.0).any()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "shrink_rmt_clip: zero-variance column");
  }
  const VecX sd = var.array().sqrt().matrix();
  const VecX inv = sd.array().inverse().matrix();
  const MatX corr = inv.asDiagonal() * s * inv.asDiagonal();
  const atx::f64 q = static_cast<atx::f64>(x.cols()) / static_cast<atx::f64>(x.rows());
  ATX_TRY(atx::core::linalg::CleanedCorr cleaned, atx::core::linalg::rmt_clean(corr, q));
  MatX sigma = sd.asDiagonal() * cleaned.corr * sd.asDiagonal();
  return atx::core::Ok(std::move(sigma));
}

// Dispatch on CovTarget (exhaustive, no default).
[[nodiscard]] inline atx::core::Result<atx::core::linalg::MatX>
estimate_covariance(const atx::core::linalg::MatX &x, CovTarget target) {
  switch (target) {
  case CovTarget::Sample:
    return sample_covariance(x);
  case CovTarget::LwIdentity: {
    ATX_TRY(ShrunkCovariance r, shrink_lw_identity(x));
    return atx::core::Ok(std::move(r.sigma));
  }
  case CovTarget::LwConstCorr: {
    ATX_TRY(ShrunkCovariance r, shrink_lw_const_corr(x));
    return atx::core::Ok(std::move(r.sigma));
  }
  case CovTarget::LwNonlinear:
    return shrink_nonlinear_lw2020(x);
  case CovTarget::RmtClip:
    return shrink_rmt_clip(x);
  }
  return atx::core::Err(atx::core::ErrorCode::Internal, "estimate_covariance: bad CovTarget");
}

} // namespace atx::engine::combine

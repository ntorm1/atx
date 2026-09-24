#pragma once

// atx::engine::risk — gp_riccati: the TRUE multi-period Gârleanu-Pedersen policy (Lane 6).
//
// ===========================================================================
//  Model (Gârleanu & Pedersen, J. Finance 68(6), 2013 — "GP")
// ===========================================================================
//    E_t[r_{t+1}] = B f_t,           f_{t+1} = (I − Φ) f_t + ε_{t+1},     Φ = diag(φ)
//    maximize  E Σ_t [ ρ̄^{t+1}(x_tᵀ B f_t − ½γ x_tᵀΣx_t) − ½ρ̄^t Δx_tᵀ Λ Δx_t ],  ρ̄ = 1 − ρ
//
//  With the value function V(x_{t−1}, f_t) = −½xᵀA_xx x + xᵀA_xf f + (f-only terms), one
//  Bellman step (dynamic programming, GP §II) gives, with
//      J = ρ̄γΣ + Λ + ρ̄A_xx,          G = ρ̄(B + A_xf(I − Φ)),
//  the optimal trade   x_t = J⁻¹(Λ x_{t−1} + G f_t)   and the Riccati map
//      A_xx ← Λ − Λ J⁻¹ Λ,           A_xf ← Λ J⁻¹ G.
//  Iterating the map from V ≡ 0 is the H-period backward recursion (H = cfg.horizon); its
//  fixed point is the infinite-horizon GP policy. Equivalently x_t = x_{t−1} +
//  M_rate (aim_t − x_{t−1}) with M_rate = I − J⁻¹Λ and aim_t = (J − Λ)⁻¹ G f_t.
//
//  Two paths:
//   * DENSE (general diagonal Λ, or Λ = λΣ): Σ = XFXᵀ + D is materialized M×M and the map
//     is iterated with a Cholesky of J per step. O(M³) per iteration ⇒ bounded to M ≤
//     kGpDenseMaxM (Err(OutOfRange) above).
//   * FACTOR-SPACE (Λ = λΣ, GP Prop. 4 closed form): A_xx = aΣ with the scalar
//         a = (−(γρ̄ + λρ) + sqrt((γρ̄ + λρ)² + 4γλρ̄²)) / (2ρ̄),
//     trade rate a/λ (scalar) and aim_t = (γΣ)⁻¹ B (I + aΦ/γ)⁻¹ f_t — applied through
//     FactorModel::apply_inverse (Woodbury, O(MK²)), never forming an M×M matrix.
//
//  Horizon 1 (V ≡ 0): x = (ρ̄γΣ + Λ)⁻¹(Λx_{t−1} + ρ̄Bf) — the myopic single-period trade with
//  quadratic cost; cfg.horizon == 1 returns exactly that map (one Riccati step from zero).
//
//  Determinism: no RNG, fixed iteration order; the dense path stops at the first iterate
//  whose max-abs change is ≤ cfg.tol, or at cfg.max_iter (a pure function of the inputs).

#include <algorithm> // std::max
#include <cmath>   // std::sqrt, std::fabs, std::isfinite
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

#include <Eigen/Cholesky>
#include <Eigen/Dense>

#include "atx/core/error.hpp"         // Result, Ok, Err
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/factor_model.hpp" // FactorModel

namespace atx::engine::risk {

inline constexpr atx::usize kGpDenseMaxM = 2000;

// Quadratic trade-cost matrix Λ. Exactly one of the two forms must be set:
//  * diag (length M, entries > 0): Λ = diag(diag)  — per-name impact (dense path);
//  * proportional > 0 with diag empty: Λ = proportional·Σ (GP's baseline; factor-space path
//    unless cfg.force_dense).
struct ImpactDiag {
  std::span<const atx::f64> diag;
  atx::f64 proportional = 0.0;
};

struct GpRiccatiCfg {
  atx::f64 rho = 0.0;            // discount rate ρ ∈ [0, 1)
  atx::usize horizon = 0;        // 0 ⇒ infinite-horizon fixed point; H ≥ 1 ⇒ H backward steps
  atx::usize max_iter = 20000;   // dense fixed-point iteration cap
  atx::f64 tol = 1e-14;          // dense fixed-point stop: max|ΔA| ≤ tol·max(1, max|A|)
  bool force_dense = false;      // proportional Λ through the dense path (oracle / testing)
};

struct GpPolicy {
  bool factor_space = false;          // scalar-rate closed form (no M×M matrices)
  atx::f64 trade_rate = 0.0;          // factor-space: scalar a/λ
  atx::core::linalg::MatX aim_load;   // factor-space: M×S map f ↦ aim
  atx::core::linalg::MatX keep;       // dense: J⁻¹Λ   (M×M)
  atx::core::linalg::MatX load;       // dense: J⁻¹G   (M×S)
  atx::core::linalg::MatX A_xx;       // dense: value curvature (M×M)
  atx::core::linalg::MatX A_xf;       // dense: value cross term (M×S)
  atx::usize iterations = 0;          // Riccati steps taken (dense) / 0 (factor-space)
  bool converged = true;              // dense infinite-horizon: fixed point reached

  // Policy dimensions: M names, S signals (0 for a default-constructed policy).
  [[nodiscard]] Eigen::Index n_names() const noexcept {
    return factor_space ? aim_load.rows() : keep.rows();
  }
  [[nodiscard]] Eigen::Index n_signals() const noexcept {
    return factor_space ? aim_load.cols() : load.cols();
  }

  // x_t given x_{t−1} (length M) and the signal vector f_t (length S). The lengths are
  // checked here, not left to Eigen's debug-only asserts: a wrong-length span in Release
  // would otherwise read out of bounds. Err(InvalidArgument) on a mismatch.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  step(std::span<const atx::f64> x_prev, std::span<const atx::f64> f) const {
    namespace co = atx::core;
    if (static_cast<Eigen::Index>(x_prev.size()) != n_names() ||
        static_cast<Eigen::Index>(f.size()) != n_signals()) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "GpPolicy::step: x_prev must have length M and f length S");
    }
    const auto m = static_cast<Eigen::Index>(x_prev.size());
    const Eigen::Map<const atx::core::linalg::VecX> xp(x_prev.data(), m);
    const Eigen::Map<const atx::core::linalg::VecX> fv(f.data(),
                                                       static_cast<Eigen::Index>(f.size()));
    atx::core::linalg::VecX x;
    if (factor_space) {
      const atx::core::linalg::VecX a = aim_load * fv;
      x = xp + trade_rate * (a - xp);
    } else {
      x = keep * xp + load * fv;
    }
    return co::Ok(std::vector<atx::f64>(x.data(), x.data() + x.size()));
  }

  // The aim portfolio aim_t = (J − Λ)⁻¹ G f_t (dense) / the GP closed form (factor-space).
  // Err(InvalidArgument) when f does not have length S.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>> aim(std::span<const atx::f64> f) const {
    namespace co = atx::core;
    if (static_cast<Eigen::Index>(f.size()) != n_signals()) {
      return co::Err(co::ErrorCode::InvalidArgument, "GpPolicy::aim: f must have length S");
    }
    const Eigen::Map<const atx::core::linalg::VecX> fv(f.data(),
                                                       static_cast<Eigen::Index>(f.size()));
    atx::core::linalg::VecX a;
    if (factor_space) {
      a = aim_load * fv;
    } else {
      // x = keep·x + load·f at the fixed point x = aim ⇒ (I − keep) aim = load f.
      const auto m = keep.rows();
      const atx::core::linalg::MatX rate = atx::core::linalg::MatX::Identity(m, m) - keep;
      a = rate.partialPivLu().solve(load * fv);
    }
    return co::Ok(std::vector<atx::f64>(a.data(), a.data() + a.size()));
  }
};

namespace detail {

[[nodiscard]] inline atx::core::linalg::MatX dense_sigma(const FactorModel &v) {
  const atx::core::linalg::MatX &x = v.exposures();
  atx::core::linalg::MatX s = x * v.factor_cov() * x.transpose();
  s.diagonal() += v.specific_var();
  return s;
}

// GP Prop. 4 scalar a for Λ = λΣ.
[[nodiscard]] inline atx::f64 gp_scalar_a(atx::f64 gamma, atx::f64 lambda, atx::f64 rho) {
  const atx::f64 rb = 1.0 - rho;
  const atx::f64 b = gamma * rb + lambda * rho;
  return (-b + std::sqrt(b * b + 4.0 * gamma * lambda * rb * rb)) / (2.0 * rb);
}

} // namespace detail

// Solve for the GP policy. `loadings` is B (M×S), `decay_phi` the per-signal mean-reversion
// speeds φ_s ∈ [0, 1] (length S). Err(InvalidArgument) on shape / sign violations
// (γ ≤ 0, ρ ∉ [0,1), Λ malformed, φ out of range); Err(OutOfRange) when the dense path
// would exceed kGpDenseMaxM names.
[[nodiscard]] inline atx::core::Result<GpPolicy>
gp_riccati(const FactorModel &v, atx::f64 gamma, const ImpactDiag &impact,
           std::span<const atx::f64> decay_phi, const atx::core::linalg::MatX &loadings,
           const GpRiccatiCfg &cfg = {}) {
  namespace co = atx::core;
  namespace cl = atx::core::linalg;
  const atx::usize m = v.n_instruments();
  const atx::usize s = decay_phi.size();
  const auto em = static_cast<Eigen::Index>(m);
  const auto es = static_cast<Eigen::Index>(s);
  if (!(gamma > 0.0) || !std::isfinite(gamma)) {
    return co::Err(co::ErrorCode::InvalidArgument, "gp_riccati: gamma must be finite and > 0");
  }
  if (!(cfg.rho >= 0.0 && cfg.rho < 1.0)) {
    return co::Err(co::ErrorCode::InvalidArgument, "gp_riccati: rho must lie in [0, 1)");
  }
  if (loadings.rows() != em || loadings.cols() != es) {
    return co::Err(co::ErrorCode::InvalidArgument, "gp_riccati: loadings must be M x S");
  }
  for (const atx::f64 phi : decay_phi) {
    if (!(phi >= 0.0 && phi <= 1.0)) {
      return co::Err(co::ErrorCode::InvalidArgument, "gp_riccati: decay_phi must be in [0,1]");
    }
  }
  const bool proportional = impact.diag.empty();
  if (proportional && !(impact.proportional > 0.0 && std::isfinite(impact.proportional))) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "gp_riccati: set impact.diag (length M) or impact.proportional > 0");
  }
  if (!proportional) {
    if (impact.diag.size() != m) {
      return co::Err(co::ErrorCode::InvalidArgument, "gp_riccati: impact.diag must be M");
    }
    for (const atx::f64 l : impact.diag) {
      if (!(l > 0.0) || !std::isfinite(l)) {
        return co::Err(co::ErrorCode::InvalidArgument, "gp_riccati: impact.diag must be > 0");
      }
    }
  }
  const atx::f64 rb = 1.0 - cfg.rho;

  GpPolicy out;
  if (proportional && !cfg.force_dense && cfg.horizon == 0U) {
    // Factor-space closed form: aim_load = (γΣ)⁻¹ B diag(1/(1 + aφ_s/γ)).
    const atx::f64 lambda = impact.proportional;
    const atx::f64 a = detail::gp_scalar_a(gamma, lambda, cfg.rho);
    out.factor_space = true;
    out.trade_rate = a / lambda;
    out.aim_load = cl::MatX::Zero(em, es);
    std::vector<atx::f64> col(m, 0.0);
    std::vector<atx::f64> sol(m, 0.0);
    for (Eigen::Index j = 0; j < es; ++j) {
      const atx::f64 shrink = 1.0 / (gamma * (1.0 + a * decay_phi[static_cast<atx::usize>(j)] /
                                                        gamma));
      for (Eigen::Index i = 0; i < em; ++i) {
        col[static_cast<atx::usize>(i)] = loadings(i, j);
      }
      v.apply_inverse(std::span<const atx::f64>(col), std::span<atx::f64>(sol));
      for (Eigen::Index i = 0; i < em; ++i) {
        out.aim_load(i, j) = shrink * sol[static_cast<atx::usize>(i)];
      }
    }
    return co::Ok(std::move(out));
  }

  if (m > kGpDenseMaxM) {
    return co::Err(co::ErrorCode::OutOfRange, "gp_riccati: dense path limited to kGpDenseMaxM");
  }
  const cl::MatX sigma = detail::dense_sigma(v);
  cl::MatX lam(em, em);
  if (proportional) {
    lam = impact.proportional * sigma;
  } else {
    lam.setZero();
    for (Eigen::Index i = 0; i < em; ++i) {
      lam(i, i) = impact.diag[static_cast<atx::usize>(i)];
    }
  }
  cl::MatX one_minus_phi = cl::MatX::Identity(es, es);
  for (Eigen::Index j = 0; j < es; ++j) {
    one_minus_phi(j, j) -= decay_phi[static_cast<atx::usize>(j)];
  }

  cl::MatX axx = cl::MatX::Zero(em, em);
  cl::MatX axf = cl::MatX::Zero(em, es);
  cl::MatX j_inv_lam(em, em);
  cl::MatX j_inv_g(em, es);
  const atx::usize steps = (cfg.horizon == 0U) ? cfg.max_iter : cfg.horizon;
  bool converged = cfg.horizon != 0U;
  atx::usize it = 0;
  for (; it < steps; ++it) {
    cl::MatX jm = rb * gamma * sigma + lam + rb * axx;
    jm = 0.5 * (jm + jm.transpose()); // keep the SPD solve symmetric to rounding
    const Eigen::LLT<cl::MatX> llt(jm);
    if (llt.info() != Eigen::Success) {
      return co::Err(co::ErrorCode::InvalidArgument, "gp_riccati: J not positive definite");
    }
    const cl::MatX g = rb * (loadings + axf * one_minus_phi);
    j_inv_lam = llt.solve(lam);
    j_inv_g = llt.solve(g);
    cl::MatX axx_next = lam - lam * j_inv_lam;
    axx_next = 0.5 * (axx_next + axx_next.transpose());
    const cl::MatX axf_next = lam * j_inv_g;
    const atx::f64 delta = std::max((axx_next - axx).cwiseAbs().maxCoeff(),
                                    es > 0 ? (axf_next - axf).cwiseAbs().maxCoeff() : 0.0);
    const atx::f64 scale = std::max(1.0, axx_next.cwiseAbs().maxCoeff());
    axx = axx_next;
    axf = axf_next;
    if (cfg.horizon == 0U && delta <= cfg.tol * scale) {
      converged = true;
      ++it;
      break;
    }
  }
  // keep/load come from the LAST step's J, built from the value of the H−1 remaining
  // periods (H = 1 ⇒ V ≡ 0 ⇒ myopic); A_xx/A_xf are that step's output (the H-period value).
  out.factor_space = false;
  out.keep = j_inv_lam;
  out.load = j_inv_g;
  out.A_xx = std::move(axx);
  out.A_xf = std::move(axf);
  out.iterations = it;
  out.converged = converged;
  return co::Ok(std::move(out));
}

} // namespace atx::engine::risk

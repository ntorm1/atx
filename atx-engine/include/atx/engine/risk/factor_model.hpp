#pragma once

// atx::engine::risk — FactorModel: the FACTORED covariance V = X F Xᵀ + D (P4-7a).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  FactorModel is a Barra-style risk model kept in FACTORED form
//      V = X F Xᵀ + D ,   X : M×K exposures,  F : K×K factor covariance (SPD),
//                          D : M specific (idiosyncratic) variances (diagonal, >0)
//  and applies it WITHOUT ever materializing the dense M×M V (M ≫ K, so a dense
//  V would be O(M²) memory and O(M²) per matvec — defeated here by working in the
//  K-dimensional factor space):
//    risk(w)               = (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i²      O(MK + K²)
//    apply_inverse(in,out) = V⁻¹·in via Woodbury               O(MK + K³)
//    neutralize(signal)    = s − X (XᵀX)⁻¹ Xᵀ s  (in place)    O(MK + K³)
//  plus a carried fit window [fit_begin, fit_end).
//
//  P4-7a SCOPE: this unit is the orchestrator's split of plan-P4-7. It stores a
//  GIVEN (X, F, D) and applies it. The per-date cross-sectional WLS that ESTIMATES
//  X, F, D — `FactorModelBuilder::build` — is P4-7b and is ADDED to this same
//  header later (see the marker near the bottom). Nothing here estimates anything.
//
// ===========================================================================
//  Woodbury inverse + the cached capacitance (the WHOLE POINT)
// ===========================================================================
//  V⁻¹·x = D⁻¹x − D⁻¹X (F⁻¹ + Xᵀ D⁻¹ X)⁻¹ Xᵀ D⁻¹x        (Sherman-Morrison-Woodbury)
//  The K×K capacitance C = F⁻¹ + Xᵀ D⁻¹ X is SPD (F⁻¹ SPD + Xᵀ D⁻¹ X PSD). It is
//  built ONCE at construction and its Cholesky (Eigen::LLT) is CACHED, so each
//  apply_inverse is two matvecs (Xᵀ·, X·) + one cached K×K solve — never a refactor
//  and never an M×M materialization. dinv = 1/D is cached too.
//
// ===========================================================================
//  The D floor + the neutralize ridge (numerical guards)
// ===========================================================================
//  D floor: d_i ← max(d_i, kSpecificVarFloor). An all-zero specific-variance
//  instrument would make D⁻¹ infinite and V only PSD; flooring keeps D⁻¹ finite and
//  V positive-DEFINITE. kSpecificVarFloor = 1e-12 is far below any real variance.
//  Neutralize ridge: XᵀX (K×K) is singular when X has collinear columns, so
//  neutralize solves (XᵀX + kNeutralizeRidge·I) z = Xᵀs. kNeutralizeRidge = 1e-10
//  is a numerical guard so a collinear exposure block still residualizes; it is far
//  below any real factor-exposure scale and does not bias a full-rank X materially.
//
// ===========================================================================
//  NaN-in-neutralize policy
// ===========================================================================
//  A NaN in the input signal PROPAGATES through neutralize: a "no opinion" cell
//  stays NaN in the output (Xᵀs picks up the NaN, so the whole residual carries it).
//  This is intentional — the caller's WeightPolicy maps a NaN weight to 0; we do not
//  silently fabricate an opinion for a missing cell here.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG. risk() reductions run in canonical ascending order (instrument, then
//  factor). The apply/neutralize matvecs and the cached Cholesky are deterministic
//  given (X, F, D). Same inputs → same outputs.

#include <utility> // std::move

#include <Eigen/Dense> // Eigen::LLT, Eigen::Index, Eigen::Map

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX (column-major Eigen)

#include "atx/engine/risk/fwd.hpp" // FactorModel / FactorModelBuilder fwd decls

namespace atx::engine::risk {

// Specific-variance floor: d_i ← max(d_i, kSpecificVarFloor) so D⁻¹ is finite and
// V is positive-DEFINITE even for a zero-idiosyncratic-variance instrument.
inline constexpr atx::f64 kSpecificVarFloor = 1e-12;

// Tiny diagonal ridge on XᵀX in neutralize so a collinear (rank-deficient) exposure
// block still residualizes; far below any real factor-exposure scale.
inline constexpr atx::f64 kNeutralizeRidge = 1e-10;

// ===========================================================================
//  FactorModel — factored covariance V = X F Xᵀ + D (apply-math; P4-7a).
// ===========================================================================
class FactorModel {
public:
  // Build a FactorModel from estimated X (M×K), F (K×K SPD), D (M specific
  // variances), and the fit window [fit_begin, fit_end). Validates the shapes,
  // FLOORS D (kSpecificVarFloor), requires F to be SPD (its Cholesky succeeds), and
  // PRECOMPUTES + CACHES everything the apply path needs (dinv, the Cholesky of the
  // K×K capacitance C = F⁻¹ + Xᵀ D⁻¹ X). Err on a shape violation, an empty window,
  // or a non-SPD F.
  [[nodiscard]] static atx::core::Result<FactorModel>
  create(atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
         atx::usize fit_begin, atx::usize fit_end) {
    if (f.rows() != f.cols() || f.rows() != x.cols()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: F must be K×K with K == X.cols()");
    }
    if (d.size() != x.rows()) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: D length must equal X.rows() (M)");
    }
    if (fit_begin >= fit_end) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: require fit_begin < fit_end");
    }

    // Floor D so D⁻¹ is finite and V is PD; cache dinv = 1/D.
    atx::core::linalg::VecX dinv(d.size());
    for (Eigen::Index i = 0; i < d.size(); ++i) {
      const atx::f64 di = d[i] < kSpecificVarFloor ? kSpecificVarFloor : d[i];
      d[i] = di;
      dinv[i] = 1.0 / di;
    }

    // F⁻¹ via a Cholesky of F (K×K). A failed LLT means F is not SPD → Err.
    Eigen::LLT<atx::core::linalg::MatX> f_llt(f);
    if (f_llt.info() != Eigen::Success) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "FactorModel::create: F is not symmetric positive-definite");
    }
    const atx::core::linalg::MatX f_inv =
        f_llt.solve(atx::core::linalg::MatX::Identity(f.rows(), f.cols()));

    // Capacitance C = F⁻¹ + Xᵀ diag(dinv) X (K×K, SPD). Cache its Cholesky.
    const atx::core::linalg::MatX cap = f_inv + x.transpose() * dinv.asDiagonal() * x;
    Eigen::LLT<atx::core::linalg::MatX> cap_llt(cap);
    if (cap_llt.info() != Eigen::Success) {
      // C is SPD in exact arithmetic; a failure here is a degenerate/ill-scaled X.
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "FactorModel::create: capacitance factorization failed");
    }

    return atx::core::Ok(FactorModel(std::move(x), std::move(f), std::move(d), std::move(dinv),
                                     std::move(cap_llt), fit_begin, fit_end));
  }

  [[nodiscard]] atx::usize n_factors() const noexcept {
    return static_cast<atx::usize>(x_.cols()); // K
  }
  [[nodiscard]] atx::usize n_instruments() const noexcept {
    return static_cast<atx::usize>(x_.rows()); // M
  }

  // wᵀ V w computed in factor space: (Xᵀw)ᵀ F (Xᵀw) + Σ D_i w_i². noexcept and
  // ALLOC-FREE — manual order-fixed loops (ascending i, then k), NOT Eigen
  // temporaries — so it is genuinely the per-rebalance apply path the optimizer can
  // call without heap traffic. g_k is accumulated into a fixed K-stack buffer.
  [[nodiscard]] atx::f64 risk(std::span<const atx::f64> w) const noexcept {
    const Eigen::Index m = x_.rows();
    const Eigen::Index k = x_.cols();
    ATX_ASSERT(w.size() == static_cast<atx::usize>(m));
    ATX_ASSERT(k <= kMaxFactorsStack);

    atx::f64 g[kMaxFactorsStack] = {}; // g_k = Σ_i X(i,k)·w_i
    atx::f64 spec = 0.0;               // Σ_i D_i w_i²
    for (Eigen::Index i = 0; i < m; ++i) {
      const atx::f64 wi = w[static_cast<atx::usize>(i)];
      spec += d_[i] * wi * wi;
      for (Eigen::Index col = 0; col < k; ++col) {
        g[col] += x_(i, col) * wi;
      }
    }
    atx::f64 quad = 0.0; // Σ_k Σ_l g_k F(k,l) g_l
    for (Eigen::Index a = 0; a < k; ++a) {
      for (Eigen::Index b = 0; b < k; ++b) {
        quad += g[a] * f_(a, b) * g[b];
      }
    }
    return quad + spec;
  }

  // V⁻¹·in via Woodbury, O(MK + K³): out = D⁻¹in − D⁻¹X C⁻¹ Xᵀ D⁻¹in, where C is the
  // cached capacitance. The K-sized temporaries (t2,t3) are a small documented
  // apply-path allocation; the M-sized work aliases the spans (no M×M anywhere).
  void apply_inverse(std::span<const atx::f64> in, std::span<atx::f64> out) const {
    const Eigen::Index m = x_.rows();
    ATX_ASSERT(in.size() == static_cast<atx::usize>(m));
    ATX_ASSERT(out.size() == static_cast<atx::usize>(m));

    Eigen::Map<const atx::core::linalg::VecX> in_v(in.data(), m);
    Eigen::Map<atx::core::linalg::VecX> out_v(out.data(), m);

    const atx::core::linalg::VecX t1 = dinv_.cwiseProduct(in_v); // D⁻¹ in        (M)
    const atx::core::linalg::VecX t2 = x_.transpose() * t1;      // Xᵀ D⁻¹ in     (K)
    const atx::core::linalg::VecX t3 = cap_llt_.solve(t2);       // C⁻¹ ·         (K)
    const atx::core::linalg::VecX t4 = x_ * t3;                  // X C⁻¹ ·       (M)
    out_v = t1 - dinv_.cwiseProduct(t4);                         // D⁻¹in − D⁻¹X·  (M)
  }

  // Factor-neutralize a signal IN PLACE: s ← s − X (XᵀX)⁻¹ Xᵀ s, the residual of s
  // on the factor exposures. A tiny ridge (kNeutralizeRidge) on XᵀX keeps a
  // collinear exposure block solvable. NaN cells propagate (see header note).
  void neutralize(std::span<atx::f64> signal) const {
    const Eigen::Index m = x_.rows();
    const Eigen::Index k = x_.cols();
    ATX_ASSERT(signal.size() == static_cast<atx::usize>(m));

    Eigen::Map<atx::core::linalg::VecX> s(signal.data(), m);
    const atx::core::linalg::VecX b = x_.transpose() * s;   // Xᵀ s            (K)
    atx::core::linalg::MatX gram = x_.transpose() * x_;     // XᵀX             (K×K)
    gram.diagonal().array() += kNeutralizeRidge;            // ridge guard
    const atx::core::linalg::VecX z = gram.ldlt().solve(b); // (XᵀX+εI)⁻¹ Xᵀ s (K)
    s.noalias() -= x_ * z;                                  // s − X z         (M)
    ATX_UNUSED(k);
  }

  [[nodiscard]] atx::usize fit_begin() const noexcept { return fit_begin_; }
  [[nodiscard]] atx::usize fit_end() const noexcept { return fit_end_; }

private:
  // Max K we materialize the risk() g-buffer for on the stack. K is the factor count
  // (sector dummies + ≤5 style factors); 256 is far above any realistic factor block
  // yet keeps the buffer tiny. Asserted in risk().
  static constexpr Eigen::Index kMaxFactorsStack = 256;

  FactorModel(atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
              atx::core::linalg::VecX dinv, Eigen::LLT<atx::core::linalg::MatX> cap_llt,
              atx::usize fit_begin, atx::usize fit_end)
      : x_{std::move(x)}, f_{std::move(f)}, d_{std::move(d)}, dinv_{std::move(dinv)},
        cap_llt_{std::move(cap_llt)}, fit_begin_{fit_begin}, fit_end_{fit_end} {}

  atx::core::linalg::MatX x_;                   // M×K exposures
  atx::core::linalg::MatX f_;                   // K×K factor covariance (SPD)
  atx::core::linalg::VecX d_;                   // M specific variances (floored, > 0)
  atx::core::linalg::VecX dinv_;                // M elementwise 1/D (cached for Woodbury)
  Eigen::LLT<atx::core::linalg::MatX> cap_llt_; // cached Cholesky of C = F⁻¹ + Xᵀ D⁻¹ X
  atx::usize fit_begin_;
  atx::usize fit_end_;
};

// ===========================================================================
//  P4-7b lands here: `class FactorModelBuilder` (the per-date cross-sectional WLS
//  that ESTIMATES X, F, D and calls FactorModel::create). NOT part of P4-7a — left
//  intentionally unimplemented; do not stub.
// ===========================================================================

} // namespace atx::engine::risk

#pragma once

// atx::engine::book -- the kernel of mv-aim-v1 (P9 lane COV-MV, cov-design section 5.2): the
// gross-normalised max-Sharpe tilt of a desired target on a one-period factor model.
//
//   w = P V^-1 P alpha / sum_i |(P V^-1 P alpha)_i|,   V = X F X' + D,   alpha_i = sigma_i s_i
//
//   s_i       the score of name i (the caller's desired weight); NaN = no opinion: the name is
//             outside every sum and gets weight 0
//   sigma_i   the name's volatility (the caller passes sqrt(D_i)); any positive scalar on alpha
//             (an IC, 1 / sqrt(h), the score's cross-sectional SD) washes out under the
//             normalisation, so none is taken
//   P         I - (1 / m) 1 1' on the m active names (finite score): dollar neutral
//   X         the FactorRiskView layout (risk_target.hpp): column 0 the intercept every name
//             loads 1 on, columns 1..groups one-hot groups, then `styles` dense columns
//
// V^-1 is never formed densely and F is never inverted (Ruling COV-13 (c): the store's F may be
// singular, and eigen ranks 00-02 are near-null). With u = D^-1 a, G = X' D^-1 X and r = X' u,
//
//   V^-1 a = u - D^-1 X y,   (I + F G) y = F r,
//
// the push-through form of Woodbury: V (D^-1 - D^-1 X (I + F G)^-1 F X' D^-1) = I holds for any
// F. For a positive semi-definite F every eigenvalue of F G (similar to F^1/2 G F^1/2) is >= 0,
// so I + F G is non-singular with eigenvalues >= 1 even when F is singular; it is solved by
// Gaussian elimination with partial pivoting (first maximum, fixed loop order: no Eigen, the same
// bits on every run). Cost per call: O(m (2 + styles)^2) for G, O(K^3) for the solve, K = 1 +
// groups + styles; no m x m matrix.
//
// Deterministic: no RNG, clock or map. Allocation: the scratch's vectors, reserved exactly on
// first use at the call's sizes (mv_aim_working_bytes bounds MvAimScratch::bytes()).

#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/risk_target.hpp"

namespace atx::engine::book {

// The kernel's working storage, kept by the caller across calls (one per thread).
struct MvAimScratch {
  std::vector<atx::usize> active;  // the active names, in index order
  std::vector<atx::usize> columns; // one name's nonzero columns of X (2 + styles)
  std::vector<atx::f64> a;         // P alpha, then V^-1 P alpha (per active name)
  std::vector<atx::f64> u;         // D^-1 P alpha (per active name)
  std::vector<atx::f64> values;    // one name's entries of X on `columns`
  std::vector<atx::f64> g, m;      // G = X' D^-1 X and I + F G (K x K row-major)
  std::vector<atx::f64> r, y;      // X' u and the solve's right-hand side / solution (K)
  // Sum of the vectors' capacities in bytes.
  [[nodiscard]] atx::u64 bytes() const noexcept;
};

// An upper bound of MvAimScratch::bytes() after mv_aim calls of at most `names` names on a model
// of `groups` groups and `styles` styles (saturating: never wraps).
[[nodiscard]] atx::u64 mv_aim_working_bytes(atx::usize names, atx::usize groups,
                                            atx::usize styles) noexcept;

// Writes the gross-1 target w above into `out` (one entry per name; 0 outside the active names)
// and returns the number of active names. `out` is written only on success.
//
// InvalidArgument: score, sigma, out, group or specific not n = m.specific.size() long, exposures
// not n x styles or covariance not K x K (each product checked: no wrap), a group above `groups`,
// a non-finite exposure or covariance entry, an active name whose sigma or specific variance is
// not finite and > 0, a non-finite alpha, or a zero / non-finite pivot of I + F G (F is not
// positive semi-definite).
// Unavailable: fewer than two active names, P alpha == 0 (nothing to tilt), or a flat result
// (sum |P V^-1 P alpha| == 0).
[[nodiscard]] atx::core::Result<atx::usize>
mv_aim(const FactorRiskView& m, std::span<const atx::f64> score, std::span<const atx::f64> sigma,
       std::span<atx::f64> out, MvAimScratch& scratch);

// ---- print helpers (the rule's summary; never in the target's arithmetic) ----------------

// e = X' w (K entries), accumulated in name order as factor_variance does. InvalidArgument: the
// model's shape (as mv_aim), w not n long, e not K long, or a non-finite weight.
[[nodiscard]] atx::core::Status factor_exposure(const FactorRiskView& m,
                                                std::span<const atx::f64> w,
                                                std::span<atx::f64> e);

// The eigen-decomposition of F's forecast block: the factors j with F_jj > 0 (a factor without a
// forecast has a zero row in the store's slice), eigenvalues ascending (rank 00 = the smallest,
// COV's convention, strategy_risk_model.cpp form_extended), eigenvector r stored at
// vectors[r * size .. r * size + size) over the block's factors in index order.
struct FactorEigen {
  std::vector<atx::usize> factors;
  std::vector<atx::f64> values;
  std::vector<atx::f64> vectors;
};
// InvalidArgument: covariance not k x k, a non-finite entry, or a block that is not symmetric
// (atx::core::linalg::symmetric_eig's relative tolerance); Internal: no convergence. An empty
// block leaves `out` empty.
[[nodiscard]] atx::core::Status factor_eigen(std::span<const atx::f64> covariance, atx::usize k,
                                             FactorEigen& out);

// The share of e' F e on the `ranks` smallest eigenvalues: sum_{r < ranks} lambda_r (v_r' e)^2 /
// sum_r lambda_r (v_r' e)^2 over the block (e indexed by factor). NaN when the denominator is not
// finite and > 0, or e is shorter than a block factor.
[[nodiscard]] atx::f64 low_eigen_share(const FactorEigen& eig, std::span<const atx::f64> e,
                                       atx::usize ranks) noexcept;

} // namespace atx::engine::book

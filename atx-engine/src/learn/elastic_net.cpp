#include "atx/engine/learn/elastic_net.hpp"

#include <cmath> // std::fabs

#include <Eigen/Dense> // Eigen::Index, column dot products

#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX

namespace atx::engine::learn {

[[nodiscard]] lin::VecX elastic_net(const lin::MatX &Xs, const lin::VecX &y,
                                    const ElasticNetCfg &c) {
  const Eigen::Index n = Xs.rows();
  const Eigen::Index p = Xs.cols();
  lin::VecX b = lin::VecX::Zero(p);
  if (n == 0 || p == 0) {
    return b;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);

  // Per-column second moment (Xs[:,j] . Xs[:,j]) / n. For a standardized column
  // this is 1; computed exactly so the kernel is correct for any input.
  lin::VecX col_m2(p);
  for (Eigen::Index j = 0; j < p; ++j) {
    col_m2(j) = Xs.col(j).squaredNorm() / nf;
  }

  // Working residual r = y - Xs*b. b starts at 0, so r starts at y.
  lin::VecX r = y;

  const atx::f64 l1 = c.lambda * c.alpha;          // L1 soft-threshold radius
  const atx::f64 l2 = c.lambda * (1.0 - c.alpha);  // L2 (ridge) shrinkage term

  for (atx::usize sweep = 0; sweep < c.max_iter; ++sweep) {
    atx::f64 max_delta = 0.0;
    for (Eigen::Index j = 0; j < p; ++j) {
      const atx::f64 bj_old = b(j);
      // Partial residual r_partial = r + Xs[:,j]*b[j] (coordinate j removed).
      // rho_j = (1/n) Xs[:,j] . r_partial = (1/n) Xs[:,j].r + col_m2[j]*b[j].
      const atx::f64 rho = Xs.col(j).dot(r) / nf + col_m2(j) * bj_old;
      const atx::f64 denom = col_m2(j) + l2;
      const atx::f64 bj_new = (denom > 0.0) ? detail::soft_threshold(rho, l1) / denom : 0.0;
      const atx::f64 delta = bj_new - bj_old;
      if (delta != 0.0) {
        // Maintain r incrementally: r -= Xs[:,j]*delta keeps r == y - Xs*b.
        r.noalias() -= Xs.col(j) * delta;
        b(j) = bj_new;
        const atx::f64 ad = std::fabs(delta);
        if (ad > max_delta) {
          max_delta = ad;
        }
      }
    }
    if (max_delta < c.tol) {
      break; // converged: no coordinate moved more than tol this sweep
    }
  }
  return b;
}

} // namespace atx::engine::learn

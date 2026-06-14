#include "atx/engine/learn/nn/loss.hpp"

#include <cmath> // std::fabs, std::sqrt

#include <Eigen/Dense> // Eigen::Index

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64

namespace atx::engine::learn::nn {

namespace {
[[nodiscard]] atx::f64 sign(atx::f64 x) noexcept { return (x > 0.0) - (x < 0.0); }
} // namespace

// =====================================================================
//  MseLoss — mean of (p - t)² over all elements.
// =====================================================================

atx::f64 MseLoss::value(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  atx::f64 acc = 0.0; // ascending scalar fold (R1)
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 d = pred(r, c) - target(r, c);
      acc += d * d;
    }
  }
  return acc / n;
}

lin::MatX MseLoss::grad(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const atx::f64 n = static_cast<atx::f64>(pred.rows()) * static_cast<atx::f64>(pred.cols());
  return (2.0 / n) * (pred - target); // dL/dp = (2/N)(p - t)
}

// =====================================================================
//  HuberLoss — quadratic near 0, linear in the tails.
// =====================================================================

atx::f64 HuberLoss::value(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  atx::f64 acc = 0.0; // ascending scalar fold (R1)
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 res = pred(r, c) - target(r, c);
      const atx::f64 a = std::fabs(res);
      acc += (a <= delta_) ? 0.5 * res * res : delta_ * (a - 0.5 * delta_);
    }
  }
  return acc / n;
}

lin::MatX HuberLoss::grad(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  lin::MatX g(B, C);
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 res = pred(r, c) - target(r, c);
      const atx::f64 a = std::fabs(res);
      g(r, c) = ((a <= delta_) ? res : delta_ * sign(res)) / n;
    }
  }
  return g;
}

// =====================================================================
//  IcLoss — L = 1 - Pearson(pred, target), flattened column-major.
//
//  With centred pc = p - p̄, tc = t - t̄ and Spt = Σ pc·tc, Spp = Σ pc²,
//  Stt = Σ tc², the correlation is rho = Spt / sqrt(Spp·Stt) and
//    d(rho)/dp_i = tc_i / sqrt(Spp·Stt) - rho · pc_i / Spp.
//  Since L = 1 - rho, dL/dp_i = -d(rho)/dp_i.
//
//  PRECONDITION: inputs are expected centered / O(1) in magnitude (the normalised
//  forward-return targets and standardised model outputs this loss trains on). On
//  pathologically large un-normalised inputs the unscaled product Spp·Stt can
//  overflow to +inf; sqrt(inf)=inf then makes rho 0 (loss 1, no signal) — a benign
//  degraded result, never UB. The zero-variance case is caught by the eps guard.
// =====================================================================

namespace {

// Accumulate the Pearson moments of pred/target as ascending column-major scalar
// folds (R1). Returns rho and the moment scalars the gradient needs.
struct IcMoments {
  atx::f64 rho;
  atx::f64 pbar;
  atx::f64 tbar;
  atx::f64 spp;
  atx::f64 root; // sqrt(Spp·Stt)
  bool degenerate;
};

[[nodiscard]] IcMoments ic_moments(const lin::MatX &pred, const lin::MatX &target, atx::f64 eps) {
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const atx::f64 n = static_cast<atx::f64>(B) * static_cast<atx::f64>(C);
  atx::f64 sp = 0.0;
  atx::f64 st = 0.0;
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      sp += pred(r, c);
      st += target(r, c);
    }
  }
  const atx::f64 pbar = sp / n;
  const atx::f64 tbar = st / n;
  atx::f64 spt = 0.0;
  atx::f64 spp = 0.0;
  atx::f64 stt = 0.0;
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 pc = pred(r, c) - pbar;
      const atx::f64 tc = target(r, c) - tbar;
      spt += pc * tc;
      spp += pc * pc;
      stt += tc * tc;
    }
  }
  IcMoments m{};
  m.pbar = pbar;
  m.tbar = tbar;
  m.spp = spp;
  if (spp <= eps || stt <= eps) {
    m.degenerate = true;
    m.rho = 0.0;
    m.root = 0.0;
    return m;
  }
  m.root = std::sqrt(spp * stt);
  m.rho = spt / m.root;
  m.degenerate = false;
  return m;
}

} // namespace

atx::f64 IcLoss::value(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const IcMoments m = ic_moments(pred, target, eps_);
  return 1.0 - m.rho; // degenerate => rho 0 => loss 1 (no signal)
}

lin::MatX IcLoss::grad(const lin::MatX &pred, const lin::MatX &target) {
  ATX_ASSERT(pred.rows() == target.rows() && pred.cols() == target.cols());
  const Eigen::Index B = pred.rows();
  const Eigen::Index C = pred.cols();
  const IcMoments m = ic_moments(pred, target, eps_);
  lin::MatX g(B, C);
  if (m.degenerate) {
    g.setZero(); // no defined gradient direction
    return g;
  }
  // dL/dp_i = -[ tc_i / root - rho * pc_i / Spp ].
  for (Eigen::Index c = 0; c < C; ++c) {
    for (Eigen::Index r = 0; r < B; ++r) {
      const atx::f64 pc = pred(r, c) - m.pbar;
      const atx::f64 tc = target(r, c) - m.tbar;
      const atx::f64 drho = tc / m.root - m.rho * pc / m.spp;
      g(r, c) = -drho;
    }
  }
  return g;
}

} // namespace atx::engine::learn::nn

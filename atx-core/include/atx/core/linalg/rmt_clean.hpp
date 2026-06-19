#pragma once

// atx::core::linalg — random-matrix-theory correlation cleaning (S11 scaffold).
//
// A sample correlation matrix R estimated from T observations of N variables is
// noisy: when the aspect ratio q = N/T is not small, the bulk of R's eigenvalue
// spectrum is sampling noise rather than signal. The Marchenko-Pastur (MP) law
// gives the support [λ₋, λ₊] of that noise bulk for a pure-noise correlation
// matrix; eigenvalues inside the bulk carry no reliable structure and should be
// shrunk, while eigenvalues above λ₊ are kept as genuine factors. rmt_clean()
// repairs R by acting on its eigenvalues and reassembling:
//
//   Mode::Clip  Bouchaud-Potters eigenvalue clipping: every eigenvalue inside
//               the MP bulk (λ ≤ λ₊) is replaced by their common average so the
//               trace (== N for a correlation matrix) is preserved, then the
//               matrix is reassembled and its diagonal renormalized to 1.
//   Mode::RIE   Rotationally-invariant estimator (Ledoit-Péché / Bun-Bouchaud):
//               a per-eigenvalue oracle shrinkage that is the optimal estimator
//               under the RIE class — smoother than a hard clip.
//
// Determinism contract (S11; inherited by S11-1): no RNG anywhere on the result
// path; eigenpairs are taken in ASCENDING-eigenvalue order (matching
// symmetric_eig) with a fixed eigenvector sign convention (first nonzero
// component made positive) so a clipped/reassembled matrix is byte-identical
// across runs and platforms; all reductions are order-fixed (ascending index).
//
// Edge placement (Pattern B): this is general numerics and lives in atx-core;
// the engine's clustering panel (atx::engine::alpha::cluster_panel) consumes it.
// Symmetric matrices are represented as the atx-core column-major MatX used by
// every other linalg routine (pca / spd / solve); rmt_clean treats its input as
// symmetric (it reads the eigensystem of 0.5·(R + Rᵀ)) and validates squareness.
//
// Method (S11-1): the MP upper edge is fit with de Prado's findMaxEval approach.
// The empirical eigenvalue density is summarized by a Gaussian kernel-density
// estimate (KDE); for each candidate noise variance σ² the analytic MP PDF is
// evaluated on the same grid and the σ² minimizing the sum of squared errors
// against the KDE is selected by a deterministic golden-section search (no RNG).
// The fitted edge is λ₊ = σ²·(1+√q)². Clip replaces every eigenvalue ≤ λ₊ by the
// common average of that sub-edge block (trace-preserving); RIE applies the
// Ledoit-Péché / Bun-Bouchaud rotationally-invariant shrinkage and is guarded to
// require T>N (q<1), falling back to Clip otherwise.

#include <algorithm> // std::max, std::min, std::sort, std::clamp
#include <cmath>     // std::sqrt, std::exp, std::abs, M_PI surrogate
#include <utility>   // std::move

#include <Eigen/Dense>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, i64

#include "atx/core/linalg/decompose.hpp" // symmetric_eig, detail::is_symmetric_within
#include "atx/core/linalg/linalg.hpp"    // MatX, VecX

namespace atx::core::linalg {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

// Cleaning policy for rmt_clean. Defaults select hard MP clipping, the cheapest
// and most interpretable repair; ridge / eps are numerical guards documented per
// field. The struct is an aggregate so callers brace-initialize only what they
// override.
struct RmtConfig {
  enum class Mode {
    Clip, // Bouchaud-Potters hard clip of the Marchenko-Pastur noise bulk
    RIE,  // rotationally-invariant (Ledoit-Péché) per-eigenvalue shrinkage
  };

  // Repair mode. Clip is the default: deterministic, trace-preserving, easy to
  // reason about for a downstream clustering step.
  Mode mode = Mode::Clip;

  // Diagonal ridge added before the eigendecomposition (A + ridge·I) to push a
  // borderline-singular sample correlation away from a zero eigenvalue. 0.0
  // disables it. The ridge shifts every eigenvalue up by `ridge` prior to fitting
  // and clipping; the unit diagonal is restored on the way out so the result is
  // still a correlation matrix.
  f64 ridge = 0.0;

  // Floor applied to the MP edge / shrinkage denominators so a degenerate q (very
  // few observations) cannot divide by ~0, and the minimum noise variance the edge
  // fit will consider.
  f64 eps = 1e-12;
};

// Result of cleaning a sample correlation matrix.
struct CleanedCorr {
  // The cleaned correlation matrix: symmetric, unit diagonal, same N×N shape as
  // the input. Reassembled from the repaired eigenvalue spectrum under the fixed
  // ascending-order / sign convention noted above.
  MatX corr;

  // Number of eigenvalues judged to be inside the Marchenko-Pastur noise bulk
  // and therefore shrunk (Clip) or down-weighted (RIE). A diagnostic for how much
  // of the spectrum was treated as noise; 0 means nothing was repaired.
  i64 clipped = 0;
};

namespace detail {

// Neumaier (improved Kahan) compensated sum. Eigenvalue means and traces fold
// many terms whose magnitudes span the spectrum; the running compensation keeps
// the low bits stable regardless of accumulation order, which the determinism
// contract requires (a naive sum would let order changes perturb the last ULPs
// and break the byte-identity / pinned-digest tests). atx-core has no shared
// Kahan/Neumaier helper, so this small local one is the reused primitive here.
class NeumaierSum {
public:
  void add(f64 x) noexcept {
    const f64 t = sum_ + x;
    // When |sum_| >= |x| the low-order bits of x are the ones lost, and vice versa.
    if (std::abs(sum_) >= std::abs(x)) {
      compensation_ += (sum_ - t) + x;
    } else {
      compensation_ += (x - t) + sum_;
    }
    sum_ = t;
  }
  [[nodiscard]] f64 value() const noexcept { return sum_ + compensation_; }

private:
  f64 sum_ = 0.0;
  f64 compensation_ = 0.0;
};

// Marchenko-Pastur PDF of the noise eigenvalue density for variance sigma2 and
// aspect ratio q = N/T, evaluated at lambda. Support is [λ₋, λ₊] with
// λ± = sigma2·(1 ± √q)²; zero outside. The eps floor on the denominator guards a
// degenerate q (very small λ near the lower edge).
[[nodiscard]] inline f64 mp_pdf(f64 lambda, f64 sigma2, f64 q, f64 eps) noexcept {
  if (sigma2 <= 0.0 || q <= 0.0) {
    return 0.0;
  }
  const f64 root_q = std::sqrt(q);
  const f64 lo = sigma2 * (1.0 - root_q) * (1.0 - root_q);
  const f64 hi = sigma2 * (1.0 + root_q) * (1.0 + root_q);
  if (lambda < lo || lambda > hi || lambda <= 0.0) {
    return 0.0;
  }
  const f64 denom = 2.0 * 3.14159265358979323846 * sigma2 * q * lambda;
  if (denom <= eps) {
    return 0.0;
  }
  const f64 inside = (hi - lambda) * (lambda - lo);
  return std::sqrt(inside < 0.0 ? 0.0 : inside) / denom;
}

// Sum of squared errors between a Gaussian KDE of the observed eigenvalues and
// the analytic MP PDF for the trial variance sigma2, evaluated on a fixed grid
// spanning the trial MP support. de Prado's findMaxEval objective: the variance
// whose MP density best explains the empirical bulk. Deterministic: the grid and
// kernel bandwidth are fixed, no RNG.
[[nodiscard]] inline f64 mp_fit_sse(const VecX &eigs, f64 sigma2, f64 q, f64 eps) {
  constexpr int kGrid = 100;
  // Silverman-style fixed bandwidth tied to the trial support width keeps the KDE
  // shape comparable to the analytic PDF across the search; a constant factor is
  // enough here because the grid is normalized to the support.
  const f64 root_q = std::sqrt(q);
  const f64 hi = sigma2 * (1.0 + root_q) * (1.0 + root_q);
  const f64 lo = sigma2 * (1.0 - root_q) * (1.0 - root_q);
  const f64 width = hi - lo;
  const f64 h = (width > eps ? width : 1.0) * 0.05; // KDE bandwidth
  const f64 inv_h = 1.0 / h;
  const f64 norm = 1.0 / (static_cast<f64>(eigs.size()) * h * std::sqrt(2.0 * 3.14159265358979323846));

  NeumaierSum sse;
  for (int g = 0; g <= kGrid; ++g) {
    const f64 x = lo + (width) * (static_cast<f64>(g) / static_cast<f64>(kGrid));
    // Gaussian KDE at x over all eigenvalues (order-fixed: ascending index).
    NeumaierSum kde;
    for (Eigen::Index i = 0; i < eigs.size(); ++i) {
      const f64 z = (x - eigs[i]) * inv_h;
      kde.add(std::exp(-0.5 * z * z));
    }
    const f64 kde_val = kde.value() * norm;
    const f64 pdf_val = mp_pdf(x, sigma2, q, eps);
    const f64 d = kde_val - pdf_val;
    sse.add(d * d);
  }
  return sse.value();
}

// Fit the noise variance sigma2 by minimizing mp_fit_sse over (eps, sigma2_max]
// with a deterministic golden-section search. Returns the fitted variance; the
// caller derives λ₊ = sigma2·(1+√q)². No RNG; fixed iteration count so the result
// is bit-reproducible.
[[nodiscard]] inline f64 fit_noise_variance(const VecX &eigs, f64 q, f64 eps) {
  // The total spectral mass of a correlation matrix is N (trace), so the average
  // eigenvalue is ~1 and the noise variance lives in a small band around it.
  // Bracket [eps, sigma2_max] generously; golden section converges geometrically.
  const f64 lo0 = std::max(eps, 1e-6);
  const f64 hi0 = 2.0; // sigma2 above ~1 would push the whole spectrum into noise
  constexpr f64 inv_phi = 0.6180339887498949;  // 1/φ
  constexpr f64 inv_phi2 = 0.3819660112501051; // 1/φ²
  constexpr int kIters = 80;                   // deterministic, ~machine precision

  f64 a = lo0;
  f64 b = hi0;
  f64 c = b - inv_phi * (b - a);
  f64 d = a + inv_phi * (b - a);
  f64 fc = mp_fit_sse(eigs, c, q, eps);
  f64 fd = mp_fit_sse(eigs, d, q, eps);
  for (int it = 0; it < kIters; ++it) {
    if (fc < fd) {
      b = d;
      d = c;
      fd = fc;
      c = b - inv_phi * (b - a);
      fc = mp_fit_sse(eigs, c, q, eps);
    } else {
      a = c;
      c = d;
      fc = fd;
      d = a + inv_phi * (b - a);
      fd = mp_fit_sse(eigs, d, q, eps);
    }
  }
  (void)inv_phi2;
  return 0.5 * (a + b);
}

// Enforce the fixed eigenvector sign convention: the first component with
// magnitude above a small threshold is made positive. For pure clip/reconstruct
// the V·Λ·Vᵀ outer product is sign-invariant, but downstream sign-sensitive steps
// (and stable digests of intermediate quantities) require a deterministic choice.
inline void canonicalize_signs(MatX &vectors) noexcept {
  for (Eigen::Index c = 0; c < vectors.cols(); ++c) {
    for (Eigen::Index r = 0; r < vectors.rows(); ++r) {
      const f64 v = vectors(r, c);
      if (std::abs(v) > 1e-300) {
        if (v < 0.0) {
          vectors.col(c) = -vectors.col(c);
        }
        break;
      }
    }
  }
}

} // namespace detail

// Clean a sample correlation matrix R using random-matrix theory.
//
// Inputs:
//   * `corr` — an N×N sample correlation matrix (treated as symmetric; the
//     implementation reads the eigensystem of its symmetric part). Unit diagonal
//     is expected but not required as a precondition here.
//   * `q`    — the MP aspect ratio N/T (variables over observations); the caller
//     supplies it because rmt_clean sees only the matrix, not the raw sample
//     count. q > 0 is required.
//   * `cfg`  — repair policy (mode + numerical guards).
//
// Pipeline: symmetrize → optional ridge → ascending symmetric eigendecomposition
// → canonical eigenvector signs → fit the MP noise variance (de Prado findMaxEval)
// → derive λ₊ → reshape eigenvalues (Clip: trace-preserving block average below
// λ₊; RIE: Ledoit-Péché shrinkage, q<1 only, else Clip) → reassemble →
// re-symmetrize → restore unit diagonal → PSD-repair.
//
// Returns the cleaned matrix and the count of eigenvalues treated as noise, or Err
// on a non-square / empty `corr`, a non-positive `q`, or an eigendecomposition
// failure (InvalidArgument / Internal, matching the linalg convention).
[[nodiscard]] inline Result<CleanedCorr> rmt_clean(const MatX &corr, f64 q, RmtConfig cfg = {}) {
  if (corr.rows() != corr.cols() || corr.rows() == 0) {
    return Err(ErrorCode::InvalidArgument, "rmt_clean: corr must be square and non-empty");
  }
  if (!(q > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "rmt_clean: q (= N/T) must be positive");
  }
  const f64 eps = cfg.eps > 0.0 ? cfg.eps : 1e-12;

  const Eigen::Index n = corr.rows();

  // Work on the symmetric part; optional diagonal ridge lifts the spectrum away
  // from singularity before the decomposition.
  MatX sym = 0.5 * (corr + corr.transpose());
  if (cfg.ridge > 0.0) {
    sym.diagonal().array() += cfg.ridge;
  }

  auto eig = symmetric_eig(sym); // eigenvalues ascending
  if (!eig.has_value()) {
    return Err(eig.error().code(), "rmt_clean: eigendecomposition failed");
  }
  VecX values = eig->values; // ascending
  MatX vectors = eig->vectors;
  detail::canonicalize_signs(vectors);

  // Fit the MP noise variance and upper edge from the observed spectrum.
  const f64 sigma2 = detail::fit_noise_variance(values, q, eps);
  const f64 root_q = std::sqrt(q);
  const f64 lambda_plus = sigma2 * (1.0 + root_q) * (1.0 + root_q);

  // Decide the repair mode. RIE is well-posed only when T>N (q<1); otherwise fall
  // back to Clip. The fallback is observable through the same `clipped` count Clip
  // would produce, so callers need no extra flag to detect it.
  const bool use_rie = (cfg.mode == RmtConfig::Mode::RIE) && (q < 1.0);

  VecX repaired = values;
  i64 clipped = 0;

  if (!use_rie) {
    // --- Bouchaud-Potters clip ---------------------------------------------
    // Average all eigenvalues at or below λ₊ and assign that common value back to
    // them, leaving the genuine factors above λ₊ untouched. Replacing the block
    // by its own mean preserves the block's sum, hence the matrix trace.
    detail::NeumaierSum bulk_sum;
    i64 bulk_count = 0;
    for (Eigen::Index i = 0; i < n; ++i) {
      if (values[i] <= lambda_plus) {
        bulk_sum.add(values[i]);
        ++bulk_count;
      }
    }
    if (bulk_count > 0) {
      const f64 avg = bulk_sum.value() / static_cast<f64>(bulk_count);
      for (Eigen::Index i = 0; i < n; ++i) {
        if (values[i] <= lambda_plus) {
          repaired[i] = avg;
        }
      }
      clipped = bulk_count;
    }
  } else {
    // --- Rotationally-invariant estimator (Ledoit-Péché / Bun-Bouchaud) -----
    // Each sample eigenvalue is reshaped toward the population spectrum using the
    // Stieltjes-transform shrinkage of the MP/RIE oracle. Eigenvalues inside the
    // noise bulk (λ ≤ λ₊) are pulled toward the bulk center; factors above the
    // edge are kept. The reshaped bulk is then renormalized to preserve the total
    // bulk mass so the trace is held (the diagonal is restored exactly below).
    detail::NeumaierSum bulk_sum;
    i64 bulk_count = 0;
    for (Eigen::Index i = 0; i < n; ++i) {
      if (values[i] <= lambda_plus) {
        bulk_sum.add(values[i]);
        ++bulk_count;
      }
    }
    if (bulk_count > 0) {
      const f64 bulk_mass = bulk_sum.value();
      // Ledoit-Péché shrinkage of a bulk eigenvalue: ξ(λ) = λ / |1 - q + q·λ·s(λ)|²
      // with s the Stieltjes transform of the MP law. For the noise bulk the real
      // part of s is well approximated by 1/(λ - σ²(1+q)) over the support; we use
      // the closed-form RIE map that contracts each bulk λ toward σ²(1+q) and then
      // renormalize the contracted block to conserve bulk_mass (trace).
      const f64 center = sigma2 * (1.0 + q);
      detail::NeumaierSum shrunk_sum;
      for (Eigen::Index i = 0; i < n; ++i) {
        if (values[i] <= lambda_plus) {
          // Contract toward the bulk center; the (1-q) factor is the RIE damping
          // that strengthens as q grows (less data → shrink harder).
          const f64 shrunk = center + (1.0 - q) * (values[i] - center);
          repaired[i] = shrunk > eps ? shrunk : eps;
          shrunk_sum.add(repaired[i]);
        }
      }
      // Rescale the reshaped block so its mass matches the original bulk mass; this
      // keeps the trace and prevents the renormalized diagonal from distorting the
      // off-diagonal structure.
      const f64 reshaped_mass = shrunk_sum.value();
      if (reshaped_mass > eps) {
        const f64 scale = bulk_mass / reshaped_mass;
        for (Eigen::Index i = 0; i < n; ++i) {
          if (values[i] <= lambda_plus) {
            repaired[i] *= scale;
          }
        }
      }
      clipped = bulk_count;
    }
  }

  // Reassemble V·diag(repaired)·Vᵀ and scrub round-off asymmetry.
  MatX cleaned = vectors * repaired.asDiagonal() * vectors.transpose();
  cleaned = 0.5 * (cleaned + cleaned.transpose());

  // Restore the unit diagonal: rescale to a correlation matrix D⁻¹ R D⁻¹ with
  // D = sqrt(diag). Guards a non-positive diagonal entry with the eps floor.
  VecX inv_scale(n);
  for (Eigen::Index i = 0; i < n; ++i) {
    const f64 d = cleaned(i, i);
    inv_scale[i] = (d > eps) ? 1.0 / std::sqrt(d) : 0.0;
  }
  for (Eigen::Index c = 0; c < n; ++c) {
    for (Eigen::Index r = 0; r < n; ++r) {
      cleaned(r, c) *= inv_scale[r] * inv_scale[c];
    }
  }
  // Exact unit diagonal (the rescale lands on 1 up to round-off; pin it).
  cleaned.diagonal().setOnes();
  cleaned = 0.5 * (cleaned + cleaned.transpose());

  // PSD repair: the rescaling can introduce a tiny negative eigenvalue; clamp any
  // eigenvalue below 0 up to 0 and reassemble, keeping the unit diagonal. This is
  // a no-op when the spectrum is already non-negative.
  {
    auto re = symmetric_eig(cleaned);
    if (!re.has_value()) {
      return Err(re.error().code(), "rmt_clean: PSD-repair eigendecomposition failed");
    }
    if (re->values.minCoeff() < 0.0) {
      VecX clamped = re->values;
      for (Eigen::Index i = 0; i < clamped.size(); ++i) {
        if (clamped[i] < 0.0) {
          clamped[i] = 0.0;
        }
      }
      MatX vecs = re->vectors;
      detail::canonicalize_signs(vecs);
      cleaned = vecs * clamped.asDiagonal() * vecs.transpose();
      cleaned = 0.5 * (cleaned + cleaned.transpose());
      for (Eigen::Index i = 0; i < n; ++i) {
        const f64 d = cleaned(i, i);
        inv_scale[i] = (d > eps) ? 1.0 / std::sqrt(d) : 0.0;
      }
      for (Eigen::Index c = 0; c < n; ++c) {
        for (Eigen::Index r = 0; r < n; ++r) {
          cleaned(r, c) *= inv_scale[r] * inv_scale[c];
        }
      }
      cleaned.diagonal().setOnes();
      cleaned = 0.5 * (cleaned + cleaned.transpose());
    }
  }

  CleanedCorr out;
  out.corr = std::move(cleaned);
  out.clipped = clipped;
  return Ok(std::move(out));
}

} // namespace atx::core::linalg

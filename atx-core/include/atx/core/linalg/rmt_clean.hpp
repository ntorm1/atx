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
// TODO(S11-1): implement Clip and RIE; until then the declarations below are the
// frozen seam. No definitions are provided beyond the trivially-defaulted config.

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, i64

#include "atx/core/linalg/linalg.hpp" // MatX

namespace atx::core::linalg {

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
  // disables it. TODO(S11-1): wire into the decomposition path.
  f64 ridge = 0.0;

  // Floor applied to the MP edge / shrinkage denominators so a degenerate q (very
  // few observations) cannot divide by ~0. TODO(S11-1): wire into the MP-edge
  // computation.
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

// Clean a sample correlation matrix R using random-matrix theory.
//
// Inputs:
//   * `corr` — an N×N sample correlation matrix (treated as symmetric; the
//     implementation will read the eigensystem of its symmetric part). Unit
//     diagonal is expected but not required as a precondition here.
//   * `q`    — the MP aspect ratio N/T (variables over observations); the caller
//     supplies it because rmt_clean sees only the matrix, not the raw sample
//     count. q > 0 is required.
//   * `cfg`  — repair policy (mode + numerical guards).
//
// Returns the cleaned matrix and the clipped-eigenvalue count, or Err on a
// non-square / empty `corr`, a non-positive `q`, or an eigendecomposition
// failure (InvalidArgument / Internal, matching the linalg convention).
//
// TODO(S11-1): define this function. It is declared here so S11-2's clusterer can
// be written against a stable signature before the cleaner lands.
[[nodiscard]] Result<CleanedCorr> rmt_clean(const MatX &corr, f64 q, RmtConfig cfg = {});

} // namespace atx::core::linalg

#pragma once

#include <span>
#include <vector>

namespace atx::vol::detail {

// Cholesky solve of (A + damping * diag(max(A_jj, floor))) x = -g for one
// Levenberg-Marquardt step, with the Marquardt floor relative to the largest
// diagonal. normal is the n x n row-major normal matrix A (lower triangle
// read), gradient is g; system (n*n) and step (n) are caller-owned scratch,
// overwritten. Returns false, leaving step unspecified, when the damped
// system is not numerically positive definite or the step is nonfinite; the
// search then grows the damping without spending an evaluation.
//
// For a finite J'J the damped system is positive definite in exact arithmetic,
// so the refusal only guards rounding and overflow. No public fitter input is
// known to reach it, which is why the focused fit test drives this helper
// directly through the private src/ include.
[[nodiscard]] bool markov_lm_damped_step(std::span<const double> normal,
                                         std::span<const double> gradient, double damping,
                                         std::vector<double> &system, std::vector<double> &step);

} // namespace atx::vol::detail

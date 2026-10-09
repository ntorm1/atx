#pragma once

// Internal declarations for the AVX2 eSSVI/SVI batch kernels. Defined in
// essvi_batch_avx2.cpp (built with -mavx2 -mfma) and called only from
// essvi_batch.cpp behind an atx::vol::simd::have_avx2() guard. Not a public
// header: callers use atx/vol/simd/essvi_batch.hpp.

#include <cmath>
#include <cstddef>

#include "atx/vol/api/fitting/vol_surface.hpp" // EssviParams, SviParams

namespace atx::vol::simd::detail {

// Is the slice's TIME axis usable as a divisor?
//
// Every sigma (implied-vol) evaluator divides total variance by slice.T, and nothing
// validated it: T == 0 served sqrt(w/0) = +inf as a volatility, T < 0 served NaN, and
// T == -inf served -0.0 — all three emitted with no flag and no route disagreement to
// notice them. The theta/phi/rho admissibility predicate lives in essvi_batch_avx2.cpp
// (it gates only the VECTOR path, falling back to the exact scalar kernel); this one is
// different in kind — it is a refusal for BOTH routes, because the scalar kernel
// divides by the same T. It therefore lives here, in the header both TUs already
// include, so the two routes cannot drift.
//
// Declining is the whole point: a refused slice must return NaN, not a number.
[[nodiscard]] inline bool essvi_slice_time_valid(const EssviParams& s) noexcept {
    return std::isfinite(s.T) && (s.T > 0.0);
}

void essvi_backbone_w_batch_avx2(const EssviParams& slice, const double* k_log,
                                 double* w_out, std::size_t n) noexcept;

void essvi_backbone_w_grad_batch_avx2(const EssviParams& slice,
                                      const double* k_log, double* w_out,
                                      double* dw_dtheta, double* dw_dphi,
                                      double* dw_drho, std::size_t n) noexcept;

void svi_total_w_batch_avx2(const SviParams& slice, const double* k_log,
                            double* w_out, std::size_t n) noexcept;

void svi_qe_basis_batch_avx2(double m, double sigma, const double* k,
                             double* u_out, double* v_out,
                             std::size_t n) noexcept;

void essvi_backbone_sigma_batch_avx2(const EssviParams& slice,
                                     const double* k_log, double* sigma_out,
                                     std::size_t n) noexcept;

} // namespace atx::vol::simd::detail

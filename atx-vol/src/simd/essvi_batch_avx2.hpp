#pragma once

// Internal declarations for the AVX2 eSSVI/SVI batch kernels. Defined in
// essvi_batch_avx2.cpp (built with -mavx2 -mfma) and called only from
// essvi_batch.cpp behind an atx::vol::simd::have_avx2() guard. Not a public
// header: callers use atx/vol/simd/essvi_batch.hpp.

#include <cstddef>

#include "atx/vol/api/fitting/vol_surface.hpp" // EssviParams, SviParams

namespace atx::vol::simd::detail {

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

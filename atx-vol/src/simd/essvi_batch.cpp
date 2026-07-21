#include "atx/vol/simd/essvi_batch.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "atx/vol/simd/cpu.hpp"
#include "atx/vol/vol_surface.hpp"

#include "essvi_batch_avx2.hpp"

namespace atx::vol::simd {

namespace {

void essvi_backbone_w_batch_scalar(const EssviParams& slice, const double* k_log,
                                   double* w_out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        w_out[i] = essvi_backbone_w(slice, k_log[i]);
    }
}

void svi_total_w_batch_scalar(const SviParams& slice, const double* k_log,
                              double* w_out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        w_out[i] = svi_total_w(slice, k_log[i]);
    }
}

// Quasi-explicit rotated basis (u, v) — source-of-truth scalar loop, op-for-op
// with svi_calib.cpp's build_and_solve_normal (div, mul+add, sqrt; 1/sqrt(2) ==
// kInvSqrt2). The non-AVX2 path is therefore bit-identical to the pre-C2.2 fit.
void svi_qe_basis_batch_scalar(double m, double sigma, const double* k,
                               double* u_out, double* v_out,
                               std::size_t n) noexcept {
    constexpr double kInvSqrt2 = 0.70710678118654752440;
    for (std::size_t i = 0; i < n; ++i) {
        const double y = (k[i] - m) / sigma;
        const double z = std::sqrt(y * y + 1.0);
        u_out[i] = (y + z) * kInvSqrt2;
        v_out[i] = (z - y) * kInvSqrt2;
    }
}

void essvi_backbone_sigma_batch_scalar(const EssviParams& slice,
                                       const double* k_log, double* sigma_out,
                                       std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const double w = essvi_backbone_w(slice, k_log[i]);
        sigma_out[i] = std::sqrt(std::max(w, 0.0) / slice.T);
    }
}

// Fused w + natural-gradient scalar fallback: the exact per-element source of
// truth (essvi_backbone_w THEN essvi_w_grad3, in that order), so a non-AVX2 host
// reproduces the historical fit bit-for-bit.
void essvi_backbone_w_grad_batch_scalar(const EssviParams& slice,
                                        const double* k_log, double* w_out,
                                        double* dw_dtheta, double* dw_dphi,
                                        double* dw_drho, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        w_out[i] = essvi_backbone_w(slice, k_log[i]);
        const std::array<double, 3> g = essvi_w_grad3(slice, k_log[i]);
        dw_dtheta[i] = g[0];
        dw_dphi[i] = g[1];
        dw_drho[i] = g[2];
    }
}

} // namespace

void essvi_backbone_w_batch(const EssviParams& slice, const double* k_log,
                            double* w_out, std::size_t n) noexcept {
    if (use_avx2()) {
        detail::essvi_backbone_w_batch_avx2(slice, k_log, w_out, n);
    } else {
        essvi_backbone_w_batch_scalar(slice, k_log, w_out, n);
    }
}

void essvi_backbone_w_grad_batch(const EssviParams& slice, const double* k_log,
                                 double* w_out, double* dw_dtheta,
                                 double* dw_dphi, double* dw_drho,
                                 std::size_t n) noexcept {
    if (use_avx2()) {
        detail::essvi_backbone_w_grad_batch_avx2(slice, k_log, w_out, dw_dtheta,
                                                 dw_dphi, dw_drho, n);
    } else {
        essvi_backbone_w_grad_batch_scalar(slice, k_log, w_out, dw_dtheta,
                                           dw_dphi, dw_drho, n);
    }
}

void svi_total_w_batch(const SviParams& slice, const double* k_log,
                       double* w_out, std::size_t n) noexcept {
    if (use_avx2()) {
        detail::svi_total_w_batch_avx2(slice, k_log, w_out, n);
    } else {
        svi_total_w_batch_scalar(slice, k_log, w_out, n);
    }
}

void svi_qe_basis_batch(double m, double sigma, const double* k, double* u_out,
                        double* v_out, std::size_t n) noexcept {
    if (use_avx2()) {
        detail::svi_qe_basis_batch_avx2(m, sigma, k, u_out, v_out, n);
    } else {
        svi_qe_basis_batch_scalar(m, sigma, k, u_out, v_out, n);
    }
}

void essvi_backbone_sigma_batch(const EssviParams& slice, const double* k_log,
                                double* sigma_out, std::size_t n) noexcept {
    if (use_avx2()) {
        detail::essvi_backbone_sigma_batch_avx2(slice, k_log, sigma_out, n);
    } else {
        essvi_backbone_sigma_batch_scalar(slice, k_log, sigma_out, n);
    }
}

} // namespace atx::vol::simd

#include "atx/vol/simd/essvi_batch.hpp"

#include <algorithm>
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

void essvi_backbone_sigma_batch_scalar(const EssviParams& slice,
                                       const double* k_log, double* sigma_out,
                                       std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const double w = essvi_backbone_w(slice, k_log[i]);
        sigma_out[i] = std::sqrt(std::max(w, 0.0) / slice.T);
    }
}

} // namespace

void essvi_backbone_w_batch(const EssviParams& slice, const double* k_log,
                            double* w_out, std::size_t n) noexcept {
    if (have_avx2()) {
        detail::essvi_backbone_w_batch_avx2(slice, k_log, w_out, n);
    } else {
        essvi_backbone_w_batch_scalar(slice, k_log, w_out, n);
    }
}

void svi_total_w_batch(const SviParams& slice, const double* k_log,
                       double* w_out, std::size_t n) noexcept {
    if (have_avx2()) {
        detail::svi_total_w_batch_avx2(slice, k_log, w_out, n);
    } else {
        svi_total_w_batch_scalar(slice, k_log, w_out, n);
    }
}

void essvi_backbone_sigma_batch(const EssviParams& slice, const double* k_log,
                                double* sigma_out, std::size_t n) noexcept {
    if (have_avx2()) {
        detail::essvi_backbone_sigma_batch_avx2(slice, k_log, sigma_out, n);
    } else {
        essvi_backbone_sigma_batch_scalar(slice, k_log, sigma_out, n);
    }
}

} // namespace atx::vol::simd

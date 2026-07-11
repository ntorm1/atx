#include "atx/vol/detail/norm_cdf_cheb.hpp"

#include "atx/core/math.hpp"

#include <cmath>

namespace atx::vol::detail {

namespace {

// π to double precision (M_PI is not portable without _USE_MATH_DEFINES).
constexpr double kPi = 3.14159265358979323846;

// Build the degree-(N-1) Chebyshev expansion of Φ on [-HalfRange, HalfRange].
//
// Samples Φ at the N Chebyshev-Gauss nodes (first kind), takes the discrete
// cosine transform to recover the coefficients, then halves the DC term so the
// Clenshaw evaluator can use `coefs[0] + t·b_1 - b_2` directly.
[[nodiscard]] std::array<double, kNormCdfChebN> build_coefs() noexcept {
    constexpr std::size_t N = kNormCdfChebN;

    std::array<double, N> samples{};
    for (std::size_t j = 0; j < N; ++j) {
        // Chebyshev node t_j = cos(π(j+½)/N) ∈ (-1, 1), mapped onto the range.
        const double t = std::cos(kPi * (static_cast<double>(j) + 0.5) /
                                  static_cast<double>(N));
        samples[j] = atx::core::norm_cdf(t * kNormCdfHalfRange);
    }

    std::array<double, N> coefs{};
    for (std::size_t k = 0; k < N; ++k) {
        double acc = 0.0;
        for (std::size_t j = 0; j < N; ++j) {
            acc += samples[j] *
                   std::cos(kPi * static_cast<double>(k) *
                            (static_cast<double>(j) + 0.5) /
                            static_cast<double>(N));
        }
        coefs[k] = (2.0 / static_cast<double>(N)) * acc;
    }
    coefs[0] *= 0.5;
    return coefs;
}

} // namespace

const std::array<double, kNormCdfChebN>& norm_cdf_cheb_coefs() noexcept {
    // Magic-static: initialized exactly once, threadsafe under C++11+.
    static const std::array<double, kNormCdfChebN> coefs = build_coefs();
    return coefs;
}

} // namespace atx::vol::detail

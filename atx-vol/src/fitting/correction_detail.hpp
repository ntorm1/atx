#pragma once

// Chebyshev primitives (first-kind / roots grid) backing CorrectionCache.
//
// Internal to the correction cache but exposed so the DCT-II round-trip,
// Clenshaw, and derivative recurrences can be locked directly in tests. Raw
// pointers are non-owning observers with the documented length contracts.
//
// Split out of the public correction.hpp API surface (Task 4, atx-vol API
// restructure): every symbol here is used only by correction.cpp and by
// correction_test.cpp's direct unit-test seam, never by any other production
// TU.

#include <cstddef>
#include <cstdint>

namespace atx::vol::detail {

// Max nodes per axis (matches C ATS_CHEB_MAX_NODES).
inline constexpr std::uint16_t kChebMaxNodes = 64;

// j-th first-kind Chebyshev node, x = cos(pi (2j+1) / (2n)) in [-1, 1].
[[nodiscard]] double cheb_node(std::uint16_t j, std::uint16_t n) noexcept;

// Affine maps between physical box [a, b] and the unit interval [-1, 1].
[[nodiscard]] constexpr double cheb_to_unit(double x, double a, double b) noexcept {
  return (2.0 * x - (a + b)) / (b - a);
}
[[nodiscard]] constexpr double cheb_from_unit(double xi, double a, double b) noexcept {
  return 0.5 * (a + b) + 0.5 * (b - a) * xi;
}

// Tensor layout: coef[i, j, k] lives at j*(n_s*n_k) + k*n_k + i (i = k_log axis,
// innermost/contiguous; j = T axis; k = sigma axis).
[[nodiscard]] constexpr std::size_t cheb_idx(std::uint16_t i, std::uint16_t j, std::uint16_t k,
                                             std::uint16_t n_k, std::uint16_t n_s) noexcept {
  return static_cast<std::size_t>(j) * static_cast<std::size_t>(n_s) *
             static_cast<std::size_t>(n_k) +
         static_cast<std::size_t>(k) * static_cast<std::size_t>(n_k) + static_cast<std::size_t>(i);
}

// Forward DCT-II: n function values at Chebyshev nodes -> n coefficients.
// `vals` and `coefs` must not alias; both have length n.
void cheb_dct2(const double *vals, double *coefs, std::uint16_t n) noexcept;

// 1D Clenshaw evaluation p(x) = a_0 + sum_{k>=1} a_k T_k(x). `coefs` length n.
[[nodiscard]] double cheb_clenshaw1d(const double *coefs, std::uint16_t n, double x) noexcept;

// Derivative-coefficient transform (Numerical Recipes 5.9). `c`/`d` length n,
// must not alias; `scale` = 2/(b-a) maps back to physical units on box [a, b].
void cheb_diff_coefs(const double *c, double *d, std::uint16_t n, double scale) noexcept;

// 3D Clenshaw evaluation. `coefs` laid out per cheb_idx(); `tmp_jk` scratch of
// at least n_T*n_s doubles.
[[nodiscard]] double cheb_clenshaw3d(const double *coefs, std::uint16_t n_k, std::uint16_t n_T,
                                     std::uint16_t n_s, double xi, double xj, double xk,
                                     double *tmp_jk) noexcept;

// 3D Clenshaw of a partial derivative along `diff_axis` (0 = k_log, 1 = T,
// 2 = sigma). `axis_scale` = 2/(box_max - box_min) for that axis. `tmp_jk`
// scratch of at least n_T*n_s doubles.
[[nodiscard]] double cheb_clenshaw3d_partial(const double *coefs, std::uint16_t n_k,
                                             std::uint16_t n_T, std::uint16_t n_s, double xi,
                                             double xj, double xk, int diff_axis, double axis_scale,
                                             double *tmp_jk) noexcept;

} // namespace atx::vol::detail

#pragma once

// atx::engine::combine -- least-squares residual of one cross-section on earlier ones.
//
// residualise_in_place replaces a dependent vector y (n rows) by its ordinary-least-squares
// residual on [1, x_1, ..., x_m], the m regressor columns over the same rows, intercept
// included: the part of y orthogonal to the constant and to every column. The columns are
// orthonormalised in their given order by modified Gram-Schmidt with one re-orthogonalisation
// pass ("twice is enough"), the intercept first as a mean removal, so the residual is orthogonal
// to the constant and to each column to rounding (about 1e-15 relative) however the columns are
// conditioned. A column whose part outside the span of the constant and the earlier kept columns
// is at most `tolerance` times its centred norm adds nothing to that span and is skipped: a
// duplicated, affine, constant or all-zero column, or a column beyond the n - 1 the rows can
// carry, leaves the residual of the independent columns (the least-squares residual is unique
// even where the coefficients are not). A dependent vector whose residual is at most `tolerance`
// times its own centred norm lies in the span: it becomes exactly 0 and the fit says `spanned`,
// so rounding noise is never passed on as a signal.
//
// Header-only for the reason group_rerank.hpp gives: atx-impl instantiates these per-row loops
// inside its optimised IC composition translation units, while the engine library itself builds
// unoptimised in Debug.

#include <cmath>   // std::isfinite, std::sqrt
#include <span>    // std::span
#include <utility> // std::pair

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::combine {

// Relative span tolerance of residualise_in_place (see the header comment).
inline constexpr atx::f64 kResidualSpanTolerance = 1e-10;

struct ResidualFit {
  atx::usize rank = 0U;  // regressor columns kept as independent (the intercept not counted)
  bool spanned = false;  // the residual was within tolerance of 0 and is now exactly 0
};

namespace residual_detail {

inline atx::f64 dot(std::span<const atx::f64> a, std::span<const atx::f64> b) noexcept {
  atx::f64 sum = 0.0;
  for (atx::usize i = 0; i < a.size(); ++i) sum += a[i] * b[i];
  return sum;
}

inline void remove_mean(std::span<atx::f64> v) noexcept {
  atx::f64 sum = 0.0;
  for (const atx::f64 x : v) sum += x;
  const atx::f64 mean = sum / static_cast<atx::f64>(v.size());
  for (atx::f64& x : v) x -= mean;
}

// One modified Gram-Schmidt pass: `v` loses its mean (the intercept), then its component along
// each of the first `rank` unit columns of `basis` (v.size() values each), in order.
inline void project_out(std::span<atx::f64> v, std::span<const atx::f64> basis, atx::usize rank) noexcept {
  const atx::usize n = v.size();
  remove_mean(v);
  for (atx::usize c = 0; c < rank; ++c) {
    const auto q = basis.subspan(c * n, n);
    const atx::f64 coefficient = dot(q, v);
    for (atx::usize i = 0; i < n; ++i) v[i] -= coefficient * q[i];
  }
}

// (centred norm of `v`, norm of `v` after two projection passes); `v` keeps the projection.
// `v` must not overlap the first `rank` columns of `basis`.
inline std::pair<atx::f64, atx::f64> orthogonalise(std::span<atx::f64> v, std::span<const atx::f64> basis,
                                                   atx::usize rank) noexcept {
  remove_mean(v);
  const atx::f64 centred = std::sqrt(dot(v, v));
  project_out(v, basis, rank);
  project_out(v, basis, rank);
  return {centred, std::sqrt(dot(v, v))};
}

} // namespace residual_detail

// `y`: in, the dependent values (n >= 1); out, the residual (see the header comment).
// `columns`: m >= 0 regressor columns of n values each, back to back (column c at [c n, (c + 1) n));
// consumed: on return its first `rank` columns hold the orthonormal basis that was used.
// Refuses (InvalidArgument), leaving both spans untouched: n == 0, columns.size() not a multiple
// of n, a non-finite value, or a tolerance outside (0, 1). Allocation free.
[[nodiscard]] inline atx::core::Result<ResidualFit> residualise_in_place(
    std::span<atx::f64> y, std::span<atx::f64> columns, atx::f64 tolerance = kResidualSpanTolerance) {
  const atx::usize n = y.size();
  if (n == 0U || columns.size() % n != 0U || !(tolerance > 0.0) || !(tolerance < 1.0))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "residualise: shape or tolerance");
  const auto finite = [](std::span<const atx::f64> v) {
    for (const atx::f64 x : v)
      if (!std::isfinite(x)) return false;
    return true;
  };
  if (!finite(y) || !finite(columns))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "residualise: non-finite value");
  ResidualFit fit;
  const atx::usize m = columns.size() / n;
  for (atx::usize c = 0; c < m; ++c) {
    // Slots [0, rank) hold the kept unit columns; rank <= c, so column c never overlaps them.
    const auto v = columns.subspan(c * n, n);
    const auto [centred, remaining] = residual_detail::orthogonalise(v, columns, fit.rank);
    if (!(centred > 0.0) || remaining <= tolerance * centred) continue;
    const auto q = columns.subspan(fit.rank * n, n);
    for (atx::usize i = 0; i < n; ++i) q[i] = v[i] / remaining;
    ++fit.rank;
  }
  const auto [y_centred, y_remaining] = residual_detail::orthogonalise(y, columns, fit.rank);
  if (!(y_centred > 0.0) || y_remaining <= tolerance * y_centred) {
    for (atx::f64& x : y) x = 0.0;
    fit.spanned = true;
  }
  return atx::core::Ok(fit);
}

} // namespace atx::engine::combine

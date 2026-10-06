// atx::engine::book -- the kernel of mv-aim-v1 (P9 lane COV-MV). Contracts live in
// book/mv_target.hpp.

#include "atx/engine/book/mv_target.hpp"

#include <cmath>   // std::abs, std::isfinite, std::isnan
#include <initializer_list>
#include <limits>  // std::numeric_limits
#include <span>    // std::span
#include <string>  // std::string
#include <utility> // std::move (ATX_TRY), std::swap
#include <vector>  // std::vector

#include "atx/core/error.hpp"
#include "atx/core/linalg/decompose.hpp"
#include "atx/core/safe_math.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

using atx::f64;
using atx::u64;
using atx::usize;

namespace {

constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();

// An error of `code` naming the rule, convertible to every Result<T>.
[[nodiscard]] auto failure(atx::core::ErrorCode code, const char* what) {
  return atx::core::Err(code, std::string("mv-aim-v1: ") + what);
}
[[nodiscard]] auto invalid(const char* what) {
  return failure(atx::core::ErrorCode::InvalidArgument, what);
}

[[nodiscard]] bool finite_positive(f64 x) noexcept { return std::isfinite(x) && x > 0.0; }

// Grows v to hold n entries with one exact allocation (no growth factor), so the scratch's bytes
// stay within mv_aim_working_bytes; then sets every entry to `fill`.
template <class T> void sized(std::vector<T>& v, usize n, T fill) {
  if (v.capacity() < n) {
    std::vector<T> fresh;
    fresh.reserve(n);
    v.swap(fresh);
  }
  v.assign(n, fill);
}

// K = 1 + groups + styles, checked (the model's factor count; Ruling COV-9's no-wrap rule).
[[nodiscard]] atx::core::Result<usize> factor_count(const FactorRiskView& m) {
  auto k = atx::core::checked_add(m.groups, m.styles);
  if (!k) return invalid("groups + styles overflows");
  auto total = atx::core::checked_add(*k, usize{1});
  if (!total) return invalid("groups + styles overflows");
  return atx::core::Ok(*total);
}

// The model's shape and entries against n = specific.size() names (every product checked).
[[nodiscard]] atx::core::Result<usize> check_view(const FactorRiskView& m) {
  ATX_TRY(const usize k, factor_count(m));
  const usize n = m.specific.size();
  const auto cells = atx::core::checked_mul(k, k);
  const auto rows = atx::core::checked_mul(n, m.styles);
  if (!cells || !rows || m.covariance.size() != *cells || m.exposures.size() != *rows ||
      m.group.size() != n)
    return invalid("groups, exposures, covariance and specific variances differ in size");
  for (usize i = 0U; i < n; ++i)
    if (m.group[i] > m.groups) return invalid("a group column above the model's groups");
  for (const f64 x : m.exposures)
    if (!std::isfinite(x)) return invalid("a non-finite style exposure");
  for (const f64 x : m.covariance)
    if (!std::isfinite(x)) return invalid("a non-finite factor covariance entry");
  return atx::core::Ok(k);
}

// Name i's nonzero columns of X and their entries (intercept, group, styles), in column order.
void row_of(const FactorRiskView& m, usize i, std::vector<usize>& columns,
            std::vector<f64>& values) {
  columns.clear();
  values.clear();
  columns.push_back(0U);
  values.push_back(1.0);
  if (m.group[i] != 0U) {
    columns.push_back(m.group[i]);
    values.push_back(1.0);
  }
  const usize style_column = 1U + m.groups;
  const f64* row = m.exposures.data() + i * m.styles;
  for (usize c = 0U; c < m.styles; ++c) {
    columns.push_back(style_column + c);
    values.push_back(row[c]);
  }
}

// Solves M y = rhs in place (M k x k row-major, overwritten) by Gaussian elimination with partial
// pivoting: the first row of maximal |entry| in each column, rows swapped, fixed loop order.
[[nodiscard]] atx::core::Status solve_pivoted(std::vector<f64>& mat, std::vector<f64>& rhs,
                                              usize k) {
  for (usize c = 0U; c < k; ++c) {
    usize pivot = c;
    for (usize r = c + 1U; r < k; ++r)
      if (std::abs(mat[r * k + c]) > std::abs(mat[pivot * k + c])) pivot = r;
    const f64 p = mat[pivot * k + c];
    if (!std::isfinite(p) || p == 0.0)
      return invalid("I + F G is singular: the factor covariance is not positive semi-definite");
    if (pivot != c) {
      for (usize j = 0U; j < k; ++j) std::swap(mat[c * k + j], mat[pivot * k + j]);
      std::swap(rhs[c], rhs[pivot]);
    }
    for (usize r = c + 1U; r < k; ++r) {
      const f64 f = mat[r * k + c] / p;
      if (f == 0.0) continue;
      for (usize j = c; j < k; ++j) mat[r * k + j] -= f * mat[c * k + j];
      rhs[r] -= f * rhs[c];
    }
  }
  for (usize c = k; c-- > 0U;) {
    f64 sum = rhs[c];
    for (usize j = c + 1U; j < k; ++j) sum -= mat[c * k + j] * rhs[j];
    rhs[c] = sum / mat[c * k + c];
  }
  for (const f64 x : rhs)
    if (!std::isfinite(x)) return invalid("a non-finite solution of (I + F G) y = F r");
  return atx::core::Ok();
}

} // namespace

u64 MvAimScratch::bytes() const noexcept {
  using atx::core::sat_add;
  using atx::core::sat_mul;
  u64 total = 0U;
  total = sat_add(total, sat_mul(u64{active.capacity()}, u64{sizeof(usize)}));
  total = sat_add(total, sat_mul(u64{columns.capacity()}, u64{sizeof(usize)}));
  for (const auto* v : {&a, &u, &values, &g, &m, &r, &y})
    total = sat_add(total, sat_mul(u64{v->capacity()}, u64{sizeof(f64)}));
  return total;
}

u64 mv_aim_working_bytes(usize names, usize groups, usize styles) noexcept {
  using atx::core::sat_add;
  using atx::core::sat_mul;
  const u64 k = sat_add(sat_add(u64{groups}, u64{styles}), u64{1});
  const u64 width = sat_add(u64{styles}, u64{2});
  const u64 per_name = u64{sizeof(usize)} + 2U * u64{sizeof(f64)}; // active, a, u
  const u64 per_column = u64{sizeof(usize)} + u64{sizeof(f64)};    // columns, values
  u64 total = sat_mul(u64{names}, per_name);
  total = sat_add(total, sat_mul(width, per_column));
  total = sat_add(total, sat_mul(sat_mul(sat_mul(k, k), u64{2}), u64{sizeof(f64)})); // g, m
  total = sat_add(total, sat_mul(sat_mul(k, u64{2}), u64{sizeof(f64)}));            // r, y
  return total;
}

atx::core::Result<usize> mv_aim(const FactorRiskView& m, std::span<const f64> score,
                                std::span<const f64> sigma, std::span<f64> out,
                                MvAimScratch& s) {
  ATX_TRY(const usize k, check_view(m));
  const usize n = m.specific.size();
  if (score.size() != n || sigma.size() != n || out.size() != n)
    return invalid("score, sigma and out need one entry per name of the model");
  // The active names (finite score) and alpha = sigma x score, then P alpha.
  sized(s.active, n, usize{0});
  s.active.clear();
  for (usize i = 0U; i < n; ++i) {
    if (std::isnan(score[i])) continue;
    if (!std::isfinite(score[i]))
      return invalid("a score that is infinite (NaN is no opinion; inf is refused)");
    if (!finite_positive(sigma[i]) || !finite_positive(m.specific[i]))
      return invalid("an active name whose sigma or specific variance is not finite and > 0");
    s.active.push_back(i);
  }
  const usize active = s.active.size();
  if (active < 2U)
    return failure(atx::core::ErrorCode::Unavailable,
                   "fewer than two active names (the projection P is undefined)");
  sized(s.a, active, 0.0);
  f64 mean = 0.0;
  for (usize j = 0U; j < active; ++j) {
    const usize i = s.active[j];
    s.a[j] = sigma[i] * score[i];
    if (!std::isfinite(s.a[j])) return invalid("a non-finite alpha (sigma x score)");
    mean += s.a[j];
  }
  mean /= static_cast<f64>(active);
  bool tilt = false;
  for (f64& x : s.a) {
    x -= mean;
    tilt = tilt || x != 0.0;
  }
  if (!tilt)
    return failure(atx::core::ErrorCode::Unavailable, "P alpha is 0 (nothing to tilt)");
  // u = D^-1 P alpha; G = X' D^-1 X and r = X' u over the active names, in name order.
  sized(s.u, active, 0.0);
  sized(s.g, k * k, 0.0);
  sized(s.r, k, 0.0);
  const usize width = 2U + m.styles;
  sized(s.columns, width, usize{0});
  sized(s.values, width, 0.0);
  for (usize j = 0U; j < active; ++j) {
    const usize i = s.active[j];
    const f64 d = m.specific[i];
    s.u[j] = s.a[j] / d;
    row_of(m, i, s.columns, s.values);
    for (usize p = 0U; p < s.columns.size(); ++p) {
      const usize row = s.columns[p];
      const f64 scaled = s.values[p] / d;
      s.r[row] += s.values[p] * s.u[j];
      for (usize q = 0U; q < s.columns.size(); ++q)
        s.g[row * k + s.columns[q]] += scaled * s.values[q];
    }
  }
  // M = I + F G and the right-hand side F r, then y = M^-1 F r (F itself is never inverted).
  sized(s.m, k * k, 0.0);
  sized(s.y, k, 0.0);
  for (usize a = 0U; a < k; ++a) {
    const f64* f = m.covariance.data() + a * k;
    f64 rhs = 0.0;
    for (usize l = 0U; l < k; ++l) rhs += f[l] * s.r[l];
    s.y[a] = rhs;
    for (usize b = 0U; b < k; ++b) {
      f64 sum = a == b ? 1.0 : 0.0;
      for (usize l = 0U; l < k; ++l) sum += f[l] * s.g[l * k + b];
      s.m[a * k + b] = sum;
    }
  }
  ATX_TRY_VOID(solve_pivoted(s.m, s.y, k));
  // V^-1 P alpha = u - D^-1 X y (into a), then P again and the gross normalisation.
  f64 mean_b = 0.0;
  for (usize j = 0U; j < active; ++j) {
    const usize i = s.active[j];
    row_of(m, i, s.columns, s.values);
    f64 xy = 0.0;
    for (usize p = 0U; p < s.columns.size(); ++p) xy += s.values[p] * s.y[s.columns[p]];
    s.a[j] = s.u[j] - xy / m.specific[i];
    mean_b += s.a[j];
  }
  mean_b /= static_cast<f64>(active);
  f64 gross = 0.0;
  for (f64& x : s.a) {
    x -= mean_b;
    gross += std::abs(x);
  }
  if (!std::isfinite(gross)) return invalid("a non-finite target (V^-1 P alpha)");
  if (gross == 0.0)
    return failure(atx::core::ErrorCode::Unavailable, "a flat target (P V^-1 P alpha is 0)");
  for (f64& x : out) x = 0.0;
  for (usize j = 0U; j < active; ++j) out[s.active[j]] = s.a[j] / gross;
  return atx::core::Ok(active);
}

atx::core::Status factor_exposure(const FactorRiskView& m, std::span<const f64> w,
                                  std::span<f64> e) {
  ATX_TRY(const usize k, check_view(m));
  const usize n = m.specific.size();
  if (w.size() != n || e.size() != k)
    return invalid("factor_exposure: one weight per name and one exposure per factor");
  for (f64& x : e) x = 0.0;
  const usize style_column = 1U + m.groups;
  for (usize i = 0U; i < n; ++i) {
    const f64 wi = w[i];
    if (!std::isfinite(wi)) return invalid("factor_exposure: a non-finite weight");
    if (wi == 0.0) continue;
    e[0] += wi;
    if (m.group[i] != 0U) e[m.group[i]] += wi;
    const f64* row = m.exposures.data() + i * m.styles;
    for (usize c = 0U; c < m.styles; ++c) e[style_column + c] += row[c] * wi;
  }
  return atx::core::Ok();
}

atx::core::Status factor_eigen(std::span<const f64> covariance, usize k, FactorEigen& out) {
  out.factors.clear();
  out.values.clear();
  out.vectors.clear();
  const auto cells = atx::core::checked_mul(k, k);
  if (!cells || covariance.size() != *cells)
    return invalid("factor_eigen: the covariance is not k x k");
  for (const f64 x : covariance)
    if (!std::isfinite(x)) return invalid("factor_eigen: a non-finite covariance entry");
  for (usize j = 0U; j < k; ++j)
    if (covariance[j * k + j] > 0.0) out.factors.push_back(j);
  const usize size = out.factors.size();
  if (size == 0U) return atx::core::Ok();
  const auto index = [](usize x) { return static_cast<Eigen::Index>(x); };
  atx::core::linalg::MatX block(index(size), index(size));
  for (usize a = 0U; a < size; ++a)
    for (usize b = 0U; b < size; ++b)
      block(index(a), index(b)) = covariance[out.factors[a] * k + out.factors[b]];
  auto eig = atx::core::linalg::symmetric_eig(block);
  if (!eig) {
    out.factors.clear();
    return atx::core::Err(eig.error().code(), "mv-aim-v1: factor_eigen: " + eig.error().message());
  }
  out.values.resize(size);
  out.vectors.resize(size * size);
  for (usize r = 0U; r < size; ++r) {
    out.values[r] = eig->values(index(r));
    for (usize a = 0U; a < size; ++a) out.vectors[r * size + a] = eig->vectors(index(a), index(r));
  }
  return atx::core::Ok();
}

f64 low_eigen_share(const FactorEigen& eig, std::span<const f64> e, usize ranks) noexcept {
  const usize size = eig.factors.size();
  if (size == 0U || eig.values.size() != size || eig.vectors.size() != size * size) return nan;
  for (const usize j : eig.factors)
    if (j >= e.size()) return nan;
  f64 low = 0.0, total = 0.0;
  for (usize r = 0U; r < size; ++r) {
    f64 projection = 0.0;
    for (usize a = 0U; a < size; ++a) projection += eig.vectors[r * size + a] * e[eig.factors[a]];
    const f64 part = eig.values[r] * projection * projection;
    total += part;
    if (r < ranks) low += part;
  }
  return std::isfinite(total) && total > 0.0 ? low / total : nan;
}

} // namespace atx::engine::book

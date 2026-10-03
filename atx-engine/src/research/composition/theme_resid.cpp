// theme-resid-v1's per-session kernel (theme_resid.hpp), relocated verbatim from
// atx-impl/src/strategy_ic_theme_resid.cpp (P9 lane D2).
#include "atx/engine/research/composition/theme_resid.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include "atx/engine/combine/group_rerank.hpp"
#include "atx/engine/combine/group_residualise.hpp"

namespace atx::engine::research::composition {
namespace {
namespace cb = atx::engine::combine;
using atx::f64;
using atx::usize;
using Ranked = std::pair<f64, usize>;

// Row `offset` of every plane becomes its theme's standardised composite: the centred tied rank
// over the present (non-NaN) names, which is the rank ew-theme-std-v1 adds; a lone present name
// gets 0; absent names stay NaN.
void standardise_rows(std::span<std::vector<f64>> planes, usize offset, usize names, std::vector<Ranked>& row) {
  for (auto& plane : planes) {
    row.clear();
    for (usize i = 0; i < names; ++i)
      if (!std::isnan(plane[offset + i])) row.emplace_back(plane[offset + i], i);
    if (row.size() == 1U) {
      plane[offset + row.front().second] = 0.0;
      continue;
    }
    cb::for_each_centered_rank(row, [&](usize i, f64 r) { plane[offset + i] = r; });
  }
}

// One theme's regression scratch, reserved once for the largest theme so no row allocates.
struct Scratch {
  std::vector<f64> columns, dependent;
  std::vector<usize> support;
};

// Ruling PM4-12: names tied in the theme's own standardised composite z_t stay tied. A tie block is
// a run of exactly equal z_t over the support (sorted by (z_t, support position), so a block lists
// its names in ascending name order); inside a block of two or more names the residual becomes the
// block mean: the sum from the block's first name, adding the others in ascending name order, divided
// by the block size. A block of one name is left untouched, so a composite without ties keeps the
// registered residual bit for bit. `row` is scratch (reserved capacity: no allocation).
void mean_over_tie_blocks(const std::vector<f64>& own, usize offset, Scratch& s, std::vector<Ranked>& row) {
  const usize n = s.support.size();
  row.clear();
  for (usize k = 0; k < n; ++k) row.emplace_back(own[offset + s.support[k]], k);
  std::sort(row.begin(), row.end());
  for (usize b = 0; b < n;) {
    usize end = b + 1U;
    while (end < n && row[end].first == row[b].first) ++end;
    if (end - b > 1U) {
      f64 sum = s.dependent[row[b].second];
      for (usize k = b + 1U; k < end; ++k) sum += s.dependent[row[k].second];
      const f64 mean = sum / static_cast<f64>(end - b);
      for (usize k = b; k < end; ++k) s.dependent[row[k].second] = mean;
    }
    b = end;
  }
}

// Theme t on row `offset` (planes already standardised on that row): theme 0 adds W_0 z_0 with
// ew-theme-std-v1's expression; theme t > 0 adds W_t times the re-rank of its residual, averaged
// inside each tie block of z_t first (Ruling PM4-12).
atx::core::Status add_theme(std::span<std::vector<f64>> planes, std::span<const f64> mass, usize t, usize offset,
                            usize names, std::span<f64> out, std::vector<Ranked>& row, Scratch& s) {
  const auto& own = planes[t];
  s.support.clear();
  for (usize i = 0; i < names; ++i)
    if (!std::isnan(own[offset + i])) s.support.push_back(i);
  const usize n = s.support.size();
  if (n < 2U) return atx::core::Ok();
  if (t == 0U) {
    for (const usize at : s.support) out[offset + at] += mass[0] * own[offset + at];
    return atx::core::Ok();
  }
  // Within the reserved capacity: n <= names and t <= themes - 1.
  s.dependent.resize(n);
  s.columns.resize(t * n);
  for (usize k = 0; k < n; ++k) {
    const usize cell = offset + s.support[k];
    s.dependent[k] = own[cell];
    for (usize j = 0; j < t; ++j) {
      const f64 x = planes[j][cell];
      s.columns[j * n + k] = std::isnan(x) ? 0.0 : x; // a preceding theme absent here: neutral 0
    }
  }
  ATX_TRY(const auto fit, cb::residualise_in_place(s.dependent, s.columns));
  if (fit.spanned) return atx::core::Ok(); // theme t is in the span of the earlier themes today
  mean_over_tie_blocks(own, offset, s, row);
  row.clear();
  for (usize k = 0; k < n; ++k) row.emplace_back(s.dependent[k], s.support[k]);
  cb::for_each_centered_rank(row, [&](usize i, f64 r) { out[offset + i] += mass[t] * r; });
  return atx::core::Ok();
}
} // namespace

atx::core::Status add_theme_residualised(std::span<std::vector<f64>> planes, std::span<const f64> mass, usize names,
                                         std::span<f64> out, std::vector<Ranked>& row) {
  if (names == 0U || planes.size() != mass.size() || out.size() % names != 0U ||
      std::any_of(planes.begin(), planes.end(), [&](const std::vector<f64>& p) { return p.size() != out.size(); }) ||
      std::any_of(mass.begin(), mass.end(), [](f64 w) { return !std::isfinite(w); }))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "theme-resid-v1: plane, theme weight or blend shape");
  if (planes.empty()) return atx::core::Ok();
  Scratch s;
  try {
    s.columns.reserve((planes.size() - 1U) * names);
    s.dependent.reserve(names);
    s.support.reserve(names);
    row.reserve(names);
  } catch (const std::bad_alloc&) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "theme-resid-v1: scratch allocation failed");
  } catch (const std::length_error&) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "theme-resid-v1: scratch extent exceeded");
  }
  const usize dates = out.size() / names;
  for (usize d = 0; d < dates; ++d) {
    const usize offset = d * names;
    standardise_rows(planes, offset, names, row);
    for (usize t = 0; t < planes.size(); ++t) ATX_TRY_VOID(add_theme(planes, mass, t, offset, names, out, row, s));
  }
  return atx::core::Ok();
}
} // namespace atx::engine::research::composition

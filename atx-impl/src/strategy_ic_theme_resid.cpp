#include "strategy_ic_theme_resid.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "atx/engine/combine/group_rerank.hpp"
#include "atx/engine/combine/group_residualise.hpp"
#include "strategy_ic_detail.hpp"

namespace atx::impl::strategy {
namespace {
namespace cb = atx::engine::combine;
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
} // namespace atx::impl::strategy

namespace atx::impl::strategy::ic_detail {
// Optional top-level `theme_residualise` (fitter flag --theme-resid, platform v8 R-11):
// {"rule":"theme-resid-v1","order":[theme, ...]} beside a theme_standardise block with rerank true,
// `order` naming each of that block's weighted themes exactly once (the registered theme order the
// fitter wrote). Every weighted candidate's theme index becomes its theme's position in `order`, so
// the composition residualises the theme at position t on the themes before it. Ruling PM4-11
// (finding R6B-O-4): every weighted theme must be in theme_resid_order and `order` must be that
// list restricted to the weighted themes; the order is recorded (pinned.residualise_order: the
// recipe and the combined manifest). Absent: nothing changes. Runs after composition_standardise,
// before any role payload.
co::Status composition_residualise(const Json& j,const Library& lib,PinnedWeights& pinned) {
  if (!j.contains("theme_residualise")) return co::Ok();
  const auto& block=j.at("theme_residualise");
  if (!block.is_object() || !block.contains("rule") || block.at("rule")!=theme_residualise_rule ||
      !block.contains("order") || !block.at("order").is_array())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise must be {rule: theme-resid-v1, "
        "order: [theme, ...]}");
  if (pinned.std_themes.empty())
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise needs a theme_standardise block with "
        "rerank true (it residualises the standardised theme composites)");
  // The standardise block's theme names by index (theme_indices: first appearance in library
  // order); composition_standardise has checked every weighted candidate's entry.
  const auto& listed=j.at("theme_standardise").at("themes");
  std::vector<std::string> names(pinned.std_theme_count);
  for (usize k=0;k<lib.candidates.size();++k)
    if (pinned.values[k]>0) names[pinned.std_themes[k]]=listed.at(lib.candidates[k].id).get<std::string>();
  const auto registered_theme=[](std::string_view theme) {
    return std::find(theme_resid_order.begin(),theme_resid_order.end(),theme)!=theme_resid_order.end();
  };
  for (const auto& theme:names)
    if (!registered_theme(theme))
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise: weighted theme "+theme+
          " is outside the registered theme order of theme-resid-v1 (Ruling PM4-11)");
  std::vector<std::string> registered; // theme_resid_order restricted to the weighted themes
  for (const std::string_view theme:theme_resid_order) {
    const auto weighted=[theme](const std::string& name) { return std::string_view(name)==theme; };
    if (std::any_of(names.begin(),names.end(),weighted)) registered.emplace_back(theme);
  }
  const auto refuse=[](const std::string& why) {
    return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise order must name each weighted theme "
        "of theme_standardise exactly once ("+why+")");
  };
  const auto& order=block.at("order");
  if (order.size()!=names.size())
    return refuse(std::to_string(order.size())+" entries for "+std::to_string(names.size())+" themes");
  std::vector<usize> position(names.size(),names.size()); // names.size(): not yet placed
  for (usize p=0;p<order.size();++p) {
    if (!order[p].is_string()) return refuse("an entry is not a string");
    const auto theme=order[p].get<std::string>();
    const auto at=std::find(names.begin(),names.end(),theme);
    if (at==names.end()) return refuse("not a weighted theme: "+theme);
    auto& slot=position[static_cast<usize>(at-names.begin())];
    if (slot!=names.size()) return refuse("repeated: "+theme);
    slot=p;
  }
  // `order` is a permutation of the weighted themes here; it must be the registered one.
  for (usize p=0;p<order.size();++p)
    if (order[p].get<std::string>()!=registered[p]) {
      std::string want;
      for (const auto& theme:registered) want+=(want.empty()?"":", ")+theme;
      return co::Err(co::ErrorCode::InvalidArgument,"IC runner: theme_residualise order must be the registered "
          "theme order restricted to the weighted themes (Ruling PM4-11): ["+want+"], not "+order.dump());
    }
  for (usize k=0;k<lib.candidates.size();++k)
    if (pinned.values[k]>0) pinned.std_themes[k]=position[pinned.std_themes[k]];
  pinned.residualise=true;
  pinned.residualise_order=std::move(registered);
  return co::Ok();
}
} // namespace atx::impl::strategy::ic_detail

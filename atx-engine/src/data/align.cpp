// atx::engine::data — align_onto: PIT alignment-rail implementation.
//
// See align.hpp for the contract. Three private helpers keep each function
// short and the invariants legible: strict-ascent validation, the plug
// InstKey→index lookup, and the DropReport position classification.

#include "atx/engine/data/align.hpp"

#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atx::engine::data {

namespace {

// True iff `dates` is strictly ascending (required for as-of binary search).
[[nodiscard]] bool is_strictly_ascending(std::span<const DateKey> dates) noexcept {
  for (atx::usize i = 1; i < dates.size(); ++i) {
    if (dates[i] <= dates[i - 1]) {
      return false;
    }
  }
  return true;
}

// Lookup-only map plug InstKey → plug instrument index. The map never affects
// output order (the canonical axis is the fixed output order), so determinism
// is preserved.
[[nodiscard]] std::unordered_map<InstKey, atx::usize>
build_plug_index(std::span<const InstKey> plug_instruments) {
  std::unordered_map<InstKey, atx::usize> idx;
  idx.reserve(plug_instruments.size());
  for (atx::usize i = 0; i < plug_instruments.size(); ++i) {
    idx.emplace(plug_instruments[i], i);
  }
  return idx;
}

// Classify every plug (date, inst) POSITION once into the DropReport: extra
// instrument (InstKey ∉ canonical universe) takes precedence over extra date
// (InstKey canonical but DateKey later than the last canonical date). In-axis
// positions are not counted. Counts positions, not per-column cells.
[[nodiscard]] DropReport classify_drops(const Dataset &canonical, const Dataset &plug) {
  std::unordered_map<InstKey, char> canonical_universe;
  canonical_universe.reserve(canonical.num_instruments());
  for (const InstKey inst : canonical.instruments()) {
    canonical_universe.emplace(inst, char{1});
  }

  const std::span<const DateKey> canonical_dates = canonical.dates();
  const bool no_canonical_dates = canonical_dates.empty();
  const DateKey max_canonical_date = no_canonical_dates ? DateKey{0} : canonical_dates.back();

  DropReport drops;
  const std::span<const InstKey> plug_instruments = plug.instruments();
  const std::span<const DateKey> plug_dates = plug.dates();
  for (atx::usize pi = 0; pi < plug_instruments.size(); ++pi) {
    const bool in_universe = canonical_universe.count(plug_instruments[pi]) != 0;
    for (atx::usize pd = 0; pd < plug_dates.size(); ++pd) {
      if (!in_universe) {
        ++drops.extra_instrument_cells;
      } else if (no_canonical_dates || plug_dates[pd] > max_canonical_date) {
        ++drops.extra_date_cells;
      }
    }
  }
  return drops;
}

} // namespace

atx::core::Result<AlignedView> align_onto(const Dataset &canonical_price, const Dataset &plug) {
  if (!is_strictly_ascending(canonical_price.dates())) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "align_onto: canonical_price.dates() is not strictly ascending");
  }
  if (!is_strictly_ascending(plug.dates())) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "align_onto: plug.dates() is not strictly ascending");
  }

  const atx::usize nd = canonical_price.num_dates();
  const atx::usize ni = canonical_price.num_instruments();
  const atx::usize ncols = plug.schema().columns.size();
  const std::unordered_map<InstKey, atx::usize> plug_index = build_plug_index(plug.instruments());
  const std::span<const DateKey> canonical_dates = canonical_price.dates();
  const std::span<const InstKey> canonical_instruments = canonical_price.instruments();
  const std::span<const DateKey> plug_dates = plug.dates();
  const atx::usize plug_ni = plug.num_instruments();
  constexpr atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();

  AlignedView view;
  view.num_dates = nd;
  view.num_instruments = ni;
  view.columns = plug.schema().columns;
  view.aligned_columns.assign(ncols, std::vector<atx::f64>(nd * ni, nan));

  for (atx::usize d = 0; d < nd; ++d) {
    const DateKey canonical_date = canonical_dates[d];
    for (atx::usize i = 0; i < ni; ++i) {
      const auto found = plug_index.find(canonical_instruments[i]);
      if (found == plug_index.end()) {
        continue; // missing coverage — cell stays NaN
      }
      const std::optional<atx::usize> pd = as_of_index(plug_dates, canonical_date);
      if (!pd) {
        continue; // no plug row on/before this date — cell stays NaN
      }
      const atx::usize flat = (*pd * plug_ni) + found->second;
      const atx::usize out = (d * ni) + i;
      for (atx::usize c = 0; c < ncols; ++c) {
        view.aligned_columns[c][out] = plug.column(c)[flat];
      }
    }
  }

  view.drops = classify_drops(canonical_price, plug);
  return atx::core::Ok(std::move(view));
}

} // namespace atx::engine::data

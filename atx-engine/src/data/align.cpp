// atx::engine::data — align_onto: PIT alignment-rail implementation.
//
// See align.hpp for the contract. Two private helpers keep each function short
// and the invariants legible: the plug InstKey→index lookup and the DropReport
// position classification. Strict-ascent validation and as-of resolution are
// the shared free utilities in dataset.hpp.

#include "atx/engine/data/align.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

namespace {

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
  std::unordered_set<InstKey> canonical_universe;
  canonical_universe.reserve(canonical.num_instruments());
  for (const InstKey inst : canonical.instruments()) {
    canonical_universe.insert(inst);
  }

  const std::span<const DateKey> canonical_dates = canonical.dates();
  const bool no_canonical_dates = canonical_dates.empty();
  const DateKey max_canonical_date = no_canonical_dates ? DateKey{0} : canonical_dates.back();

  DropReport drops;
  const std::span<const InstKey> plug_instruments = plug.instruments();
  const std::span<const DateKey> plug_dates = plug.available_dates();
  // Availability dates are sorted (validated by align_onto). Count the future
  // suffix once instead of scanning every date for every instrument.
  const atx::usize future_dates = no_canonical_dates ? plug_dates.size() :
      static_cast<atx::usize>(plug_dates.end() -
          std::upper_bound(plug_dates.begin(), plug_dates.end(), max_canonical_date));
  for (atx::usize pi = 0; pi < plug_instruments.size(); ++pi) {
    const bool in_universe = canonical_universe.contains(plug_instruments[pi]);
    if (!in_universe) {
      drops.extra_instrument_cells += plug_dates.size();
    } else {
      drops.extra_date_cells += future_dates;
    }
  }
  return drops;
}

} // namespace

atx::core::Result<AlignedView> align_onto(const Dataset &canonical_price, const Dataset &plug) {
  return align_onto(canonical_price, plug, AlignOptions{});
}

atx::core::Result<AlignedView> align_onto(const Dataset &canonical_price, const Dataset &plug,
                                          const AlignOptions &options) {
  if (!options.column_rules.empty() &&
      options.column_rules.size() != plug.schema().columns.size()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "align_onto: column_rules must be empty or one rule per plug column");
  }
  if (canonical_price.schema().date_encoding != plug.schema().date_encoding) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "align_onto: date encodings must match");
  }
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
  const atx::usize plug_ni = plug.num_instruments();
  constexpr atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  // Identity mapping is independent of date. Resolve it once, outside the
  // date/instrument/column loop (plug_ni is an out-of-range missing sentinel).
  std::vector<atx::usize> instrument_index(ni, plug_ni);
  for (atx::usize i = 0; i < ni; ++i) {
    const auto found = plug_index.find(canonical_instruments[i]);
    if (found != plug_index.end()) {
      instrument_index[i] = found->second;
    }
  }

  // D-05 coverage guard: a canonical date after the plug's last availability date
  // would otherwise silently forward-fill (freeze) the plug's final row.
  const std::span<const DateKey> plug_available = plug.available_dates();
  if (options.require_coverage && nd > 0 &&
      (plug_available.empty() || canonical_dates.back() > plug_available.back())) {
    return atx::core::Err(atx::core::ErrorCode::OutOfRange,
                          "align_onto: canonical dates extend past the plug's coverage "
                          "(last canonical date > last plug availability date)");
  }

  // Per-column staleness caps (empty rules => legacy unbounded as-of for every column).
  std::vector<atx::usize> max_stale(ncols, kAlignUnboundedStaleness);
  for (atx::usize c = 0; c < options.column_rules.size(); ++c) {
    max_stale[c] = options.column_rules[c].max_stale_sessions;
  }

  AlignedView view;
  view.num_dates = nd;
  view.num_instruments = ni;
  view.columns = plug.schema().columns;
  view.aligned_columns.assign(ncols, std::vector<atx::f64>(nd * ni, nan));

  for (atx::usize d = 0; d < nd; ++d) {
    // As-of resolution is invariant across instruments — compute once per date.
    ATX_TRY(const auto pd, plug.available_as_of_index(canonical_dates[d]));
    if (!pd) {
      continue; // no plug row on/before this date — whole row stays NaN
    }
    // Staleness in CANONICAL sessions: the number of canonical dates in
    // [availability, canonical_dates[d]) — 0 on the row's own session, or on the
    // first session after an off-axis row. A row available before the first
    // canonical date has unknown (unbounded) staleness.
    const DateKey available = plug_available[*pd];
    const atx::usize first_session = static_cast<atx::usize>(
        std::lower_bound(canonical_dates.begin(), canonical_dates.end(), available) -
        canonical_dates.begin());
    const atx::usize staleness = (available < canonical_dates.front())
                                     ? kAlignUnboundedStaleness
                                     : d - first_session;
    const atx::usize pd_base = *pd * plug_ni;
    for (atx::usize i = 0; i < ni; ++i) {
      const atx::usize pi = instrument_index[i];
      if (pi == plug_ni) {
        continue; // missing coverage — cell stays NaN
      }
      const atx::usize flat = pd_base + pi;
      const atx::usize out = (d * ni) + i;
      for (atx::usize c = 0; c < ncols; ++c) {
        const bool unbounded = max_stale[c] == kAlignUnboundedStaleness;
        if (unbounded || staleness <= max_stale[c]) {
          view.aligned_columns[c][out] = plug.column(c)[flat];
        }
      }
    }
  }

  view.drops = classify_drops(canonical_price, plug);
  return atx::core::Ok(std::move(view));
}

} // namespace atx::engine::data

#pragma once

// atx::engine::alpha — the zero-copy bridge from a sealed atx-tsdb segment to a
// borrowed alpha::Panel the batch VM evaluates in place (atx-tsdb v2).
//
// attach_segment_panel maps a segment, slices a [start,end) date window, derives
// the point-in-time universe (present-bitmap by default, or an explicit field),
// and builds an alpha::Panel whose field columns ALIAS the mapped bytes — no
// copy, no f64<->Decimal conversion. The returned MappedPanel OWNS the mapping
// (via SegmentReader) and is MOVE-ONLY, so the borrowed Panel can never outlive
// the bytes it reads. Moving a MappedPanel is safe: the OS mapping address is
// stable across a Mapping move, so the Panel's column spans stay valid.

#include <algorithm> // std::lower_bound
#include <cmath>     // std::isnan
#include <limits>    // std::numeric_limits
#include <span>
#include <string>
#include <utility> // std::move
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"

#include "atx/tsdb/segment_reader.hpp"

namespace atx::engine::alpha {

/// Half-open unix-nanos window [start_nanos, end_nanos). The default selects the
/// whole segment. end_nanos is the no-look-ahead cutoff (rows >= end are excluded).
struct TimeWindow {
  atx::i64 start_nanos{std::numeric_limits<atx::i64>::min()};
  atx::i64 end_nanos{std::numeric_limits<atx::i64>::max()};
};

/// Where the point-in-time universe mask comes from.
enum class UniverseKind : atx::u8 { PresentBitmap, Field };

struct UniversePolicy {
  UniverseKind kind{UniverseKind::PresentBitmap};
  std::string field_name{"universe"}; // used only when kind == Field (0/NaN => out)
};

/// Owns a sealed segment's mapping (via SegmentReader) plus a borrowed alpha::Panel
/// that aliases it. Move-only: the Panel is reachable only through this owner, so
/// it cannot outlive the mapping. The mapping address is stable across moves.
class MappedPanel {
public:
  MappedPanel(MappedPanel &&) noexcept = default;
  MappedPanel &operator=(MappedPanel &&) noexcept = default;
  MappedPanel(const MappedPanel &) = delete;
  MappedPanel &operator=(const MappedPanel &) = delete;
  ~MappedPanel() = default;

  /// The borrowed batch Panel (pass to alpha::Engine{panel()}).
  [[nodiscard]] const Panel &panel() const noexcept { return panel_; }

  /// The underlying reader (for content_hash, times, etc.).
  [[nodiscard]] const atx::tsdb::SegmentReader &reader() const noexcept { return reader_; }

  /// Make the whole mapping resident (best-effort; window-ranged prefetch is a
  /// deferred refinement — see the plan's Deferred section).
  void prefetch() const noexcept { reader_.prefetch(); }

  // Construction is via the factory below; public so Result/aggregate moves are
  // simple. Misuse (a panel not built from `reader`) is a programmer error.
  MappedPanel(atx::tsdb::SegmentReader reader, Panel panel) noexcept
      : reader_{std::move(reader)}, panel_{std::move(panel)} {}

private:
  // Declared reader_ FIRST so it is destroyed LAST (after panel_): the Panel's
  // borrowed spans must not dangle during its own destruction.
  atx::tsdb::SegmentReader reader_;
  Panel panel_;
};

namespace detail {

/// Materialize a dates*instruments u8 universe over the window [d0, d0+dates).
[[nodiscard]] inline std::vector<std::uint8_t>
universe_from_present(const atx::tsdb::SegmentReader &reader, atx::usize d0, atx::usize dates) {
  const atx::u32 n = reader.instrument_count();
  std::vector<std::uint8_t> uni(dates * n, std::uint8_t{0});
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::u32 j = 0; j < n; ++j) {
      uni[d * n + j] = reader.present(d0 + d, j) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return uni;
}

/// Materialize a universe from an explicit field column (cell != 0 and non-NaN).
[[nodiscard]] inline std::vector<std::uint8_t>
universe_from_field(const atx::tsdb::SegmentReader &reader, atx::u32 field, atx::usize d0,
                    atx::usize dates) {
  const atx::u32 n = reader.instrument_count();
  const std::span<const atx::f64> col = reader.field_block_view(field);
  std::vector<std::uint8_t> uni(dates * n, std::uint8_t{0});
  for (atx::usize d = 0; d < dates; ++d) {
    for (atx::u32 j = 0; j < n; ++j) {
      const atx::f64 v = col[(d0 + d) * n + j];
      uni[d * n + j] = (!std::isnan(v) && v != 0.0) ? std::uint8_t{1} : std::uint8_t{0};
    }
  }
  return uni;
}

} // namespace detail

/// Attach `path` and build a borrowed batch Panel over the window [start,end).
/// `fields` empty => all segment fields in segment order; otherwise exactly the
/// named fields, in the given order. Err(NotFound) if a named field (or the
/// universe field) is absent; propagates SegmentReader::attach errors.
[[nodiscard]] inline atx::core::Result<MappedPanel>
attach_segment_panel(const std::string &path, TimeWindow window = {},
                     std::span<const std::string> fields = {}, UniversePolicy universe = {}) {
  ATX_TRY(atx::tsdb::SegmentReader reader, atx::tsdb::SegmentReader::attach(path));

  // Resolve the half-open date window [d0, d1) over the ascending time axis.
  const std::span<const atx::i64> axis = reader.times();
  const auto lo = std::lower_bound(axis.begin(), axis.end(), window.start_nanos);
  const auto hi = std::lower_bound(axis.begin(), axis.end(), window.end_nanos);
  const atx::usize d0 = static_cast<atx::usize>(lo - axis.begin());
  const atx::usize d1 = static_cast<atx::usize>(hi - axis.begin());
  const atx::usize dates = d1 - d0;
  const atx::u32 n = reader.instrument_count();
  const atx::usize cells = dates * static_cast<atx::usize>(n);

  // Resolve the field set (names + segment indices).
  std::vector<std::string> names;
  std::vector<atx::u32> field_ids;
  if (fields.empty()) {
    names.reserve(reader.field_count());
    field_ids.reserve(reader.field_count());
    for (atx::u32 f = 0; f < reader.field_count(); ++f) {
      names.emplace_back(reader.field_name(f));
      field_ids.push_back(f);
    }
  } else {
    names.reserve(fields.size());
    field_ids.reserve(fields.size());
    for (const std::string &fn : fields) {
      const auto idx = reader.field_index(fn);
      if (!idx.has_value()) {
        return atx::core::Err(atx::core::ErrorCode::NotFound,
                              "attach_segment_panel: unknown field '" + fn + "'");
      }
      names.push_back(fn);
      field_ids.push_back(idx.value());
    }
  }

  // Build borrowed, windowed column spans (zero-copy: subspan into each block).
  std::vector<std::span<const atx::f64>> columns;
  columns.reserve(field_ids.size());
  for (const atx::u32 fid : field_ids) {
    const std::span<const atx::f64> block = reader.field_block_view(fid); // length T*N
    columns.push_back(block.subspan(d0 * static_cast<atx::usize>(n), cells));
  }

  // Universe mask.
  std::vector<std::uint8_t> uni;
  if (universe.kind == UniverseKind::Field) {
    const auto uidx = reader.field_index(universe.field_name);
    if (!uidx.has_value()) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "attach_segment_panel: unknown universe field '" + universe.field_name +
                                "'");
    }
    uni = detail::universe_from_field(reader, uidx.value(), d0, dates);
  } else {
    uni = detail::universe_from_present(reader, d0, dates);
  }

  ATX_TRY(Panel panel, Panel::create_borrowed(dates, static_cast<atx::usize>(n), std::move(names),
                                              std::move(columns), std::move(uni)));
  return atx::core::Ok(MappedPanel{std::move(reader), std::move(panel)});
}

} // namespace atx::engine::alpha

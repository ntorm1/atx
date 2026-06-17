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

#include <limits> // std::numeric_limits
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
[[nodiscard]] std::vector<std::uint8_t>
universe_from_present(const atx::tsdb::SegmentReader &reader, atx::usize d0, atx::usize dates);

/// Materialize a universe from an explicit field column (cell != 0 and non-NaN).
[[nodiscard]] std::vector<std::uint8_t>
universe_from_field(const atx::tsdb::SegmentReader &reader, atx::u32 field, atx::usize d0,
                    atx::usize dates);

} // namespace detail

/// Attach `path` and build a borrowed batch Panel over the window [start,end).
/// `fields` empty => all segment fields in segment order; otherwise exactly the
/// named fields, in the given order. Err(NotFound) if a named field (or the
/// universe field) is absent; propagates SegmentReader::attach errors.
[[nodiscard]] atx::core::Result<MappedPanel>
attach_segment_panel(const std::string &path, TimeWindow window = {},
                     std::span<const std::string> fields = {}, UniversePolicy universe = {});

/// Span a directory of per-date sealed segments into ONE owned alpha::Panel over
/// [window). Enumerates `*.seg` in `seg_dir`, sorts lexicographically (ISO
/// YYYY-MM-DD names sort chronologically), unions the per-segment securityID
/// instrument axes (join by symbol NAME, first-seen across dates in ascending date
/// order), and MATERIALIZES an owned date-major Panel (each per-date segment is a
/// separate mmap with its own instrument ordering, so a borrowed cross-segment
/// Panel is not possible without a global build-time axis — owned is correct here).
///
/// `fields` empty => the field set of the first in-window segment, in segment order;
/// otherwise exactly the named fields (Err(NotFound) if any segment lacks one).
/// A cell absent on a date reads NaN; the universe mask follows `universe` (the
/// present-bitmap by default). Err(InvalidArgument) if no .seg files / no in-window
/// dates. Returns an OWNED Panel (the readers are released before return).
[[nodiscard]] atx::core::Result<Panel>
attach_multi_segment_panel(const std::string &seg_dir, TimeWindow window = {},
                           std::span<const std::string> fields = {},
                           UniversePolicy universe = {});

} // namespace atx::engine::alpha

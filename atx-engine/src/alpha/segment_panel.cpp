#include "atx/engine/alpha/segment_panel.hpp"

#include <algorithm> // std::lower_bound
#include <cmath>     // std::isnan

namespace atx::engine::alpha {

namespace detail {

std::vector<std::uint8_t>
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

std::vector<std::uint8_t>
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

atx::core::Result<MappedPanel>
attach_segment_panel(const std::string &path, TimeWindow window,
                     std::span<const std::string> fields, UniversePolicy universe) {
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

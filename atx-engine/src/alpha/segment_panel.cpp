#include "atx/engine/alpha/segment_panel.hpp"

#include <algorithm>     // std::lower_bound, std::sort
#include <cmath>         // std::isnan
#include <filesystem>    // std::filesystem
#include <limits>        // std::numeric_limits
#include <optional>      // std::optional
#include <unordered_map> // std::unordered_map

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

atx::core::Result<Panel>
attach_multi_segment_panel(const std::string &seg_dir, TimeWindow window,
                           std::span<const std::string> fields, UniversePolicy universe) {
  namespace fs = std::filesystem;
  // 1. Enumerate + sort .seg paths (ISO names sort chronologically).
  std::vector<std::string> paths;
  std::error_code ec;
  for (const auto &e : fs::directory_iterator(seg_dir, ec)) {
    if (e.path().extension() == ".seg") paths.push_back(e.path().string());
  }
  if (ec)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: cannot open directory '" + seg_dir +
                              "': " + ec.message());
  if (paths.empty())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: no .seg files in " + seg_dir);
  std::sort(paths.begin(), paths.end());

  // 2. Open readers; collect in-window (segment, local-row, date_nanos) triples and
  //    the global date axis (unique, ascending).
  std::vector<atx::tsdb::SegmentReader> readers;
  readers.reserve(paths.size());
  struct Row {
    atx::usize seg;
    atx::usize t;
    atx::i64 nanos;
  };
  std::vector<Row> rows;
  for (const auto &p : paths) {
    ATX_TRY(auto rdr, atx::tsdb::SegmentReader::attach(p));
    const auto times = rdr.times();
    for (atx::usize t = 0; t < times.size(); ++t) {
      if (times[t] >= window.start_nanos && times[t] < window.end_nanos)
        rows.push_back({readers.size(), t, times[t]});
    }
    readers.push_back(std::move(rdr));
  }
  if (rows.empty())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: window selects no dates");
  // Global date axis (rows already in ascending path/date order; one date per row here).
  std::vector<atx::i64> date_axis;
  for (const auto &r : rows)
    if (date_axis.empty() || date_axis.back() != r.nanos) date_axis.push_back(r.nanos);
  const atx::usize D = date_axis.size();
  std::unordered_map<atx::i64, atx::usize> date_row;
  for (atx::usize d = 0; d < D; ++d) date_row.emplace(date_axis[d], d);

  // 3. Global instrument union (first-seen across rows in ascending date order).
  std::vector<std::string> inst_names;
  std::unordered_map<std::string, atx::usize> inst_of;
  for (const auto &r : rows) {
    const auto &rdr = readers[r.seg];
    for (atx::u32 j = 0; j < rdr.instrument_count(); ++j) {
      const std::string nm{rdr.symbol_name(j)};
      if (inst_of.emplace(nm, inst_names.size()).second) inst_names.push_back(nm);
    }
  }
  const atx::usize N = inst_names.size();

  // 4. Resolve the field list (empty => first reader's fields in order).
  std::vector<std::string> field_names;
  if (fields.empty()) {
    const auto &r0 = readers[rows.front().seg];
    for (atx::u32 f = 0; f < r0.field_count(); ++f) field_names.emplace_back(r0.field_name(f));
  } else {
    field_names.assign(fields.begin(), fields.end());
  }
  const atx::usize F = field_names.size();

  // 5. Allocate owned columns (NaN) + universe mask (0); fill.
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  std::vector<std::vector<atx::f64>> data(F, std::vector<atx::f64>(D * N, nan));
  std::vector<std::uint8_t> mask(D * N, 0);
  for (const auto &r : rows) {
    const auto &rdr = readers[r.seg];
    const atx::usize d = date_row.at(r.nanos);
    // Per-field local indices in THIS segment.
    std::vector<std::optional<atx::u32>> fmap(F);
    for (atx::usize f = 0; f < F; ++f) fmap[f] = rdr.field_index(field_names[f]);
    for (atx::u32 j = 0; j < rdr.instrument_count(); ++j) {
      const atx::usize gi = inst_of.at(std::string{rdr.symbol_name(j)});
      const atx::usize cell = d * N + gi;
      bool present_here = false;
      if (universe.kind == UniverseKind::PresentBitmap) present_here = rdr.present(r.t, j);
      for (atx::usize f = 0; f < F; ++f) {
        if (!fmap[f].has_value()) {
          if (!fields.empty())
            return atx::core::Err(atx::core::ErrorCode::NotFound,
                                  "attach_multi_segment_panel: field '" + field_names[f] +
                                      "' absent in a segment");
          continue;
        }
        data[f][cell] = rdr.value(*fmap[f], r.t, j);
      }
      if (universe.kind == UniverseKind::Field) {
        const auto ufid = rdr.field_index(universe.field_name);
        if (ufid.has_value()) {
          const atx::f64 v = rdr.value(*ufid, r.t, j);
          present_here = !std::isnan(v) && v != 0.0;
        }
      }
      if (present_here) mask[cell] = 1;
    }
  }

  // 6. Build the owned Panel; readers drop at scope exit (data is copied).
  return Panel::create(D, N, std::move(field_names), std::move(data), std::move(mask));
}

} // namespace atx::engine::alpha

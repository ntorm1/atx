#include "atx/engine/alpha/segment_panel.hpp"

#include <algorithm>     // std::lower_bound, std::sort
#include <cmath>         // std::isnan
#include <filesystem>    // std::filesystem
#include <limits>        // std::numeric_limits
#include <optional>      // std::optional
#include <string_view>
#include <unordered_map> // std::unordered_map
#include <unordered_set>

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

atx::core::Result<IndexedPanel>
attach_indexed_multi_segment_panel(const std::string &seg_dir, TimeWindow window,
                                   std::span<const std::string> fields, UniversePolicy universe) {
  namespace fs = std::filesystem;
  if (window.start_nanos > window.end_nanos) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: reversed time window");
  }
  // 1. Deterministic path order is only a tie-break, never the time axis.
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
  std::vector<std::string> source_paths;
  for (const auto &p : paths) {
    ATX_TRY(auto rdr, atx::tsdb::SegmentReader::attach(p));
    const auto times = rdr.times();
    bool selected = false;
    for (atx::usize t = 0; t < times.size(); ++t) {
      if (t > 0 && times[t] <= times[t - 1]) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "attach_multi_segment_panel: non-ascending source timestamps in " + p);
      }
      if (times[t] >= window.start_nanos && times[t] < window.end_nanos) {
        rows.push_back({readers.size(), t, times[t]});
        selected = true;
      }
    }
    if (selected) {
      std::unordered_set<std::string_view> names;
      names.reserve(rdr.instrument_count());
      for (atx::u32 j = 0; j < rdr.instrument_count(); ++j) {
        const auto name = rdr.symbol_name(j);
        if (name.empty() || !names.insert(name).second) {
          return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                "attach_multi_segment_panel: empty/duplicate symbol in " + p);
        }
      }
      source_paths.push_back(p);
    }
    readers.push_back(std::move(rdr));
  }
  if (rows.empty())
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: window selects no dates");
  std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    return a.nanos < b.nanos;
  });
  // Global date axis from actual timestamps, independent of file naming.
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
  if (N != 0 && D > std::numeric_limits<atx::usize>::max() / N) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "attach_multi_segment_panel: date/instrument shape overflows");
  }

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
  // Separate source presence from universe eligibility: masked-out duplicate
  // observations are still ambiguous. Only one date's seen set is needed.
  std::vector<std::uint8_t> seen(N, 0);
  std::optional<atx::usize> seen_date;
  for (const auto &r : rows) {
    const auto &rdr = readers[r.seg];
    const atx::usize d = date_row.at(r.nanos);
    if (!seen_date || *seen_date != d) {
      std::fill(seen.begin(), seen.end(), std::uint8_t{0});
      seen_date = d;
    }
    // Per-field local indices in THIS segment.
    std::vector<std::optional<atx::u32>> fmap(F);
    for (atx::usize f = 0; f < F; ++f) {
      fmap[f] = rdr.field_index(field_names[f]);
      if (!fmap[f] && !fields.empty()) {
        return atx::core::Err(atx::core::ErrorCode::NotFound,
                              "attach_multi_segment_panel: field '" + field_names[f] +
                                  "' absent in a segment");
      }
    }
    std::optional<atx::u32> universe_fid;
    if (universe.kind == UniverseKind::Field) {
      universe_fid = rdr.field_index(universe.field_name);
      if (!universe_fid) {
        return atx::core::Err(atx::core::ErrorCode::NotFound,
                              "attach_multi_segment_panel: universe field absent in a segment");
      }
    }
    for (atx::u32 j = 0; j < rdr.instrument_count(); ++j) {
      if (!rdr.present(r.t, j)) continue; // padding must not erase a present observation
      const atx::usize gi = inst_of.at(std::string{rdr.symbol_name(j)});
      if (seen[gi] != 0) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "attach_multi_segment_panel: duplicate cell for symbol '" +
                                  inst_names[gi] + "' at timestamp " + std::to_string(r.nanos));
      }
      seen[gi] = 1;
      const atx::usize cell = d * N + gi;
      for (atx::usize f = 0; f < F; ++f) {
        if (!fmap[f]) continue;
        data[f][cell] = rdr.value(*fmap[f], r.t, j);
      }
      bool present_here = true;
      if (universe.kind == UniverseKind::Field) {
        const atx::f64 v = rdr.value(*universe_fid, r.t, j);
        present_here = !std::isnan(v) && v != 0.0;
      }
      if (present_here) mask[cell] = 1;
    }
  }

  // 6. Build the owned Panel; readers drop at scope exit (data is copied).
  ATX_TRY(auto panel,
          Panel::create(D, N, std::move(field_names), std::move(data), std::move(mask)));
  return atx::core::Ok(IndexedPanel{std::move(panel), std::move(date_axis),
                                  std::move(inst_names), std::move(source_paths)});
}

atx::core::Result<Panel>
attach_multi_segment_panel(const std::string &seg_dir, TimeWindow window,
                           std::span<const std::string> fields, UniversePolicy universe) {
  ATX_TRY(auto indexed, attach_indexed_multi_segment_panel(seg_dir, window, fields, universe));
  return atx::core::Ok(std::move(indexed.panel));
}

} // namespace atx::engine::alpha

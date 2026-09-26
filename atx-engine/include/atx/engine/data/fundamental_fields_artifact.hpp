#pragma once

// Explicit D1 v2 TSV decoder. V1 points.csv is deliberately not reinterpreted.
// Input is an already hash-validated artifact member; this decoder performs no IO.
#include <charconv>
#include "atx/engine/data/fundamental_fields.hpp"

namespace atx::engine::data::fundamentals {
inline constexpr std::string_view kIntervalHeader =
    "sr_id\towner_id\tlink_id\tavailable_ns\tperiod_end_ns\tidentity_valid_from_ns\t"
    "identity_valid_to_ns\tlink_available_ns\tidentity_only\tlink_priority";

[[nodiscard]] inline atx::core::Result<std::vector<PitRecord>> decode_interval_points(
    std::string_view text, std::span<const std::string> axis_ids,
    atx::usize max_rows = 1'000'000) {
  const auto error = [] { return atx::core::Err(atx::core::ErrorCode::ParseError,
                                               "fundamentals: invalid interval-v2 TSV"); };
  if (text.size() > 256ULL * 1024ULL * 1024ULL || max_rows == 0) return error();
  const auto line = [&text]() {
    const auto end = text.find('\n');
    const auto out = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    return out;
  };
  if (line() != "ATX-FUNDAMENTAL-INTERVALS\t2") return error();
  std::string expected{kIntervalHeader};
  for (auto field : kRawFieldNames) { expected += '\t'; expected += field; }
  if (line() != expected) return error();
  std::unordered_map<std::string_view, atx::usize> axes;
  for (atx::usize i = 0; i < axis_ids.size(); ++i)
    if (!axes.emplace(axis_ids[i], i).second) return error();
  std::vector<PitRecord> result;
  atx::usize count = 0;
  while (!text.empty()) {
    const auto row = line();
    if (row.empty() || row.size() > 8192 || ++count > max_rows) return error();
    std::array<std::string_view, 10 + kRawFieldCount> cells;
    auto rest = row;
    for (atx::usize c = 0; c < cells.size(); ++c) {
      const auto end = rest.find('\t');
      if ((c + 1 < cells.size()) != (end != std::string_view::npos)) return error();
      cells[c] = rest.substr(0, end);
      rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    const auto integer = [](std::string_view value, atx::i64& out) {
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
      return ec == std::errc{} && end == value.data() + value.size();
    };
    PitRecord rec;
    rec.identity_rule = IdentityRule::DatedLinksV2;
    rec.owner_id = cells[1]; rec.link_id = cells[2];
    atx::i64 marker = 0, priority = 0;
    if (!integer(cells[3], rec.available_ns) || !integer(cells[4], rec.period_end_ns) ||
        !integer(cells[5], rec.identity_valid_from_ns) || !integer(cells[6], rec.identity_valid_to_ns) ||
        !integer(cells[7], rec.link_available_ns) || !integer(cells[8], marker) ||
        !integer(cells[9], priority) || marker < 0 || marker > 1 || priority < 0 || priority > 1 ||
        rec.owner_id.empty() || rec.link_id.empty() ||
        rec.identity_valid_from_ns >= rec.identity_valid_to_ns || rec.period_end_ns > rec.available_ns)
      return error();
    rec.identity_only = marker != 0; rec.link_priority = static_cast<atx::u8>(priority);
    for (atx::usize f = 0; f < kRawFieldCount; ++f) {
      const auto value = cells[10 + f];
      rec.values[f] = std::numeric_limits<atx::f64>::quiet_NaN();
      if (value.empty()) continue;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), rec.values[f]);
      if (ec != std::errc{} || end != value.data() + value.size() ||
          !std::isfinite(rec.values[f]) || rec.identity_only) return error();
    }
    const auto axis = axes.find(cells[0]);
    if (axis != axes.end()) { rec.instrument = axis->second; result.push_back(std::move(rec)); }
  }
  return atx::core::Ok(std::move(result));
}
} // namespace atx::engine::data::fundamentals
